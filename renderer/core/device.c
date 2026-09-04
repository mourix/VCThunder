/* device.c -- Glide surfaces, buffers, state and LFB operations.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The core renders into the requested surface without owning a window; platform
 * code presents the visible raster separately.
 */
#include <stdlib.h>
#include <string.h>

#include "vgl.h"
#include "vcglide.h"

vgl_dev_t vgl;

/* The staging buffers behind a write-mode grLfbLock. A caller may write any
 * subset of the region it locked, so handing back the framebuffer converted
 * into the write format and comparing at unlock is the only way to know which
 * pixels it touched: `shadow` is what we handed it, `stage` is what came
 * back, and a pixel that still matches its shadow was never written and its
 * original full-colour value survives untouched. Converting every pixel back
 * instead would quietly round the whole surface through the lock format. */
static uint16_t *s_lfb_stage, *s_lfb_shadow;
static uint32_t *s_lfb_upload;
static uint32_t  s_lfb_words;
static int       s_lfb_pitch;
static int       s_lfb_active, s_lfb_buffer, s_lfb_mode, s_lfb_pp, s_lfb_origin;
static int       s_lfb_full_write;
static int       s_lfb_replace_page;
static int       s_lfb_center_page;
static int       s_lfb_field_width, s_lfb_field_offset;
/* Positive swap intervals identify partial-updating operator menus, which must
 * not use the full-page LFB fast path. */
static int       s_lfb_partial_hint;
static int       s_pipeline_initialized;
static void      saved_reset(void);

/* Diagnostic side-margin census. Record clear, primitive, full-page, and unlock
 * activity per frame to detect alternating-buffer flicker. */
#define MT_CLEAR  1u                    /* grBufferClear covered the clip     */
#define MT_GEOM   2u                    /* a primitive reached a side strip   */
#define MT_PAGE   4u                    /* a full-page LFB write was unlocked */
#define MT_STRIP  8u                    /* the unlock cleared the strips      */
#define MT_WINDOW 300u
static unsigned  s_mt_mask;
static unsigned  s_mt_count[16];
static unsigned  s_mt_frames;

/* Convert core XRGB (0x00RRGGBB) to bytes R,G,B,A in little-endian memory,
 * which is what SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM uploads. */
static uint32_t lfb_upload_rgba_alpha(uint32_t v, unsigned a)
{
    return ((a & 0xffu) << 24) | ((v & 0x000000ffu) << 16) |
           (v & 0x0000ff00u) | ((v & 0x00ff0000u) >> 16);
}

static uint32_t lfb_upload_rgba(uint32_t v)
{
    return lfb_upload_rgba_alpha(v, 255);
}

/* ---- the surface -------------------------------------------------------- */

static int refresh_hz(int refresh_enum)
{
    /* Glide 2 GrScreenRefresh_t values 0..8.  NONE/unknown means the default
     * cabinet timing, which is 60 Hz for both observed opens. */
    static const int hz[] = { 60, 70, 72, 75, 80, 90, 100, 85, 120 };

    if (refresh_enum >= 0 &&
        (unsigned)refresh_enum < sizeof hz / sizeof hz[0])
        return hz[refresh_enum];
    return 60;
}

int vgl_device_open(void *caller_window, int res_enum, int refresh_enum,
                    int origin, int color_buffers, int aux_buffers)
{
    size_t px;
    int reset_pipeline = !s_pipeline_initialized;

    vgl_device_close();

    if (res_enum < 0 || (unsigned)res_enum >= vgl_resolution_count) {
        vgl_log(0, "device: grSstWinOpen with resolution enum %d, which is "
                   "not in the table: refusing", res_enum);
        return 0;
    }
    vgl.w = vgl_resolutions[res_enum].width;
    vgl.h = vgl_resolutions[res_enum].height;
    vgl_refresh_geometry(vgl.w, vgl.h);
    s_lfb_field_width = vgl_cfg.lfb_width;
    s_lfb_field_offset = s_lfb_field_width > 0
        ? (vgl_cfg.raster_width - s_lfb_field_width) / 2 : 0;
    px = (size_t)vgl.w * (size_t)vgl.h;

    /* The Voodoo2 LFB is not tightly pitched.  Both titles receive 2048
     * bytes (1024 16-bit pixels) per row from the cabinet-compatible wrapper
     * at every observed 512- and 640-wide mode, and their service renderers
     * retain that board layout.  A tight 1280-byte staging pitch left normal
     * paths intact because they use GrLfbInfo::strideInBytes, but folded both
     * service screens across rows.  Keep the logical surface width separate
     * from the board-style power-of-two LFB pitch. */
    s_lfb_pitch = 1024;
    while (s_lfb_pitch < vgl.w)
        s_lfb_pitch <<= 1;

    vgl.color[0] = (uint32_t *)calloc(px, 4);
    vgl.color[1] = (color_buffers >= 2) ? (uint32_t *)calloc(px, 4)
                                        : vgl.color[0];
    vgl.depth    = (aux_buffers >= 1) ? (uint16_t *)calloc(px, 2) : NULL;
    if (!vgl.color[0] || !vgl.color[1]) {
        vgl_log(0, "device: cannot allocate a %dx%d surface", vgl.w, vgl.h);
        vgl_device_close();
        return 0;
    }
    /* Allocate one extra surface of slack because game occlusion tests may read
     * beyond the nominal framebuffer while remaining within cabinet memory. */
    s_lfb_words = (uint32_t)((size_t)s_lfb_pitch * (size_t)vgl.h);
    s_lfb_stage  = (uint16_t *)calloc((size_t)s_lfb_words * 2u, 2);
    s_lfb_shadow = (uint16_t *)calloc((size_t)s_lfb_words * 2u, 2);
    s_lfb_upload = (uint32_t *)calloc(px, 4);
    if ((aux_buffers >= 1 && !vgl.depth) || !s_lfb_stage || !s_lfb_shadow ||
        !s_lfb_upload) {
        vgl_log(0, "device: cannot allocate the depth/LFB surfaces for %dx%d",
                vgl.w, vgl.h);
        vgl_device_close();
        return 0;
    }

    vgl.origin_lower_left = (origin == GR_ORIGIN_LOWER_LEFT);
    vgl.draw_buffer = (color_buffers >= 2) ? 1 : 0;
    vgl.open = 1;
    /* grSstWinOpen replaces the surface, not Glide's global pipeline state.
     * Offroad proves the distinction at frame 9: it opens its final gameplay
     * surface after enabling W buffering and never sends grDepthBufferMode
     * again.  Resetting here disabled depth for the rest of attract mode,
     * letting distant snow walls overwrite the foreground barriers.  The
     * first open in a Glide session gets the documented defaults; subsequent
     * opens keep the caller's already-established state. */
    if (reset_pipeline) {
        vgl_device_reset_state();
        s_pipeline_initialized = 1;
    }
    vgl.clip_x0 = 0; vgl.clip_y0 = 0;
    vgl.clip_x1 = vgl.w; vgl.clip_y1 = vgl.h;
    vgl.field_x0 = s_lfb_field_width > 0 ? s_lfb_field_offset : 0;
    vgl.field_x1 = s_lfb_field_width > 0
        ? s_lfb_field_offset + s_lfb_field_width : vgl.w;
    vgl.margin_drawn = 0;
    vgl_tex_init();

    vgl.gpu = 0;
    if (strcmp(vgl_cfg.renderer, "cpu") != 0)
        vgl.gpu = vgl_gpu_open(caller_window, vgl.w, vgl.h,
                               vgl_cfg.raster_width, vgl_cfg.raster_height,
                               vgl_cfg.render_scale, vgl_cfg.scale,
                               refresh_hz(refresh_enum), vgl_cfg.present);
    if (!vgl.gpu && strcmp(vgl_cfg.renderer, "gpu") == 0)
        vgl_log(0, "device: requested SDL GPU backend unavailable; "
                   "continuing with the CPU reference renderer");

    vgl_log(0, "device: %dx%d full-colour surface, %d-byte LFB pitch, %d "
               "colour buffer(s), %s depth, origin %s: %s renderer",
            vgl.w, vgl.h, s_lfb_pitch * 2,
            color_buffers, vgl.depth ? "16-bit" : "no",
            vgl.origin_lower_left ? "lower-left" : "upper-left",
            vgl.gpu ? "SDL GPU" : "CPU reference");
    if (s_lfb_field_width > 0)
        vgl_log(0, "lfb: native %d-wide 2D fields -> x=%d..%d in the "
                   "%d-wide raster (read coordinates unchanged)",
                s_lfb_field_width, s_lfb_field_offset,
                s_lfb_field_offset + s_lfb_field_width - 1,
                vgl_cfg.raster_width);
    if (!vgl.gpu)
        vgl_par_open(vgl_cfg.threads);
    if (!vgl.gpu && vgl_cfg.present)
        vgl.presenting = vgl_present_open(caller_window, vgl.w, vgl.h,
                                          vgl_cfg.scale,
                                          refresh_hz(refresh_enum));
    else
        vgl.presenting = vgl.gpu && vgl_cfg.present;
    return 1;
}

void vgl_device_close(void)
{
    if (vgl.gpu)
        vgl_gpu_close();
    else if (vgl.presenting)
        vgl_present_close();
    /* The worker pool belongs to the raster surface, not to presentation.
     * Headless replay normally hid this distinction: with presentation off,
     * repeated grSstWinOpen calls kept the old pool alive.  A live reopen did
     * close it, and the replacement workers then inherited a stale job
     * generation and ran the previous triangle's dead stack context. */
    vgl_par_close();
    vgl.presenting = 0;
    if (vgl.color[1] != vgl.color[0])
        free(vgl.color[1]);
    free(vgl.color[0]);
    free(vgl.depth);
    free(s_lfb_stage);
    free(s_lfb_shadow);
    free(s_lfb_upload);
    vgl.color[0] = vgl.color[1] = NULL;
    vgl.depth = NULL;
    s_lfb_stage = s_lfb_shadow = NULL;
    s_lfb_upload = NULL;
    s_lfb_words = 0;
    s_lfb_pitch = 0;
    s_lfb_active = 0;
    s_lfb_full_write = 0;
    s_lfb_replace_page = 0;
    s_lfb_center_page = 0;
    s_lfb_field_width = s_lfb_field_offset = 0;
    s_lfb_partial_hint = 0;
    vgl.open = 0;
    vgl.gpu = 0;
}

void vgl_device_shutdown(void)
{
    vgl_device_close();
    s_pipeline_initialized = 0;
    saved_reset();
}

static uint32_t *color_ptr(int which)
{
    switch (which) {
    case VGL_BUFFER_FRONT: return vgl.color[0];
    case VGL_BUFFER_BACK:  return vgl.color[vgl.draw_buffer];
    default:               return NULL;
    }
}

static uint16_t *depth_ptr(int which)
{
    return (which == VGL_BUFFER_AUX || which == VGL_BUFFER_DEPTH)
             ? vgl.depth : NULL;
}

/* Glide's own defaults, plus the ones grDisableAllEffects restores. Called at
 * the first grSstWinOpen in a session and by grDisableAllEffects, which is why
 * the two halves are split: the second is what "all effects off" means. */
static void reset_effects(void)
{
    vgl.depth_mode = VGL_DEPTH_DISABLE;
    vgl.depth_func = VGL_CMP_LESS;
    vgl.depth_mask = 0;
    vgl.depth_bias = 0;
    vgl.alpha_func = VGL_CMP_ALWAYS;
    vgl.alpha_ref  = 0;
    vgl.blend_rgb_src = VGL_BLEND_ONE;
    vgl.blend_rgb_dst = VGL_BLEND_ZERO;
    vgl.blend_a_src   = VGL_BLEND_ONE;
    vgl.blend_a_dst   = VGL_BLEND_ZERO;
    vgl.fog_mode      = 0;
    vgl.chroma_mode   = 0;
}

/* grDisableAllEffects turns the special effects OFF (blending, alpha test,
 * depth buffering, fog, chroma-key), and that is ALL it does. It is not a
 * device reset: the combine units, the texture filters and the constant colour
 * survive it, and a provider that reset those as well would silently undo
 * state the caller never restated. Hydro calls it 87 times in 1,500 frames,
 * always around its 2D layer. */
void vgl_disable_all_effects(void)
{
    reset_effects();
}

void vgl_device_reset_state(void)
{
    int t;

    reset_effects();
    vgl.dither = VGL_DITHER_DISABLE;
    vgl.color_mask_rgb = 1;
    vgl.color_mask_a = 0;
    vgl.constant_color = 0xffffffffu;
    vgl.chroma_value = 0;
    vgl.fog_color = 0;
    vgl.cull_mode = 0;
    vgl.itrgb_lighting = 0;
    vgl.stw_hint = 0;
    vgl.lfb_const_alpha = 0xff;
    vgl.lfb_const_depth = 0;
    vgl.cc.func = VGL_CF_LOCAL; vgl.cc.factor = VGL_FAC_ZERO;
    vgl.cc.local = VGL_LOCAL_ITERATED; vgl.cc.other = VGL_OTHER_ITERATED;
    vgl.cc.invert = 0;
    vgl.ac = vgl.cc;

    for (t = 0; t < VGL_TMUS; t++) {
        vgl_tmu_t *u = &vgl.tmu[t];

        u->min_filter = u->mag_filter = VGL_TEXFILTER_BILINEAR;
        u->mipmap_mode = VGL_MIPMAP_DISABLE;
        u->lod_blend = 0;
        u->clamp_s = u->clamp_t = VGL_TEXCLAMP_WRAP;
        u->lod_bias = 0.0f;
        u->c_func = VGL_CF_LOCAL; u->c_factor = VGL_FAC_ZERO;
        u->a_func = VGL_CF_LOCAL; u->a_factor = VGL_FAC_ZERO;
        u->c_invert = u->a_invert = 0;
    }
}

/* ---- clear and swap ----------------------------------------------------- */

void vgl_buffer_clear(uint32_t color, uint32_t alpha, uint32_t depth)
{
    uint32_t *c = vgl.color[vgl.draw_buffer];
    int x, y;
    uint32_t v;

    (void)alpha;                        /* no alpha buffer: auxbufs is depth */
    if (!vgl.open)
        return;
    vgl.frame_drawn = 1;
    vgl.margin_drawn = 1;                /* a clear covers the whole clip */
    s_mt_mask |= MT_CLEAR;
    if (vgl.gpu) {
        vgl_gpu_clear(color, depth);
        return;
    }
    /* grSstWinOpen's colour format is ARGB for both games (cformat 0). The
     * software surface has no alpha plane, so retain RGB at its full input
     * precision and discard alpha. */
    v = color & 0x00ffffffu;
    for (y = vgl.clip_y0; y < vgl.clip_y1; y++) {
        uint32_t *row = c + (size_t)y * vgl.w;
        uint16_t *zrow = vgl.depth ? vgl.depth + (size_t)y * vgl.w : NULL;
        for (x = vgl.clip_x0; x < vgl.clip_x1; x++) {
            row[x] = v;
            if (zrow)
                zrow[x] = (uint16_t)depth;
        }
    }
}

/* Clear untouched widescreen margins at swap so native-width 2D screens cannot
 * alternate stale margin content between colour buffers. */
static void margin_strips_clear(void)
{
    uint32_t *dst = vgl.color[vgl.draw_buffer];
    int x, y;

    s_mt_mask |= MT_STRIP;
    if (vgl.gpu) {
        vgl_gpu_clear_margins(vgl.clip_x0, vgl.clip_x1, vgl.clip_y0,
                              vgl.clip_y1, vgl.field_x0, vgl.field_x1);
        return;
    }
    if (!dst)
        return;
    for (y = vgl.clip_y0; y < vgl.clip_y1; y++) {
        uint32_t *d = dst + (size_t)y * vgl.w;

        for (x = vgl.clip_x0; x < vgl.clip_x1; x++)
            if (x < vgl.field_x0 || x >= vgl.field_x1)
                d[x] = 0;
    }
}

/* One line per MT_WINDOW frames, naming every pattern that occurred and how
 * often. Two patterns in roughly equal numbers is a flicker; one pattern is a
 * steady picture, whatever that picture is. */
static void margin_trace_frame(void)
{
    unsigned i;

    if (vgl.margin_geom)
        s_mt_mask |= MT_GEOM;
    s_mt_count[s_mt_mask & 15u]++;
    if (++s_mt_frames < MT_WINDOW)
        return;
    vgl_log(0, "margin trace: %u frames, native field %d..%d of %d wide; "
               "c=clear g=geometry p=full LFB page s=strip clear",
            s_mt_frames, vgl.field_x0, vgl.field_x1, vgl.w);
    for (i = 0; i < 16u; i++) {
        if (!s_mt_count[i])
            continue;
        vgl_log(0, "margin trace:   %c%c%c%c  %u frames",
                (i & MT_CLEAR) ? 'c' : '-', (i & MT_GEOM)  ? 'g' : '-',
                (i & MT_PAGE)  ? 'p' : '-', (i & MT_STRIP) ? 's' : '-',
                s_mt_count[i]);
    }
    memset(s_mt_count, 0, sizeof s_mt_count);
    s_mt_frames = 0;
}

void vgl_buffer_swap(int interval)
{
    if (!vgl.open)
        return;
    s_lfb_partial_hint = interval > 0;
    vgl.frames++;
    if (s_lfb_field_width > 0 && !vgl.margin_drawn)
        margin_strips_clear();
    if (!vgl.gpu)
        vgl_raster_collect();           /* the workers' counters become ours */
    if (vgl.color[1] != vgl.color[0]) {
        uint32_t *t = vgl.color[0];
        vgl.color[0] = vgl.color[1];
        vgl.color[1] = t;
        /* draw_buffer stays 1: the back buffer is always the one not shown. */
    }
    if (vgl.gpu) {
        vgl_gpu_swap(interval);
    } else if (vgl.presenting) {
        vgl_present_frame(vgl.color[0], vgl.w, vgl.h, interval);
        vgl_present_pump();
    }
    if (vgl_cfg.margin_trace)
        margin_trace_frame();
    vgl.frame_drawn = 0;
    vgl.margin_drawn = 0;
    vgl.margin_geom = 0;
    s_mt_mask = 0;
}

/* ---- the linear frame buffer -------------------------------------------- */

static void put32(void *p, unsigned off, uint32_t v)
{
    memcpy((uint8_t *)p + off, &v, 4);
}

/* Map the visible raster to the bottom of a taller lower-left surface. Apply
 * the resulting row offset to upper-left LFB locks and bound every access. */
static int lfb_row_bias(void)
{
    int pad;

    if (s_lfb_field_width <= 0 || vgl_cfg.raster_height <= 0)
        return 0;
    pad = vgl.h - vgl_cfg.raster_height;
    return pad > 0 ? pad : 0;
}

/* The full-colour surface <-> the 16-bit write formats the two games use.
 * Hydro locks with 555 and 1555, Offroad with 565; nothing else has ever been
 * seen and anything else says so. */
static uint32_t from_writemode(uint16_t v, int mode, int *alpha)
{
    int r, g, b;

    switch (mode) {
    case VGL_LFBWRITE_565:
        r = (v >> 11) & 0x1f; g = (v >> 5) & 0x3f; b = v & 0x1f;
        *alpha = 255;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        break;
    case VGL_LFBWRITE_555:
        r = (v >> 10) & 0x1f; g = (v >> 5) & 0x1f; b = v & 0x1f;
        *alpha = 255;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        break;
    case VGL_LFBWRITE_1555:
        r = (v >> 10) & 0x1f; g = (v >> 5) & 0x1f; b = v & 0x1f;
        *alpha = (v & 0x8000u) ? 255 : 0;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        break;
    default:
        *alpha = 255;
        return from_writemode(v, VGL_LFBWRITE_565, alpha);
    }
    return (uint32_t)((r << 16) | (g << 8) | b);
}

static uint16_t to_writemode(uint32_t rgb, int mode)
{
    int r = (int)((rgb >> 16) & 0xff), g = (int)((rgb >> 8) & 0xff);
    int b = (int)(rgb & 0xff);
    int r5 = vgl_narrow8(r, 31, 127), g5 = vgl_narrow8(g, 31, 127);
    int g6 = vgl_narrow8(g, 63, 127), b5 = vgl_narrow8(b, 31, 127);

    switch (mode) {
    case VGL_LFBWRITE_555:  return (uint16_t)((r5 << 10) | (g5 << 5) | b5);
    case VGL_LFBWRITE_1555: return (uint16_t)(0x8000u | (r5 << 10) |
                                              (g5 << 5) | b5);
    case VGL_LFBWRITE_565:
    default:                return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
}

int vgl_lfb_lock(uint32_t type, uint32_t buffer, uint32_t write_mode,
                 uint32_t origin, uint32_t pixel_pipeline, void *info)
{
    static int logged_1555_pipeline;
    uint32_t *src = color_ptr((int)buffer);
    uint16_t *zsrc = depth_ptr((int)buffer);
    uint32_t i;
    int rows = vgl.h, w = vgl.w, pitch = s_lfb_pitch;
    int direct_write;

    if (!vgl.open || !info || (!src && !zsrc) || !s_lfb_stage) {
        /* Capped: one per grLfbLock, and both titles lock every frame. */
        VGL_LOG_CAPPED(3, "lfb: lock refused (open=%d buffer=%u)",
                       vgl.open, buffer);
        return 0;
    }
    if (s_lfb_active)
        VGL_LOG_CAPPED(3, "lfb: a lock is already active; the previous one is "
                          "lost");
    if (!logged_1555_pipeline && pixel_pipeline &&
        (int)write_mode == VGL_LFBWRITE_1555) {
        logged_1555_pipeline = 1;
        vgl_log(0, "lfb: first 1555 pixel-pipeline lock; alpha %d ref %d, "
                   "blend rgb %d/%d, frame already drawn %d, clip %d,%d-%d,%d",
                vgl.alpha_func, vgl.alpha_ref, vgl.blend_rgb_src,
                vgl.blend_rgb_dst, vgl.frame_drawn,
                vgl.clip_x0, vgl.clip_y0, vgl.clip_x1, vgl.clip_y1);
    }

    /* Both measured software-render paths use WRITE_ONLY locks. Opaque boot
     * pages replace the whole clipped native layer; Hydro's later 1555 path
     * is either a complete 2D page or a 1-bit SRC_ALPHA overlay over 3D drawn
     * earlier in this frame. Neither needs the old framebuffer handed back,
     * and synchronously reading the GPU here once per frame was the main
     * accelerated renderer stall. Other pixel-pipeline tuples keep the fully
     * general read/modify/write path below. */
    direct_write = type == GR_LFB_WRITE_ONLY &&
        (!pixel_pipeline ||
         ((int)write_mode == VGL_LFBWRITE_1555 &&
          vgl.blend_rgb_src == VGL_BLEND_SRC_ALPHA &&
          vgl.blend_rgb_dst == VGL_BLEND_ONE_MINUS_SRC_ALPHA &&
          vgl.alpha_func == VGL_CMP_GEQUAL));
    /* WRITE_ONLY permits partial updates. Use the full-page fast path only for
     * known full writers; seed partial pages from the current colour buffer. */
    s_lfb_full_write = (vgl.gpu || s_lfb_field_width > 0) && direct_write &&
                       !s_lfb_partial_hint;
    s_lfb_replace_page = s_lfb_full_write && pixel_pipeline &&
                         !vgl.frame_drawn;
    /* Clear around a native-width page only when no earlier draw established
     * wider frame content. */
    s_lfb_center_page = s_lfb_field_width > 0 && s_lfb_full_write &&
                        !vgl.frame_drawn;
    if (vgl.gpu && src && !s_lfb_full_write &&
        !vgl_gpu_sync_color((int)buffer, src, vgl.w, vgl.h))
        /* Capped, and this one matters: a readback that fails is a symptom a
         * sick device shows every frame, which is the condition under which an
         * unbounded line fills a disk. */
        VGL_LOG_CAPPED(3, "lfb: SDL GPU colour readback failed; returning the "
                          "last native-resolution snapshot");

    s_lfb_active = 1;
    s_lfb_buffer = (int)buffer;
    s_lfb_mode   = (int)write_mode;
    s_lfb_pp     = (int)pixel_pipeline;
    s_lfb_origin = (int)origin;

    /* WRITE_ONLY promises the caller no previous contents. Returning our old
     * staging allocation anyway made any pixel the software renderer skipped
     * survive into its next page; Hydro exposed that as startup text ghosted
     * below the Midway logo. Zero is deterministic, becomes transparent in
     * 1555, and costs no GPU synchronization. */
    if (s_lfb_full_write)
        memset(s_lfb_stage, 0, (size_t)s_lfb_words * 2);

    /* Present staging rows in the caller's origin convention. Seed write locks
     * with the same row and field-offset mapping used at unlock so untouched
     * pixels preserve their screen positions. */
    for (i = 0; i < (uint32_t)rows && !s_lfb_full_write; i++) {
        /* Same row bias as the write-back, for the same reason as the column
         * one: staging row i must mean the same surface row in and out. A row
         * the bias pushes off the surface has nothing to seed from. */
        int sy = origin == GR_ORIGIN_LOWER_LEFT
                   ? (rows - 1 - (int)i) : (int)i + lfb_row_bias();
        uint16_t *d = s_lfb_stage + (size_t)i * pitch;
        int seed_off = (type != GR_LFB_READ_ONLY && s_lfb_field_width > 0)
                         ? s_lfb_field_offset : 0;
        int x;

        if (sy < 0 || sy >= rows) {
            memset(d, 0, (size_t)w * 2);
            continue;
        }
        if (zsrc) {
            memcpy(d, zsrc + (size_t)sy * w, (size_t)w * 2);
        } else {
            for (x = 0; x < w; x++) {
                int sx = x + seed_off;

                d[x] = (sx >= 0 && sx < w)
                     ? to_writemode(src[(size_t)sy * w + sx],
                           type == GR_LFB_READ_ONLY ? VGL_LFBWRITE_565
                                                    : (int)write_mode)
                     : 0;
            }
        }
    }
    if (type != GR_LFB_READ_ONLY)
        memcpy(s_lfb_shadow, s_lfb_stage, (size_t)s_lfb_words * 2);

    put32(info, GR_LFBINFO_PTR,    (uint32_t)(uintptr_t)s_lfb_stage);
    put32(info, GR_LFBINFO_STRIDE, (uint32_t)(pitch * 2));
    put32(info, GR_LFBINFO_WRITE,  write_mode);
    put32(info, GR_LFBINFO_ORIGIN, origin);
    return 1;
}

int vgl_lfb_unlock(uint32_t type, uint32_t buffer)
{
    uint32_t *dst = color_ptr((int)buffer);
    int y, x, w = vgl.w, rows = vgl.h, pitch = s_lfb_pitch;

    if (!s_lfb_active)
        return 0;
    s_lfb_active = 0;
    if (type == GR_LFB_READ_ONLY || !dst || buffer == VGL_BUFFER_AUX ||
        buffer == VGL_BUFFER_DEPTH) {
        s_lfb_full_write = 0;
        s_lfb_replace_page = 0;
        s_lfb_center_page = 0;
        return 1;
    }

    if (vgl.gpu && s_lfb_upload)
        memset(s_lfb_upload, 0, (size_t)w * (size_t)rows * 4u);
    /* Clear side margins after native-width page updates unless another draw
     * already painted them. Preserve the field for partial page writers. */
    if (s_lfb_full_write)
        s_mt_mask |= MT_PAGE;
    if (s_lfb_field_width > 0 && !vgl.margin_drawn) {
        s_mt_mask |= MT_STRIP;
        for (y = vgl.clip_y0; y < vgl.clip_y1; y++) {
            uint32_t *d = dst + (size_t)y * w;

            for (x = vgl.clip_x0; x < vgl.clip_x1; x++) {
                if (x >= vgl.field_x0 && x < vgl.field_x1)
                    continue;
                d[x] = 0;
                if (s_lfb_upload)
                    s_lfb_upload[(size_t)y * w + x] = lfb_upload_rgba(0);
            }
        }
    }
    if (s_lfb_full_write) {
        /* A complete native 2D page owns the whole visible field. Clear it
         * first so nothing of the preceding page survives inside it; HUD
         * overlays deliberately skip this because their transparent pixels
         * preserve the scene beneath them. The strips are handled above. */
        if (s_lfb_center_page) {
            for (y = vgl.clip_y0; y < vgl.clip_y1; y++) {
                uint32_t *d = dst + (size_t)y * w;
                for (x = vgl.field_x0; x < vgl.field_x1; x++) {
                    if (x < vgl.clip_x0 || x >= vgl.clip_x1)
                        continue;
                    d[x] = 0;
                    if (s_lfb_upload)
                        s_lfb_upload[(size_t)y * w + x] =
                            lfb_upload_rgba(0);
                }
            }
        }
        for (y = 0; y < rows; y++) {
            const uint16_t *s = s_lfb_stage + (size_t)y * pitch;
            int dy = (s_lfb_origin == GR_ORIGIN_LOWER_LEFT)
                       ? rows - 1 - y : y + lfb_row_bias();
            uint32_t *d;

            if (dy < 0 || dy >= rows)
                continue;
            d = dst + (size_t)dy * w;
            int sx0 = s_lfb_field_width > 0 ? 0 : vgl.clip_x0;
            int sx1 = s_lfb_field_width > 0 ? s_lfb_field_width : vgl.clip_x1;

            if (dy < vgl.clip_y0 || dy >= vgl.clip_y1)
                continue;
            for (x = sx0; x < sx1; x++) {
                int a;
                int dx = x + s_lfb_field_offset;
                uint32_t v = from_writemode(s[x], s_lfb_mode, &a);
                unsigned upload_a = s_lfb_pp && !s_lfb_replace_page
                                      ? (unsigned)a : 255u;

                if (dx < vgl.clip_x0 || dx >= vgl.clip_x1)
                    continue;

                /* Hydro uses the same 1555 stream for two jobs. With no 3D or
                 * clear earlier in the frame it is a complete 2D page: alpha
                 * zero is its black background and must erase the preceding
                 * startup page (otherwise its version text shadows Midway's
                 * logo). After triangles, it is the HUD: alpha zero preserves
                 * the freshly drawn 3D target. Opaque texels replace in both. */
                if (!a && !s_lfb_replace_page)
                    continue;
                if (!a)
                    v = 0;
                d[dx] = v;
                if (s_lfb_upload)
                    s_lfb_upload[(size_t)dy * w + dx] =
                        lfb_upload_rgba_alpha(v, upload_a);
                vgl.n_lfb_writes++;
            }
        }
        if (vgl.gpu && s_lfb_upload)
            vgl_gpu_upload_lfb((int)buffer, s_lfb_upload, w, rows);
        vgl.frame_drawn = 1;
        s_lfb_full_write = 0;
        s_lfb_replace_page = 0;
        s_lfb_center_page = 0;
        return 1;
    }
    for (y = 0; y < rows; y++) {
        const uint16_t *s = s_lfb_stage + (size_t)y * pitch;
        const uint16_t *sh = s_lfb_shadow + (size_t)y * pitch;
        int dy = (s_lfb_origin == GR_ORIGIN_LOWER_LEFT)
                   ? (rows - 1 - y) : y + lfb_row_bias();
        uint32_t *d;

        if (dy < 0 || dy >= rows)
            continue;
        d = dst + (size_t)dy * w;

        for (x = 0; x < w; x++) {
            int a, sr, sg, sb;
            int dx;
            uint32_t v;

            if (s[x] == sh[x])          /* never written */
                continue;
            if (s_lfb_field_width > 0 && x >= s_lfb_field_width)
                continue;
            dx = x + s_lfb_field_offset;
            if (dx < vgl.clip_x0 || dx >= vgl.clip_x1 ||
                dy < vgl.clip_y0 || dy >= vgl.clip_y1)
                continue;
            v = from_writemode(s[x], s_lfb_mode, &a);
            vgl.n_lfb_writes++;

            if (!s_lfb_pp) {
                d[dx] = v;
                if (vgl.gpu && s_lfb_upload)
                    s_lfb_upload[(size_t)dy * w + dx] = lfb_upload_rgba(v);
                continue;
            }
            /* The pixel pipeline, for the part of it an LFB write can reach:
             * the alpha test and the blend. There is no depth test here:
             * an LFB write's depth is grLfbConstantDepth, which neither game
             * ever sets, so it stays at the near end where every comparison
             * the games use passes. If a title is ever seen setting it, that
             * assumption is the first thing to remove. */
            if (vgl.alpha_func != VGL_CMP_ALWAYS) {
                unsigned av = (unsigned)(s_lfb_mode == VGL_LFBWRITE_1555
                                         ? a : (int)vgl.lfb_const_alpha);
                switch (vgl.alpha_func) {
                case VGL_CMP_NEVER:    continue;
                case VGL_CMP_LESS:     if (!(av <  (unsigned)vgl.alpha_ref)) continue; break;
                case VGL_CMP_EQUAL:    if (!(av == (unsigned)vgl.alpha_ref)) continue; break;
                case VGL_CMP_LEQUAL:   if (!(av <= (unsigned)vgl.alpha_ref)) continue; break;
                case VGL_CMP_GREATER:  if (!(av >  (unsigned)vgl.alpha_ref)) continue; break;
                case VGL_CMP_NOTEQUAL: if (!(av != (unsigned)vgl.alpha_ref)) continue; break;
                case VGL_CMP_GEQUAL:   if (!(av >= (unsigned)vgl.alpha_ref)) continue; break;
                default: break;
                }
            }
            if (vgl.blend_rgb_src == VGL_BLEND_ONE &&
                vgl.blend_rgb_dst == VGL_BLEND_ZERO) {
                d[dx] = v;
                if (vgl.gpu && s_lfb_upload)
                    s_lfb_upload[(size_t)dy * w + dx] = lfb_upload_rgba(v);
                continue;
            }
            sr = (int)((v >> 16) & 0xff);
            sg = (int)((v >> 8) & 0xff);
            sb = (int)(v & 0xff);
            {
                int dr = (int)((d[dx] >> 16) & 0xff);
                int dg = (int)((d[dx] >> 8) & 0xff);
                int db = (int)(d[dx] & 0xff), sf, df, r, g, b;
                sf = (vgl.blend_rgb_src == VGL_BLEND_SRC_ALPHA) ? a
                   : (vgl.blend_rgb_src == VGL_BLEND_ZERO) ? 0 : 255;
                df = (vgl.blend_rgb_dst == VGL_BLEND_ONE_MINUS_SRC_ALPHA) ? 255 - a
                   : (vgl.blend_rgb_dst == VGL_BLEND_ONE) ? 255 : 0;
                r = vgl_clamp8(vgl_mul8(sr, sf) + vgl_mul8(dr, df));
                g = vgl_clamp8(vgl_mul8(sg, sf) + vgl_mul8(dg, df));
                b = vgl_clamp8(vgl_mul8(sb, sf) + vgl_mul8(db, df));
                d[dx] = (uint32_t)((r << 16) | (g << 8) | b);
                if (vgl.gpu && s_lfb_upload)
                    s_lfb_upload[(size_t)dy * w + dx] =
                        lfb_upload_rgba(d[dx]);
            }
        }
    }
    if (vgl.gpu && s_lfb_upload)
        vgl_gpu_upload_lfb((int)buffer, s_lfb_upload, w, rows);
    vgl.frame_drawn = 1;
    s_lfb_full_write = 0;
    s_lfb_replace_page = 0;
    s_lfb_center_page = 0;
    return 1;
}

/* Read a top-down framebuffer region for capture and conformance tests. */
int vgl_lfb_read_region(uint32_t buffer, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint32_t stride, void *dst)
{
    const uint32_t *src = color_ptr((int)buffer);
    const uint16_t *zsrc = depth_ptr((int)buffer);
    uint32_t r, col;
    uint16_t *out;

    if (!vgl.open || (!src && !zsrc) || !dst)
        return 0;
    if (x + w > (uint32_t)vgl.w || y + h > (uint32_t)vgl.h)
        return 0;
    if (vgl.gpu && src &&
        !vgl_gpu_sync_color((int)buffer, (uint32_t *)src, vgl.w, vgl.h))
        return 0;
    for (r = 0; r < h; r++) {
        if (zsrc) {
            memcpy((uint8_t *)dst + (size_t)r * stride,
                   zsrc + (size_t)(y + r) * vgl.w + x, (size_t)w * 2);
            continue;
        }
        out = (uint16_t *)((uint8_t *)dst + (size_t)r * stride);
        for (col = 0; col < w; col++)
            out[col] = to_writemode(
                src[(size_t)(y + r) * vgl.w + x + col], VGL_LFBWRITE_565);
    }
    return 1;
}

/* ---- grGlideGetState / grGlideSetState ----------------------------------
 *
 * A caller allocates a GrState of the REAL Glide's size and hands us the
 * pointer. We do not know that size and may not invent one, so the caller's
 * buffer holds an eight-byte opaque token into a provider-owned table. Tokens
 * are never recycled during a Glide session: a ninth live state block must not
 * silently become an alias for the first. */
#define VGL_STATE_MAGIC  0x56474C53u    /* "VGLS" */

typedef struct {
    uint32_t token;
    vgl_pipeline_state_t pipeline;
} vgl_saved_state_t;

static vgl_saved_state_t *s_saved;
static size_t s_saved_count, s_saved_cap;
static uint32_t s_saved_next = 1;

static vgl_saved_state_t *saved_find(uint32_t token)
{
    size_t i;

    for (i = 0; i < s_saved_count; i++)
        if (s_saved[i].token == token)
            return &s_saved[i];
    return NULL;
}

static vgl_saved_state_t *saved_new(void)
{
    vgl_saved_state_t *grown, *slot;
    size_t cap;
    uint32_t token;

    if (s_saved_count == s_saved_cap) {
        cap = s_saved_cap ? s_saved_cap * 2u : 8u;
        grown = (vgl_saved_state_t *)realloc(s_saved, cap * sizeof *grown);
        if (!grown) {
            vgl_log(0, "state: cannot allocate another saved-state slot");
            return NULL;
        }
        s_saved = grown;
        s_saved_cap = cap;
    }
    do {
        token = s_saved_next++;
        if (!s_saved_next)
            s_saved_next = 1;
    } while (!token || saved_find(token));
    slot = &s_saved[s_saved_count++];
    memset(slot, 0, sizeof *slot);
    slot->token = token;
    return slot;
}

/* Both directions are generated from VGL_PIPELINE_MEMBERS (renderer/core/vgl.h),
 * so a member can only be in the snapshot or out of it, never half in. */
#define PIPE_SAVE_SCALAR(type, name)    s->name = vgl.name;
#define PIPE_SAVE_ARRAY(type, name, n)  memcpy(s->name, vgl.name, sizeof s->name);

static void pipeline_save(vgl_pipeline_state_t *s)
{
    VGL_PIPELINE_MEMBERS(PIPE_SAVE_SCALAR, PIPE_SAVE_ARRAY)
}

#define PIPE_LOAD_SCALAR(type, name)    vgl.name = s->name;
#define PIPE_LOAD_ARRAY(type, name, n)  memcpy(vgl.name, s->name, sizeof vgl.name);

/* A texture unit carries five members that belong to THIS session rather than
 * to the saved state: the memory it was given, its extent, the addresses the
 * game was told, and the generation the caches key on. Restoring a snapshot
 * must not roll any of them back -- a state saved before a texture upload would
 * otherwise hand the unit a stale mem_ver and the GPU would keep a texture the
 * game has already replaced. They are read out before the copy and put back
 * after it. */
static void pipeline_restore(const vgl_pipeline_state_t *s)
{
    struct { uint8_t *mem; uint32_t bytes, lo, hi; unsigned ver; }
        live[VGL_TMUS];
    int t;

    for (t = 0; t < VGL_TMUS; t++) {
        live[t].mem   = vgl.tmu[t].mem;
        live[t].bytes = vgl.tmu[t].mem_bytes;
        live[t].lo    = vgl.tmu[t].min_addr;
        live[t].hi    = vgl.tmu[t].max_addr;
        live[t].ver   = vgl.tmu[t].mem_ver;
    }

    VGL_PIPELINE_MEMBERS(PIPE_LOAD_SCALAR, PIPE_LOAD_ARRAY)

    for (t = 0; t < VGL_TMUS; t++) {
        vgl.tmu[t].mem       = live[t].mem;
        vgl.tmu[t].mem_bytes = live[t].bytes;
        vgl.tmu[t].min_addr  = live[t].lo;
        vgl.tmu[t].max_addr  = live[t].hi;
        vgl.tmu[t].mem_ver   = live[t].ver;
    }
}

static void saved_reset(void)
{
    free(s_saved);
    s_saved = NULL;
    s_saved_count = s_saved_cap = 0;
    s_saved_next = 1;
}

void vgl_state_get(void *blob)
{
    uint32_t magic, token;
    vgl_saved_state_t *slot;

    if (!blob)
        return;
    memcpy(&magic, blob, 4);
    memcpy(&token, (uint8_t *)blob + 4, 4);
    slot = magic == VGL_STATE_MAGIC ? saved_find(token) : NULL;
    if (!slot)
        slot = saved_new();
    if (!slot)
        return;
    pipeline_save(&slot->pipeline);
    magic = VGL_STATE_MAGIC;
    memcpy(blob, &magic, 4);
    memcpy((uint8_t *)blob + 4, &slot->token, 4);
}

void vgl_state_set(const void *blob)
{
    uint32_t magic, token;
    vgl_saved_state_t *slot;

    if (!blob)
        return;
    memcpy(&magic, blob, 4);
    memcpy(&token, (const uint8_t *)blob + 4, 4);
    slot = magic == VGL_STATE_MAGIC ? saved_find(token) : NULL;
    if (!slot) {
        static int announced;
        if (!announced) {
            announced = 1;
            vgl_log(0, "state: grGlideSetState with a block this provider "
                       "never filled: ignored. A state saved through one "
                       "provider cannot be restored through another");
        }
        return;
    }
    /* draw_state_version was incremented by native.c before this call. It is
     * deliberately outside the snapshot, so the GPU cache can never mistake a
     * newly restored state for an older state carrying the same generation. */
    pipeline_restore(&slot->pipeline);
}
