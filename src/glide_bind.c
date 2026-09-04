/* glide_bind.c -- redirect the games' linked-in Glide entries to vcglide.dll.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Generated profiles provide per-title addresses and argument sizes. Thunks
 * bridge the games' cdecl calls to the provider's stdcall exports.
 */
#include "vcthunder.h"
#include "glide_int.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static HMODULE s_glide;
static char    s_path[MAX_PATH];
/* The provider's health, asked once per frame. NULL when the provider does not
 * export it, which is how an older vcglide.dll beside a newer host behaves:
 * the check is skipped, not faked. Anything but 0 is a fault; the provider's
 * own log says which one, and this side deliberately does not grow a copy of
 * that table across the DLL boundary. */
typedef int (__stdcall *health_fn)(void);
static health_fn s_health;

/* Glide passes the mode selected by the game to grSstWinOpen as an enum, not
 * pixel dimensions, and the game passes a null cabinet HWND with it. Translate
 * the game's own enum and size the host client accordingly: there is no
 * independent shim resolution policy. */
static const struct { int width, height; } GLIDE_RESOLUTION[] = {
    {  320,  200 }, {  320,  240 }, {  400,  256 }, {  512,  384 },
    {  640,  200 }, {  640,  350 }, {  640,  400 }, {  640,  480 },
    {  800,  600 }, {  960,  720 }, {  856,  480 }, {  512,  256 },
    { 1024,  768 }, { 1280, 1024 }, { 1600, 1200 }, {  400,  300 },
};

/* The wrapper's SURFACE, which is not necessarily what the game draws into: a
 * 400-line raster is only obtainable from a 640x400 surface, and the game then
 * composes 512x400 of it. The surface sizes the LFB region; the visible raster
 * (window.c) sizes the window and decides the aspect. */
static int  s_game_width, s_game_height;
static int  s_view_width, s_view_height;
static int  s_native_lfb_width;
static int  s_resize_watch;
static int  s_geometry_reported;

/* Detect host exit at frame boundaries. Poll Escape asynchronously and treat
 * queued close requests or disappearance of the backend window as shutdown.
 * Do not dispatch a close that could destroy the active surface mid-frame. */
static int close_requested(const MSG *msg)
{
    if (msg->message == WM_QUIT || msg->message == WM_CLOSE)
        return 1;
    return msg->message == WM_SYSCOMMAND &&
           (msg->wParam & 0xfff0) == SC_CLOSE;
}

static int escape_down(void)
{
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && window_input_focused();
}

typedef int (__stdcall *win_open_fn)(uintptr_t, int, int, int, int, int, int);
static win_open_fn s_real_win_open;

/* A forced video_mode selects an entry in the game's own table through
 * _init3dfx_Init.  Offroad's text layer later calls grSstWinOpen directly and
 * asks for the cabinet enum again, so changing only _init3dfx_Init is not
 * enough: its last open silently wins.  Latch the resolution that the selected
 * table entry actually supplies during _init3dfx_Init, then reuse that observed
 * enum for every later open.  This deliberately does not duplicate either
 * title's mode table in the host. */
static int s_forced_init_active;
static int s_forced_resolution = -1;

/* Forward the hardware query unchanged. vcglide reports the cabinet's two-TMU
 * board descriptor, including the fbiRev value used by Hydro to select its
 * Voodoo2 path. */

/* Select the smallest legal Glide surface containing the visible raster. The
 * 512x400 cabinet raster uses 640x400; a 711x400 widescreen raster uses
 * 856x480. Extra surface pixels are excluded from presentation. */
static int glide_enum_for_raster(int requested, int want_width, int want_height)
{
    unsigned i;
    int best = requested;
    unsigned best_area = ~0u;

    if (want_width <= 0 || want_height <= 0 || requested < 0 ||
        requested >= (int)(sizeof GLIDE_RESOLUTION / sizeof GLIDE_RESOLUTION[0]))
        return requested;
    if (GLIDE_RESOLUTION[requested].width >= want_width &&
        GLIDE_RESOLUTION[requested].height >= want_height)
        return requested;

    for (i = 0; i < sizeof GLIDE_RESOLUTION / sizeof GLIDE_RESOLUTION[0]; i++) {
        unsigned area;

        if (GLIDE_RESOLUTION[i].width < want_width)
            continue;
        if (GLIDE_RESOLUTION[i].height < want_height)
            continue;
        area = (unsigned)GLIDE_RESOLUTION[i].width *
               (unsigned)GLIDE_RESOLUTION[i].height;
        if (area < best_area) {
            best_area = area;
            best = (int)i;
        }
    }
    return best;
}

/* Return the nearest integer 16:9 width for a fixed raster height. */
static int widescreen_width(int height)
{
    return height > 0 ? (height * 16 + 4) / 9 : 0;
}

/* Publish the host-known visible raster for each surface open. This preserves
 * the cabinet raster across Glide reinitialisation without relying on a later
 * grClipWindow call. */
static void glide_publish_raster(int w, int h, int native_lfb_width)
{
    char n[32];

    s_native_lfb_width = 0;
    if (w > 0 && h > 0) {
        snprintf(n, sizeof n, "%d", w);
        SetEnvironmentVariableA("VCGLIDE_RASTER_WIDTH", n);
        snprintf(n, sizeof n, "%d", h);
        SetEnvironmentVariableA("VCGLIDE_RASTER_HEIGHT", n);
        if (native_lfb_width > 0 && native_lfb_width < w) {
            s_native_lfb_width = native_lfb_width;
            snprintf(n, sizeof n, "%d", native_lfb_width);
            SetEnvironmentVariableA("VCGLIDE_LFB_WIDTH", n);
            LOGI("geometry: native %d-wide 2D LFB fields are centred at x=%d "
                 "inside the %d-wide raster", native_lfb_width,
                 (w - native_lfb_width) / 2, w);
        } else {
            SetEnvironmentVariableA("VCGLIDE_LFB_WIDTH", NULL);
        }
    } else {
        SetEnvironmentVariableA("VCGLIDE_RASTER_WIDTH", NULL);
        SetEnvironmentVariableA("VCGLIDE_RASTER_HEIGHT", NULL);
        SetEnvironmentVariableA("VCGLIDE_LFB_WIDTH", NULL);
    }
}

/* EVERY GEOMETRY DECISION THIS HOST MAKES, IN ONE FUNCTION.
 *
 * There are three numbers and they are easy to confuse, which is most of why
 * this is now written down in one place instead of spread down the middle of
 * grSstWinOpen:
 *
 *   `resolution`  the enum the GAME asked for, possibly redirected by
 *                 video_mode through the title's own mode table.
 *   `raster`      the VISIBLE picture; what the window is sized from, what
 *                 the provider presents, and what the widescreen origin is
 *                 computed against. Hor+ widens it; raster_height sets it.
 *   `surface`     the smallest legal Glide surface that CONTAINS the raster.
 *                 Backing storage only. Sizing anything from it exposes pad
 *                 pixels and computes the aspect from the wrong rectangle.
 *
 * Everything downstream (the window, the provider, the widescreen engine)
 * consumes this one answer, so there is one place for M5's cabinet timings to
 * be derived and one place they can be wrong. */
typedef struct {
    int resolution;                 /* after any video_mode redirection    */
    int surface;                    /* the enum actually opened            */
    int surface_w, surface_h;
    int raster_w, raster_h;
    int native_w;                   /* the width the game would have had   */
} glide_geometry_t;

static int resolution_is_known(int enum_value)
{
    return enum_value >= 0 &&
           enum_value < (int)(sizeof GLIDE_RESOLUTION /
                              sizeof GLIDE_RESOLUTION[0]);
}

static void resolve_geometry(int resolution, glide_geometry_t *g)
{
    memset(g, 0, sizeof *g);

    if (g_cfg.video_mode >= 0) {
        if (s_forced_init_active && s_forced_resolution < 0) {
            s_forced_resolution = resolution;
            LOGI("geometry: video_mode=%d selected resolution enum %d through "
                 "the game's own table; all grSstWinOpen callers will use it",
                 g_cfg.video_mode, s_forced_resolution);
        } else if (s_forced_resolution >= 0 &&
                   resolution != s_forced_resolution) {
            LOGI("geometry: grSstWinOpen resolution enum %d -> %d because "
                 "video_mode=%d is forced",
                 resolution, s_forced_resolution, g_cfg.video_mode);
            resolution = s_forced_resolution;
        }
    }
    g->resolution = resolution;

    if (resolution_is_known(resolution)) {
        g->native_w = GLIDE_RESOLUTION[resolution].width;
        g->raster_h = (g_cfg.raster_height > 0 && g_cfg.video_mode < 0)
                        ? g_cfg.raster_height
                        : GLIDE_RESOLUTION[resolution].height;
        g->raster_w = g_cfg.widescreen ? widescreen_width(g->raster_h)
                                       : GLIDE_RESOLUTION[resolution].width;
        if (g->raster_w < GLIDE_RESOLUTION[resolution].width)
            g->raster_w = GLIDE_RESOLUTION[resolution].width;
    }
    g->surface = glide_enum_for_raster(resolution, g->raster_w, g->raster_h);
    if (resolution_is_known(g->surface)) {
        g->surface_w = GLIDE_RESOLUTION[g->surface].width;
        g->surface_h = GLIDE_RESOLUTION[g->surface].height;
    }
}

static int gl_sst_win_open(uintptr_t hwnd, int resolution, int refresh,
                           int color_format, int origin,
                           int color_buffers, int aux_buffers)
{
    glide_geometry_t g;
    int desired_width, desired_height;
    int result;
    int surface;

    resolve_geometry(resolution, &g);
    resolution    = g.resolution;
    surface       = g.surface;
    desired_width = g.raster_w;
    desired_height = g.raster_h;

    /* Optionally enable SSE FTZ and DAZ on the render thread. The game uses x87,
     * so this affects only provider arithmetic on denormal inputs. */
    if (g_cfg.sse_ftz) {
        unsigned mxcsr;
        __asm__ volatile ("stmxcsr %0" : "=m" (mxcsr));
        mxcsr |= 0x8000u | 0x0040u;             /* FTZ | DAZ */
        __asm__ volatile ("ldmxcsr %0" : : "m" (mxcsr));
        LOGI("glide: MXCSR -> 0x%08X on the render thread (FTZ+DAZ) so the "
             "wrapper's SSE does not take denormal assists on the game's "
             "vertices; the game's own x87 math is untouched", mxcsr);
    }

    if (surface != resolution) {
        if (g_cfg.widescreen) {
            LOGI("geometry: widescreen raster %dx%d needs a containing Glide "
                 "surface; enum %d (%dx%d) -> enum %d (%dx%d)",
                 desired_width, desired_height, resolution,
                 GLIDE_RESOLUTION[resolution].width,
                 GLIDE_RESOLUTION[resolution].height,
                 surface, GLIDE_RESOLUTION[surface].width,
                 GLIDE_RESOLUTION[surface].height);
        } else {
            LOGI("glide: raster_height=%d needs a taller surface than enum %d "
                 "(%dx%d); asking the wrapper for enum %d (%dx%d) instead: "
                 "the game still clips itself to %dx%d",
                 g_cfg.raster_height, resolution,
                 GLIDE_RESOLUTION[resolution].width,
                 GLIDE_RESOLUTION[resolution].height,
                 surface, GLIDE_RESOLUTION[surface].width,
                 GLIDE_RESOLUTION[surface].height,
                 GLIDE_RESOLUTION[resolution].width, g_cfg.raster_height);
        }
    }

    /* The LFB extent follows the legal surface the wrapper actually creates.
     * The WINDOW follows the raster the game composes into it, which may be
     * smaller on either axis. Sizing from the backing surface would expose pad
     * pixels and compute the presentation aspect from the wrong rectangle. */
    if (surface >= 0 && surface < (int)(sizeof GLIDE_RESOLUTION /
                                        sizeof GLIDE_RESOLUTION[0])) {
        s_game_width = GLIDE_RESOLUTION[surface].width;
        s_game_height = GLIDE_RESOLUTION[surface].height;
        s_view_width = desired_width;
        /* A forced standard video mode supplies its own raster height and must
         * ignore cabinet-raster reconciliation. */
        s_view_height = desired_height;
        s_resize_watch = 600;          /* frames. Written for a provider that
                                        * reset the mode asynchronously; kept
                                        * as a general watch, since SDL resizes
                                        * its own window too */
        LOGI("glide: game requested resolution enum %d -> %dx%d, refresh enum %d, "
             "colour format %d, origin %s, %d colour buffers, %d aux",
             resolution, GLIDE_RESOLUTION[resolution].width,
             GLIDE_RESOLUTION[resolution].height, refresh, color_format,
             origin ? "LOWER_LEFT" : "UPPER_LEFT", color_buffers, aux_buffers);
        LOGI("glide: surface %dx%d, visible raster %dx%d", s_game_width,
             s_game_height, s_view_width, s_view_height);

        if (g_cfg.widescreen) {
            int native_width = GLIDE_RESOLUTION[resolution].width;

            if (s_view_width != native_width) {
                LOGW("geometry: widescreen 16:9; visible raster %dx%d -> "
                     "%dx%d (nearest integer width). The game is told it has "
                     "%d columns; its vertical resolution remains %d. The "
                     "%dx%d Glide surface is backing storage only.",
                     native_width, s_view_height, s_view_width, s_view_height,
                     s_view_width, s_view_height, s_game_width, s_game_height);
            } else {
                LOGW("geometry: widescreen is INERT; a 16:9 raster at %d "
                     "lines is not wider than the game's existing %dx%d",
                     s_view_height, native_width, s_view_height);
            }
        }

        if (s_view_width > s_game_width || s_view_height > s_game_height)
            LOGW("geometry: visible raster %dx%d exceeds the %dx%d surface; "
                 "the excess pixels have nowhere to go",
                 s_view_width, s_view_height, s_game_width, s_game_height);

        /* A cross-check against a second crop stood here: it had to name the
         * displayed region before the provider loaded, from the
         * cabinet's known mode rather than from this call, so a game asking
         * for something else made the crop silently wrong. There is no crop to
         * disagree with now: the provider is handed this call's own numbers
         * by glide_publish_raster() below, which is the same fact arriving
         * from the right side. */
    } else {
        s_game_width = s_game_height = s_resize_watch = 0;
        s_view_width = s_view_height = 0;
        LOGW("glide: game requested unknown resolution enum %d", resolution);
    }

    glide_publish_raster(s_view_width, s_view_height,
        g_cfg.widescreen && resolution >= 0 &&
        resolution < (int)(sizeof GLIDE_RESOLUTION /
                           sizeof GLIDE_RESOLUTION[0])
            ? GLIDE_RESOLUTION[resolution].width : 0);
    /* Recompute the widescreen origin for each newly opened surface. */
    ws_settle(s_native_lfb_width, s_view_width);
    window_set_hwnd((HWND)hwnd);
    result = s_real_win_open ? s_real_win_open(hwnd, surface, refresh,
                 color_format, origin, color_buffers, aux_buffers) : 0;
    window_geometry(s_view_width, s_view_height);
    return result;
}

/* Cabinet mode 1 is nominally GR_RESOLUTION_512x384 plus a proprietary
 * Voodoo2 512x400 CRT timing supplied through grSstVidMode. Modern Glide
 * wrappers own the output mode and ignore that extra raster timing, but Hydro
 * caches 400 near the END of _init3dfx_Init, after grSstWinOpen has returned.
 * A post-call hook is therefore the first safe point to match the game's
 * geometry to the actual wrapper surface, before the viewport is constructed.
 * HYDRO.EXE remains untouched; this changes only its unpacked runtime DATA. */
typedef int (__cdecl *init3dfx_fn)(int, void *);
static init3dfx_fn s_real_init3dfx;

static double rdf_opt(uint32_t va, double absent)
{
    return va ? (double)*(const float *)(uintptr_t)va : absent;
}

static int gl_init3dfx(int mode, void *opaque)
{
    int result;
    int render_height;
    const int *tmu_rev;
    const int *tmu_bytes;
    static const int zero2[2] = { 0, 0 };

    /* Select an entry from the game's video-mode table. Mode 2 is a 640x480
     * comparison mode for emulators and modern displays only; it is unsafe for
     * the original cabinet monitor. Forced modes disable raster-height repair. */
    if (g_cfg.video_mode >= 0 && g_cfg.video_mode != mode) {
        LOGW("geometry: video_mode=%d forced; the game asked for mode %d. "
             "This selects the GAME'S OWN table entry, nothing is synthesised. "
             "NOT SAFE for an original arcade monitor; comparison use only.",
             g_cfg.video_mode, mode);
        mode = g_cfg.video_mode;
    }

    if (g_cfg.video_mode >= 0) {
        s_forced_resolution = -1;
        s_forced_init_active = 1;
    }
    result = s_real_init3dfx ? s_real_init3dfx(mode, opaque) : 0;
    s_forced_init_active = 0;
    if (g_cfg.video_mode >= 0 && s_forced_resolution < 0)
        LOGW("geometry: video_mode=%d was forced but _init3dfx_Init did not "
             "reach grSstWinOpen; later direct opens cannot be normalized",
             g_cfg.video_mode);
    render_height = rd_opt(G->init3dfx_vpixels, 0);
    tmu_rev = (const int *)(uintptr_t)G->init3dfx_tmu_rev;
    tmu_bytes = (const int *)(uintptr_t)G->init3dfx_tmu_bytes;

    if (!tmu_rev)
        tmu_rev = zero2;
    if (!tmu_bytes)
        tmu_bytes = zero2;

    LOGI("glide: game detected chipset=%d, FBI rev=%d memory=%d KiB, "
         "TMUs=%d [rev %d/%d, memory %d/%d KiB]",
         rd_opt(G->init3dfx_chipset, -1),
         rd_opt(G->init3dfx_fbi_rev, -1),
         rd_opt(G->init3dfx_fb_bytes, 0) / 1024,
         rd_opt(G->init3dfx_tmu_count, -1),
         tmu_rev[0], tmu_rev[1], tmu_bytes[0] / 1024, tmu_bytes[1] / 1024);

    /* Reconcile the cabinet's 512x400 raster with legal Glide surfaces. The
     * default preserves 400 rows in a cropped 640x400 surface; auto normalises
     * the game to the standard 512x384 mode. */
    if (g_cfg.video_mode >= 0) {
        LOGI("geometry: raster_height ignored while video_mode=%d is forced; "
             "it reconciles the CABINET raster with a legal Glide surface and "
             "mode %d has neither. The game's own %d-line raster stands.",
             g_cfg.video_mode, g_cfg.video_mode, render_height);
    } else if (g_cfg.raster_height > 0) {
        if (render_height != g_cfg.raster_height) {
            LOGI("geometry: raster_height=%d forced (game had %d)",
                 g_cfg.raster_height, render_height);
            if (G->init3dfx_vpixels)
                *(int *)(uintptr_t)G->init3dfx_vpixels = g_cfg.raster_height;
        }
        if (s_game_height > 0 && g_cfg.raster_height > s_game_height)
            LOGW("geometry: raster_height=%d exceeds the %dx%d surface the "
                 "wrapper created: the bottom %d rows have nowhere to go",
                 g_cfg.raster_height, s_game_width, s_game_height,
                 g_cfg.raster_height - s_game_height);
    } else if (s_game_height > 0 && render_height > 0 &&
               render_height != s_game_height) {
        LOGI("geometry: wrapper has no OEM %d-line raster; normalizing game "
             "height %d -> %d", render_height, render_height, s_game_height);
        if (G->init3dfx_vpixels)
            *(int *)(uintptr_t)G->init3dfx_vpixels = s_game_height;
    }

    /* Keep the game's native 2D width. Widescreen expands only the frustum
     * during viewport initialisation and applies a raster origin at the seam. */
    if (g_cfg.widescreen && s_view_width > 0) {
        int render_width = rd_opt(G->init3dfx_hpixels, 0);

        render_height = rd_opt(G->init3dfx_vpixels, 0);
        LOGI("geometry: widescreen; the game keeps its own %dx%d render "
             "geometry; the %dx%d visible raster is reached by widening the "
             "frustum at _viewport_InitToFullScreen and translating the seam, "
             "so no 2D coordinate in the title is rewritten",
             render_width, render_height, s_view_width, s_view_height);
    }
    return result;
}

/* Record the game's actual clip rectangle. This is more useful than inferring
 * geometry from the host window: all 2D and 3D draws share this Glide boundary,
 * so a mismatch here would prove a game/wrapper crop before presentation. */
typedef void (__stdcall *clip_fn)(unsigned, unsigned, unsigned, unsigned);
static clip_fn s_real_clip;
static unsigned s_clip_min_x, s_clip_min_y, s_clip_max_x, s_clip_max_y;
static int s_have_clip;
static int s_ws_clip_reported;

static void gl_clip_window(unsigned min_x, unsigned min_y,
                           unsigned max_x, unsigned max_y)
{
    /* Under widescreen, substitute the host raster clip for the game's native
     * screen rectangle. The game's earlier 2D clip remains native-width. */
    if (ws_origin() > 0.0f && s_view_width > 0 && s_view_height > 0) {
        unsigned rw = (unsigned)s_view_width, rh = (unsigned)s_view_height;

        if (max_x + 1u < rw || max_y + 1u < rh) {
            if (!s_ws_clip_reported) {
                s_ws_clip_reported = 1;
                LOGI("geometry: widescreen; the game's clip (%u,%u)-(%u,%u) "
                     "is its native screen rect; the raster clip (0,0)-(%u,%u) "
                     "is sent instead so the widened 3D is not cropped back",
                     min_x, min_y, max_x, max_y, rw - 1u, rh - 1u);
            }
            min_x = 0; min_y = 0;
            max_x = rw - 1u; max_y = rh - 1u;
        }
    }
    if (!s_have_clip || min_x != s_clip_min_x || min_y != s_clip_min_y ||
        max_x != s_clip_max_x || max_y != s_clip_max_y) {
        LOGI("geometry: grClipWindow (%u,%u)-(%u,%u) = %ux%u",
             min_x, min_y, max_x, max_y,
             max_x >= min_x ? max_x - min_x + 1u : 0u,
             max_y >= min_y ? max_y - min_y + 1u : 0u);
        s_clip_min_x = min_x;
        s_clip_min_y = min_y;
        s_clip_max_x = max_x;
        s_clip_max_y = max_y;
        s_have_clip = 1;
    }
    if (s_real_clip)
        s_real_clip(min_x, min_y, max_x, max_y);
}

/* Configure and preload vcglide before game mapping. Configuration crosses the
 * DLL boundary through environment variables, and vglInit loads backend
 * dependencies while LoadLibrary remains available. */
static void vcglide_configure(void)
{
    char n[32];

    SetEnvironmentVariableA("VCGLIDE_LOG",     g_cfg.vcglide_log);
    SetEnvironmentVariableA("VCGLIDE_CAPTURE", g_cfg.glide_capture);
    SetEnvironmentVariableA("VCGLIDE_FRAMES",  g_cfg.glide_frames);
    SetEnvironmentVariableA("VCGLIDE_TITLE",   G ? G->id : "unknown");
    snprintf(n, sizeof n, "%u", g_cfg.glide_capture_frames);
    SetEnvironmentVariableA("VCGLIDE_CAPTURE_FRAMES", n);
    snprintf(n, sizeof n, "%d", g_cfg.vcglide_verbose ? 1 : 0);
    SetEnvironmentVariableA("VCGLIDE_VERBOSE", n);
    /* Pass the worker count without boolean normalization. */
    snprintf(n, sizeof n, "%d", g_cfg.vcglide_threads);
    SetEnvironmentVariableA("VCGLIDE_THREADS", n);
    snprintf(n, sizeof n, "%d", g_cfg.margin_trace ? 1 : 0);
    SetEnvironmentVariableA("VCGLIDE_MARGIN_TRACE", n);
    SetEnvironmentVariableA("VCGLIDE_RENDERER", g_cfg.vcglide_renderer);
    SetEnvironmentVariableA("VCGLIDE_ASPECT", g_cfg.aspect);
    snprintf(n, sizeof n, "%u", g_cfg.render_scale > 0 ? g_cfg.render_scale : 1);
    SetEnvironmentVariableA("VCGLIDE_RENDER_SCALE", n);
    /* Its window is its own, so it needs the scale the host would have used.
     * Window and internal-render scale are deliberately separate: the first
     * only changes presentation size; the SDL GPU backend uses the second to
     * allocate its offscreen colour/depth targets. */
    snprintf(n, sizeof n, "%d", g_cfg.window_scale > 0 ? g_cfg.window_scale : 1);
    SetEnvironmentVariableA("VCGLIDE_SCALE", n);
}

static void vcglide_init(void)
{
    typedef void (__stdcall *init_fn)(void);
    init_fn init = (init_fn)(void *)GetProcAddress(s_glide, "vglInit@0");

    s_health = (health_fn)(void *)GetProcAddress(s_glide, "vglHealth@0");
    if (!s_health)
        LOGW("glide: this vcglide.dll does not export vglHealth@0; a lost GPU "
             "device will not be noticed this run");

    if (!init) {
        LOGW("glide: bundled vcglide.dll does not export vglInit@0; no "
             "rasteriser, capture or frame dump is available this run");
        return;
    }
    LOGI("glide: this provider is vcglide; log '%s'%s%s",
         g_cfg.vcglide_log,
         g_cfg.glide_capture[0] ? ", capturing to " : "",
         g_cfg.glide_capture[0] ? g_cfg.glide_capture : "");
    init();
}

/* Must run before game_map(): mapping the game destroys this process's PE
 * headers and LoadLibrary stops working (see win32_preload). */
int glide_preload(void)
{
    static const char dll[] = "vcglide.dll";
    char here[MAX_PATH], *slash;

    vcglide_configure();

    /* Prefer the dll sitting next to the exe, so a portable pack is
     * self-contained and does not pick up a system-wide Glide DLL. */
    if (GetModuleFileNameA(NULL, here, sizeof here)) {
        slash = strrchr(here, '\\');
        snprintf(slash ? slash + 1 : here,
                 sizeof here - (size_t)(slash ? slash + 1 - here : 0), "%s", dll);
        s_glide = LoadLibraryA(here);
        if (s_glide)
            snprintf(s_path, sizeof s_path, "%s", here);
    }
    if (!s_glide) {
        s_glide = LoadLibraryA(dll);
        if (s_glide)
            snprintf(s_path, sizeof s_path, "%s", dll);
    }
    if (!s_glide) {
        LOGW("glide: '%s' not found (err=%lu); Glide stays stubbed, the game "
             "will report no 3D board", dll, GetLastError());
        return 0;
    }
    LOGI("glide: loaded '%s' at %p", s_path, (void *)s_glide);
    vcglide_init();
    return 1;
}

/* Frame-boundary hook. Pump host messages, update I/O, and call the provider's
 * stdcall swap from the game's cdecl entry. */
/* Optional cached LFB shadow. Copy read locks in and write locks out in bulk for
 * backends that expose slow mapped memory. Disabled by default because vcglide
 * already supplies cacheable staging memory. */
#define GR_LFB_WRITE_ONLY 0x01
#define LFB_SHADOW_MAX    (4u * 1024u * 1024u)

/* Required GrLfbInfo ABI offsets. Access fields by offset to keep this boundary
 * independent of external structure declarations. */

typedef int (__stdcall *lfb_lock_fn)(int, int, int, int, int, void *);
typedef int (__stdcall *lfb_unlock_fn)(int, int);
static lfb_lock_fn   s_real_lfb_lock;
static lfb_unlock_fn s_real_lfb_unlock;

static unsigned char *s_lfb_shadow;
static void     *s_lfb_real;
static unsigned  s_lfb_stride, s_lfb_bytes;
static int       s_lfb_write, s_lfb_active, s_lfb_reported, s_lfb_logged;

static unsigned lfb_region_bytes(unsigned stride)
{
    unsigned h = (unsigned)(s_game_height > 0 ? s_game_height : 384);
    unsigned n;

    if (h > 2048u)
        h = 2048u;
    n = stride * h;
    return n > LFB_SHADOW_MAX ? LFB_SHADOW_MAX : n;
}

static int gl_lfb_lock(int type, int buffer, int write_mode, int origin,
                       int pixel_pipeline, void *info)
{
    int rc = s_real_lfb_lock(type, buffer, write_mode, origin,
                             pixel_pipeline, info);
    unsigned char *p = (unsigned char *)info;
    void *real;
    unsigned stride;
    unsigned read_offset = 0;

    s_lfb_active = 0;
    if (!rc || !info)
        return rc;

    real   = *(void **)(p + 0x04);          /* GrLfbInfo_t.lfbPtr        */
    stride = *(unsigned *)(p + 0x08);       /* GrLfbInfo_t.strideInBytes */

    /* The 2D layer (menus, HUD, the pre-race screen) is software-rendered
     * straight into this pointer, so a cut-off or displaced picture is decided
     * here, not in the 3D path. Log what the backend REALLY provides: the
     * stride gives the surface width the wrapper made, and the origin says
     * which end of the buffer the game believes row 0 is. A game composing for
     * one origin against a buffer using the other is displaced by exactly the
     * surface height, and cropped by the difference if the heights disagree. */
    if (s_lfb_logged < 6) {
        LOGI("lfb: lock #%d type=%d buffer=%d writeMode=%d origin=%s "
             "ptr=%p stride=%u bytes (= %u px at 16bpp), game raster %dx%d",
             s_lfb_logged, type, buffer, write_mode,
             origin ? "LOWER_LEFT" : "UPPER_LEFT", real, stride, stride / 2u,
             s_game_width, s_game_height);
        s_lfb_logged++;
    }

    /* The one widescreen repair that lands on this boundary rather than on a
     * primitive. It belongs to the engine and answers for itself. */
    read_offset = ws_lfb_read_offset(type, stride);

    if (!g_cfg.lfb_shadow) {
        if (read_offset)
            *(void **)(p + 0x04) = (unsigned char *)real + read_offset;
        return rc;
    }
    if (!real || !stride || stride > 65536u)
        return rc;

    if (!s_lfb_shadow) {
        s_lfb_shadow = (unsigned char *)VirtualAlloc(NULL, LFB_SHADOW_MAX,
                                                     MEM_RESERVE | MEM_COMMIT,
                                                     PAGE_READWRITE);
        if (!s_lfb_shadow) {
            LOGW("lfb: shadow allocation failed; using the wrapper pointer");
            if (read_offset)
                *(void **)(p + 0x04) = (unsigned char *)real + read_offset;
            return rc;
        }
    }

    s_lfb_real   = real;
    s_lfb_stride = stride;
    s_lfb_bytes  = lfb_region_bytes(stride);
    s_lfb_write  = ((type & 0x0F) != GR_LFB_READ_ONLY);
    s_lfb_active = 1;

    /* Only pay the read-in cost when the game actually reads the surface. */
    if ((type & 0x0F) == GR_LFB_READ_ONLY || !s_lfb_write)
        memcpy(s_lfb_shadow, real, s_lfb_bytes);

    *(void **)(p + 0x04) = s_lfb_shadow + read_offset;

    if (!s_lfb_reported) {
        s_lfb_reported = 1;
        LOGI("lfb: shadowing %u bytes (stride %u, height %d) in cached memory; "
             "bulk copy on unlock", s_lfb_bytes, stride, s_game_height);
    }
    return rc;
}

static int gl_lfb_unlock(int type, int buffer)
{
    if (s_lfb_active && s_lfb_write && s_lfb_real && s_lfb_shadow)
        memcpy(s_lfb_real, s_lfb_shadow, s_lfb_bytes);
    s_lfb_active = 0;
    return s_real_lfb_unlock(type, buffer);
}
typedef void (__stdcall *swap_fn)(int);
static swap_fn s_real_swap;

/* Report per-entry-point calls per frame from the binding thunks. */
static uint32_t s_glide_calls[VCT_MAX_GLIDE];
static uint32_t s_glide_calls_prev[VCT_MAX_GLIDE];

/* Per-entry-point CYCLES, when [diagnostics] glide_timing is on.
 *
 * Counts said the mix was dominated by state calls; dropping those changed
 * nothing, while dropping submission removed 1.4-1.5x of frame time and the
 * entire stall population. So the expensive calls are not the frequent ones,
 * and only cycles can say which they are. */
static void report_glide_cold(void);   /* defined with the first-call capture */

static struct glide_timer s_glide_timer[VCT_MAX_GLIDE];
static uint64_t s_glide_cycles_prev[VCT_MAX_GLIDE];
/* `cycles` is summed across the whole run; the frame-time accounting below reads
 * it directly, so keep a plain accessor rather than reaching into the struct in
 * four places. */
#define s_glide_cycles(i) (s_glide_timer[i].cycles)

static void report_glide_cycles(uint64_t frames)
{
    struct { unsigned idx; uint64_t d; } top[10];
    unsigned n = 0, i, j, k;
    uint64_t total = 0;

    if (!frames || !g_cfg.glide_timing)
        return;
    for (i = 0; i < (unsigned)G->glide_count; i++) {
        uint64_t d = s_glide_cycles(i) - s_glide_cycles_prev[i];
        s_glide_cycles_prev[i] = s_glide_cycles(i);
        total += d;
        if (!d)
            continue;
        for (j = 0; j < n; j++)
            if (d > top[j].d)
                break;
        if (j >= 10u)
            continue;
        if (n < 10u)
            n++;
        for (k = n - 1; k > j; k--)
            top[k] = top[k - 1];
        top[j].idx = i;
        top[j].d = d;
    }
    if (!total)
        return;
    /* ms assumes the render thread's own cycle rate, already reported above as
     * ~4.48 G/s wall; the SHARE is exact regardless. */
    LOGI("glide cycles: %.2f Mcycles/frame total inside the wrapper",
         (double)total / (double)frames / 1.0e6);
    for (i = 0; i < n; i++) {
        struct glide_timer *gt = &s_glide_timer[top[i].idx];
        uint32_t calls = s_glide_calls[top[i].idx] -
                         s_glide_calls_prev[top[i].idx];
        /* min and the fast fraction alongside the mean, for the reason on
         * struct glide_timer: 20,274 cyc/call flat and 20,274 cyc/call as a mean
         * over a periodic spike are different defects with different fixes, and
         * a sum cannot tell them apart. Cumulative over the run, not per
         * interval: a minimum is only meaningful over the largest population
         * available. */
        LOGI("    %5.1f%%  %8.3f Mcyc/frame  %9.0f cyc/call  min %7u  "
             "%5.1f%% under %u  %s",
             100.0 * (double)top[i].d / (double)total,
             (double)top[i].d / (double)frames / 1.0e6,
             calls ? (double)top[i].d / (double)calls : 0.0,
             gt->min == 0xFFFFFFFFu ? 0u : gt->min,
             s_glide_calls[top[i].idx] ?
                 100.0 * (double)gt->fast / (double)s_glide_calls[top[i].idx]
                 : 0.0,
             GLIDE_FAST_CYCLES, G->glide[top[i].idx].name);
    }

    /* The full shape, for the one entry point that carries the frame. */
    for (i = 0; i < n; i++) {
        struct glide_timer *gt = &s_glide_timer[top[i].idx];
        char line[240];
        unsigned b, len = 0;
        uint64_t seen = 0;

        if (strcmp(G->glide[top[i].idx].name, "grDrawTriangle") != 0)
            continue;
        for (b = 0; b < 32u; b++)
            seen += gt->hist[b];
        if (!seen)
            break;
        for (b = 6; b < 24u && len + 24 < sizeof line; b++) {
            if (!gt->hist[b])
                continue;
            len += (unsigned)snprintf(line + len, sizeof line - len,
                                      " 2^%u:%.1f%%", b,
                                      100.0 * (double)gt->hist[b] /
                                          (double)seen);
        }
        LOGI("    grDrawTriangle cost histogram over %llu calls (log2 cycles):%s",
             (unsigned long long)seen, line);
        break;
    }
}

/* Report every used Glide entry point so low-frequency persistent state changes
 * remain visible. */
static void report_glide_mix(uint64_t frames)
{
    struct { unsigned idx; uint32_t delta; } top[64];
    unsigned n = 0, i, j;
    uint32_t total = 0;

    if (!frames)
        return;
    for (i = 0; i < (unsigned)G->glide_count; i++) {
        uint32_t d = s_glide_calls[i] - s_glide_calls_prev[i];
        s_glide_calls_prev[i] = s_glide_calls[i];
        total += d;
        if (!d)
            continue;
        for (j = 0; j < n; j++)
            if (d > top[j].delta)
                break;
        if (j >= 64u)
            continue;
        if (n < 64u)
            n++;
        for (unsigned k = n - 1; k > j; k--)
            top[k] = top[k - 1];
        top[j].idx = i;
        top[j].delta = d;
    }
    LOGI("glide mix: %.0f calls/frame total", (double)total / (double)frames);
    for (i = 0; i < n; i++)
        LOGI("    %8.1f/frame  %s", (double)top[i].delta / (double)frames,
             G->glide[top[i].idx].name);
}

static LARGE_INTEGER s_perf_freq, s_perf_start;
static uint64_t s_presented_frames, s_swap_ticks;
static unsigned s_perf_interval_seconds = 5u;
static ULONG64 s_thread_cycles_start;
static FILETIME s_process_kernel_start, s_process_user_start;
static int s_runtime_reported;

static uint64_t filetime_ticks(FILETIME value)
{
    return ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
}

/* Report page-fault rate and resident-set changes. Resolve the process-memory
 * query dynamically and omit the report when unavailable. */
struct vct_pmc {
    DWORD  cb;
    DWORD  PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage;
};
typedef BOOL (WINAPI *pmi_fn)(HANDLE, struct vct_pmc *, DWORD);

static void report_faults(double wall_seconds)
{
    static pmi_fn pmi;
    static int    resolved;
    static DWORD  prev_faults;
    struct vct_pmc pmc;

    if (!resolved) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        pmi = k32 ? (pmi_fn)(void *)GetProcAddress(k32,
                                     "K32GetProcessMemoryInfo") : NULL;
        resolved = 1;
    }
    if (!pmi)
        return;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (!pmi(GetCurrentProcess(), &pmc, sizeof pmc))
        return;
    if (prev_faults && wall_seconds > 0.0)
        LOGI("runtime: %lu page faults over the interval (%.0f/s, %.1f per "
             "frame at 22 fps); working set %.1f MB",
             (unsigned long)(pmc.PageFaultCount - prev_faults),
             (double)(pmc.PageFaultCount - prev_faults) / wall_seconds,
             (double)(pmc.PageFaultCount - prev_faults) / (wall_seconds * 22.0),
             (double)pmc.WorkingSetSize / (1024.0 * 1024.0));
    prev_faults = pmc.PageFaultCount;
}

static void report_runtime_once(void)
{
    DWORD_PTR process_affinity = 0, system_affinity = 0;
    BOOL in_job = FALSE;
    unsigned short x87_control;
    unsigned mxcsr;

    if (s_runtime_reported)
        return;
    GetProcessAffinityMask(GetCurrentProcess(), &process_affinity, &system_affinity);
    IsProcessInJob(GetCurrentProcess(), NULL, &in_job);
    __asm__ volatile ("fnstcw %0" : "=m" (x87_control));
    __asm__ volatile ("stmxcsr %0" : "=m" (mxcsr));
    LOGI("runtime: priority class 0x%lX, render thread priority %d, "
         "affinity process=0x%llX system=0x%llX, in_job=%d",
         GetPriorityClass(GetCurrentProcess()),
         GetThreadPriority(GetCurrentThread()),
         (unsigned long long)process_affinity,
         (unsigned long long)system_affinity, in_job ? 1 : 0);
    LOGI("runtime: x87 control=0x%04X, MXCSR=0x%08X", x87_control, mxcsr);
    s_runtime_reported = 1;
}

static void report_geometry_once(void)
{
    HWND hwnd;
    RECT client = { 0, 0, 0, 0 };

    if (s_geometry_reported)
        return;
    hwnd = window_game();
    if (hwnd)
        GetClientRect(hwnd, &client);
    LOGI("geometry: surface %dx%d, raster %dx%d, aspect %s, %s",
         s_game_width, s_game_height, s_view_width, s_view_height, g_cfg.aspect,
         window_is_fullscreen() ? "borderless fullscreen" : "windowed");
    LOGI("geometry: game mode %dx%d, init3dfx %dx%d, viewport %dx%d, client %ldx%ld",
         s_view_width, s_view_height,
         rd_opt(G->init3dfx_hpixels, -1),
         rd_opt(G->init3dfx_vpixels, -1),
         rd_opt(G->viewport_hres, -1),
         rd_opt(G->viewport_vres, -1),
         (long)(client.right - client.left), (long)(client.bottom - client.top));
    s_geometry_reported = 1;
}

static void report_frame_periods(void);

static void report_frame_timing(uint64_t swap_ticks, int interval)
{
    LARGE_INTEGER now;
    uint64_t elapsed;
    const float *frame = (const float *)(uintptr_t)G->gameloop_frame_seconds;
    const float *draw = (const float *)(uintptr_t)G->gameloop_draw_seconds;
    const float *work = (const float *)(uintptr_t)G->gameloop_work_seconds;
    const float *net = (const float *)(uintptr_t)G->gameloop_net_seconds;
    const float *win = (const float *)(uintptr_t)G->gameloop_win_seconds;
    double frame_sum = 0.0, draw_sum = 0.0, work_sum = 0.0;
    double net_sum = 0.0, win_sum = 0.0;
    unsigned samples = 0, i;

    if (!s_perf_freq.QuadPart) {
        FILETIME creation, exit_time;
        QueryPerformanceFrequency(&s_perf_freq);
        QueryPerformanceCounter(&s_perf_start);
        QueryThreadCycleTime(GetCurrentThread(), &s_thread_cycles_start);
        GetProcessTimes(GetCurrentProcess(), &creation, &exit_time,
                        &s_process_kernel_start, &s_process_user_start);
    }
    QueryPerformanceCounter(&now);
    s_presented_frames++;
    profile_attract_poll();
    s_swap_ticks += swap_ticks;
    elapsed = (uint64_t)(now.QuadPart - s_perf_start.QuadPart);
    if (!s_perf_freq.QuadPart ||
        elapsed < (uint64_t)s_perf_freq.QuadPart * s_perf_interval_seconds)
        return;

    LOGI("pacing: %.2f presented fps over %.1f s; swap wait %.3f ms/frame; "
         "game target %.2f fps (%.2f fields, %.3f ms), last interval %d",
         (double)s_presented_frames * (double)s_perf_freq.QuadPart / (double)elapsed,
         (double)elapsed / (double)s_perf_freq.QuadPart,
         s_presented_frames ?
             1000.0 * (double)s_swap_ticks /
             ((double)s_perf_freq.QuadPart * (double)s_presented_frames) : 0.0,
         rdf_opt(G->gameloop_target_fps, 0.0),
         rdf_opt(G->gameloop_target_fields, 0.0),
         1000.0 * rdf_opt(G->gameloop_target_frame_time, 0.0),
         interval);

    /* The healer's premise, re-stated with evidence rather than asserted once
     * at startup. A thread that handed the game off and parked shows a crossing
     * count that stopped moving between two of these reports; a thread that is
     * genuinely running game code alongside another does not. This is the only
     * instrument that can tell those apart, and diego_frame_boundary()'s choice
     * of seam depends on which it is. */
    game_thread_report();
    game_irq_report();

    /* Report available game-loop phase samples beside wall-clock FPS. */
    if (frame)
        for (i = 0; i < 50u; i++) {
            if (frame[i] <= 0.0f || frame[i] >= 10.0f)
                continue;
            frame_sum += frame[i];
            if (draw) draw_sum += draw[i];
            if (work) work_sum += work[i];
            if (net)  net_sum  += net[i];
            if (win)  win_sum  += win[i];
            samples++;
        }
    if (samples) {
        LOGI("pacing: game phases over %u frames: total %.3f ms, draw %.3f, "
             "work %.3f, network/update %.3f%s, platform %.3f",
             samples,
             1000.0 * frame_sum / samples,
             1000.0 * draw_sum / samples,
             1000.0 * work_sum / samples,
             1000.0 * net_sum / samples, net ? "" : " (absent)",
             1000.0 * win_sum / samples);
    }
    {
        FILETIME creation, exit_time, kernel_now, user_now;
        ULONG64 cycles_now = 0;
        double wall_seconds = (double)elapsed / (double)s_perf_freq.QuadPart;
        double cpu_seconds = 0.0;

        QueryThreadCycleTime(GetCurrentThread(), &cycles_now);
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time,
                            &kernel_now, &user_now)) {
            uint64_t cpu_ticks =
                filetime_ticks(kernel_now) - filetime_ticks(s_process_kernel_start) +
                filetime_ticks(user_now) - filetime_ticks(s_process_user_start);
            cpu_seconds = (double)cpu_ticks / 10000000.0;
            s_process_kernel_start = kernel_now;
            s_process_user_start = user_now;
        }
        LOGI("runtime: render %.3f billion cycles/s wall; process CPU %.1f%% "
             "of one core over interval",
             wall_seconds > 0.0 ?
                 (double)(cycles_now - s_thread_cycles_start) /
                     (wall_seconds * 1000000000.0) : 0.0,
             wall_seconds > 0.0 ? 100.0 * cpu_seconds / wall_seconds : 0.0);
        s_thread_cycles_start = cycles_now;
        report_faults(wall_seconds);
    }
    report_frame_periods();
    report_glide_cycles(s_presented_frames);
    texstate_report_cycles(s_presented_frames);
    win32_report_calls(s_presented_frames);
    win32_report_nocache();
    report_glide_mix(s_presented_frames);
    report_glide_cold();
    profile_report_geometry(s_presented_frames);
    profile_report_call_times(s_presented_frames);
    profile_report();
    r2_cache_report();
    net_link_report(s_presented_frames);
    s_perf_start = now;
    s_presented_frames = 0;
    s_swap_ticks = 0;
    s_perf_interval_seconds = 30u;
}

/* Pair wall-clock periods with same-frame game telemetry. The game's limiter
 * stores its target after waiting, so limiter-engaged totals indicate pacing
 * state rather than measured frame cost. */
#define PERIOD_BUCKETS 10
static const double PERIOD_EDGE[PERIOD_BUCKETS] = {
    20.0, 25.0, 30.0, 33.9, 40.0, 50.0, 60.0, 80.0, 120.0, 1e9
};
static uint32_t s_period_hist[PERIOD_BUCKETS];
static uint32_t s_period_parity_hist[2][PERIOD_BUCKETS];
static uint64_t s_period_parity_n[2], s_period_parity_slow[2];
static double   s_period_parity_ms[2];
static uint64_t s_last_swap_qpc;
static uint64_t s_limited_n, s_overran_n, s_paired_n;

/* Partition normal and stalled frame populations. Compare wall time with the
 * game's phase totals to separate in-loop work from provider, presentation,
 * driver, and scheduler time. */
/* Partition provider-cycle deltas by the same normal and stalled frame classes. */
static uint64_t s_glide_at_frame;
static uint64_t s_stall_wrap, s_normal_wrap;

static uint64_t s_stall_n, s_normal_n;
static double   s_stall_wall, s_stall_claim, s_stall_draw, s_stall_work;
static double   s_normal_wall, s_normal_claim, s_normal_draw, s_normal_work;
static double   s_limited_wall_ms, s_overran_wall_ms, s_overran_claim_ms;
static double   s_wall_sum_ms, s_claim_sum_ms;

static void record_frame_period(uint64_t now)
{
    double ms, claimed_ms, target_ms;
    const float *hist, *draw_hist, *work_hist;
    int idx, prev, i, parity;

    if (s_last_swap_qpc && s_perf_freq.QuadPart) {
        ms = 1000.0 * (double)(now - s_last_swap_qpc) /
             (double)s_perf_freq.QuadPart;
        parity = (*(const int *)(uintptr_t)G->gameloop_odd_frame != 0);
        for (i = 0; i < PERIOD_BUCKETS; i++)
            if (ms < PERIOD_EDGE[i]) {
                s_period_hist[i]++;
                s_period_parity_hist[parity][i]++;
                break;
            }
        s_period_parity_n[parity]++;
        s_period_parity_ms[parity] += ms;
        if (ms >= 50.0)
            s_period_parity_slow[parity]++;

        /* The game writes index i then advances it, so the entry it just
         * completed is the one before the current index. */
        hist = (const float *)(uintptr_t)G->gameloop_frame_seconds;
        draw_hist = (const float *)(uintptr_t)G->gameloop_draw_seconds;
        work_hist = (const float *)(uintptr_t)G->gameloop_work_seconds;
        idx = *(const int *)(uintptr_t)G->gameloop_history_index;
        target_ms = 1000.0 *
            (double)*(const float *)(uintptr_t)G->gameloop_target_frame_time;
        if (idx >= 0 && idx < 50 && target_ms > 0.0) {
            prev = (idx + 49) % 50;
            claimed_ms = 1000.0 * (double)hist[prev];
            /* Exactly the target means the clamp ran; nothing else lands there. */
            /* Sums, not per-frame pairs, are the honest comparison: the swap-to-
             * swap window contains frame n's limiter wait and frame n+1's work,
             * so any single pairing is off by one phase. Over hundreds of frames
             * that offset washes out and the ratio is a clean timebase test:
             * game seconds against real seconds. */
            s_paired_n++;
            s_wall_sum_ms += ms;
            s_claim_sum_ms += claimed_ms;
            if (claimed_ms > target_ms - 0.001 && claimed_ms < target_ms + 0.001) {
                s_limited_n++;
                s_limited_wall_ms += ms;
            } else {
                s_overran_n++;
                s_overran_wall_ms += ms;
                s_overran_claim_ms += claimed_ms;
            }
            {
                double d_ms = 1000.0 * (double)draw_hist[prev];
                double w_ms = 1000.0 * (double)work_hist[prev];
                uint64_t wrap_now = 0, wrap_delta;
                unsigned gi;

                for (gi = 0; gi < (unsigned)G->glide_count; gi++)
                    wrap_now += s_glide_cycles(gi);
                wrap_delta = wrap_now - s_glide_at_frame;
                s_glide_at_frame = wrap_now;

                if (ms >= (double)g_cfg.stall_ms) {
                    s_stall_wrap += wrap_delta;
                    s_stall_n++;
                    s_stall_wall  += ms;
                    s_stall_claim += claimed_ms;
                    s_stall_draw  += d_ms;
                    s_stall_work  += w_ms;
                } else {
                    s_normal_wrap += wrap_delta;
                    s_normal_n++;
                    s_normal_wall  += ms;
                    s_normal_claim += claimed_ms;
                    s_normal_draw  += d_ms;
                    s_normal_work  += w_ms;
                }
            }
        }
    }
    s_last_swap_qpc = now;
}

static void report_stalls(void)
{
    double s_wall, s_claim, s_unacc, n_wall, n_claim, n_unacc;
    uint64_t total = s_stall_n + s_normal_n;

    if (!total || !s_normal_n)
        return;

    n_wall  = s_normal_wall  / (double)s_normal_n;
    n_claim = s_normal_claim / (double)s_normal_n;
    n_unacc = n_wall - n_claim;

    if (!s_stall_n) {
        LOGI("pacing: no frames over %u ms in %llu; nothing stalling",
             g_cfg.stall_ms, (unsigned long long)total);
    } else {
        s_wall  = s_stall_wall  / (double)s_stall_n;
        s_claim = s_stall_claim / (double)s_stall_n;
        s_unacc = s_wall - s_claim;

        LOGI("pacing: STALLS >= %u ms: %llu of %llu frames (%.1f%%), and they "
             "cost %.0f%% of all wall time",
             g_cfg.stall_ms, (unsigned long long)s_stall_n,
             (unsigned long long)total,
             100.0 * (double)s_stall_n / (double)total,
             100.0 * s_stall_wall / (s_stall_wall + s_normal_wall));
        LOGI("    stalled frames: wall %7.2f ms | game claims %7.2f "
             "(draw %6.2f, work %6.2f) | UNACCOUNTED %7.2f ms",
             s_wall, s_claim, s_stall_draw / (double)s_stall_n,
             s_stall_work / (double)s_stall_n, s_unacc);
        LOGI("    normal frames:  wall %7.2f ms | game claims %7.2f "
             "(draw %6.2f, work %6.2f) | UNACCOUNTED %7.2f ms",
             n_wall, n_claim, s_normal_draw / (double)s_normal_n,
             s_normal_work / (double)s_normal_n, n_unacc);
        if (g_cfg.glide_timing) {
            double sw = s_stall_wrap  / (double)s_stall_n  / 1.0e6;
            double nw = s_normal_wrap / (double)s_normal_n / 1.0e6;
            LOGI("    wrapper cycles: %8.3f Mcyc/stalled frame vs %8.3f "
                 "Mcyc/normal frame (%.1fx): %.0f%% of the stall's excess",
                 sw, nw, nw > 0.0 ? sw / nw : 0.0,
                 (s_wall - n_wall) > 0.0 ?
                     100.0 * ((sw - nw) * 1.0e6 / 4.475e9 * 1000.0) /
                         (s_wall - n_wall) : 0.0);
        }

        /* The whole point of the instrument, stated rather than left to the
         * reader: the excess has to be either inside the game's own timed loop
         * or outside it, and those are bugs in different codebases. */
        {
            double d_claim = s_claim - n_claim;
            double d_unacc = s_unacc - n_unacc;

            LOGI("    => excess %.2f ms/stall: %.2f ms INSIDE the game loop "
                 "(draw %+.2f, work %+.2f), %.2f ms OUTSIDE it (wrapper, "
                 "present, driver or OS): %s",
                 s_wall - n_wall, d_claim,
                 s_stall_draw / (double)s_stall_n -
                     s_normal_draw / (double)s_normal_n,
                 s_stall_work / (double)s_stall_n -
                     s_normal_work / (double)s_normal_n,
                 d_unacc,
                 d_unacc > d_claim ? "OUTSIDE dominates: not the game's code"
                                   : "INSIDE dominates: the game's own phases");
        }
    }
    s_stall_wrap = s_normal_wrap = 0;
    s_stall_n = s_normal_n = 0;
    s_stall_wall = s_stall_claim = s_stall_draw = s_stall_work = 0.0;
    s_normal_wall = s_normal_claim = s_normal_draw = s_normal_work = 0.0;
}

static void report_frame_periods(void)
{
    static const char *const EDGE_NAME[PERIOD_BUCKETS] = {
        "   <20", " 20-25", " 25-30", " 30-34", " 34-40",
        " 40-50", " 50-60", " 60-80", "80-120", "  >120"
    };
    uint64_t total = 0;
    int i;

    for (i = 0; i < PERIOD_BUCKETS; i++)
        total += s_period_hist[i];
    if (!total)
        return;

    report_stalls();

    LOGI("pacing: wall frame period histogram over %llu frames (ms)",
         (unsigned long long)total);
    for (i = 0; i < PERIOD_BUCKETS; i++)
        if (s_period_hist[i])
            LOGI("    %s ms  %5.1f%%  (%u)", EDGE_NAME[i],
                 100.0 * (double)s_period_hist[i] / (double)total,
                 s_period_hist[i]);
    for (i = 0; i < 2; i++)
        if (s_period_parity_n[i])
            LOGI("pacing: %s frames: %llu, mean %.2f ms, >=50 ms %.1f%% "
                 "(flag sampled at the completing swap)",
                 i ? "odd" : "even",
                 (unsigned long long)s_period_parity_n[i],
                 s_period_parity_ms[i] / (double)s_period_parity_n[i],
                 100.0 * (double)s_period_parity_slow[i] /
                     (double)s_period_parity_n[i]);
    if (s_paired_n)
        LOGI("pacing: over %llu frames the game's own clock totals %.0f ms "
             "against %.0f ms of wall time (ratio %.3f) - game seconds vs real "
             "seconds, offset-robust because it is summed, not paired",
             (unsigned long long)s_paired_n, s_claim_sum_ms, s_wall_sum_ms,
             s_wall_sum_ms > 0.0 ? s_claim_sum_ms / s_wall_sum_ms : 0.0);
    if (s_limited_n)
        LOGI("pacing: limiter ENGAGED on %llu frames, mean wall %.2f ms "
             "(one-frame phase offset: this window holds frame n's wait and "
             "frame n+1's work, so read the histogram, not this mean)",
             (unsigned long long)s_limited_n,
             s_limited_wall_ms / (double)s_limited_n);
    if (s_overran_n)
        LOGI("pacing: limiter MISSED on %llu frames - game claims %.2f ms, "
             "mean wall %.2f ms",
             (unsigned long long)s_overran_n,
             s_overran_claim_ms / (double)s_overran_n,
             s_overran_wall_ms / (double)s_overran_n);

    memset(s_period_hist, 0, sizeof s_period_hist);
    memset(s_period_parity_hist, 0, sizeof s_period_parity_hist);
    memset(s_period_parity_n, 0, sizeof s_period_parity_n);
    memset(s_period_parity_slow, 0, sizeof s_period_parity_slow);
    memset(s_period_parity_ms, 0, sizeof s_period_parity_ms);
    s_limited_n = s_overran_n = s_paired_n = 0;
    s_limited_wall_ms = s_overran_wall_ms = s_overran_claim_ms = 0.0;
    s_wall_sum_ms = s_claim_sum_ms = 0.0;
}

typedef void (__stdcall *drawtri_fn)(const void *, const void *, const void *);
typedef void (__stdcall *fogmode_fn)(uint32_t);

/* Diagnostic fog bisection. Force fog off to test its contribution to provider
 * draw cost; the resulting picture is intentionally incorrect. */
static fogmode_fn s_real_fog_mode;

/* Capture first-call arguments for persistent Glide modes. Print each value as
 * integer, hexadecimal, and float without assuming its semantic type. */
static uint32_t s_glide_seen[VCT_MAX_GLIDE];
static uint8_t  s_glide_claimed[VCT_MAX_GLIDE];

static void __cdecl glide_first_call(uint32_t idx, const uint32_t *args)
{
    char line[256];
    unsigned n, k, len = 0;

    if (idx >= (uint32_t)G->glide_count)
        return;
    n = (unsigned)G->glide[idx].argbytes / 4u;
    for (k = 0; k < n && len + 40 < sizeof line; k++) {
        float f;
        memcpy(&f, &args[k], sizeof f);
        /* Print the float form only when it is one a game would plausibly pass;
         * otherwise it is noise beside the hex. */
        if (f > -1.0e6f && f < 1.0e6f && (f > 1.0e-6f || f < -1.0e-6f))
            len += (unsigned)snprintf(line + len, sizeof line - len,
                                      " a%u=0x%08X(%d, %.4f)", k, args[k],
                                      (int)args[k], (double)f);
        else
            len += (unsigned)snprintf(line + len, sizeof line - len,
                                      " a%u=0x%08X(%d)", k, args[k],
                                      (int)args[k]);
    }
    LOGI("glide first call: %-32s%s", G->glide[idx].name, n ? line : " (void)");
}

/* The other half of the differential, and the half a rate cannot express: which
 * entry points this title NEVER called.
 *
 * NOT one-shot, and that was a real bug for one run: the first report fires at
 * 5 s, before the game has drawn a triangle, so a single emission listed 60 of
 * 69 as cold including grDrawTriangle. Re-emitted whenever the set shrinks, so
 * it converges visibly instead of describing the boot sequence. */
static void report_glide_cold(void)
{
    static unsigned last_cold = (unsigned)-1;
    char line[1024];
    unsigned i, len = 0, cold = 0;

    if (!g_cfg.glide_first_call)
        return;
    for (i = 0; i < (unsigned)G->glide_count; i++) {
        if (s_glide_calls[i] || s_glide_claimed[i])
            continue;
        cold++;
        if (len + 32 < sizeof line)
            len += (unsigned)snprintf(line + len, sizeof line - len, " %s",
                                      G->glide[i].name);
    }
    if (cold == last_cold)
        return;
    last_cold = cold;
    {
        unsigned c, claimed = 0;
        for (c = 0; c < (unsigned)G->glide_count; c++)
            claimed += s_glide_claimed[c] ? 1u : 0u;
        LOGI("glide: %u/%u entry points NEVER called so far (%u more are "
             "claimed by texstate and cannot be counted here):%s",
             cold, G->glide_count, claimed, cold ? line : " (none)");
    }
}

static void __cdecl gl_fog_mode(uint32_t mode)
{
    static int announced;

    if (!announced) {
        announced = 1;
        LOGW("null_fog=true: grFogMode(%u) forced to GR_FOG_DISABLE; "
             "diagnostic output will be incorrect.",
             (unsigned)mode);
    }
    if (s_real_fog_mode)
        s_real_fog_mode(0);
}

static int s_timers_claimed;

/* Call direct and depth-controlled canaries at triangle cadence. This isolates
 * sparse instrumentation and call-depth overhead from game-code cost. */
static drawtri_fn s_real_drawtri;

static void __cdecl gl_draw_triangle_probe(const void *a, const void *b,
                                           const void *c)
{
    profile_interleave_tick();
    if (s_real_drawtri)
        s_real_drawtri(a, b, c);
}

static void gl_buffer_swap(int interval)
{
    MSG msg;
    LARGE_INTEGER before = { 0 }, after = { 0 };

    ws_frame_boundary();
    /* Both titles fill their checksum cells during module init, well before
     * the first swap; this reads them until they are populated and then stops
     * asking. Diagnostic, and it writes nothing the game can see. */
    cksum_frame();
    /* The link's receive budget is per frame, and this is the frame. Both
     * titles pump the link from the game thread inside the frame, so the
     * counter is only ever touched from that thread. */
    net_link_frame();
    game_thread_seen("grBufferSwap");
    /* The game cannot be inside one of its own `cli` regions and inside
     * grBufferSwap at the same time, so this is the point at which a region
     * that never reached its `sti` can be written off. It runs on the game's
     * own thread (the only thread that raises the depth), so the reset
     * cannot race the raise. Without it, one unbalanced region would silence
     * the audio tick for the rest of the run. */
    game_irq_resync("grBufferSwap");
    /* The cabinet's I/O board, run from the thread that runs the game. The
     * decoder and the control-driver callback are the GAME'S OWN code, and
     * running them here rather than on a thread of ours is what makes the
     * cli/sti main.c emulates mean anything for them: a critical section
     * cannot exclude a thread that is already inside the same code. See
     * diego_frame_boundary(). */
    diego_frame_boundary();
    if (s_real_swap) {
        QueryPerformanceCounter(&before);
        record_frame_period((uint64_t)before.QuadPart);
        /* The heartbeat, and on the first swap the identification of the
         * thread that produced it. */
        wd_note_swap((uint64_t)before.QuadPart);
        /* And the call timers want the same identification, for the same
         * reason: they are installed on the loader thread, but the thread whose
         * frames they describe is this one. On Hydro these are the same thread
         * and this changes nothing; on Offroad they are not, and without this
         * time_calls hangs it. */
        if (g_cfg.time_calls && !s_timers_claimed) {
            s_timers_claimed = 1;
            patch_timer_set_owner((uint32_t)GetCurrentThreadId());
            LOGI("time: call timers claimed by render thread %lu; calls from "
                 "any other thread will be declined, not measured",
                 GetCurrentThreadId());
        }
        s_real_swap(interval);
        QueryPerformanceCounter(&after);
        /* The provider's own verdict on the frame it just presented. A hung
         * GPU device does not stop the frames -- it stops the pictures, which
         * is why the watchdog cannot see it and why this is asked here rather
         * than watched for. Measured: 90 minutes of a frozen picture at a
         * reported 27.8 fps. */
        if (s_health && s_health() != 0)
            shim_exit(3, "the GPU device was lost and cannot be recovered");
    }

    /* Poll rather than waiting for WM_KEYDOWN, which does not reach this
     * game/render thread's queue under the current wrapper. */
    if (escape_down())
        shim_exit(0, "Escape pressed");
    window_hotkeys();

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (close_requested(&msg))
            shim_exit(0, "host window closed");
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (window_gone())
        shim_exit(0, "host window destroyed");

    if (s_resize_watch > 0) {
        window_sync(0);
        s_resize_watch--;
    }
    /* One dump, once the scene being complained about is actually on screen:
     * an attract-mode menu draws almost nothing, so reporting at frame 1 would
     * describe the wrong thing entirely. */
    if (g_cfg.trace_draw_state) {
        static DWORD trace_start;
        texstate_frame_end();
        if (!trace_start)
            trace_start = GetTickCount();
        if (GetTickCount() - trace_start >= g_cfg.trace_draw_state * 1000u)
            texstate_report();
    }

    /* Same shape, own window: trace_lod's value is SECONDS, so the census
     * describes a drawn race rather than the attract loading screen. Counting
     * frames here rather than reusing s_presented_frames keeps the per-frame
     * rates over the whole window instead of the last 30 s report interval. */
    if (g_cfg.trace_lod) {
        static DWORD lod_start;
        static uint64_t lod_frames;
        lod_frames++;
        if (!lod_start)
            lod_start = GetTickCount();
        if (GetTickCount() - lod_start >= g_cfg.trace_lod * 1000u)
            texstate_report_lod(lod_frames);
    }

    report_geometry_once();
    report_runtime_once();
    report_frame_timing((uint64_t)(after.QuadPart - before.QuadPart), interval);
}

/* Diagnostic-only bisection that suppresses primitive submission while leaving
 * game transforms, lighting, clipping, and state changes intact. */
/* Level 2 keeps only the entry points whose RETURN VALUE or side effect the
 * game consumes: initialisation, the state blob, the LFB pointer it writes
 * pixels into, and the texture-memory arithmetic it allocates against. Nulling
 * anything else cannot change control flow, so what remains is the game's own
 * per-frame cost with the backend removed entirely: the floor. */
static int glide_must_be_real(const char *name)
{
    static const char *const KEEP[] = {
        "grGlideInit", "grGlideShutdown", "grSstQueryHardware", "grSstSelect",
        "grSstVidMode", "grSstWinOpen", "grSstWinClose", "grBufferSwap",
        "grLfbLock", "grLfbUnlock", "grGlideGetState", "grGlideSetState",
        "grTexMinAddress", "grTexMaxAddress", "grTexTextureMemRequired",
        "grSstIdle", "grErrorSetCallback",
    };
    unsigned i;
    for (i = 0; i < sizeof KEEP / sizeof KEEP[0]; i++)
        if (strcmp(name, KEEP[i]) == 0)
            return 1;
    return 0;
}

static int is_primitive_submit(const char *name)
{
    static const char *const DRAW[] = {
        "grDrawTriangle", "grAADrawTriangle", "grDrawLine", "grAADrawLine",
        "grDrawPlanarPolygonVertexList", "grDrawPolygonVertexList",
        "grAADrawPolygonVertexList", "grDrawPoint", "grAADrawPoint",
    };
    unsigned i;
    for (i = 0; i < sizeof DRAW / sizeof DRAW[0]; i++)
        if (strcmp(name, DRAW[i]) == 0)
            return 1;
    return 0;
}

int glide_bind(void)
{
    int bound = 0, missing = 0;
    unsigned i;

    if (!s_glide)
        return 0;

    /* Validate profiler attribution by replacing the target with a cdecl zero
     * return. This intentionally destroys rendering and is diagnostic-only. */
    if (g_cfg.null_draw >= 3) {
        patch_ret_imm(G->mesh3d_drawgroup, 0);
        LOGW("null_draw=3: _mesh3d_DrawGroup_NoFrustumTest @0x%08X stubbed; "
             "geometry will be MISSING. Compare frame time against the same "
             "scene with null_draw=0; the profile predicts ~30%% cheaper.",
             G->mesh3d_drawgroup);
    }

    /* Before the loop: the mode claims primitive entry points inside it. */
    ws_arm();

    for (i = 0; i < G->glide_count; i++) {
        char decorated[128];
        void *fn, *thunk;

        /* A Glide 2.x DLL exports stdcall-decorated names: _grDrawTriangle@12 */
        snprintf(decorated, sizeof decorated, "_%s@%d",
                 G->glide[i].name, G->glide[i].argbytes);

        fn = (void *)GetProcAddress(s_glide, decorated);
        if (!fn) {
            LOGW("glide: '%s' not exported; left stubbed", decorated);
            missing++;
            continue;
        }
        if (g_cfg.null_draw >= 2 && !glide_must_be_real(G->glide[i].name)) {
            patch_ret_imm(G->glide[i].va, 0);
            continue;
        }

        if (g_cfg.null_draw == 1 && is_primitive_submit(G->glide[i].name)) {
            patch_ret_imm(G->glide[i].va, 0);
            LOGW("glide: NULL-DRAW diagnostic - %s dropped, not submitted",
                 G->glide[i].name);
            continue;
        }

        /* The widescreen origin comes first among the optional claims: it has
         * to reach EVERY primitive or not be applied at all, which is why
         * widescreen_arm() already refused the mode if a diagnostic wanted one
         * of these entry points. */
        if (ws_claim_primitive(G->glide[i].name, fn, G->glide[i].va)) {
            LOGT("glide: 0x%08X %-32s -> %s @%p (+ widescreen origin)",
                 G->glide[i].va, G->glide[i].name, decorated, fn);
            bound++;
            continue;
        }

        /* grDrawTriangle is the only entry point called at the game's own inner
         * cadence, which is what the interleaved control needs. It takes the
         * entry away from the counting/timing thunk for the duration of a
         * time_calls run; that is stated rather than silent, because a missing
         * grDrawTriangle count in a diagnostic log is otherwise a puzzle. */
        if (g_cfg.null_fog && strcmp(G->glide[i].name, "grFogMode") == 0) {
            s_real_fog_mode = (fogmode_fn)fn;
            patch_jmp(G->glide[i].va, (void *)gl_fog_mode);
            bound++;
            continue;
        }

        if (g_cfg.time_calls && strcmp(G->glide[i].name, "grDrawTriangle") == 0) {
            s_real_drawtri = (drawtri_fn)fn;
            patch_jmp(G->glide[i].va, (void *)gl_draw_triangle_probe);
            LOGW("glide: grDrawTriangle carries the interleaved sqrt control "
                 "(time_calls=true): its per-call count and cycles are NOT "
                 "collected in this run");
            bound++;
            continue;
        }

        if ((g_cfg.trace_draw_state || g_cfg.no_additive_light ||
             g_cfg.trace_lod) &&
            texstate_claim(G->glide[i].name, fn, G->glide[i].va)) {
            /* Count claimed entries separately because they bypass the thunk. */
            s_glide_claimed[i] = 1;
            LOGT("glide: 0x%08X %-32s -> %s @%p (+ draw-state trace)",
                 G->glide[i].va, G->glide[i].name, decorated, fn);
            bound++;
            continue;
        }

        if (strcmp(G->glide[i].name, "grLfbLock") == 0) {
            s_real_lfb_lock = (lfb_lock_fn)fn;
            patch_jmp(G->glide[i].va, (void *)gl_lfb_lock);
            bound++;
            continue;
        }
        if (strcmp(G->glide[i].name, "grLfbUnlock") == 0) {
            s_real_lfb_unlock = (lfb_unlock_fn)fn;
            patch_jmp(G->glide[i].va, (void *)gl_lfb_unlock);
            bound++;
            continue;
        }

        if (G->glide[i].va == G->grbufferswap) {
            s_real_swap = (swap_fn)fn;
            patch_jmp(G->glide[i].va, (void *)gl_buffer_swap);
            LOGT("glide: 0x%08X %-32s -> %s @%p (+ message pump)",
                 G->glide[i].va, G->glide[i].name, decorated, fn);
            bound++;
            continue;
        }
        if (G->glide[i].va == G->grsstwinopen) {
            s_real_win_open = (win_open_fn)fn;
            patch_jmp(G->glide[i].va, (void *)gl_sst_win_open);
            LOGT("glide: 0x%08X %-32s -> %s @%p (+ game-driven window size)",
                 G->glide[i].va, G->glide[i].name, decorated, fn);
            bound++;
            continue;
        }
        if (strcmp(G->glide[i].name, "grClipWindow") == 0) {
            s_real_clip = (clip_fn)fn;
            patch_jmp(G->glide[i].va, (void *)gl_clip_window);
            LOGT("glide: 0x%08X %-32s -> %s @%p (+ geometry trace)",
                 G->glide[i].va, G->glide[i].name, decorated, fn);
            bound++;
            continue;
        }
        thunk = thunk_cdecl_to_stdcall(fn, G->glide[i].argbytes,
                                       G->glide[i].name, &s_glide_calls[i],
                                       g_cfg.glide_timing ?
                                           &s_glide_timer[i] : NULL,
                                       g_cfg.glide_first_call ?
                                           glide_first_call : NULL,
                                       i, &s_glide_seen[i]);
        if (!thunk)
            return bound;

        patch_jmp(G->glide[i].va, thunk);
        LOGT("glide: 0x%08X %-32s -> %s @%p",
             G->glide[i].va, G->glide[i].name, decorated, fn);
        bound++;
    }

    LOGI("glide: %d/%d entry points forwarded, %d missing",
         bound, G->glide_count, missing);

    /* Prologue length comes from the profile, not from a constant: Hydro opens
     * this function with a 5-byte `mov eax,[abs32]` and Offroad with a 6-byte
     * `sub esp,0x94`, and hardcoding Hydro's 5 truncated Offroad's instruction
     * inside the trampoline; the process died here with no fault and no log.
     * See hook_call_through() in patch.c. */
    if (game_require("glide", "relocatable _init3dfx_Init prologue",
                     G->init3dfx_init && G->hook_init3dfx)) {
        s_real_init3dfx = (init3dfx_fn)hook_call_through(
            G->init3dfx_init, (void *)gl_init3dfx, G->hook_init3dfx);
        if (!s_real_init3dfx)
            LOGE("glide: could not install _init3dfx_Init geometry hook");
    }
    return bound;
}

/* ---- the seam (src/glide_int.h) ----------------------------------------- */

int glide_view_width(void)
{
    return s_view_width;
}
