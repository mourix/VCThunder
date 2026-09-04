/* native.c -- decode untyped Glide stack slots and dispatch them to the core.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Unobserved entries use defined defaults and log their first invocation.
 */
#include <string.h>

#include "vgl.h"
#include "vcglide.h"

/* An argument slot is a stack slot. A float one is a float's bits in it. */
static inline float as_float(uint32_t v)
{
    union { uint32_t u; float f; } c;
    c.u = v;
    return c.f;
}

static inline const float *as_verts(uint32_t v)
{
    return (const float *)(uintptr_t)v;
}

static void once(int *flag, const char *what)
{
    if (!*flag) {
        *flag = 1;
        vgl_log(0, "native: %s; no capture of either game contains this "
                   "call; the documented default is used", what);
    }
}

/* gu* helpers set a canned combine. Only the values the games use are
 * translated; anything else keeps the current combine and says so, because a
 * wrong canned combine is a wrong picture and a missing one is a log line. */
static void gu_color_combine(int fnc)
{
    switch (fnc) {
    case 2:                             /* ITRGB: the iterated colour */
        vgl.cc.func = VGL_CF_LOCAL;
        vgl.cc.factor = VGL_FAC_ZERO;
        vgl.cc.local = VGL_LOCAL_ITERATED;
        vgl.cc.other = VGL_OTHER_ITERATED;
        vgl.cc.invert = 0;
        break;
    case 1:                             /* CCRGB: the constant colour */
        vgl.cc.func = VGL_CF_LOCAL;
        vgl.cc.factor = VGL_FAC_ZERO;
        vgl.cc.local = VGL_LOCAL_CONSTANT;
        vgl.cc.other = VGL_OTHER_ITERATED;
        vgl.cc.invert = 0;
        break;
    case 4:                             /* DECAL_TEXTURE */
        vgl.cc.func = VGL_CF_SCALE_OTHER;
        vgl.cc.factor = VGL_FAC_ONE_MINUS;   /* ONE */
        vgl.cc.local = VGL_LOCAL_ITERATED;
        vgl.cc.other = VGL_OTHER_TEXTURE;
        vgl.cc.invert = 0;
        break;
    case 6:                             /* TEXTURE_TIMES_ITRGB */
        vgl.cc.func = VGL_CF_SCALE_OTHER;
        vgl.cc.factor = VGL_FAC_LOCAL;
        vgl.cc.local = VGL_LOCAL_ITERATED;
        vgl.cc.other = VGL_OTHER_TEXTURE;
        vgl.cc.invert = 0;
        break;
    default: {
        static int flag;
        if (!flag) {
            flag = 1;
            vgl_log(0, "native: guColorCombineFunction(%d) is not translated "
                       "-- the combine is left as it was", fnc);
        }
        break;
    }
    }
}

static void gu_tex_combine(int tmu, int fnc)
{
    vgl_tmu_t *u;

    if (tmu < 0 || tmu >= VGL_TMUS)
        return;
    u = &vgl.tmu[tmu];
    switch (fnc) {
    case 0:                             /* ZERO */
        u->c_func = u->a_func = VGL_CF_ZERO;
        u->c_factor = u->a_factor = VGL_FAC_ZERO;
        break;
    case 1:                             /* DECAL: the unit's own texel */
        u->c_func = u->a_func = VGL_CF_LOCAL;
        u->c_factor = u->a_factor = VGL_FAC_ZERO;
        break;
    case 2:                             /* OTHER: pass the next unit through */
        u->c_func = u->a_func = VGL_CF_SCALE_OTHER;
        u->c_factor = u->a_factor = VGL_FAC_ONE_MINUS;
        break;
    case 4:                             /* MULTIPLY */
        u->c_func = u->a_func = VGL_CF_SCALE_OTHER;
        u->c_factor = u->a_factor = VGL_FAC_LOCAL;
        break;
    default: {
        static int flag;
        if (!flag) {
            flag = 1;
            vgl_log(0, "native: guTexCombineFunction(%d, %d) is not translated "
                       "-- the combine is left as it was", tmu, fnc);
        }
        break;
    }
    }
    u->c_invert = u->a_invert = 0;
}

/* Does this call change what the GPU backend caches as a draw state?
 *
 * The answer is a column of entries.def, not a switch here. It was a switch
 * here -- forty cases, beside the forty-odd cases below that do the work -- and
 * two lists over one set of entry points is the shape src/settings.def exists to
 * refuse: an entry added to the dispatch and forgotten in the other one leaves
 * the GPU drawing with a state it has already been told to replace, and nothing
 * anywhere reports it. */
static int changes_draw_state(unsigned ix)
{
    return ix < VGL_COUNT && vgl_entries[ix].state == ST_DRAW;
}

int vgl_native_call(unsigned ix, const uint32_t *a)
{
    if (changes_draw_state(ix))
        vgl.draw_state_version++;
    switch (ix) {

    /* ---- device ---- */
    case VGL_IX_grGlideInit:
        vgl_log(0, "native: grGlideInit");
        return 1;
    case VGL_IX_grGlideShutdown:
        vgl_device_shutdown();
        vgl_tex_free();
        return 1;
    case VGL_IX_grSstSelect:
    case VGL_IX_grSstIdle:
        return 1;
    case VGL_IX_grSstStatus:
        return 0;
    case VGL_IX_grSstWinOpen:
        /* (hwnd, res, refresh, cformat, origin, colour buffers, aux buffers) */
        return vgl_device_open((void *)(uintptr_t)a[0], (int)a[1], (int)a[2],
                               (int)a[4], (int)a[5], (int)a[6]);
    case VGL_IX_grSstWinClose:
        vgl_device_close();
        return 1;
    case VGL_IX_grSstOrigin:
        vgl.origin_lower_left = ((int)a[0] == GR_ORIGIN_LOWER_LEFT);
        return 1;
    case VGL_IX_grSstVidMode: {
        /* Preserve the cabinet timing payload for diagnostic output. */
        static int flag;
        if (!flag) {
            flag = 1;
            vgl_log(0, "native: grSstVidMode(%u, %p); the caller's CRT "
                       "timing table is not applied by this provider",
                    a[0], (void *)(uintptr_t)a[1]);
        }
        return 1;
    }
    case VGL_IX_grSstQueryHardware: {
        /* Report the cabinet's one-board, two-TMU hardware layout. */
        uint32_t *hw = (uint32_t *)(uintptr_t)a[0];
        if (!hw)
            return 0;
        hw[0] = 1;                      /* boards            */
        hw[1] = 0;                      /* type: Voodoo      */
        hw[2] = 2;                      /* FBI memory, MB    */
        hw[3] = 0x101;                  /* fbiRev            */
        hw[4] = 2;                      /* texelfx units     */
        hw[5] = 0;                      /* not an SLI pair   */
        hw[6] = 1; hw[7] = 4;           /* TMU0 rev, MB      */
        hw[8] = 1; hw[9] = 4;           /* TMU1 rev, MB      */
        return 1;
    }
    case VGL_IX_grSplash:
        return 1;
    case VGL_IX_grErrorSetCallback:
        return 1;
    case VGL_IX_grHints:
        if (a[0] == 0)
            vgl.stw_hint = a[1];
        return 1;
    case VGL_IX_grGammaCorrectionValue:
        return 1;

    /* ---- buffers ---- */
    case VGL_IX_grBufferClear:
        vgl_buffer_clear(a[0], a[1], a[2]);
        return 1;
    case VGL_IX_grBufferSwap:
        vgl_buffer_swap((int)a[0]);
        return 1;
    case VGL_IX_grClipWindow: {
        int x0 = (int)a[0], y0 = (int)a[1];
        int x1 = (int)a[2], y1 = (int)a[3];
        int rx0, ry0, rx1, ry1;

        /* Treat maxx and maxy as exclusive draw bounds while the visible raster
         * is one pixel larger. Convert lower-left device coordinates to the
         * renderer's top-down storage origin. */
        if (x0 < 0) x0 = 0; else if (x0 > vgl.w) x0 = vgl.w;
        if (x1 < 0) x1 = 0; else if (x1 > vgl.w) x1 = vgl.w;
        if (y0 < 0) y0 = 0; else if (y0 > vgl.h) y0 = vgl.h;
        if (y1 < 0) y1 = 0; else if (y1 > vgl.h) y1 = vgl.h;

        vgl.clip_x0 = x0;
        vgl.clip_x1 = x1;
        if (vgl.origin_lower_left) {
            vgl.clip_y0 = vgl.h - y1;
            vgl.clip_y1 = vgl.h - y0;
        } else {
            vgl.clip_y0 = y0;
            vgl.clip_y1 = y1;
        }
        /* Use the clip window as the visible raster for presentation. */
        if (vgl.presenting || vgl.gpu) {
            rx0 = x0;
            rx1 = x1 < vgl.w ? x1 + 1 : vgl.w;
            if (vgl.origin_lower_left) {
                ry0 = vgl.h - (y1 < vgl.h ? y1 + 1 : vgl.h);
                ry1 = vgl.h - y0;
            } else {
                ry0 = y0;
                ry1 = y1 < vgl.h ? y1 + 1 : vgl.h;
            }
            if (vgl.gpu)
                vgl_gpu_set_raster(rx0, ry0, rx1 - rx0, ry1 - ry0,
                                   vgl.clip_x0, vgl.clip_y0,
                                   vgl.clip_x1, vgl.clip_y1);
            else
                vgl_present_raster(rx0, ry0, rx1 - rx0, ry1 - ry0,
                                   vgl.clip_x0, vgl.clip_y0,
                                   vgl.clip_x1, vgl.clip_y1);
        }
        return 1;
    }

    /* ---- the pixel pipeline ---- */
    case VGL_IX_grAlphaBlendFunction:
        vgl.blend_rgb_src = (int)a[0];
        vgl.blend_rgb_dst = (int)a[1];
        vgl.blend_a_src   = (int)a[2];
        vgl.blend_a_dst   = (int)a[3];
        return 1;
    case VGL_IX_grAlphaTestFunction:
        vgl.alpha_func = (int)a[0];
        return 1;
    case VGL_IX_grAlphaTestReferenceValue:
        vgl.alpha_ref = (int)(a[0] & 0xff);
        return 1;
    case VGL_IX_grAlphaControlsITRGBLighting:
        vgl.itrgb_lighting = (int)a[0];
        return 1;
    case VGL_IX_grColorCombine:
        vgl.cc.func = (int)a[0]; vgl.cc.factor = (int)a[1];
        vgl.cc.local = (int)a[2]; vgl.cc.other = (int)a[3];
        vgl.cc.invert = (int)a[4];
        return 1;
    case VGL_IX_grAlphaCombine:
        vgl.ac.func = (int)a[0]; vgl.ac.factor = (int)a[1];
        vgl.ac.local = (int)a[2]; vgl.ac.other = (int)a[3];
        vgl.ac.invert = (int)a[4];
        return 1;
    case VGL_IX_grConstantColorValue:
        vgl.constant_color = a[0];
        return 1;
    case VGL_IX_grColorMask:
        vgl.color_mask_rgb = (int)a[0];
        vgl.color_mask_a = (int)a[1];
        return 1;
    case VGL_IX_grCullMode:
        vgl.cull_mode = (int)a[0];
        return 1;
    case VGL_IX_grDitherMode:
        vgl.dither = (int)a[0];
        return 1;
    case VGL_IX_grDepthBufferMode:
        vgl.depth_mode = (int)a[0];
        return 1;
    case VGL_IX_grDepthBufferFunction:
        vgl.depth_func = (int)a[0];
        return 1;
    case VGL_IX_grDepthMask:
        vgl.depth_mask = (int)a[0];
        return 1;
    case VGL_IX_grDepthBiasLevel:
        vgl.depth_bias = (int)(int32_t)a[0];
        return 1;
    case VGL_IX_grChromakeyMode:
        vgl.chroma_mode = (int)a[0];
        return 1;
    case VGL_IX_grChromakeyValue:
        vgl.chroma_value = a[0];
        return 1;
    case VGL_IX_grDisableAllEffects:
        vgl_disable_all_effects();
        return 1;
    case VGL_IX_grFogMode: {
        static int flag;
        vgl.fog_mode = (int)a[0];
        if (vgl.fog_mode && !flag) {
            flag = 1;
            vgl_log(0, "native: grFogMode(%u); fog ON. The 1,500-frame "
                       "attract capture this renderer was scoped against "
                       "contains only grFogMode(0), so the census said the "
                       "games never fog; they do, in a race", a[0]);
        }
        if ((vgl.fog_mode & 7) != 0 && (vgl.fog_mode & 7) != 2) {
            static int other;
            if (!other) {
                other = 1;
                vgl_log(0, "native: grFogMode(%u) is not the table mode, which "
                           "is the only one implemented", a[0]);
            }
        }
        return 1;
    }
    case VGL_IX_grFogColorValue:
        vgl.fog_color = a[0];
        return 1;
    case VGL_IX_grFogTable:
        if (a[0])
            memcpy(vgl.fog_table, (const void *)(uintptr_t)a[0],
                   GR_FOG_TABLE_BYTES);
        return 1;

    /* ---- state save/restore ---- */
    case VGL_IX_grGlideGetState:
        vgl_state_get((void *)(uintptr_t)a[0]);
        return 1;
    case VGL_IX_grGlideSetState:
        vgl_state_set((const void *)(uintptr_t)a[0]);
        return 1;

    /* ---- the linear frame buffer ---- */
    case VGL_IX_grLfbLock:
        return vgl_lfb_lock(a[0], a[1], a[2], a[3], a[4],
                            (void *)(uintptr_t)a[5]);
    case VGL_IX_grLfbUnlock:
        return vgl_lfb_unlock(a[0], a[1]);
    case VGL_IX_grLfbConstantAlpha:
        vgl.lfb_const_alpha = a[0];
        return 1;
    case VGL_IX_grLfbConstantDepth:
        vgl.lfb_const_depth = a[0];
        return 1;
    case VGL_IX_grLfbWriteColorFormat: {
        static int flag;
        once(&flag, "grLfbWriteColorFormat");
        return 1;
    }

    /* ---- texture units ---- */
    case VGL_IX_grTexSource:
        vgl_tex_source((int)a[0], a[1], (int)a[2],
                       (const void *)(uintptr_t)a[3]);
        return 1;
    case VGL_IX_grTexDownloadMipMap:
        vgl_tex_download((int)a[0], a[1], (int)a[2],
                         (const void *)(uintptr_t)a[3]);
        return 1;
    case VGL_IX_grTexDownloadMipMapLevelPartial:
        /* (tmu, start, thisLod, largeLod, aspect, format, evenOdd, data,
         *  start_row, end_row); the one entry that carries a chain's
         *  description in its own arguments instead of a GrTexInfo. */
        vgl_tex_download_partial((int)a[0], a[1], (int)a[2], (int)a[3],
                                 (int)a[4], (int)a[5], (int)a[6],
                                 (const void *)(uintptr_t)a[7],
                                 (int)a[8], (int)a[9]);
        return 1;
    case VGL_IX_grTexDownloadTable:
        vgl_tex_table((int)a[0], (int)a[1], (const void *)(uintptr_t)a[2]);
        return 1;
    case VGL_IX_grTexTextureMemRequired:
        return (int)vgl_tex_mem_required((int)a[0],
                                         (const void *)(uintptr_t)a[1]);
    case VGL_IX_grTexMinAddress:
        return (int)vgl.tmu[a[0] < VGL_TMUS ? a[0] : 0].min_addr;
    case VGL_IX_grTexMaxAddress:
        return (int)vgl.tmu[a[0] < VGL_TMUS ? a[0] : 0].max_addr;
    case VGL_IX_grTexClampMode:
        if (a[0] < VGL_TMUS) {
            vgl.tmu[a[0]].clamp_s = (int)a[1];
            vgl.tmu[a[0]].clamp_t = (int)a[2];
        }
        return 1;
    case VGL_IX_grTexFilterMode:
        if (a[0] < VGL_TMUS) {
            vgl.tmu[a[0]].min_filter = (int)a[1];
            vgl.tmu[a[0]].mag_filter = (int)a[2];
        }
        return 1;
    case VGL_IX_grTexMipMapMode:
        if (a[0] < VGL_TMUS) {
            vgl.tmu[a[0]].mipmap_mode = (int)a[1];
            vgl.tmu[a[0]].lod_blend = (int)a[2];
        }
        return 1;
    case VGL_IX_grTexLodBiasValue:
        if (a[0] < VGL_TMUS)
            vgl.tmu[a[0]].lod_bias = as_float(a[1]);
        return 1;
    case VGL_IX_grTexCombine:
        if (a[0] < VGL_TMUS) {
            vgl_tmu_t *u = &vgl.tmu[a[0]];
            u->c_func = (int)a[1]; u->c_factor = (int)a[2];
            u->a_func = (int)a[3]; u->a_factor = (int)a[4];
            u->c_invert = (int)a[5]; u->a_invert = (int)a[6];
        }
        return 1;
    case VGL_IX_grTexCombineFunction:
        gu_tex_combine((int)a[0], (int)a[1]);
        return 1;
    case VGL_IX_grTexDetailControl: {
        static int flag;
        /* Accept the unused detail-control entry point without state changes. */
        once(&flag, "grTexDetailControl");
        return 1;
    }
    case VGL_IX_grTexMultibase: {
        static int flag;
        once(&flag, "grTexMultibase");
        return 1;
    }
    case VGL_IX_grTexNCCTable: {
        static int flag;
        once(&flag, "grTexNCCTable");
        return 1;
    }

    /* ---- the gu helpers ---- */
    case VGL_IX_guColorCombineFunction:
        gu_color_combine((int)a[0]);
        return 1;
    case VGL_IX_guTexCombineFunction:
        gu_tex_combine((int)a[0], (int)a[1]);
        return 1;
    case VGL_IX_guTexMemReset:
        return 1;

    /* ---- primitives ---- */
    case VGL_IX_grDrawTriangle:
        vgl_draw_triangle(as_verts(a[0]), as_verts(a[1]), as_verts(a[2]));
        return 1;
    case VGL_IX_grAADrawTriangle: {
        static int flag;
        once(&flag, "grAADrawTriangle");
        vgl_draw_triangle(as_verts(a[0]), as_verts(a[1]), as_verts(a[2]));
        return 1;
    }
    case VGL_IX_grDrawPolygonVertexList:
        vgl_draw_vertex_list(0, (int)a[0], as_verts(a[1]));
        return 1;
    case VGL_IX_grDrawPlanarPolygonVertexList:
        vgl_draw_vertex_list(1, (int)a[0], as_verts(a[1]));
        return 1;
    case VGL_IX_grDrawLine:
    case VGL_IX_grAADrawLine:
        vgl_draw_line(as_verts(a[0]), as_verts(a[1]));
        return 1;

    default: {
        /* Unreachable while entries.def and this switch agree, and it says so
         * rather than returning a quiet zero if they ever stop agreeing. */
        static int reported[VGL_COUNT];
        if (ix < VGL_COUNT && !reported[ix]) {
            reported[ix] = 1;
            vgl_log(0, "native: %s has no implementation; returning 0",
                    vgl_entries[ix].name);
        }
        return 0;
    }
    }
}
