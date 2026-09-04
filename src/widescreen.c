/* widescreen.c -- the Hor+ engine.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game is told it has a wider raster than the cabinet gave it, every
 * primitive is translated by a whole-pixel seam origin, and the frustum is
 * widened to match so the extra columns contain scene rather than stretch.
 * Six consumers decide before that seam or round-trip their state through it
 * and need a title-scoped repair each; they are the reason this is an engine
 * and not a matrix multiply.
 *
 * See src/glide_int.h for the seam and what stays behind it.
 */
#include "vcthunder.h"
#include "glide_int.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The origin and the state around it. gl_clip_window and gl_sst_win_open
 * consult it through ws_origin() and ws_lfb_read_offset(), which is the whole
 * reason this engine can live outside glide_bind.c. */
static int   s_ws_armed;          /* the profile allows the mode             */
static float s_ws_dx;             /* the seam origin, 0 when inert           */
static int   s_ws_cull_width;     /* the width pass 1 CULLS to; see below    */
static int   s_ws_native_width;   /* N: the raster the game would have had */
static int   s_ws_reported;
static int   s_ws_lensflare_depth;
static int   s_ws_native_fov, s_ws_have_native_fov;
static unsigned s_ws_viewport_generation;
static unsigned s_ws_repair_reported;
#define WS_REPAIR_LENS       0x01u
#define WS_REPAIR_DRAWQUEUE  0x02u
#define WS_REPAIR_SPARK      0x04u
#define WS_REPAIR_RACER_TAG  0x08u
#define WS_REPAIR_RACE_FOV   0x10u
#define WS_REPAIR_FLAG_FOV   0x20u
/* Both sky-quad builders are hooked, so the backdrop reaching the seam is
 * already the raster and must not take the origin. Declared here because the
 * primitive wrappers sit above the mode's own section. */
static int   s_ws_sky_wide;
/* Nonzero while the game is inside its ortho 2D path, where every primitive is
 * 2D by construction and the native field is the whole of the picture. */
static int   s_ws_ortho_depth;


/* Widescreen preserves the game's native 2D coordinate system and expands only
 * 3D culling. The host applies dx=(wide-native)/2 at the Glide seam. A Hor+
 * FOV transform keeps projection scale unchanged; screen_limit remains the
 * native 2D rectangle. */

typedef void (__cdecl *viewport_init_fn)(float znear, int fov);
static viewport_init_fn s_real_viewport_init;

/* The game's angles are binary: 65536 == 360 degrees (_viewport_InitToFullScreen
 * halves with `sar` and indexes _sinfulltbl). Widening the horizontal field
 * while holding the projection scale is the standard Hor+ recut. */
/* 65536 binary units = 2*pi, so one unit is pi/32768 radians and the HALF
 * angle fov/2 is fov * pi/65536. Getting that factor of two wrong would widen
 * or narrow the field silently, which is why the caller logs zscreen on both
 * sides of the transform rather than trusting the arithmetic here. */
#define WS_HALF_ANGLE_RAD (M_PI / 65536.0)

static double half_angle_tan(int fov)
{
    return tan((double)fov * WS_HALF_ANGLE_RAD);
}

static int widen_fov(int fov, int native, int wide)
{
    double t = half_angle_tan(fov);
    double out;

    if (native <= 0 || wide <= native || t <= 0.0)
        return fov;
    out = atan(t * (double)wide / (double)native) / WS_HALF_ANGLE_RAD;
    return (int)(out + 0.5);
}

static void wr_f(uint32_t va, float v)
{
    if (va)
        *(float *)(uintptr_t)va = v;
}

static void wr_i(uint32_t va, int v)
{
    if (va)
        *(int *)(uintptr_t)va = v;
}

static float rd_f(uint32_t va)
{
    return va ? *(const float *)(uintptr_t)va : 0.0f;
}

/* Reject widescreen mode unless every required anchor is present. */
static void __cdecl gl_viewport_init(float znear, int fov)
{
    int native = s_ws_native_width;
    /* The CULLING width, not the raster width: it is the raster rounded up to
     * a whole-pixel margin on each side, so that after the origin's integer
     * translation the frustum still contains the raster's last column. See
     * widescreen_settle(). */
    int wide = s_ws_cull_width > native ? s_ws_cull_width : glide_view_width();
    float half_native = (float)native * 0.5f;
    float zs_wide;

    if (!s_real_viewport_init)
        return;
    /* The game treats this argument as its state identity in two Offroad
     * paths. Keep that value separately because the real viewport block must
     * retain the widened value for sky/weather projection. */
    s_ws_native_fov = fov;
    s_ws_have_native_fov = 1;
    if (s_ws_dx <= 0.0f || native <= 0 || wide <= native) {
        s_real_viewport_init(znear, fov);
        s_ws_viewport_generation++;
        return;
    }

    /* Pass 1 tells the real body it has W columns, so it derives the frustum,
     * the culling norms and zscreen for the wide field. */
    wr_i(G->init3dfx_hpixels, wide);
    s_real_viewport_init(znear, widen_fov(fov, native, wide));
    s_ws_viewport_generation++;
    wr_i(G->init3dfx_hpixels, native);
    zs_wide = rd_f(G->viewport_zscreen);

    /* ONE-SHOT: what pass 1 actually produced. Whether the culling widened is
     * the only question this mode turns on, and it is not visible from a
     * screenshot: a native world with a few unclipped strays in the margins
     * looks a lot like a widened one that is being cropped. */
    if (!s_ws_reported) {
        const float *fl = (const float *)(uintptr_t)G->viewport_frustum_limit;
        const float *sl = (const float *)(uintptr_t)G->viewport_screen_limit;

        LOGI("geometry: widescreen pass1 hpixels=%d hres=%d hres_f=%.1f "
             "half_hres_f=%.2f zscreen=%.4f frustum_limit[0]=%.2f "
             "screen_limit=%.1f,%.1f,%.1f,%.1f",
             rd_opt(G->init3dfx_hpixels, -1),
             rd_opt(G->viewport_hres, -1),
             (double)rd_f(G->viewport_hres_f),
             (double)rd_f(G->viewport_half_hres_f), (double)zs_wide,
             (double)fl[0], (double)sl[0], (double)sl[1], (double)sl[2],
             (double)sl[3]);
        /* The culling planes are the only thing pass 1 exists to widen, and
         * nothing else in the run reports them. rect_norm sits between
         * frustum_limit and screen_limit in the block, so it is reached from
         * the two anchors we have rather than needing one of its own. */
        {
            const float *rect = (const float *)(uintptr_t)
                (G->viewport_frustum_limit + 0x50);
            LOGI("geometry: widescreen pass1 rect_norm %.4f %.4f %.4f %.4f "
                 "%.4f %.4f (native half-angle %.2f deg, wide %.2f deg)",
                 (double)rect[0], (double)rect[1], (double)rect[2],
                 (double)rect[3], (double)rect[4], (double)rect[5],
                 atan((double)native * 0.5 / (double)zs_wide) * 180.0 / M_PI,
                 /* the RASTER's half-width: pass 1 widened the frustum to the
                  * raster, which is not the same as the native half plus the
                  * origin now that the origin is a whole pixel */
                 atan((double)wide * 0.5 / (double)zs_wide) * 180.0 / M_PI);
        }
    }

    /* Pass 2: put back everything the 2D layer reads. These are the members
     * `gref.py` found an external reader for; the rest of the block is either
     * culling (keep it wide) or written and never read. */
    wr_i(G->viewport_hres, native);
    wr_i(G->viewport_half_hres, native / 2);
    wr_f(G->viewport_hres_f, (float)native);
    wr_f(G->viewport_half_hres_f, half_native);
    wr_f(G->viewport_frustum_limit, 1.0f - half_native);     /* [0] */
    wr_f(G->viewport_frustum_limit + 4, half_native);        /* [1], unread */
    /* Restore the native screen_limit. Its two 2D readers interpret the left
     * member with different signs and agree only at the native zero value.
     * Full-raster fades widen it only for their own draw. */
    if (G->viewport_screen_limit) {
        wr_f(G->viewport_screen_limit, 0.0f);                     /* left  */
        wr_f(G->viewport_screen_limit + 4, (float)native - 1.0f); /* right */
    }

    if (!s_ws_reported) {
        s_ws_reported = 1;
        LOGI("geometry: widescreen origin; the game keeps its %d-wide "
             "coordinate system and the seam adds x=%g. fov %d -> %d holds "
             "zscreen at %.4f (native %.4f); a difference here would mean the "
             "picture was rescaled rather than widened.",
             native, (double)s_ws_dx, fov, widen_fov(fov, native, wide),
             (double)zs_wide,
             half_native / half_angle_tan(fov));
    }
}

/* Some consumers project with the native screen origin and then clip/reject
 * before a Glide primitive exists. Run only those functions in raster
 * coordinates: move the projection origin now, widen their own right bounds,
 * and suppress the later seam origin for their submitted primitives. */
typedef struct ws_raster_scope {
    int active, hres;
    float dx, hres_f, frustum_left, screen_right;
} ws_raster_scope_t;

static int ws_raster_enter(ws_raster_scope_t *s)
{
    memset(s, 0, sizeof *s);
    if (s_ws_dx <= 0.0f || glide_view_width() <= 0)
        return 0;

    s->active = 1;
    s->dx = s_ws_dx;
    s->hres = *(const int *)(uintptr_t)G->viewport_hres;
    s->hres_f = rd_f(G->viewport_hres_f);
    s->frustum_left = rd_f(G->viewport_frustum_limit);
    s->screen_right = G->viewport_screen_limit
                        ? rd_f(G->viewport_screen_limit + 4) : 0.0f;

    wr_i(G->viewport_hres, glide_view_width());
    wr_f(G->viewport_hres_f, (float)glide_view_width());
    wr_f(G->viewport_frustum_limit, s->frustum_left - s->dx);
    if (G->viewport_screen_limit)
        wr_f(G->viewport_screen_limit + 4, (float)glide_view_width() - 1.0f);
    s_ws_dx = 0.0f;
    return 1;
}

static void ws_raster_leave(const ws_raster_scope_t *s)
{
    if (!s->active)
        return;
    if (G->viewport_screen_limit)
        wr_f(G->viewport_screen_limit + 4, s->screen_right);
    wr_f(G->viewport_frustum_limit, s->frustum_left);
    wr_f(G->viewport_hres_f, s->hres_f);
    wr_i(G->viewport_hres, s->hres);
    s_ws_dx = s->dx;
}

/* The viewport stores the transformed FOV because geometric sky/weather
 * readers need it, while two Offroad functions use the same member as the
 * game's native state. Expose the native shadow only for those functions. If
 * either legitimately calls viewport init, the generation changes and the
 * hook's newly widened value is already the correct state to retain. */
typedef struct ws_fov_scope {
    int active, wide_fov;
    unsigned generation;
} ws_fov_scope_t;

static int ws_fov_enter(ws_fov_scope_t *s)
{
    memset(s, 0, sizeof *s);
    if (s_ws_dx <= 0.0f || !s_ws_have_native_fov || !G->viewport_fov)
        return 0;
    s->active = 1;
    s->wide_fov = *(const int *)(uintptr_t)G->viewport_fov;
    s->generation = s_ws_viewport_generation;
    wr_i(G->viewport_fov, s_ws_native_fov);
    return 1;
}

static void ws_fov_leave(const ws_fov_scope_t *s)
{
    if (s->active && s->generation == s_ws_viewport_generation)
        wr_i(G->viewport_fov, s->wide_fov);
}

typedef void (__cdecl *ws_void_fn)(void);
typedef int  (__cdecl *ws_one_fn)(void *);
typedef void (__cdecl *ws_flag_fn)(uint32_t, uint32_t);
static ws_void_fn s_real_ws_lensflare, s_real_ws_drawqueue;
static ws_one_fn  s_real_ws_spark_cluster, s_real_ws_racer_tag;
static ws_void_fn s_real_ws_race_update;
static ws_flag_fn s_real_ws_flag_icon;

static void __cdecl gl_ws_lensflare(void)
{
    if (!s_real_ws_lensflare)
        return;
    s_ws_lensflare_depth++;
    s_real_ws_lensflare();
    s_ws_lensflare_depth--;
}

static void __cdecl gl_ws_drawqueue(void)
{
    ws_raster_scope_t scope;

    if (!s_real_ws_drawqueue)
        return;
    if (ws_raster_enter(&scope) &&
        !(s_ws_repair_reported & WS_REPAIR_DRAWQUEUE)) {
        s_ws_repair_reported |= WS_REPAIR_DRAWQUEUE;
        LOGI("geometry: widescreen repair; Hydro draw queue projects and "
             "clips winner-camera lines in the %d-wide raster", glide_view_width());
    }
    s_real_ws_drawqueue();
    ws_raster_leave(&scope);
}

static int __cdecl gl_ws_spark_cluster(void *cluster)
{
    ws_raster_scope_t scope;
    int result;

    if (!s_real_ws_spark_cluster)
        return 0;
    if (ws_raster_enter(&scope) &&
        !(s_ws_repair_reported & WS_REPAIR_SPARK)) {
        s_ws_repair_reported |= WS_REPAIR_SPARK;
        LOGI("geometry: widescreen repair; Offroad spark projection and "
             "reject bounds use the %d-wide raster", glide_view_width());
    }
    result = s_real_ws_spark_cluster(cluster);
    ws_raster_leave(&scope);
    return result;
}

static int __cdecl gl_ws_racer_tag(void *racer)
{
    ws_raster_scope_t scope;
    int result;

    if (!s_real_ws_racer_tag)
        return 0;
    if (ws_raster_enter(&scope) &&
        !(s_ws_repair_reported & WS_REPAIR_RACER_TAG)) {
        s_ws_repair_reported |= WS_REPAIR_RACER_TAG;
        LOGI("geometry: widescreen repair; Offroad racer-tag projection, "
             "reject and clip bounds use the %d-wide raster", glide_view_width());
    }
    result = s_real_ws_racer_tag(racer);
    ws_raster_leave(&scope);
    return result;
}

static void __cdecl gl_ws_race_update(void)
{
    ws_fov_scope_t scope;
    int report;

    if (!s_real_ws_race_update)
        return;
    ws_fov_enter(&scope);
    report = scope.active &&
             !(s_ws_repair_reported & WS_REPAIR_RACE_FOV);
    s_real_ws_race_update();
    if (report) {
        s_ws_repair_reported |= WS_REPAIR_RACE_FOV;
        LOGI("geometry: widescreen repair; Offroad race dynamics compared "
             "the native FOV identity; viewport reinitialisations in this "
             "update: %u",
             s_ws_viewport_generation - scope.generation);
    }
    ws_fov_leave(&scope);
}

static void __cdecl gl_ws_flag_icon(uint32_t a0, uint32_t a1)
{
    ws_fov_scope_t scope;
    int report;

    if (!s_real_ws_flag_icon)
        return;
    ws_fov_enter(&scope);
    report = scope.active &&
             !(s_ws_repair_reported & WS_REPAIR_FLAG_FOV);
    s_real_ws_flag_icon(a0, a1);
    if (report) {
        s_ws_repair_reported |= WS_REPAIR_FLAG_FOV;
        LOGI("geometry: widescreen repair; Offroad CTF flag icon saved and "
             "restored the native FOV through %u viewport initialisations",
             s_ws_viewport_generation - scope.generation);
    }
    ws_fov_leave(&scope);
}

/* Apply the widescreen x origin before the provider and capture boundary. Copy
 * each 60-byte GrVertex only while widescreen is active. The hook remains cdecl
 * and calls the provider through its stdcall function pointer. */
#define WS_VSIZE 60u

typedef void (__stdcall *ws_tri_fn)(const void *, const void *, const void *);
typedef void (__stdcall *ws_aatri_fn)(const void *, const void *, const void *,
                                      int, int, int);
typedef void (__stdcall *ws_line_fn)(const void *, const void *);
typedef void (__stdcall *ws_vlist_fn)(int, const void *);
static ws_tri_fn   s_ws_real_tri;
static ws_aatri_fn s_ws_real_aatri;
static ws_line_fn  s_ws_real_line, s_ws_real_aaline;
static ws_vlist_fn s_ws_real_vlist, s_ws_real_planar_vlist;

/* Measure the distribution of submitted vertices across both side margins.
 * Population counts distinguish a widened field from isolated outliers. */
static float s_ws_seen_min = 1e30f, s_ws_seen_max = -1e30f;
/* The bounding box of the primitive currently being shifted, in the GAME's
 * coordinates: the margin census below reads it. */
static float s_ws_bx0 = 1e30f, s_ws_bx1 = -1e30f;
static float s_ws_by0 = 1e30f, s_ws_by1 = -1e30f;
static unsigned s_ws_seen_verts, s_ws_left_verts, s_ws_right_verts;
static int s_ws_extent_reported;
/* Declared up here because ws_census_wanted() below is its first reader;
 * the margin census it gates lives further down with its own table. */
static int s_ws_margin_enabled;

/* Is anything still going to READ the census below?
 *
 * Two consumers, and both stop. The extent report is one-shot and fires a few
 * seconds into any session; the margin census is a developer switch that is
 * off by default. Everything the census computes after that is a compare and a
 * store whose value nothing will ever look at again, and it runs per VERTEX,
 * on the game's own thread, in the shipped default configuration, for the rest
 * of the run. It was roughly ten times the arithmetic of the one thing this
 * function exists to do, which is to add an origin to x. */
static int ws_census_wanted(void)
{
    return !s_ws_extent_reported || s_ws_margin_enabled;
}

/* ...and is there anything left to do to a primitive at all?
 *
 * A widescreen raster that is not wider than the game's own leaves the origin
 * at zero (widescreen_settle logs it as INERT), but the primitives stay
 * claimed: patch_jmp cannot be taken back. Without this the wrappers copy
 * three 60-byte vertices to a stack buffer in order to add zero to each.
 *
 * The ortho clamp has to be off too: it is not a function of the origin and
 * applies at dx = 0 exactly as it does above it. The one arithmetic difference
 * this skips is that `-0.0f + 0.0f` is `+0.0f` while an untouched vertex keeps
 * `-0.0f`; the two compare equal and floor and ceil alike to 0, so no pixel
 * can tell them apart. */
static int ws_shift_is_identity(void)
{
    return s_ws_dx == 0.0f && !s_ws_ortho_depth && !ws_census_wanted();
}

static void ws_shift(void *dst, const void *src, unsigned count)
{
    /* Both hoisted out of the loop: they are per-call facts, and the branch
     * they save is per vertex. */
    const int census = ws_census_wanted();
    const int clamp  = s_ws_ortho_depth;
    const float dx   = s_ws_dx;
    unsigned i;

    memcpy(dst, src, (size_t)count * WS_VSIZE);
    for (i = 0; i < count; i++) {
        float *x = (float *)((char *)dst + (size_t)i * WS_VSIZE);

        /* Inside the ortho path, the game's 2D field is [0, N] and anything
         * outside it is the one-pixel overhang the cabinet's own clip dropped.
         * Clamp rather than clip: a clamp is a pure function of x, so two
         * triangles sharing a vertex get the same answer and no mesh cracks. */
        if (clamp) {
            if (*x < 0.0f)                          *x = 0.0f;
            else if (*x > (float)s_ws_native_width) *x = (float)s_ws_native_width;
        }

        if (census) {
            float y = *(const float *)((const char *)dst +
                                       (size_t)i * WS_VSIZE + 4);

            if (*x < s_ws_bx0) s_ws_bx0 = *x;
            if (*x > s_ws_bx1) s_ws_bx1 = *x;
            if (y < s_ws_by0) s_ws_by0 = y;
            if (y > s_ws_by1) s_ws_by1 = y;
            if (*x < s_ws_seen_min) s_ws_seen_min = *x;
            if (*x > s_ws_seen_max) s_ws_seen_max = *x;
            if (*x < 0.0f)                            s_ws_left_verts++;
            else if (*x > (float)s_ws_native_width)   s_ws_right_verts++;
        }
        *x += dx;
    }
    if (!census)
        return;
    s_ws_seen_verts += count;
    if (s_ws_seen_verts >= 200000u && !s_ws_extent_reported) {
        s_ws_extent_reported = 1;
        LOGI("geometry: widescreen extent; %u vertices: %.2f%% left of 0, "
             "%.2f%% right of %d, range %.1f .. %.1f. A widened frustum fills "
             "%.1f .. %.1f and should put several per cent in each margin; a "
             "trickle means the culling is still native and only overhang is "
             "escaping.",
             s_ws_seen_verts,
             100.0 * (double)s_ws_left_verts / (double)s_ws_seen_verts,
             100.0 * (double)s_ws_right_verts / (double)s_ws_seen_verts,
             s_ws_native_width, (double)s_ws_seen_min, (double)s_ws_seen_max,
             /* what pass 1 widened the frustum TO: the raster centred on the
              * field, which is half a pixel wider each side than the whole-pixel
              * origin the vertices are then translated by */
             (double)(s_ws_native_width - s_ws_cull_width) * 0.5,
             (double)(s_ws_native_width + s_ws_cull_width) * 0.5);
    }
}

/* Attribute margin geometry to game call sites before applying the seam origin.
 * Report and reset the bounded census periodically. */
#define WS_MARGIN_SITES   10u
#define WS_MARGIN_WINDOW  300u          /* swaps: ~10 s at 30 fps */
#define WS_MARGIN_REPORTS 12u

static struct {
    uint32_t ra;
    unsigned n;
    float    x0, x1, y0, y1;
} s_ws_margin[WS_MARGIN_SITES];
static unsigned s_ws_margin_used, s_ws_margin_swaps, s_ws_margin_reports;

static void ws_batch_begin(void)
{
    s_ws_bx0 = s_ws_by0 = 1e30f;
    s_ws_bx1 = s_ws_by1 = -1e30f;
}

static void ws_batch_end(void *ra_p)
{
    uint32_t ra = (uint32_t)(uintptr_t)ra_p;
    unsigned i;

    if (!s_ws_margin_enabled || s_ws_bx1 < s_ws_bx0)
        return;
    if (s_ws_bx0 >= 0.0f && s_ws_bx1 <= (float)s_ws_native_width)
        return;                         /* wholly inside the native field */
    for (i = 0; i < s_ws_margin_used; i++)
        if (s_ws_margin[i].ra == ra)
            break;
    if (i == s_ws_margin_used) {
        if (s_ws_margin_used >= WS_MARGIN_SITES)
            return;                     /* the table is a census, not a log */
        s_ws_margin_used++;
        s_ws_margin[i].ra = ra;
        s_ws_margin[i].n  = 0;
        s_ws_margin[i].x0 = s_ws_bx0; s_ws_margin[i].x1 = s_ws_bx1;
        s_ws_margin[i].y0 = s_ws_by0; s_ws_margin[i].y1 = s_ws_by1;
    }
    s_ws_margin[i].n++;
    if (s_ws_bx0 < s_ws_margin[i].x0) s_ws_margin[i].x0 = s_ws_bx0;
    if (s_ws_bx1 > s_ws_margin[i].x1) s_ws_margin[i].x1 = s_ws_bx1;
    if (s_ws_by0 < s_ws_margin[i].y0) s_ws_margin[i].y0 = s_ws_by0;
    if (s_ws_by1 > s_ws_margin[i].y1) s_ws_margin[i].y1 = s_ws_by1;
}

/* Called from the swap. Reports the window's table and starts a new one. */
static void ws_margin_tick(void)
{
    unsigned i;

    if (!s_ws_margin_enabled)
        return;
    if (++s_ws_margin_swaps < WS_MARGIN_WINDOW)
        return;
    s_ws_margin_swaps = 0;
    if (s_ws_margin_used && s_ws_margin_reports < WS_MARGIN_REPORTS) {
        s_ws_margin_reports++;
        LOGI("geometry: widescreen margin census #%u; %u call site(s) "
             "submitted geometry outside the game's 0..%d field:",
             s_ws_margin_reports, s_ws_margin_used, s_ws_native_width);
        for (i = 0; i < s_ws_margin_used; i++)
            LOGI("geometry:   caller 0x%08X  %u batches  x %.1f..%.1f  "
                 "y %.1f..%.1f%s", s_ws_margin[i].ra, s_ws_margin[i].n,
                 (double)s_ws_margin[i].x0, (double)s_ws_margin[i].x1,
                 (double)s_ws_margin[i].y0, (double)s_ws_margin[i].y1,
                 s_ws_margin[i].x0 < 0.0f ? "  LEFT MARGIN" : "");
    }
    s_ws_margin_used = 0;
}

static void __cdecl ws_draw_triangle(const void *a, const void *b,
                                       const void *c)
{
    char v[3 * WS_VSIZE];

    if (!s_ws_real_tri)
        return;
    if (ws_shift_is_identity()) {
        s_ws_real_tri(a, b, c);
        return;
    }
    ws_batch_begin();
    ws_shift(v, a, 1);
    ws_shift(v + WS_VSIZE, b, 1);
    ws_shift(v + 2 * WS_VSIZE, c, 1);
    ws_batch_end(__builtin_return_address(0));
    s_ws_real_tri(v, v + WS_VSIZE, v + 2 * WS_VSIZE);
}

static void __cdecl ws_aa_draw_triangle(const void *a, const void *b,
                                          const void *c, int ab, int bc,
                                          int ca)
{
    char v[3 * WS_VSIZE];

    if (!s_ws_real_aatri)
        return;
    if (ws_shift_is_identity()) {
        s_ws_real_aatri(a, b, c, ab, bc, ca);
        return;
    }
    ws_shift(v, a, 1);
    ws_shift(v + WS_VSIZE, b, 1);
    ws_shift(v + 2 * WS_VSIZE, c, 1);
    s_ws_real_aatri(v, v + WS_VSIZE, v + 2 * WS_VSIZE, ab, bc, ca);
}

static void __cdecl ws_draw_line(const void *a, const void *b)
{
    char v[2 * WS_VSIZE];

    if (!s_ws_real_line)
        return;
    if (ws_shift_is_identity()) {
        s_ws_real_line(a, b);
        return;
    }
    ws_shift(v, a, 1);
    ws_shift(v + WS_VSIZE, b, 1);
    s_ws_real_line(v, v + WS_VSIZE);
}

static void __cdecl ws_aa_draw_line(const void *a, const void *b)
{
    char v[2 * WS_VSIZE];

    if (!s_ws_real_aaline)
        return;
    if (ws_shift_is_identity()) {
        s_ws_real_aaline(a, b);
        return;
    }
    ws_shift(v, a, 1);
    ws_shift(v + WS_VSIZE, b, 1);
    s_ws_real_aaline(v, v + WS_VSIZE);
}

/* A vertex list is unbounded in principle. Hydro's own Glide walks it with
 * `count * 3 * 5 * 4`, so the stride is the same 60 bytes; the largest list in
 * either capture is far under this ceiling, and a list over it is drawn
 * unshifted with one loud line rather than silently truncated or stack-smashed. */
#define WS_VLIST_MAX 256u

static void ws_vlist(ws_vlist_fn real, int count, const void *verts)
{
    static char v[WS_VLIST_MAX * WS_VSIZE];
    static int over_reported;

    if (!real)
        return;
    if (count <= 0 || !verts || ws_shift_is_identity()) {
        real(count, verts);
        return;
    }
    if ((unsigned)count > WS_VLIST_MAX) {
        if (!over_reported) {
            over_reported = 1;
            LOGE("geometry: widescreen; a %d-vertex polygon list exceeds the "
                 "%u the origin buffer holds; it is submitted UNSHIFTED and "
                 "will appear %g pixels left of the rest of the picture",
                 count, WS_VLIST_MAX, (double)s_ws_dx);
        }
        real(count, verts);
        return;
    }
    ws_shift(v, verts, (unsigned)count);
    real(count, v);
}

static void __cdecl ws_draw_vertex_list(int count, const void *verts)
{
    ws_vlist(s_ws_real_vlist, count, verts);
}

/* Hydro's planar sky builder emits coordinates spanning the full raster, so
 * forward these lists without the seam origin. Other titles do not claim this
 * entry point. */
#define WS_SKY_REPORTS 16u
#define WS_SKY_SAMPLE  601u
static unsigned s_ws_sky_lists, s_ws_sky_reports;

static void __cdecl ws_draw_planar_vertex_list(int count, const void *verts)
{
    int wide = s_ws_sky_wide && s_ws_dx > 0.0f;

    if (wide && count > 0 && verts &&
        (s_ws_sky_lists < 4u || s_ws_sky_lists % WS_SKY_SAMPLE == 0u) &&
        s_ws_sky_reports < WS_SKY_REPORTS) {
        float lo = 1e30f, hi = -1e30f;
        int i;

        for (i = 0; i < count && (unsigned)i < WS_VLIST_MAX; i++) {
            float x = *(const float *)((const char *)verts +
                                       (size_t)i * WS_VSIZE);

            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        s_ws_sky_reports++;
        LOGI("geometry: widescreen sky list %u; %d vertices at x %.1f .. "
             "%.1f, submitted with no origin into a 0 .. %d raster",
             s_ws_sky_lists, count, (double)lo, (double)hi, glide_view_width() - 1);
    }
    if (wide) {
        s_ws_sky_lists++;
        if (s_ws_real_planar_vlist)
            s_ws_real_planar_vlist(count, verts);
        return;
    }
    ws_vlist(s_ws_real_planar_vlist, count, verts);
}

/* Execute full-screen fades with the wide viewport and no seam origin so the
 * quad covers the raster. The cdecl pass-through uses opaque argument slots to
 * support both title signatures. */
typedef void (__cdecl *fade_fn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t);
static fade_fn s_real_screen_fade;

/* The wide/no-origin state the fade runs in, held in statics rather than in
 * gl_screen_fade's frame because the post-fade hook below has to put it down
 * and pick it up again in the middle of the same call. */
static int   s_ws_fade_depth;
static int   s_ws_fade_hres;
static float s_ws_fade_hres_f, s_ws_fade_right, s_ws_fade_dx;

static void ws_fade_wide(void)
{
    wr_i(G->viewport_hres, glide_view_width());
    wr_f(G->viewport_hres_f, (float)glide_view_width());
    /* The fade quad reaches the 2D clipper like any other flat 2D, and that
     * clipper's right plane is `x <= screen_limit[1]`. Widening the viewport
     * without widening the rect the clipper enforces would build a raster-wide
     * quad and then cut it back to the native field one call later. Only the
     * RIGHT member moves: the fade starts at 0 and the origin is off for this
     * call, so the native left bound is already the raster's. */
    if (G->viewport_screen_limit)
        wr_f(G->viewport_screen_limit + 4, (float)glide_view_width() - 1.0f);
    s_ws_dx = 0.0f;
}

static void ws_fade_native(void)
{
    s_ws_dx = s_ws_fade_dx;
    if (G->viewport_screen_limit)
        wr_f(G->viewport_screen_limit + 4, s_ws_fade_right);
    wr_i(G->viewport_hres, s_ws_fade_hres);
    wr_f(G->viewport_hres_f, s_ws_fade_hres_f);
}

static void __cdecl gl_screen_fade(uint32_t a0, uint32_t a1, uint32_t a2,
                                   uint32_t a3, uint32_t a4, uint32_t a5,
                                   uint32_t a6, uint32_t a7)
{
    if (!s_real_screen_fade)
        return;
    if (s_ws_dx <= 0.0f) {
        s_real_screen_fade(a0, a1, a2, a3, a4, a5, a6, a7);
        return;
    }
    s_ws_fade_hres = *(const int *)(uintptr_t)G->viewport_hres;
    s_ws_fade_hres_f = *(const float *)(uintptr_t)G->viewport_hres_f;
    s_ws_fade_right = G->viewport_screen_limit
                        ? rd_f(G->viewport_screen_limit + 4) : 0.0f;
    s_ws_fade_dx = s_ws_dx;

    s_ws_fade_depth++;
    ws_fade_wide();
    s_real_screen_fade(a0, a1, a2, a3, a4, a5, a6, a7);
    ws_fade_native();
    s_ws_fade_depth--;
}

/* Offroad's fade invokes post-fade overlays. Restore the seam origin around the
 * callback runner only; Hydro has no corresponding anchor. */
typedef void (__cdecl *post_fade_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
static post_fade_fn s_real_post_fade;

static void __cdecl gl_post_fade_callbacks(uint32_t a0, uint32_t a1,
                                           uint32_t a2, uint32_t a3)
{
    if (!s_real_post_fade)
        return;
    if (!s_ws_fade_depth) {
        s_real_post_fade(a0, a1, a2, a3);
        return;
    }
    ws_fade_native();
    s_real_post_fade(a0, a1, a2, a3);
    ws_fade_wide();
}

/* Widen Hydro's screen-space sky quad during its two builder calls. Offroad's
 * geometric sky follows the expanded frustum without hooks. */
typedef void (__cdecl *sky_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
static sky_fn s_real_sky_viewport, s_real_sky_init;
static int s_ws_sky_reported;

static void ws_sky_build(sky_fn real, const char *which,
                         uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    float native_half, wide_half;

    if (!real)
        return;
    if (s_ws_dx <= 0.0f || !G->viewport_half_hres_f) {
        real(a0, a1, a2, a3);
        return;
    }
    native_half = rd_f(G->viewport_half_hres_f);
    /* The sky builder uses twice this value as a raster-width quad with its
     * left edge fixed at zero. */
    wide_half = (float)glide_view_width() * 0.5f;

    wr_f(G->viewport_half_hres_f, wide_half);
    real(a0, a1, a2, a3);
    wr_f(G->viewport_half_hres_f, native_half);

    if (!s_ws_sky_reported) {
        s_ws_sky_reported = 1;
        LOGI("geometry: widescreen; %s built the sky quad at half-width "
             "%.2f instead of the cabinet's %.2f, which everything else still "
             "sees. The quad comes out %.0f wide with its left edge at 0, so "
             "it is the raster and is submitted without the origin.",
             which, (double)wide_half, (double)native_half,
             (double)(2.0 * wide_half));
    }
}

static void __cdecl gl_sky_viewport(uint32_t a0, uint32_t a1, uint32_t a2,
                                    uint32_t a3)
{
    ws_sky_build(s_real_sky_viewport, "_sky_ChangeViewport", a0, a1, a2, a3);
}

static void __cdecl gl_sky_init(uint32_t a0, uint32_t a1, uint32_t a2,
                                uint32_t a3)
{
    ws_sky_build(s_real_sky_init, "_sky_Init", a0, a1, a2, a3);
}

/* Clamp Hydro's oversize ortho coordinates to the native 2D field before the
 * host substitutes a wide raster clip. Offroad's flat-2D path clips itself. */
typedef void (__cdecl *ortho_fn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t, uint32_t);
static ortho_fn s_real_drawortho;

static void __cdecl gl_draw_ortho(uint32_t a0, uint32_t a1, uint32_t a2,
                                  uint32_t a3, uint32_t a4, uint32_t a5,
                                  uint32_t a6, uint32_t a7)
{
    if (!s_real_drawortho)
        return;
    if (s_ws_dx <= 0.0f) {
        s_real_drawortho(a0, a1, a2, a3, a4, a5, a6, a7);
        return;
    }
    s_ws_ortho_depth++;
    s_real_drawortho(a0, a1, a2, a3, a4, a5, a6, a7);
    s_ws_ortho_depth--;
}

/* Returns nonzero when this entry point has been taken over by the origin.
 * Armed from the profile at bind time; the origin VALUE arrives later, when
 * grSstWinOpen settles the raster, and is 0 until then. */
int ws_claim_primitive(const char *name, void *real, uint32_t va)
{
    if (!s_ws_armed)
        return 0;
    if (strcmp(name, "grDrawTriangle") == 0) {
        s_ws_real_tri = (ws_tri_fn)real;
        patch_jmp(va, (void *)ws_draw_triangle);
    } else if (strcmp(name, "grAADrawTriangle") == 0) {
        s_ws_real_aatri = (ws_aatri_fn)real;
        patch_jmp(va, (void *)ws_aa_draw_triangle);
    } else if (strcmp(name, "grDrawLine") == 0) {
        s_ws_real_line = (ws_line_fn)real;
        patch_jmp(va, (void *)ws_draw_line);
    } else if (strcmp(name, "grAADrawLine") == 0) {
        s_ws_real_aaline = (ws_line_fn)real;
        patch_jmp(va, (void *)ws_aa_draw_line);
    } else if (strcmp(name, "grDrawPolygonVertexList") == 0) {
        s_ws_real_vlist = (ws_vlist_fn)real;
        patch_jmp(va, (void *)ws_draw_vertex_list);
    } else if (strcmp(name, "grDrawPlanarPolygonVertexList") == 0) {
        s_ws_real_planar_vlist = (ws_vlist_fn)real;
        patch_jmp(va, (void *)ws_draw_planar_vertex_list);
    } else {
        return 0;
    }
    return 1;
}

/* Validate all widescreen anchors before binding Glide primitives. */
static void widescreen_arm(void)
{
    int hydro_repairs, offroad_repairs;

    s_ws_armed = 0;
    s_ws_sky_wide = 0;
    s_ws_fade_depth = 0;
    s_ws_ortho_depth = 0;
    s_ws_dx = 0.0f;
    s_ws_native_width = 0;
    s_ws_cull_width = 0;
    s_ws_lensflare_depth = 0;
    s_ws_native_fov = 0;
    s_ws_have_native_fov = 0;
    s_ws_viewport_generation = 0;
    s_ws_repair_reported = 0;
    s_real_ws_lensflare = s_real_ws_drawqueue = NULL;
    s_real_ws_spark_cluster = s_real_ws_racer_tag = NULL;
    s_real_ws_race_update = NULL;
    s_real_ws_flag_icon = NULL;
    if (!g_cfg.widescreen)
        return;
    if (!game_require("widescreen", "the sole writer of the viewport block and "
                      "its relocatable prologue",
                      G->viewport_init && G->hook_viewport_init) ||
        !game_require("widescreen", "the viewport members the 2D layer reads",
                      G->viewport_fov && G->viewport_hres &&
                      G->viewport_hres_f &&
                      G->viewport_half_hres && G->viewport_half_hres_f &&
                      G->viewport_frustum_limit && G->viewport_screen_limit &&
                      G->viewport_zscreen) ||
        !game_require("widescreen", "_Init3dfx_nHorizontalPixels",
                      G->init3dfx_hpixels))
        return;
    hydro_repairs = strcmp(G->id, "hydro") == 0;
    offroad_repairs = strcmp(G->id, "offroad") == 0;
    if (hydro_repairs) {
        if (!game_require("widescreen", "Hydro's audited pre-seam consumers "
                          "and relocatable prologues",
                          G->ws_lensflare && G->hook_ws_lensflare &&
                          G->ws_drawqueue && G->hook_ws_drawqueue))
            return;
    } else if (offroad_repairs) {
        if (!game_require("widescreen", "Offroad's audited pre-seam/state "
                          "consumers and relocatable prologues",
                          G->ws_spark_cluster && G->hook_ws_spark_cluster &&
                          G->ws_racer_tag && G->hook_ws_racer_tag &&
                          G->ws_race_update && G->hook_ws_race_update &&
                          G->ws_flag_icon && G->hook_ws_flag_icon))
            return;
    } else {
        LOGE("geometry: widescreen refused; %s has no audited consumer "
             "repair profile", G->id);
        return;
    }
    /* Both of these take primitive entry points away from this origin, and an
     * origin applied to some triangles and not others is worse than one that
     * is not applied at all. */
    if (g_cfg.time_calls || g_cfg.trace_draw_state) {
        LOGE("geometry: widescreen refused; time_calls / trace_draw_state "
             "claim grDrawTriangle, so the seam origin could not reach every "
             "primitive. Turn the diagnostic off or the mode off.");
        return;
    }
    s_real_viewport_init = (viewport_init_fn)hook_call_through(
        G->viewport_init, (void *)gl_viewport_init, G->hook_viewport_init);
    if (!s_real_viewport_init) {
        LOGE("geometry: widescreen; the viewport hook was refused; the mode "
             "is off");
        return;
    }
    if (hydro_repairs) {
        s_real_ws_lensflare = (ws_void_fn)hook_call_through(
            G->ws_lensflare, (void *)gl_ws_lensflare,
            G->hook_ws_lensflare);
        s_real_ws_drawqueue = (ws_void_fn)hook_call_through(
            G->ws_drawqueue, (void *)gl_ws_drawqueue,
            G->hook_ws_drawqueue);
        if (!s_real_ws_lensflare || !s_real_ws_drawqueue) {
            LOGE("geometry: widescreen; a required Hydro consumer hook was "
                 "refused; the mode is off");
            return;
        }
        LOGI("geometry: widescreen; Hydro's lens-flare LFB read and "
             "projected draw queue are hooked at their pre-seam decisions");
    } else {
        s_real_ws_spark_cluster = (ws_one_fn)hook_call_through(
            G->ws_spark_cluster, (void *)gl_ws_spark_cluster,
            G->hook_ws_spark_cluster);
        s_real_ws_racer_tag = (ws_one_fn)hook_call_through(
            G->ws_racer_tag, (void *)gl_ws_racer_tag,
            G->hook_ws_racer_tag);
        s_real_ws_race_update = (ws_void_fn)hook_call_through(
            G->ws_race_update, (void *)gl_ws_race_update,
            G->hook_ws_race_update);
        s_real_ws_flag_icon = (ws_flag_fn)hook_call_through(
            G->ws_flag_icon, (void *)gl_ws_flag_icon,
            G->hook_ws_flag_icon);
        if (!s_real_ws_spark_cluster || !s_real_ws_racer_tag ||
            !s_real_ws_race_update || !s_real_ws_flag_icon) {
            LOGE("geometry: widescreen; a required Offroad consumer hook "
                 "was refused; the mode is off");
            return;
        }
        LOGI("geometry: widescreen; Offroad's spark/racer-tag projection "
             "and race/flag FOV state consumers are hooked at their pre-seam "
             "decisions");
    }
    /* Install a title's post-fade callback hook before its fade hook. The pair
     * is atomic because installed hooks cannot be removed safely. */
    if (game_require("widescreen", "the title's full-screen fade",
                     G->screen_fade && G->hook_screen_fade)) {
        int fade_ok = 1;

        if (G->post_fade_callbacks) {
            if (game_require("widescreen", "the fade's post-callback runner",
                             G->post_fade_callbacks && G->hook_post_fade))
                s_real_post_fade = (post_fade_fn)hook_call_through(
                    G->post_fade_callbacks, (void *)gl_post_fade_callbacks,
                    G->hook_post_fade);
            fade_ok = s_real_post_fade != NULL;
            if (!fade_ok)
                LOGE("geometry: widescreen; this title's fade runs callbacks "
                     "and the runner could not be hooked, so the fade is left "
                     "alone too; it will cover the native field only, which is "
                     "wrong in one place instead of everywhere those callbacks "
                     "draw");
        }
        if (fade_ok) {
            s_real_screen_fade = (fade_fn)hook_call_through(
                G->screen_fade, (void *)gl_screen_fade, G->hook_screen_fade);
            if (!s_real_screen_fade)
                LOGE("geometry: widescreen; the full-screen fade hook was "
                     "refused; fades will cover the native field only");
            else if (G->post_fade_callbacks)
                LOGI("geometry: widescreen; the fade and its post-callback "
                     "runner are both hooked, so only the fade's own box is "
                     "drawn at the raster's width");
        }
    }
    /* Also not fatal, and Hydro-only. Without it the mode still works and one
     * column of the 2D layer's own edge slop reaches the left margin. */
    if (G->mesh3d_drawortho) {
        if (game_require("widescreen", "the ortho 2D path",
                         G->mesh3d_drawortho && G->hook_drawortho))
            s_real_drawortho = (ortho_fn)hook_call_through(
                G->mesh3d_drawortho, (void *)gl_draw_ortho, G->hook_drawortho);
        if (!s_real_drawortho)
            LOGE("geometry: widescreen; the ortho 2D hook was refused; the "
                 "2D layer's one-pixel edge slop will reach the left margin");
        else
            LOGI("geometry: widescreen; the ortho 2D path is hooked; its "
                 "geometry is clamped to the game's own 0..%d field, which is "
                 "what the cabinet's clip did",
                 s_ws_native_width > 0 ? s_ws_native_width : 512);
    }
    /* Also not fatal, and absent by design in Offroad; game_require() is
     * asked once for the pair so a title that has neither says so once rather
     * than reporting two missing anchors as if something were wrong. */
    if (G->sky_viewport || G->sky_init) {
        if (game_require("widescreen", "the sky quad's two builders",
                         G->sky_viewport && G->hook_sky_viewport &&
                         G->sky_init && G->hook_sky_init)) {
            s_real_sky_viewport = (sky_fn)hook_call_through(
                G->sky_viewport, (void *)gl_sky_viewport, G->hook_sky_viewport);
            s_real_sky_init = (sky_fn)hook_call_through(
                G->sky_init, (void *)gl_sky_init, G->hook_sky_init);
            if (!s_real_sky_viewport || !s_real_sky_init)
                LOGE("geometry: widescreen; a sky-quad hook was refused; the "
                     "backdrop will cover the native field only and the side "
                     "strips will be black wherever the world does not cover "
                     "them");
            else {
                s_ws_sky_wide = 1;
                LOGI("geometry: widescreen; both sky-quad builders are "
                     "hooked; the backdrop is built at the raster's half-width "
                     "and placed with no origin, and everything else keeps the "
                     "cabinet's");
            }
        }
    } else {
        LOGI("geometry: widescreen; this title has no screen-space sky quad "
             "to widen; its sky is frustum-tested geometry and follows the "
             "widened culling by itself");
    }

    s_ws_margin_enabled = g_cfg.margin_census && s_ws_native_width >= 0;
    if (g_cfg.margin_census)
        LOGW("geometry: widescreen margin census is ON; it names every game "
             "call site that draws outside the native field, and costs four "
             "float compares per submitted vertex");
    s_ws_armed = 1;
    LOGI("geometry: widescreen armed; the game keeps its own 2D coordinate "
         "system and the seam carries the origin; the raster settles it at "
         "grSstWinOpen");
}

/* Settle the origin once the surface is open and the raster is known. Called
 * from gl_sst_win_open, which is also where a Glide reinit re-settles it. */
static void widescreen_settle(int native_width, int raster_width)
{
    if (!s_ws_armed)
        return;
    if (native_width <= 0 || raster_width <= native_width) {
        s_ws_dx = 0.0f;
        s_ws_native_width = 0;
        s_ws_cull_width = 0;
        LOGI("geometry: widescreen is inert; the game's own raster is not "
             "narrower than the visible one");
        return;
    }
    s_ws_native_width = native_width;
    /* Use an integer seam origin. Rounding down matches the LFB field offset and
     * keeps integer 2D coordinates away from ambiguous half-pixel tile seams. */
    s_ws_dx = (float)((raster_width - native_width) / 2);
    /* Round the symmetric 3D culling width up so an integer origin still covers
     * the final raster pixel centre. Full-screen 2D draws use the raster width. */
    s_ws_cull_width = native_width + 2 * (int)s_ws_dx +
                      (((raster_width - native_width) & 1) ? 2 : 0);
    s_ws_reported = 0;
    s_ws_sky_reported = 0;
    LOGI("geometry: widescreen origin; %d-wide game coordinates centred at "
         "x=%g inside a %d-wide raster; every primitive and every LFB field "
         "moves by that WHOLE-PIXEL number and no 2D coordinate is rewritten. "
         "The 3D is culled to %d, half a column wider at each end, so the "
         "raster's last column is still inside the frustum after the "
         "translation.",
         native_width, (double)s_ws_dx, raster_width, s_ws_cull_width);
}

/* ---- the seam (src/glide_int.h) ----------------------------------------- */

void ws_arm(void)
{
    widescreen_arm();
}

void ws_settle(int native_width, int raster_width)
{
    widescreen_settle(native_width, raster_width);
}

void ws_frame_boundary(void)
{
    ws_margin_tick();
}

float ws_origin(void)
{
    return s_ws_dx;
}

unsigned ws_lfb_read_offset(int lock_type, unsigned stride)
{
    unsigned offset;

    if (!s_ws_lensflare_depth || s_ws_dx <= 0.0f ||
        (lock_type & 0x0F) != GR_LFB_READ_ONLY)
        return 0;
    offset = (unsigned)s_ws_dx * 2u;    /* READ_ONLY is RGB565 */
    if (offset >= stride)
        return 0;
    if (!(s_ws_repair_reported & WS_REPAIR_LENS)) {
        s_ws_repair_reported |= WS_REPAIR_LENS;
        LOGI("geometry: widescreen repair; Hydro lens-flare READ_ONLY "
             "LFB sampling follows the displayed pixel at x + %u",
             offset / 2u);
    }
    return offset;
}
