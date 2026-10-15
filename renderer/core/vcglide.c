/* vcglide.c -- Glide export seam and native renderer dispatch.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A single entries.def list defines indices, descriptors, stubs, and linker
 * exports. The host supplies configuration through environment variables before
 * loading this DLL; calls may also be captured for deterministic replay.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcglide.h"
#include "vgl.h"
#include "png.h"

/* ---- the table ---------------------------------------------------------- */

const vgl_entry_desc_t vgl_entries[VGL_COUNT] = {
#define VGL_ENTRY(name, argbytes, payload, state) \
    { #name, argbytes, payload, state },
#include "../api/entries.def"
#undef VGL_ENTRY
};

unsigned  vgl_calls[VGL_COUNT];

vgl_config_t vgl_cfg;
vgl_bezel_t vgl_bezel;
int vgl_ready;
int vgl_capturing;
int vgl_surface_w, vgl_surface_h;

static HMODULE  s_self;
static FILE    *s_log;
static unsigned s_frame;

/* grSstWinOpen's resolution enum. The host keeps the same table for the
 * window it sizes (src/glide_bind.c); here it exists only to size an LFB
 * region, since a region's height is the surface's and nothing in the lock
 * arguments carries it. */
const vgl_res_t vgl_resolutions[] = {
    {  320,  200 }, {  320,  240 }, {  400,  256 }, {  512,  384 },
    {  640,  200 }, {  640,  350 }, {  640,  400 }, {  640,  480 },
    {  800,  600 }, {  960,  720 }, {  856,  480 }, {  512,  256 },
    { 1024,  768 }, { 1280, 1024 }, { 1600, 1200 }, {  400,  300 },
};
const unsigned vgl_resolution_count =
    sizeof vgl_resolutions / sizeof vgl_resolutions[0];

/* ---- logging ------------------------------------------------------------ */

void vgl_log(int level, const char *fmt, ...)
{
    va_list ap;

    if (level && !vgl_cfg.verbose)
        return;
    if (!s_log)
        return;
    fprintf(s_log, "[%8lu] ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
}

/* A LOG LINE ON A REPEATABLE FAILURE PATH IS A DISK-FILLING BUG: 28.4 GB of
 * identical lines in 139 minutes, from one failure returning the same answer
 * every frame. This is gpu.c's per-site budget for every other path that can
 * fail once per frame, lock or download.
 *
 * `seen` is the caller's own counter, so the budget is per SITE and lasts the
 * life of the process. The last permitted line says the site has gone quiet. */
void vgl_log_capped(unsigned *seen, unsigned max, const char *fmt, ...)
{
    va_list ap;

    if (!s_log || !seen || ++*seen > max)
        return;
    fprintf(s_log, "[%8lu] ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    if (*seen == max)
        fputs("  [further occurrences at this site are not logged]", s_log);
    fputc('\n', s_log);
    fflush(s_log);
}

/* ---- the window's name -------------------------------------------------- */

/* Format one window title for all backends and the host. A non-positive FPS
 * omits the measurement while preserving the title shape. */
void vgl_window_title(char *buf, size_t n, double fps)
{
    char name[32];

    snprintf(name, sizeof name, "%s", vgl_cfg.title);
    if (name[0] >= 'a' && name[0] <= 'z')
        name[0] = (char)(name[0] - 'a' + 'A');

    if (!name[0] || _stricmp(name, "unknown") == 0)
        name[0] = '\0';                 /* a standalone caller named no title */

    if (name[0] && fps > 0.0)
        snprintf(buf, n, "VCThunder - %s (%.1f fps)", name, fps);
    else if (name[0])
        snprintf(buf, n, "VCThunder - %s", name);
    else if (fps > 0.0)
        snprintf(buf, n, "VCThunder (%.1f fps)", fps);
    else
        snprintf(buf, n, "VCThunder");
}

/* ---- configuration ------------------------------------------------------ */

/* Report every setting with its ORIGIN. "which file won" has been the shape
 * of every configuration mistake in this project, and the answer is free. */
static void cfg_str(const char *var, char *dst, size_t n, const char *dflt)
{
    DWORD got = GetEnvironmentVariableA(var, dst, (DWORD)n);

    if (got == 0 || got >= n) {
        snprintf(dst, n, "%s", dflt);
        vgl_log(0, "config: %-24s = %-32s (default)", var, dst[0] ? dst : "(none)");
    } else {
        vgl_log(0, "config: %-24s = %-32s (host)", var, dst[0] ? dst : "(none)");
    }
}

static unsigned cfg_num(const char *var, unsigned dflt)
{
    char buf[32];
    DWORD got = GetEnvironmentVariableA(var, buf, sizeof buf);
    unsigned v = dflt;
    const char *src = "default";

    if (got > 0 && got < sizeof buf) {
        v = (unsigned)strtoul(buf, NULL, 0);
        src = "host";
    }
    vgl_log(0, "config: %-24s = %-32u (%s)", var, v, src);
    return v;
}

/* Preload renderer dependencies before the host maps over its PE headers.
 * Keep the explicit entry idempotent and avoid loading under DllMain's lock. */
void __stdcall vglInit(void)
{
    vgl_init();
}

/* The provider's own health, read by the host after every swap. 0 is healthy;
 * VGL_HEALTH_DEVICE_LOST says the GPU device is gone and is not coming back,
 * which no other signal in this system can see. */
int __stdcall vglHealth(void)
{
    return vgl_gpu_lost() ? VGL_HEALTH_DEVICE_LOST : VGL_HEALTH_OK;
}

/* The host is about to draw a picture it will throw away, and then has. */
void __stdcall vglSpecBegin(void)
{
    if (!vgl_ready)
        vgl_init();
    vgl_spec_begin();
}

void __stdcall vglSpecEnd(void)
{
    if (vgl_ready)
        vgl_spec_end();
}

/* The host knows a screen frames its world in native-width art; black both
 * side strips at the next swap even though the frame drew into them. */
void __stdcall vglBlackMargins(void)
{
    vgl.margin_black = 1;
}

static void bezel_release(void)
{
    int i;
    for (i = 0; i < vgl_bezel.levels; i++)
        free(vgl_bezel.px[i]);
    memset(vgl_bezel.px, 0, sizeof vgl_bezel.px);
    vgl_bezel.levels = 0;
}

/* The host's bezel art: premultiplied RGBA8, top-down, w*h, with the screen
 * hole in its pixels. Copied, with a 2x2 box mip chain under it, because the
 * art is drawn smaller than it was made whenever the screen is (a 4096-wide
 * frame on a 1920-wide screen is a 2.1:1 reduction). NULL clears it. Kept
 * across surface opens: a backend uploads it again whenever it opens. */
int __stdcall vglSetBezel(const void *rgba, int w, int h, int hole_x,
                          int hole_y, int hole_w, int hole_h)
{
    int k;

    if (!vgl_ready)
        vgl_init();
    bezel_release();
    vgl_bezel.generation++;
    if (!rgba || w <= 0 || h <= 0 || hole_w <= 0 || hole_h <= 0 ||
        hole_x < 0 || hole_y < 0 || hole_x + hole_w > w || hole_y + hole_h > h) {
        if (rgba)
            vgl_log(0, "bezel: refused %dx%d art with hole %d,%d %dx%d",
                    w, h, hole_x, hole_y, hole_w, hole_h);
        return 0;
    }
    vgl_bezel.w[0] = w;
    vgl_bezel.h[0] = h;
    vgl_bezel.px[0] = (uint8_t *)malloc((size_t)w * h * 4);
    if (!vgl_bezel.px[0]) {
        vgl_log(0, "bezel: no memory for %dx%d art; no bezel", w, h);
        return 0;
    }
    memcpy(vgl_bezel.px[0], rgba, (size_t)w * h * 4);
    vgl_bezel.levels = 1;
    for (k = 1; k < VGL_BEZEL_LEVELS && (vgl_bezel.w[k - 1] > 1 ||
                                         vgl_bezel.h[k - 1] > 1); k++) {
        int pw = vgl_bezel.w[k - 1], ph = vgl_bezel.h[k - 1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1, x, y, c;
        const uint8_t *src = vgl_bezel.px[k - 1];
        uint8_t *dst = (uint8_t *)malloc((size_t)nw * nh * 4);

        if (!dst)
            break;
        for (y = 0; y < nh; y++) {
            int y0 = 2 * y < ph ? 2 * y : ph - 1;
            int y1 = 2 * y + 1 < ph ? 2 * y + 1 : ph - 1;
            for (x = 0; x < nw; x++) {
                int x0 = 2 * x < pw ? 2 * x : pw - 1;
                int x1 = 2 * x + 1 < pw ? 2 * x + 1 : pw - 1;
                for (c = 0; c < 4; c++)
                    dst[((size_t)y * nw + x) * 4 + c] = (uint8_t)
                        ((src[((size_t)y0 * pw + x0) * 4 + c] +
                          src[((size_t)y0 * pw + x1) * 4 + c] +
                          src[((size_t)y1 * pw + x0) * 4 + c] +
                          src[((size_t)y1 * pw + x1) * 4 + c] + 2) / 4);
            }
        }
        vgl_bezel.w[k] = nw;
        vgl_bezel.h[k] = nh;
        vgl_bezel.px[k] = dst;
        vgl_bezel.levels = k + 1;
    }
    vgl_bezel.hole_x = hole_x;
    vgl_bezel.hole_y = hole_y;
    vgl_bezel.hole_w = hole_w;
    vgl_bezel.hole_h = hole_h;
    vgl_log(0, "bezel: %dx%d art, %d levels, screen hole %d,%d %dx%d",
            w, h, vgl_bezel.levels, hole_x, hole_y, hole_w, hole_h);
    return 1;
}

void vgl_init(void)
{
    if (vgl_ready)
        return;
    vgl_ready = 1;              /* set first: nothing below may re-enter */

    cfg_str("VCGLIDE_LOG", vgl_cfg.log, sizeof vgl_cfg.log, "vcglide.log");
    if (vgl_cfg.log[0]) {
        s_log = fopen(vgl_cfg.log, "w");
        /* The two config lines above went nowhere: the log did not exist
         * yet. Restate them now rather than leave a log whose first entry
         * silently omits where the log itself came from. */
        vgl_log(0, "vcglide: VCThunder's Glide 2.x provider and rasteriser");
        vgl_log(0, "config: %-24s = %s", "VCGLIDE_LOG", vgl_cfg.log);
    }
    cfg_str("VCGLIDE_CAPTURE", vgl_cfg.capture, sizeof vgl_cfg.capture, "");
    cfg_str("VCGLIDE_FRAMES", vgl_cfg.frames, sizeof vgl_cfg.frames, "");
    cfg_str("VCGLIDE_DISPLAY_DUMP", vgl_cfg.display_dump,
            sizeof vgl_cfg.display_dump, "");
    cfg_str("VCGLIDE_DISPLAY_FRAMES", vgl_cfg.display_frames,
            sizeof vgl_cfg.display_frames, "");
    cfg_str("VCGLIDE_TITLE", vgl_cfg.title, sizeof vgl_cfg.title, "unknown");
    cfg_str("VCGLIDE_RENDERER", vgl_cfg.renderer, sizeof vgl_cfg.renderer,
            "auto");
    if (_stricmp(vgl_cfg.renderer, "auto") == 0)
        snprintf(vgl_cfg.renderer, sizeof vgl_cfg.renderer, "auto");
    else if (_stricmp(vgl_cfg.renderer, "gpu") == 0)
        snprintf(vgl_cfg.renderer, sizeof vgl_cfg.renderer, "gpu");
    else if (_stricmp(vgl_cfg.renderer, "cpu") == 0)
        snprintf(vgl_cfg.renderer, sizeof vgl_cfg.renderer, "cpu");
    else {
        vgl_log(0, "config: VCGLIDE_RENDERER value is invalid; using auto");
        snprintf(vgl_cfg.renderer, sizeof vgl_cfg.renderer, "auto");
    }
    cfg_str("VCGLIDE_ASPECT", vgl_cfg.aspect, sizeof vgl_cfg.aspect, "raster");
    if (_stricmp(vgl_cfg.aspect, "raster") == 0)
        snprintf(vgl_cfg.aspect, sizeof vgl_cfg.aspect, "raster");
    else if (_stricmp(vgl_cfg.aspect, "4:3") == 0 ||
             _stricmp(vgl_cfg.aspect, "4_3") == 0 ||
             _stricmp(vgl_cfg.aspect, "43") == 0)
        snprintf(vgl_cfg.aspect, sizeof vgl_cfg.aspect, "4:3");
    else if (_stricmp(vgl_cfg.aspect, "16:9") == 0)
        snprintf(vgl_cfg.aspect, sizeof vgl_cfg.aspect, "16:9");
    else if (_stricmp(vgl_cfg.aspect, "stretch") == 0)
        snprintf(vgl_cfg.aspect, sizeof vgl_cfg.aspect, "stretch");
    else {
        vgl_log(0, "config: VCGLIDE_ASPECT value is invalid; using raster");
        snprintf(vgl_cfg.aspect, sizeof vgl_cfg.aspect, "raster");
    }
    cfg_str("VCGLIDE_UPSCALE_2D", vgl_cfg.upscale_2d,
            sizeof vgl_cfg.upscale_2d, "bilinear");
    if (_stricmp(vgl_cfg.upscale_2d, "catmull_rom") == 0)
        snprintf(vgl_cfg.upscale_2d, sizeof vgl_cfg.upscale_2d, "catmull_rom");
    else if (_stricmp(vgl_cfg.upscale_2d, "fsr1") == 0)
        snprintf(vgl_cfg.upscale_2d, sizeof vgl_cfg.upscale_2d, "fsr1");
    else {
        if (_stricmp(vgl_cfg.upscale_2d, "bilinear") != 0)
            vgl_log(0, "config: VCGLIDE_UPSCALE_2D value is invalid; "
                       "using bilinear");
        snprintf(vgl_cfg.upscale_2d, sizeof vgl_cfg.upscale_2d, "bilinear");
    }
    cfg_str("VCGLIDE_TEXMAG", vgl_cfg.texmag, sizeof vgl_cfg.texmag,
            "bilinear");
    if (_stricmp(vgl_cfg.texmag, "catmull_rom") == 0)
        snprintf(vgl_cfg.texmag, sizeof vgl_cfg.texmag, "catmull_rom");
    else {
        if (_stricmp(vgl_cfg.texmag, "bilinear") != 0)
            vgl_log(0, "config: VCGLIDE_TEXMAG value is invalid; "
                       "using bilinear");
        snprintf(vgl_cfg.texmag, sizeof vgl_cfg.texmag, "bilinear");
    }
    vgl_cfg.capture_frames = cfg_num("VCGLIDE_CAPTURE_FRAMES", 0);
    vgl_cfg.verbose        = (int)cfg_num("VCGLIDE_VERBOSE", 0);
    vgl_cfg.present        = (int)cfg_num("VCGLIDE_PRESENT", 1);
    vgl_cfg.scale          = (int)cfg_num("VCGLIDE_SCALE", 1);
    vgl_cfg.render_scale   = (int)cfg_num("VCGLIDE_RENDER_SCALE", 1);
    if (vgl_cfg.render_scale < 1) vgl_cfg.render_scale = 1;
    if (vgl_cfg.render_scale > 8) vgl_cfg.render_scale = 8;
    /* 0 = one worker per logical processor. 1 turns threading off, which is
     * the control every performance claim about it is measured against. */
    vgl_cfg.threads        = (int)cfg_num("VCGLIDE_THREADS", 0);
    /* Who owns the two side strips, per frame, as a pattern census. The
     * host's margin census names the game call sites that draw outside the
     * native field; this one says what happened to the strips afterwards,
     * which is the other half of a flicker report. */
    vgl_cfg.margin_trace   = (int)cfg_num("VCGLIDE_MARGIN_TRACE", 0);
    /* Fail the GPU device from this frame. The device-loss path is the one
     * piece of this provider whose real trigger cannot be produced on demand,
     * and a recovery path nobody has ever run is not a recovery path. */
    vgl_cfg.fault_device   = cfg_num("VCGLIDE_FAULTDEVICE", 0);
    cfg_str("VCGLIDE_FAULTSITE", vgl_cfg.fault_site,
            sizeof vgl_cfg.fault_site, "all");

    /* Resolve diagnostic pipeline readings from the environment. ITRGB keeps
     * control and legacy alternatives for reproducible A/B comparisons. */
    vgl_opt.itrgb   = (int)cfg_num("VCGLIDE_ITRGB", 1);
    vgl_opt.lodfrac = (int)cfg_num("VCGLIDE_LODFRAC", 0);
    vgl_opt.lodgrad = (int)cfg_num("VCGLIDE_LODGRAD", 1);
    vgl_opt.forcelod = (int)cfg_num("VCGLIDE_FORCELOD", 0);
    vgl_opt.lodclamp = (int)cfg_num("VCGLIDE_LODCLAMP", 1);
    vgl_opt.lodf     = (int)cfg_num("VCGLIDE_LODF", (unsigned)-1);
    if (vgl_opt.lodf > 255)
        vgl_opt.lodf = -1;                      /* unset, or out of range */
    vgl_log(0, "entry points: %u, GrVertex %u bytes", VGL_COUNT,
            (unsigned)GR_VERTEX_BYTES);
    /* render_scale is the GPU backend's internal target multiple and nothing
     * else reads it: device.c hands it to vgl_gpu_open() only when the
     * renderer is not "cpu", so the rasteriser draws at the cabinet's own
     * resolution whatever the ini asked for. This line printed the request,
     * which reported an internal resolution no pixel was ever rendered at,
     * and the number came out of the user's ini, so it read as a measurement.
     * "auto" and "gpu" can still end on the CPU path by falling back; that is
     * not knowable here, and device.c names the renderer that did open. */
    const int cpu_only = strcmp(vgl_cfg.renderer, "cpu") == 0;
    vgl_log(0, "MODE: rasteriser, renderer=%s, internal=%dx%s. "
               "There is no forwarding path and no backend is loaded; every "
               "pixel below is ours. XRGB8888, no dither; "
               "itrgb=%d lodfrac=%d lodgrad=%d lodclamp=%d",
            vgl_cfg.renderer, cpu_only ? 1 : vgl_cfg.render_scale,
            cpu_only && vgl_cfg.render_scale > 1
                ? " (render_scale is the GPU backend's; this path ignores it)"
                : "",
            vgl_opt.itrgb, vgl_opt.lodfrac, vgl_opt.lodgrad, vgl_opt.lodclamp);
    /* Preload graphics modules before the host replaces its PE mappings. */
    if (!cpu_only)
        vgl_gpu_preload();
    if (vgl_cfg.capture[0])
        vgl_cap_open();
}

void vgl_destination_rect(int fullscreen, int raster_w, int raster_h,
                          int client_w, int client_h,
                          int *x, int *y, int *w, int *h)
{
    static char last_aspect[16];
    static int last_cw, last_ch, last_x, last_y, last_w, last_h;
    int target_w = raster_w, target_h = raster_h;

    *x = *y = 0;
    *w = client_w;
    *h = client_h;
    if (!fullscreen || client_w <= 0 || client_h <= 0)
        return;
    if (_stricmp(vgl_cfg.aspect, "stretch") == 0)
        goto report;

    if (_stricmp(vgl_cfg.aspect, "4:3") == 0) {
        target_w = 4;
        target_h = 3;
    } else if (_stricmp(vgl_cfg.aspect, "16:9") == 0) {
        target_w = 16;
        target_h = 9;
    }
    if (target_w <= 0 || target_h <= 0)
        return;

    if ((int64_t)client_w * target_h > (int64_t)client_h * target_w)
        *w = (int)((int64_t)client_h * target_w / target_h);
    else
        *h = (int)((int64_t)client_w * target_h / target_w);
    *x = (client_w - *w) / 2;
    *y = (client_h - *h) / 2;

report:
    if (strcmp(last_aspect, vgl_cfg.aspect) != 0 || last_cw != client_w ||
        last_ch != client_h || last_x != *x || last_y != *y ||
        last_w != *w || last_h != *h) {
        vgl_log(0, "present: fullscreen aspect %s, raster %dx%d, client %dx%d "
                   "-> destination %d,%d %dx%d", vgl_cfg.aspect,
                raster_w, raster_h, client_w, client_h, *x, *y, *w, *h);
        snprintf(last_aspect, sizeof last_aspect, "%s", vgl_cfg.aspect);
        last_cw = client_w; last_ch = client_h;
        last_x = *x; last_y = *y; last_w = *w; last_h = *h;
    }
}

int vgl_present_layout(int fullscreen, int raster_w, int raster_h,
                       int client_w, int client_h, int ready,
                       vgl_rect_t *picture, vgl_rect_t *canvas)
{
    static int last_cw, last_ch, last_rw, last_rh;
    static vgl_rect_t last_pic;
    int wide;

    canvas->x = canvas->y = 0;
    canvas->w = client_w;
    canvas->h = client_h;
    if (!vgl_bezel.levels) {
        vgl_destination_rect(fullscreen, raster_w, raster_h, client_w, client_h,
                             &picture->x, &picture->y, &picture->w, &picture->h);
        return 0;
    }
    /* `aspect` is not consulted. The cabinet's raster (512x400) and every
     * operator screen are shown 4:3, the cabinet monitor's shape, and cover
     * the hole; a widescreen raster is wider than 4:3, keeps its own shape
     * and spans the hole's width. Read from the raster, not from the ini,
     * because a title can refuse widescreen after the ini asked for it. */
    wide = (int64_t)raster_w * 3 > (int64_t)raster_h * 4;
    if (!ready) {
        /* The backend could not draw the art: nothing would cover what
         * passes the hole, so the picture is fitted to the window instead,
         * still in its own shape. */
        VGL_LOG_ONCE("present: the bezel art cannot be drawn by this "
                     "presenter; the picture is fitted to the window without it");
        vgl_fit_layout(client_w, client_h, wide ? raster_w : 4,
                       wide ? raster_h : 3, picture);
        return 0;
    }
    vgl_bezel_layout(client_w, client_h, vgl_bezel.w[0], vgl_bezel.h[0],
                     vgl_bezel.hole_x, vgl_bezel.hole_y,
                     vgl_bezel.hole_w, vgl_bezel.hole_h,
                     wide ? raster_w : 4, wide ? raster_h : 3, !wide,
                     canvas, picture);
    if (last_cw != client_w || last_ch != client_h || last_rw != raster_w ||
        last_rh != raster_h || memcmp(&last_pic, picture, sizeof last_pic)) {
        vgl_log(0, "present: bezel, raster %dx%d, client %dx%d -> art %d,%d "
                   "%dx%d, picture %d,%d %dx%d", raster_w, raster_h,
                client_w, client_h, canvas->x, canvas->y, canvas->w, canvas->h,
                picture->x, picture->y, picture->w, picture->h);
        last_cw = client_w; last_ch = client_h;
        last_rw = raster_w; last_rh = raster_h;
        last_pic = *picture;
    }
    return 1;
}

/* Raster geometry is published immediately before grSstWinOpen, which is
 * later than vglInit. Refresh it at the surface boundary instead of caching a
 * stale preload-time value. device.c stays platform-neutral and consumes the
 * resolved integers from vgl_cfg. */
void vgl_refresh_geometry(int surface_w, int surface_h)
{
    vgl_cfg.raster_width = (int)cfg_num("VCGLIDE_RASTER_WIDTH",
                                        (unsigned)surface_w);
    vgl_cfg.raster_height = (int)cfg_num("VCGLIDE_RASTER_HEIGHT",
                                         (unsigned)surface_h);
    vgl_cfg.lfb_width = (int)cfg_num("VCGLIDE_LFB_WIDTH", 0);
    if (vgl_cfg.raster_width <= 0 || vgl_cfg.raster_width > surface_w)
        vgl_cfg.raster_width = surface_w;
    if (vgl_cfg.raster_height <= 0 || vgl_cfg.raster_height > surface_h)
        vgl_cfg.raster_height = surface_h;
    if (vgl_cfg.lfb_width <= 0 ||
        vgl_cfg.lfb_width >= vgl_cfg.raster_width)
        vgl_cfg.lfb_width = 0;
}

/* Optional golden-frame PNG output for capture validation. Each frame requires
 * a synchronous framebuffer readback and is unsuitable for performance timing. */
static int s_read_probed;

void vgl_dump_frame(unsigned frame)
{
    unsigned w = (unsigned)vgl.w, h = (unsigned)vgl.h;
    uint16_t *px;
    char path[MAX_PATH];

    if (!s_read_probed) {
        s_read_probed = 1;
        CreateDirectoryA(vgl_cfg.frames, NULL);
        vgl_log(0, "frames: dumping to '%s'; readback is our own framebuffer",
                vgl_cfg.frames);
    }
    if (!w || !h)
        return;

    px = (uint16_t *)malloc((size_t)w * h * 2u);
    if (!px)
        return;
    if (vgl_lfb_read_region(GR_BUFFER_BACKBUFFER, 0, 0, w, h, w * 2u, px)) {
        /* %.*s: vgl_cfg.frames is itself a full-width path, so bound the
           directory and let the filename always survive. The reservation is
           for a 10-digit frame number, not the 5 that %05u implies: %05u is
           a minimum width, and past 99999 it simply gets wider. */
        snprintf(path, sizeof path, "%.*s\\frame%05u.png",
                 (int)(sizeof path - sizeof "\\frame.png" - 10),
                 vgl_cfg.frames, frame);
        if (!png_write565(path, px, w, h, w))
            vgl_log(0, "frames: cannot write %s", path);
    } else {
        vgl_log(0, "frames: grLfbReadRegion refused frame %u", frame);
    }
    free(px);
}

/* ---- the seam ----------------------------------------------------------- */

/* Runs before the call reaches native.c. Bookkeeping, the capture and the
 * golden frame; it has nothing left to refuse, so it returns void. */
static void vgl_enter(unsigned ix, const uint32_t *a)
{
    if (!vgl_ready)
        vgl_init();
    vgl_calls[ix]++;

    if (vgl_capturing) {
        /* Captured BEFORE the call, because a caller's buffer is only
         * guaranteed to hold what it meant to send until the callee has had
         * it. The two exceptions (an LFB region and an out-parameter) are
         * handled on the way out instead. */
        if (vgl_entries[ix].payload == PL_LFBUNLOCK)
            vgl_cap_lfb_pending();
        vgl_cap_call(ix, a);
    }

    /* Read the completed frame before swap, matching replay convention. Apply
     * the same frame limit to capture and golden output. */
    if (vgl_cfg.frames[0] && vgl_entries[ix].payload == PL_SWAP &&
        (!vgl_cfg.capture_frames || s_frame < vgl_cfg.capture_frames))
        vgl_dump_frame(s_frame);
}

/* Runs after the call, and returns the value the caller will see. Only
 * three entries need anything here, and each is a fact that is not knowable
 * until the renderer has answered. */
static int vgl_leave(unsigned ix, const uint32_t *a, int ret)
{
    switch (vgl_entries[ix].payload) {
    case PL_WINOPEN:
        /* Size LFB regions from the selected surface resolution. */
        if (a[1] < vgl_resolution_count) {
            vgl_surface_w = vgl_resolutions[a[1]].width;
            vgl_surface_h = vgl_resolutions[a[1]].height;
        }
        vgl_log(0, "grSstWinOpen(hwnd=0x%08X, res=%u -> %dx%d, refresh=%u, "
                   "cformat=%u, origin=%u, colbufs=%u, auxbufs=%u) = %d",
                a[0], a[1], vgl_surface_w, vgl_surface_h, a[2], a[3], a[4],
                a[5], a[6], ret);
        break;
    case PL_LFBINFO:
        vgl_cap_lfb_locked(a, ret);
        break;
    case PL_SWAP:
        s_frame++;
        if (vgl_capturing) {
            vgl_cap_frame(s_frame);
            if (vgl_cfg.capture_frames &&
                s_frame >= vgl_cfg.capture_frames)
                vgl_cap_close("frame limit reached");
        }
        break;
    default:
        break;
    }
    return ret;
}

/* Export one stub per argument-byte count. Carry each stack slot as uint32_t,
 * avoid the x87 stack, and return through EAX for all entry signatures. */

/* Pack stack slots, run seam bookkeeping, and dispatch to native.c. */
#define VGL_BODY(name) \
    vgl_enter(VGL_IX_##name, a); \
    return vgl_leave(VGL_IX_##name, a, vgl_native_call(VGL_IX_##name, a))

#define VGL_STUB0(name) \
    int __stdcall name(void) { \
        uint32_t a[1] = { 0 }; \
        VGL_BODY(name); }

#define VGL_STUB4(name) \
    int __stdcall name(uint32_t a0) { \
        uint32_t a[1]; a[0] = a0; \
        VGL_BODY(name); }

#define VGL_STUB8(name) \
    int __stdcall name(uint32_t a0, uint32_t a1) { \
        uint32_t a[2]; a[0] = a0; a[1] = a1; \
        VGL_BODY(name); }

#define VGL_STUB12(name) \
    int __stdcall name(uint32_t a0, uint32_t a1, uint32_t a2) { \
        uint32_t a[3]; a[0] = a0; a[1] = a1; a[2] = a2; \
        VGL_BODY(name); }

#define VGL_STUB16(name) \
    int __stdcall name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { \
        uint32_t a[4]; a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; \
        VGL_BODY(name); }

#define VGL_STUB20(name) \
    int __stdcall name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, \
                       uint32_t a4) { \
        uint32_t a[5]; a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; \
        VGL_BODY(name); }

#define VGL_STUB24(name) \
    int __stdcall name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, \
                       uint32_t a4, uint32_t a5) { \
        uint32_t a[6]; a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; \
        a[5] = a5; \
        VGL_BODY(name); }

#define VGL_STUB28(name) \
    int __stdcall name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, \
                       uint32_t a4, uint32_t a5, uint32_t a6) { \
        uint32_t a[7]; a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; \
        a[5] = a5; a[6] = a6; \
        VGL_BODY(name); }

#define VGL_STUB40(name) \
    int __stdcall name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, \
                       uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, \
                       uint32_t a8, uint32_t a9) { \
        uint32_t a[10]; a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; \
        a[5] = a5; a[6] = a6; a[7] = a7; a[8] = a8; a[9] = a9; \
        VGL_BODY(name); }

#define VGL_ENTRY(name, argbytes, payload, state) VGL_STUB##argbytes(name)
#include "../api/entries.def"
#undef VGL_ENTRY

/* Export grLfbReadRegion outside entries.def for replay readback. The games do
 * not call this entry directly. */
int __stdcall grLfbReadRegion(uint32_t a0, uint32_t a1, uint32_t a2,
                              uint32_t a3, uint32_t a4, uint32_t a5,
                              uint32_t a6)
{
    if (!vgl_ready)
        vgl_init();
    return vgl_lfb_read_region(a0, a1, a2, a3, a4, a5, (void *)(uintptr_t)a6);
}

/* ---- module ------------------------------------------------------------- */

/* Nothing here but bookkeeping. See vglInit for why the graphics runtime is
 * not brought up from under the loader lock. */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        s_self = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (vgl_capturing)
            vgl_cap_close("process detach");
        if (s_log) {
            char line[256];
            int n, k;

            vgl_raster_collect();       /* anything since the last swap */
            vgl_log(0, "raster: %llu triangles (%llu degenerate), "
                       "%llu spans, %llu pixels shaded, %llu written, "
                       "%llu LFB pixels, over %u frames",
                    (unsigned long long)vgl.n_tris,
                    (unsigned long long)vgl.n_tris_culled,
                    (unsigned long long)vgl.n_spans,
                    (unsigned long long)vgl.n_pixels,
                    (unsigned long long)vgl.n_pixels_written,
                    (unsigned long long)vgl.n_lfb_writes,
                    vgl.frames);

            n = 0;
            for (k = 0; k < (int)(sizeof vgl_lod_hist /
                                  sizeof vgl_lod_hist[0]); k++)
                n += snprintf(line + n, sizeof line - (size_t)n,
                              "%s%llu", k ? " " : "",
                              (unsigned long long)vgl_lod_hist[k]);
            vgl_log(0, "raster: LOD asked for, by bucket (<=-1, 0, 1, ..): %s",
                    line);
            n = 0;
            for (k = 0; k < (int)(sizeof vgl_level_hist /
                                  sizeof vgl_level_hist[0]); k++)
                n += snprintf(line + n, sizeof line - (size_t)n,
                              "%s%llu", k ? " " : "",
                              (unsigned long long)vgl_level_hist[k]);
            vgl_log(0, "raster: mip level used, 0..8: %s", line);
            /* Both backends fill this one: it is counted at grTexSource. */
            vgl_log(0, "tex: split chains sourced, by the parity of their "
                       "sharpest present level: %llu even, %llu ODD",
                    (unsigned long long)vgl_split_parity[0],
                    (unsigned long long)vgl_split_parity[1]);
            vgl_log(0, "shutdown after %u frames", s_frame);
            fclose(s_log);
            s_log = NULL;
        }
    }
    return TRUE;
}
