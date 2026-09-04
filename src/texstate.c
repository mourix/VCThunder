/* texstate.c -- diagnostic capture of distinct triangle draw states.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * It records colour and texture-combine state, texture-source provenance,
 * screen bounds, and draw counts. Disabled by default.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>

#define TEX_UPLOADS  1024u
#define DRAW_STATES   256u

/* GrTexInfo: smallLod, largeLod, aspectRatio, format, data. */
#define TEXINFO_SMALL_LOD  0x00
#define TEXINFO_LARGE_LOD  0x04
#define TEXINFO_ASPECT     0x08
#define TEXINFO_FORMAT     0x0c
#define TEXINFO_DATA       0x10

typedef struct {
    unsigned start;
    int      format, small_lod, large_lod, aspect;
    unsigned count;
    /* Mipmap level mask: EVEN=1, ODD=2, BOTH=3. Split masks identify the two-TMU
     * trilinear path. */
    unsigned even_odd;
    unsigned even_odd_seen;/* bitmask of every evenOdd this address was sent */
    char     name[16];      /* the R2 lookup in flight when this was uploaded */
    /* What the texels ACTUALLY are, sampled from the buffer the game hands to
     * grTexDownloadMipMap. Every hypothesis so far has assumed the data is
     * right and the rendering of it wrong; nothing has checked. A surface that
     * renders white because the bytes uploaded for it are white is not a
     * wrapper defect at all, and no amount of combine bisection would ever
     * find it. */
    int      sampled;
    unsigned mean_lum;      /* 0..255 over the top mip */
    unsigned pct_white;     /* percent of texels at or near full white */
    unsigned pct_opaque;
} tex_upload_t;

typedef struct {
    unsigned tex_start[2], constant;
    int      tex_known[2], tex_format[2];
    int      cc[5], tc[2][7], ac[5];
    unsigned triangles;
    /* Bounds for ONE frame, not the run. A 52-second union under a moving
     * camera smears every box across most of the screen and identifies
     * nothing; the last frame's geometry is a coherent picture. `cur` fills
     * during the frame, `box` keeps the last frame in which the state drew. */
    float    cx0, cy0, cx1, cy1;
    float    x0, y0, x1, y1;
    unsigned frame_tris;
    /* Provider cycles and triangle count for this state. Timing excludes the
     * diagnostic lookup itself. */
    uint64_t cycles;
    unsigned timed_tris;
} draw_state_t;

static tex_upload_t s_uploads[TEX_UPLOADS];
static unsigned s_upload_count;

static draw_state_t s_states[DRAW_STATES];
static unsigned s_state_count, s_states_lost;

/* Repeat probe: the same triangle drawn twice back to back. */
static uint64_t s_repeat_cycles;
static unsigned s_repeat_count;

/* Live Glide state, as the game last set it. */
/* PER TMU, because the dominant setup chains them: grTexCombine(TMU0,
 * SCALE_OTHER, FACTOR_ONE) makes TMU0 discard its own texel and pass through
 * TMU1's. Recording one "current texture" across both units therefore attributes
 * the wrong texture to the majority of the scene, which the first version of
 * this instrument did. */
static unsigned s_cur_tex[2] = { 0xffffffffu, 0xffffffffu };
static unsigned s_cur_constant;
static int s_cur_cc[5], s_cur_tc[2][7], s_cur_ac[5];
static int s_reported;

typedef void (__stdcall *tex_fn)(int, unsigned, unsigned, void *);
typedef void (__stdcall *combine4_fn)(int, int, int, int, int);
typedef void (__stdcall *texcombine_fn)(int, int, int, int, int, int, int);
typedef void (__stdcall *color_fn)(unsigned);
typedef void (__stdcall *tri_fn)(const void *, const void *, const void *);

static tex_fn        s_real_tex_source, s_real_tex_download;
static combine4_fn   s_real_color_combine, s_real_alpha_combine;
static texcombine_fn s_real_tex_combine;
static color_fn      s_real_constant;
static tri_fn        s_real_triangle;

static const char *format_name(int f)
{
    static const char *const NAMES[] = {
        "RGB_332", "YIQ_422", "ALPHA_8", "INTENSITY_8", "ALPHA_INTENSITY_44",
        "P_8", "RSVD0", "RSVD1", "ARGB_8332", "AYIQ_8422", "RGB_565",
        "ARGB_1555", "ARGB_4444", "ALPHA_INTENSITY_88", "AP_88", "RSVD2"
    };
    return (f >= 0 && f < (int)(sizeof NAMES / sizeof NAMES[0])) ? NAMES[f]
                                                                 : "?";
}

/* The dimensions of a chain's largest map. Written out three times before
 * this, and only two of them clamped `large_lod` at the BOTTOM: a shift by a
 * negative count is undefined, and the third copy could reach one from a
 * texture object the game had not filled in. One function, one clamp. */
static void mip_dims(int large_lod, int aspect, int *w, int *h)
{
    int big = 256 >> (large_lod > 8 ? 8 : (large_lod < 0 ? 0 : large_lod));

    if (aspect <= 3) {
        *w = big;
        *h = big >> (3 - aspect);
    } else {
        *h = big;
        *w = big >> (aspect - 3);
    }
}

static tex_upload_t *upload_for(unsigned start)
{
    unsigned i;
    for (i = 0; i < s_upload_count; i++)
        if (s_uploads[i].start == start)
            return &s_uploads[i];
    return NULL;
}

static int upload_is_bound(unsigned start)
{
    unsigned i;
    for (i = 0; i < s_state_count; i++)
        if (s_states[i].tex_start[0] == start || s_states[i].tex_start[1] == start)
            return 1;
    return 0;
}

/* Summarise the top mip the game is uploading. ARGB_1555 and ARGB_4444 only:
 * those are the formats every surface under investigation uses. */
static void sample_texels(tex_upload_t *slot, const void *info)
{
    const unsigned short *px =
        *(const unsigned short *const *)((const char *)info + TEXINFO_DATA);
    int fmt = slot->format, w, h, i, n;
    unsigned long lum = 0;
    unsigned white = 0, opaque = 0;

    slot->sampled = 0;
    if (!px || (fmt != 11 && fmt != 12))
        return;
    mip_dims(slot->large_lod, slot->aspect, &w, &h);
    n = w * h;
    if (n <= 0 || n > 256 * 256)
        return;

    for (i = 0; i < n; i++) {
        unsigned p = px[i], r, g, b, a;
        if (fmt == 11) {
            a = (p & 0x8000u) ? 255u : 0u;
            r = ((p >> 10) & 0x1Fu) * 255u / 31u;
            g = ((p >> 5) & 0x1Fu) * 255u / 31u;
            b = (p & 0x1Fu) * 255u / 31u;
        } else {
            a = ((p >> 12) & 0xFu) * 17u;
            r = ((p >> 8) & 0xFu) * 17u;
            g = ((p >> 4) & 0xFu) * 17u;
            b = (p & 0xFu) * 17u;
        }
        lum += (r * 77u + g * 151u + b * 28u) >> 8;
        if (r > 230u && g > 230u && b > 230u) white++;
        if (a > 127u) opaque++;
    }
    slot->mean_lum = (unsigned)(lum / (unsigned)n);
    slot->pct_white = white * 100u / (unsigned)n;
    slot->pct_opaque = opaque * 100u / (unsigned)n;
    slot->sampled = 1;
}

/* Account for entry points claimed by this module, which bypass the common
 * timed thunk. Glide calls are single-threaded. */
enum { TS_DOWNLOAD, TS_SOURCE, TS_CCOMB, TS_ACOMB, TS_TCOMB, TS_CONST, TS_N };
static const char *const TS_NAME[TS_N] = {
    "grTexDownloadMipMap", "grTexSource", "grColorCombine",
    "grAlphaCombine", "grTexCombine", "grConstantColorValue"
};
static uint64_t s_ts_cycles[TS_N], s_ts_calls[TS_N];
static uint64_t s_ts_t0;

/* Gated, because texstate_claim() takes six entry points UNCONDITIONALLY
 * (grTexSource, both colour combines, grTexCombine, grConstantColorValue and
 * grTexDownloadMipMap), while the only reader of what these accumulate,
 * texstate_report_cycles(), returns immediately unless glide_timing is set.
 * Every one of those calls was paying two serialising reads for a report that
 * is off in every shipped configuration.
 *
 * Not reentrant: one s_ts_t0 for all six. They are called from the render
 * thread and none of them nests inside another across the DLL boundary. */
/* diag_rdtsc_fenced() honours [diagnostics] time_fence, which is required
 * around a call this short: two bare reads sit in the same
 * out-of-order window and retire at nearly the same value, which is a wrong
 * answer rather than an imprecise one. */
#define TS_ENTER() do { if (g_cfg.glide_timing) \
                            s_ts_t0 = diag_rdtsc_fenced(); } while (0)
#define TS_LEAVE(k) do { if (g_cfg.glide_timing) { \
                             s_ts_cycles[k] += diag_rdtsc_fenced() - s_ts_t0; \
                             s_ts_calls[k]++; } } while (0)

void texstate_report_cycles(uint64_t frames)
{
    unsigned k;

    if (!frames || !g_cfg.glide_timing)
        return;
    for (k = 0; k < TS_N; k++) {
        if (!s_ts_calls[k])
            continue;
        LOGI("    %8.3f Mcyc/frame  %10.0f cyc/call  %8.1f/frame  %s",
             (double)s_ts_cycles[k] / (double)frames / 1.0e6,
             (double)s_ts_cycles[k] / (double)s_ts_calls[k],
             (double)s_ts_calls[k] / (double)frames, TS_NAME[k]);
        s_ts_cycles[k] = 0;
        s_ts_calls[k] = 0;
    }
}

/* ------------------------------------------------------- the LOD census: */
/* Capture grTexMipMapMode and grTexDownloadMipMap arguments that identify
 * split-chain two-TMU trilinear filtering and LOD-fraction blending. */
enum { LOD_COMBOS = 16u };

typedef struct {
    int      tmu, mode, lod_blend;
    uint64_t calls;
} lod_combo_t;

static lod_combo_t s_lod_combos[LOD_COMBOS];
static unsigned    s_lod_combo_count;
static uint64_t    s_lod_mode_calls;
static uint64_t    s_lod_downloads[4];   /* indexed by evenOdd & 3 */
static int         s_lod_reported;

/* __stdcall like every other real-function pointer here: this end is the
 * provider's export, and the hook itself stays cdecl because that end is the game. Getting
 * this backwards cleans the stack twice. */
typedef void (__stdcall *mipmode_fn)(int, int, int);
static mipmode_fn s_real_mipmap_mode;

static const char *lod_mode_name(int mode)
{
    switch (mode) {
    case 0:  return "GR_MIPMAP_DISABLE";
    case 1:  return "GR_MIPMAP_NEAREST";
    case 2:  return "GR_MIPMAP_NEAREST_DITHER";
    default: return "?";
    }
}

static const char *lod_mask_name(unsigned even_odd)
{
    switch (even_odd & 3u) {
    case 1:  return "EVEN";
    case 2:  return "ODD";
    case 3:  return "BOTH";
    default: return "NONE";
    }
}

static void lod_note_download(int tmu, unsigned even_odd)
{
    (void)tmu;
    s_lod_downloads[even_odd & 3u]++;
}

/* Count claimed entries here because they bypass the Glide counting thunk. */
static void gl_mipmap_mode(int tmu, int mode, int lod_blend)
{
    unsigned i;

    s_lod_mode_calls++;
    for (i = 0; i < s_lod_combo_count; i++) {
        if (s_lod_combos[i].tmu == tmu && s_lod_combos[i].mode == mode &&
            s_lod_combos[i].lod_blend == lod_blend) {
            s_lod_combos[i].calls++;
            break;
        }
    }
    if (i == s_lod_combo_count && s_lod_combo_count < LOD_COMBOS) {
        s_lod_combos[i].tmu = tmu;
        s_lod_combos[i].mode = mode;
        s_lod_combos[i].lod_blend = lod_blend;
        s_lod_combos[i].calls = 1;
        s_lod_combo_count++;
    }
    s_real_mipmap_mode(tmu, mode, lod_blend);
}

static void gl_tex_download(int tmu, unsigned start, unsigned even_odd,
                            void *info)
{
    tex_upload_t *slot;

    TS_ENTER();
    s_real_tex_download(tmu, start, even_odd, info);
    TS_LEAVE(TS_DOWNLOAD);
    if (!info)
        return;

    slot = upload_for(start);
    if (!slot && s_upload_count < TEX_UPLOADS)
        slot = &s_uploads[s_upload_count++];
    if (!slot)
        return;

    slot->start     = start;
    slot->format    = *(const int *)((const char *)info + TEXINFO_FORMAT);
    slot->small_lod = *(const int *)((const char *)info + TEXINFO_SMALL_LOD);
    slot->large_lod = *(const int *)((const char *)info + TEXINFO_LARGE_LOD);
    slot->aspect    = *(const int *)((const char *)info + TEXINFO_ASPECT);
    slot->even_odd  = even_odd;
    slot->even_odd_seen |= even_odd & 3u;
    lod_note_download(tmu, even_odd);
    sample_texels(slot, info);
    if (!slot->name[0])
        snprintf(slot->name, sizeof slot->name, "%s", r2_last_lookup());
    slot->count++;
}

static void gl_tex_source(int tmu, unsigned start, unsigned even_odd,
                          void *info)
{
    if (tmu >= 0 && tmu < 2)
        s_cur_tex[tmu] = start;
    TS_ENTER();
    s_real_tex_source(tmu, start, even_odd, info);
    TS_LEAVE(TS_SOURCE);
}

/* Diagnostic bisection for SCALE_OTHER_ADD_LOCAL_ALPHA. Replacing it with
 * SCALE_OTHER removes the additive iterated-alpha lighting term. */
#define GR_COMBINE_FUNCTION_SCALE_OTHER                 3
#define GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA 5

static void gl_color_combine(int func, int factor, int local, int other,
                             int invert)
{
    if (g_cfg.no_additive_light &&
        func == GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA) {
        static int announced;
        if (!announced) {
            announced = 1;
            LOGW("drawstate: no_additive_light; rewriting colour combine "
                 "SCALE_OTHER_ADD_LOCAL_ALPHA -> SCALE_OTHER. Additive "
                 "lighting is GONE while this is set; it is a bisection, not "
                 "a mode. If dark surfaces stop being white, the additive "
                 "term is the defect");
        }
        func = GR_COMBINE_FUNCTION_SCALE_OTHER;
    }

    s_cur_cc[0] = func; s_cur_cc[1] = factor; s_cur_cc[2] = local;
    s_cur_cc[3] = other; s_cur_cc[4] = invert;
    TS_ENTER();
    s_real_color_combine(func, factor, local, other, invert);
    TS_LEAVE(TS_CCOMB);
}

static void gl_alpha_combine(int func, int factor, int local, int other,
                             int invert)
{
    s_cur_ac[0] = func; s_cur_ac[1] = factor; s_cur_ac[2] = local;
    s_cur_ac[3] = other; s_cur_ac[4] = invert;
    TS_ENTER();
    s_real_alpha_combine(func, factor, local, other, invert);
    TS_LEAVE(TS_ACOMB);
}

/* Factor 13 is ONE_MINUS_LOD_FRACTION. Together with alternating EVEN and ODD
 * uploads, it implements two-TMU trilinear filtering rather than detail
 * texturing. VCGLIDE_LODF may pin the fraction for diagnostics. */

static void gl_tex_combine(int tmu, int rgb_func, int rgb_factor,
                           int alpha_func, int alpha_factor, int rgb_invert,
                           int alpha_invert)
{
    if (tmu >= 0 && tmu < 2) {
        int *t = s_cur_tc[tmu];
        t[0] = tmu; t[1] = rgb_func; t[2] = rgb_factor;
        t[3] = alpha_func; t[4] = alpha_factor;
        t[5] = rgb_invert; t[6] = alpha_invert;
    }
    TS_ENTER();
    s_real_tex_combine(tmu, rgb_func, rgb_factor, alpha_func, alpha_factor,
                       rgb_invert, alpha_invert);
    TS_LEAVE(TS_TCOMB);
}

static void gl_constant_color(unsigned color)
{
    s_cur_constant = color;
    TS_ENTER();
    s_real_constant(color);
    TS_LEAVE(TS_CONST);
}

/* GrVertex begins with float x, y in screen space. That is the one handle on
 * "the thing in that part of the picture" that needs no asset name, no material
 * layout and no upload correlation: all three of which have now failed to
 * identify the ramp. A large white surface on screen has a large bounding box
 * in the region where it appears, and the draw state that owns that box owns
 * the surface. */
static void bounds_add(draw_state_t *st, const void *v)
{
    float x = ((const float *)v)[0], y = ((const float *)v)[1];

    if (x < st->cx0) st->cx0 = x;
    if (x > st->cx1) st->cx1 = x;
    if (y < st->cy0) st->cy0 = y;
    if (y > st->cy1) st->cy1 = y;
}

/* Called from the swap: retire the frame just drawn. */
void texstate_frame_end(void)
{
    unsigned i;

    for (i = 0; i < s_state_count; i++) {
        draw_state_t *e = &s_states[i];
        if (e->frame_tris) {
            e->x0 = e->cx0; e->y0 = e->cy0;
            e->x1 = e->cx1; e->y1 = e->cy1;
        }
        e->frame_tris = 0;
        e->cx0 = e->cy0 = 1.0e9f;
        e->cx1 = e->cy1 = -1.0e9f;
    }
}

static void gl_triangle(const void *a, const void *b, const void *c)
{
    unsigned i;
    draw_state_t *st = NULL;

    for (i = 0; i < s_state_count; i++) {
        draw_state_t *e = &s_states[i];
        if (memcmp(e->tex_start, s_cur_tex, sizeof e->tex_start) == 0 &&
            e->constant == s_cur_constant &&
            memcmp(e->cc, s_cur_cc, sizeof e->cc) == 0 &&
            memcmp(e->tc, s_cur_tc, sizeof e->tc) == 0 &&
            memcmp(e->ac, s_cur_ac, sizeof e->ac) == 0) {
            st = e;
            break;
        }
    }
    if (!st) {
        if (s_state_count < DRAW_STATES) {
            unsigned t;
            st = &s_states[s_state_count++];
            st->constant  = s_cur_constant;
            memcpy(st->tex_start, s_cur_tex, sizeof st->tex_start);
            memcpy(st->cc, s_cur_cc, sizeof st->cc);
            memcpy(st->tc, s_cur_tc, sizeof st->tc);
            memcpy(st->ac, s_cur_ac, sizeof st->ac);
            for (t = 0; t < 2u; t++) {
                const tex_upload_t *up = upload_for(s_cur_tex[t]);
                st->tex_known[t]  = up != NULL;
                st->tex_format[t] = up ? up->format : -1;
            }
            st->triangles = 0;
            st->cx0 = st->cy0 = st->x0 = st->y0 = 1.0e9f;
            st->cx1 = st->cy1 = st->x1 = st->y1 = -1.0e9f;
            st->frame_tris = 0;
        } else {
            s_states_lost++;
        }
    }
    if (st) {
        uint64_t t0;

        st->triangles++;
        st->frame_tris++;
        bounds_add(st, a);
        bounds_add(st, b);
        bounds_add(st, c);
        /* Deliberately UNFENCED, unlike the state calls above: this brackets a
         * whole triangle, which is far longer than the out-of-order window, and
         * it runs per primitive. */
        t0 = diag_rdtsc();
        s_real_triangle(a, b, c);
        st->cycles += diag_rdtsc() - t0;
        st->timed_tris++;

        /* Repeat one triangle in 64 immediately to separate cold-call overhead
         * from provider draw cost. */
        if ((st->timed_tris & 63u) == 0u) {
            t0 = diag_rdtsc();
            s_real_triangle(a, b, c);
            s_repeat_cycles += diag_rdtsc() - t0;
            s_repeat_count++;
        }
        return;
    }
    s_real_triangle(a, b, c);
}

/* Claim the entry points this instrument needs, from glide_bind's dispatch
 * loop. Returns nonzero when the entry point has been taken over. */
int texstate_claim(const char *name, void *real, uint32_t va)
{
    if (strcmp(name, "grTexDownloadMipMap") == 0) {
        s_real_tex_download = (tex_fn)real;
        patch_jmp(va, (void *)gl_tex_download);
    } else if (strcmp(name, "grTexSource") == 0) {
        s_real_tex_source = (tex_fn)real;
        patch_jmp(va, (void *)gl_tex_source);
    } else if (strcmp(name, "grColorCombine") == 0) {
        s_real_color_combine = (combine4_fn)real;
        patch_jmp(va, (void *)gl_color_combine);
    } else if (strcmp(name, "grAlphaCombine") == 0) {
        s_real_alpha_combine = (combine4_fn)real;
        patch_jmp(va, (void *)gl_alpha_combine);
    } else if (strcmp(name, "grTexCombine") == 0) {
        s_real_tex_combine = (texcombine_fn)real;
        patch_jmp(va, (void *)gl_tex_combine);
    } else if (strcmp(name, "grConstantColorValue") == 0) {
        s_real_constant = (color_fn)real;
        patch_jmp(va, (void *)gl_constant_color);
    } else if (strcmp(name, "grTexMipMapMode") == 0) {
        /* Opt-in, unlike the rest. Claiming an entry point removes it from the
         * mix census permanently, and this one's 45.9-107.1/frame is the
         * measurement that pointed here, so do not blind it by default. */
        if (!g_cfg.trace_lod)
            return 0;
        s_real_mipmap_mode = (mipmode_fn)real;
        patch_jmp(va, (void *)gl_mipmap_mode);
    } else if (strcmp(name, "grDrawTriangle") == 0) {
        /* The per-triangle state search is diagnostic-only and must remain
         * disabled outside trace_draw_state. */
        if (!g_cfg.trace_draw_state)
            return 0;
        s_real_triangle = (tri_fn)real;
        patch_jmp(va, (void *)gl_triangle);
    } else {
        return 0;
    }
    return 1;
}

/* Print one representative report ordered by triangle count. */
/* Inspect cached ramp mesh/material pairs without assuming the material value
 * is an absolute pointer. Validate every address before following generated
 * field offsets. */
/* Validate readable memory with VirtualQuery because game allocations may be
 * outside the mapped image. */
static int game_ptr_ok(uint32_t v)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY;

    if (!v || (v & 3u))
        return 0;
    if (!VirtualQuery((LPCVOID)(uintptr_t)v, &mbi, sizeof mbi))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    return (mbi.Protect & readable) != 0 &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

static void report_ramp_materials(void)
{
    unsigned i, cached = 0, followed = 0;

    LOGI("drawstate: --- arrow-ramp materials (from _anim_land_InitArrowSign) ---");
    for (i = 0; i < (unsigned)G->arrow_material_count; i++) {
        uint32_t mat_slot = G->arrow_material[i];
        uint32_t mesh_slot = mat_slot - 4u;     /* the pair's mesh half */
        uint32_t mesh = *(const uint32_t *)(uintptr_t)mesh_slot;
        uint32_t mat  = *(const uint32_t *)(uintptr_t)mat_slot;
        uint32_t tex, start;
        const int *info;
        int w, h;

        if (!mesh && !mat)
            continue;                            /* no ramp of this kind here */
        cached++;

        if (!game_ptr_ok(mat)) {
            LOGI("drawstate: ramp %u: mesh=0x%08X material=0x%08X; material "
                 "is not readable memory, not following it%s", i, mesh, mat,
                 game_ptr_ok(mesh) ? " (the mesh half is readable)" : "");
            continue;
        }

        tex = *(const uint32_t *)(uintptr_t)(mat + G->material_texture);
        if (!tex) {
            LOGW("drawstate: ramp %u: mesh=0x%08X material=0x%08X, texture is "
                 "NULL: _material_SetState skips _tmem_Select, so this "
                 "surface draws with whatever the PREVIOUS draw left bound",
                 i, mesh, mat);
            continue;
        }
        if (!game_ptr_ok(tex)) {
            LOGW("drawstate: ramp %u: material 0x%08X has texture 0x%08X, which "
                 "is not a valid pointer", i, mat, tex);
            continue;
        }

        start = *(const uint32_t *)(uintptr_t)(tex + G->texobj_start);
        info  = (const int *)(uintptr_t)(tex + G->texobj_texinfo);
        mip_dims(info[1], info[2], &w, &h);
        followed++;
        LOGI("drawstate: ramp %u: mesh=0x%08X material=0x%08X texture=0x%08X "
             "-> tmem 0x%08X %dx%d %s lod %d..%d%s", i, mesh, mat, tex, start,
             w, h, format_name(info[3]), info[0], info[1],
             upload_for(start) ? "" : "   <-- nothing ever uploaded there");
    }
    LOGI("drawstate: %u ramp slots cached, %u followed to a texture", cached,
         followed);
}

static void report_cost(void);

void texstate_report(void)
{
    unsigned i, j, order[DRAW_STATES], n = s_state_count;

    if (!g_cfg.trace_draw_state || s_reported || !s_state_count)
        return;
    s_reported = 1;

    for (i = 0; i < n; i++)
        order[i] = i;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (s_states[order[j]].triangles > s_states[order[i]].triangles) {
                unsigned t = order[i]; order[i] = order[j]; order[j] = t;
            }

    report_ramp_materials();

    LOGI("drawstate: %u distinct draw states, %u textures uploaded%s",
         s_state_count, s_upload_count,
         s_states_lost ? " (state table full, some states not recorded)" : "");

    /* Identify relevant uploads by dimensions because asset names are absent. */
    /* Names are a correlation: the game resolves an asset shortly before it
     * uploads the texture, so the lookup in flight is usually (not always)
     * this texture's. Enough to find M_XARROWC11 in a list of 240. */
    LOGI("drawstate: --- uploads, ARGB_1555 only (the ramp/sign format) ---");
    for (i = 0; i < s_upload_count; i++) {
        const tex_upload_t *u = &s_uploads[i];
        int w, h;

        if (u->format != 11)                     /* GR_TEXFMT_ARGB_1555 */
            continue;
        if (u->name[0] && _strnicmp(u->name, "M_XARROW", 8) == 0)
            LOGW("drawstate: >>> '%s' uploaded at 0x%08X <<<", u->name,
                 u->start);
        mip_dims(u->large_lod, u->aspect, &w, &h);
        LOGI("drawstate: upload 0x%08X  %3dx%-3d lod %d..%d  mask %-4s  lum %3u"
             "  white %3u%%  opaque %3u%%  x%u  '%s'%s", u->start, w, h,
             u->small_lod, u->large_lod, lod_mask_name(u->even_odd),
             u->sampled ? u->mean_lum : 0u,
             u->sampled ? u->pct_white : 0u, u->sampled ? u->pct_opaque : 0u,
             u->count, u->name,
             upload_is_bound(u->start) ? "" : "   [never bound]");
    }

    LOGI("drawstate: --- draw states, most triangles first ---");
    LOGI("drawstate: cc/ac are function,factor,local,other,invert; tc is "
         "rgbfunc,rgbfactor,afunc,afactor,rgbinv,ainv. TMU0 tc=SCALE_OTHER(3),"
         "ONE(8) means TMU0 passes TMU1's texel through and its own is unused");

    for (i = 0; i < n; i++) {
        const draw_state_t *e = &s_states[order[i]];
        /* Which unit actually feeds the pixel pipeline: TMU0 unless TMU0 is a
         * pass-through of TMU1 (SCALE_OTHER with factor ONE). */
        int via1 = e->tc[0][1] == 3 && e->tc[0][2] == 8;
        unsigned eff = e->tex_start[via1 ? 1 : 0];
        int eff_known = e->tex_known[via1 ? 1 : 0];
        int eff_fmt = e->tex_format[via1 ? 1 : 0];

        LOGI("drawstate: %6u tri  eff=TMU%d 0x%08X %-14s  const=0x%08X  "
             "cc=%d,%d,%d,%d,%d  ac=%d,%d,%d,%d,%d  last-frame box (%.0f,%.0f)-(%.0f,%.0f)",
             e->triangles, via1 ? 1 : 0, eff,
             eff_known ? format_name(eff_fmt) : "NEVER-UPLOADED", e->constant,
             e->cc[0], e->cc[1], e->cc[2], e->cc[3], e->cc[4],
             e->ac[0], e->ac[1], e->ac[2], e->ac[3], e->ac[4],
             (double)e->x0, (double)e->y0, (double)e->x1, (double)e->y1);
        LOGI("drawstate:            tmu0 0x%08X tc=%d,%d,%d,%d,%d,%d   "
             "tmu1 0x%08X tc=%d,%d,%d,%d,%d,%d",
             e->tex_start[0], e->tc[0][1], e->tc[0][2], e->tc[0][3],
             e->tc[0][4], e->tc[0][5], e->tc[0][6],
             e->tex_start[1], e->tc[1][1], e->tc[1][2], e->tc[1][3],
             e->tc[1][4], e->tc[1][5], e->tc[1][6]);
    }

    report_cost();
}

/* Report provider cost by draw state and screen bounds. Timing excludes the
 * diagnostic state lookup. */
static void report_cost(void)
{
    unsigned order[DRAW_STATES];
    unsigned i, j, k, n = s_state_count;
    uint64_t total = 0;
    unsigned s_timed_total = 0;

    for (i = 0; i < n; i++) {
        order[i] = i;
        total += s_states[i].cycles;
        s_timed_total += s_states[i].timed_tris;
    }
    if (!total)
        return;

    for (i = 1; i < n; i++) {                       /* insertion sort, desc */
        unsigned v = order[i];
        for (j = i; j > 0 && s_states[order[j - 1]].cycles <
                             s_states[v].cycles; j--)
            order[j] = order[j - 1];
        order[j] = v;
    }

    LOGI("drawstate: --- wrapper cost per draw state, dearest first ---");
    LOGI("drawstate: %llu Mcyc total inside grDrawTriangle over %u states",
         (unsigned long long)(total / 1000000u), n);
    if (s_repeat_count)
        LOGW("drawstate: REPEAT PROBE; the same triangle drawn again "
             "immediately costs %.0f cycles, against %.0f for the first call "
             "(%.1fx). %u samples",
             (double)s_repeat_cycles / (double)s_repeat_count,
             (double)total / (double)s_timed_total,
             s_timed_total && s_repeat_count
                 ? ((double)total / (double)s_timed_total) /
                   ((double)s_repeat_cycles / (double)s_repeat_count) : 0.0,
             s_repeat_count);

    for (k = 0; k < n && k < 20u; k++) {
        const draw_state_t *e = &s_states[order[k]];
        int via1 = e->tc[0][1] == 3 && e->tc[0][2] == 8;
        if (!e->timed_tris)
            continue;
        LOGI("drawstate: %5.1f%%  %8.0f cyc/tri  %6u tri  eff=TMU%d 0x%08X  "
             "box (%.0f,%.0f)-(%.0f,%.0f)  %.0fx%.0f px",
             100.0 * (double)e->cycles / (double)total,
             (double)e->cycles / (double)e->timed_tris,
             e->timed_tris, via1 ? 1 : 0, e->tex_start[via1 ? 1 : 0],
             (double)e->x0, (double)e->y0, (double)e->x1, (double)e->y1,
             (double)(e->x1 - e->x0), (double)(e->y1 - e->y0));
    }
}

/* THE LOD CENSUS REPORT.
 *
 * Prints once, after trace_lod seconds, so it describes a drawn scene rather
 * than the loading screen. Everything here is an argument the game passed and
 * the thunks discarded: no inference, no dereferencing anything on faith. */
void texstate_report_lod(uint64_t frames)
{
    unsigned i, split = 0, both = 0, blend_true = 0;
    uint64_t downloads = s_lod_downloads[0] + s_lod_downloads[1] +
                         s_lod_downloads[2] + s_lod_downloads[3];

    if (!g_cfg.trace_lod || s_lod_reported)
        return;
    if (!frames || !s_lod_mode_calls)
        return;                 /* nothing drawn yet; wait for a real scene */
    s_lod_reported = 1;

    LOGI("lod: grTexMipMapMode %llu calls (%.1f/frame), %u distinct "
         "(tmu, mode, lodBlend) combination(s)",
         (unsigned long long)s_lod_mode_calls,
         (double)s_lod_mode_calls / (double)frames, s_lod_combo_count);
    for (i = 0; i < s_lod_combo_count; i++) {
        const lod_combo_t *c = &s_lod_combos[i];
        if (c->lod_blend)
            blend_true++;
        LOGI("lod:   TMU%d  mode %d %-24s  lodBlend %-7s  x%llu",
             c->tmu, c->mode, lod_mode_name(c->mode),
             c->lod_blend ? "FXTRUE" : "FXFALSE",
             (unsigned long long)c->calls);
    }

    LOGI("lod: grTexDownloadMipMap %llu upload(s) by level mask; "
         "BOTH %llu, EVEN %llu, ODD %llu, NONE %llu",
         (unsigned long long)downloads,
         (unsigned long long)s_lod_downloads[3],
         (unsigned long long)s_lod_downloads[1],
         (unsigned long long)s_lod_downloads[2],
         (unsigned long long)s_lod_downloads[0]);

    for (i = 0; i < s_upload_count; i++) {
        unsigned seen = s_uploads[i].even_odd_seen;
        if (seen == 3u)
            both++;             /* one address given both halves = a full chain */
        else if (seen == 1u || seen == 2u)
            split++;            /* only ever half a chain at this address */
    }

    /* State the conclusion the numbers support, in the log, so the next reader
     * does not have to re-derive it, and state it as a disjunction, because
     * both branches are real answers and only one of them was ever assumed. */
    if (s_lod_downloads[1] || s_lod_downloads[2])
        LOGW("lod: SPLIT CHAINS PRESENT; %u address(es) received only EVEN or "
             "only ODD levels, %u received both. Hydro is using the Voodoo2's "
             "two-TMU trilinear path, so each unit holds HALF a mip chain. That "
             "is the rare path, it is the likely defect, and DisableMipmapping "
             "'fixes' it only by stopping the game sampling the missing half",
             split, both);
    else
        LOGW("lod: NO SPLIT CHAINS; every upload carried the BOTH mask, so "
             "each address holds a COMPLETE mip chain and the two-TMU trilinear "
             "hypothesis is dead. The defect is in how the provider samples a "
             "complete chain, not in a half-chain it was never given");

    if (!blend_true)
        LOGW("lod: lodBlend is FXFALSE in every combination; the LOD-fraction "
             "cross-fade is never armed, which retires it as the mechanism and "
             "leaves the mip mode itself");
}
