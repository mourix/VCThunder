/* glide3.c -- the Glide 3 entries Arctic Thunder imports, on the Glide 2 seam.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Written under the renderer's clean-room rules: every constant below names
 * its source.
 *
 * THE DESIGN: a TRANSLATOR, not a second front end. Every Glide 3 call becomes
 * one or more calls of this provider's existing Glide 2 entries, so the seam
 * that counts calls, captures them and dumps golden frames sees an ordinary
 * Glide 2 stream, and glide-replay replays Arctic without knowing it exists.
 *   - 26 entries have the same name, stack bytes and meaning in both APIs:
 *     they ARE the Glide 2 exports (the Glide 3.0 Programming Guide's porting
 *     notes list what changed, and none of these is among them).
 *     grSstWinOpen passes through a wrapper that notes where the surface put
 *     the game's picture (s_origin_x).
 *   - Vertices: grVertexLayout describes the caller's struct; each vertex is
 *     repacked into the 60-byte Glide 2 vertex and drawn as triangles.
 *   - Textures: the GR_LOD and GR_ASPECT constants became log2 values (Guide
 *     ch. 9, Table 9.2 and porting note); converted here, nothing else moves.
 *
 * THE NUMBERS. The Guide names the parameters, modes and queries but gives no
 * values, and no SDK header may be read. Each value used is therefore DERIVED
 * from Arctic's own calls and code, against the Guide's definition of what the
 * name means: what lies at an offset says what the parameter is, and how the game
 * uses an answer says which query it asked. A parameter, mode or query the game
 * was never seen to use is not given a meaning here: it is logged, once.
 *
 * ON THE GAME'S THREAD: Arctic draws from a fiber on goose's process-loop
 * thread with the x87 FPU set to 24-bit precision (_controlfp(_PC_24), snow.exe
 * 0x4d9867). Nothing here formats a float or relies on x87 precision.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "vgl.h"
#include "vcglide.h"
#include "../api/glide2.h"

/* ---- the Glide 2 entries this translator calls -------------------------- */

#define G2(name, ...) int __stdcall name(__VA_ARGS__);
#define U uint32_t
G2(grAlphaBlendFunction, U, U, U, U)
G2(grAlphaCombine, U, U, U, U, U)
G2(grAlphaTestFunction, U)
G2(grAlphaTestReferenceValue, U)
G2(grBufferClear, U, U, U)
G2(grBufferSwap, U)
G2(grChromakeyMode, U)
G2(grChromakeyValue, U)
G2(grColorCombine, U, U, U, U, U)
G2(grConstantColorValue, U)
G2(grCullMode, U)
G2(grDepthBiasLevel, U)
G2(grDepthBufferFunction, U)
G2(grDepthBufferMode, U)
G2(grDepthMask, U)
G2(grFogColorValue, U)
G2(grFogMode, U)
G2(grFogTable, U)
G2(grGlideInit, void)
G2(grGlideShutdown, void)
G2(grSstSelect, U)
G2(grSstWinOpen, U, U, U, U, U, U, U)
G2(grTexClampMode, U, U, U)
G2(grTexCombine, U, U, U, U, U, U, U)
G2(grTexFilterMode, U, U, U)
G2(grTexLodBiasValue, U, U)
G2(grTexMipMapMode, U, U, U)
G2(grDrawTriangle, U, U, U)
G2(grTexSource, U, U, U, U)
G2(grTexDownloadMipMap, U, U, U, U)
G2(grTexDownloadMipMapLevelPartial, U, U, U, U, U, U, U, U, U, U)
G2(grTexDownloadTable, U, U, U)
G2(grTexTextureMemRequired, U, U)
G2(grLfbReadRegion, U, U, U, U, U, U, U)
G2(grLfbLock, U, U, U, U, U, U)
G2(grLfbUnlock, U, U)
#undef G2

/* ---- the numbers -------------------------------------------------------- */

/* grVertexLayout params. Window coordinate space (Guide Table 2.2: the game
 * never calls grCoordinateSpace), so the values mean what Glide 2's vertex
 * fields meant (Guide, "Choosing a Coordinate Space"). */
#define P_XY     0x01u  /* two floats at offset 0 in every layout; the Guide
                         * requires XY at offset 0; values span 0..640, 0..480 */
#define P_Q      0x04u  /* one float, exactly 1.0 on every 2D quad and in
                         * (0.1, 0.37) in 3D: 1/w (Table 2.2, GR_PARAM_Q) */
#define P_A      0x10u  /* one float in [0, 255]: alpha (GR_PARAM_A) */
#define P_PARGB  0x30u  /* one 32-bit word whose high byte is 0xff on opaque
                         * draws: packed ARGB (GR_PARAM_PARGB) */
#define P_ST0    0x40u  /* two floats, 0..256 on 2D quads where Q is 1, s/w
                         * and t/w in 3D: TMU0's s and t (GR_PARAM_ST0) */
#define P_ST1    0x41u  /* TMU1's; always at ST0's offset when enabled */
#define P_ENABLE 1u     /* mode 1 with an offset, 0 with none: ENABLE */

/* grDrawVertexArray[Contiguous] modes. */
#define M_FAN    5u     /* 3..6 vertices; the 2D quads arrive in perimeter
                         * order (TL, TR, BR, BL), which a fan covers and a
                         * strip does not, and the 3D ones are convex polygons
                         * clipped to the screen: GR_TRIANGLE_FAN */
#define M_TRIS   6u     /* every count seen (3..174) a multiple of three:
                         * GR_TRIANGLES */

/* grGet pnames, by what the game does with the answer. */
#define Q_FOG_ENTRIES   0x04u   /* snow.exe 0x46db58 refuses fewer than nine
                                 * and sizes its fog table by it */
#define Q_GAMMA_ENTRIES 0x05u   /* gobj.dll sizes its grLoadGammaTable by it */
#define Q_MEMORY_TMU    0x0du   /* gobj.dll keeps it beside the per-TMU
                                 * grTexMin/MaxAddress pairs */
#define Q_NUM_TMU       0x13u   /* gobj.dll's loop bound over TMUs */
#define Q_VIEWPORT      0x26u   /* snow.exe 0x4dc817 requires 16 bytes and
                                 * keeps values 2 and 3 as the screen size:
                                 * GR_VIEWPORT is x, y, width, height */

/* ---- the vertex layout -------------------------------------------------- */

static struct {
    int xy, q, a, pargb, st0, st1;      /* offsets; -1 = disabled */
} s_lay = { 0, -1, -1, -1, -1, -1 };
static unsigned s_unknown_seen;

static void __stdcall g3_grVertexLayout(uint32_t param, int32_t offset, uint32_t mode)
{
    int off = mode == P_ENABLE ? (int)offset : -1;

    switch (param) {
    case P_XY:    s_lay.xy = off; break;
    case P_Q:     s_lay.q = off; break;
    case P_A:     s_lay.a = off; break;
    case P_PARGB: s_lay.pargb = off; break;
    case P_ST0:   s_lay.st0 = off; break;
    case P_ST1:   s_lay.st1 = off; break;
    default:
        if (mode == P_ENABLE)
            vgl_log_capped(&s_unknown_seen, 8, "glide3: vertex parameter %#x"
                           " enabled at %d; its meaning is not established"
                           " and it is ignored", param, (int)offset);
        break;
    }
}

/* A disabled colour keeps the last one seen (Guide: "Disabling a parameter
 * will potentially cause it to inherit the last known value"). */
static float s_last_rgba[4] = { 255.0f, 255.0f, 255.0f, 255.0f };

/* Where the game's own picture sits in the surface. Under the host's
 * widescreen raster the provider centres a native field (device.c, from the
 * published LFB width); the game draws in that field's coordinates, so every
 * vertex moves by the field's whole-pixel offset and GR_VIEWPORT reports the
 * field. 0 without a field. Set at grSstWinOpen. */
static float s_origin_x;
/* Did a vertex this frame lie outside the game's field, in its own
 * coordinates? A vertex ON an edge covers nothing beyond it, so the full-
 * screen 2D art (0..w exactly) does not count. */
static int s_outside;

/* One caller vertex into the 15 floats of the Glide 2 vertex this provider
 * draws (x, y, -, r, g, b, 1/z, a, 1/w, then s/w, t/w, 1/w per TMU; the
 * rasteriser's planes, raster.c). `limit` is how many bytes the caller's
 * vertex is known to hold (a contiguous array's stride), 0 for unknown: a
 * colour declared past it is not read, because Glide reads the colour only
 * when the combine asks for it and the game declares one past a 20-byte
 * stride on draws that do not. */
static void repack(const uint8_t *v, unsigned limit, float *o)
{
    float q = 1.0f;

    memcpy(&o[0], v + s_lay.xy, 8);
    if (o[0] < 0.0f || o[0] > (float)vgl_cfg.lfb_width)
        s_outside = 1;
    o[0] += s_origin_x;
    o[2] = 0.0f;
    o[6] = 0.0f;
    if (s_lay.q >= 0)
        memcpy(&q, v + s_lay.q, 4);
    o[8] = o[11] = o[14] = q;
    if (s_lay.pargb >= 0 && (!limit || (unsigned)s_lay.pargb + 4 <= limit)) {
        uint32_t c;
        memcpy(&c, v + s_lay.pargb, 4);
        s_last_rgba[0] = (float)((c >> 16) & 0xff);
        s_last_rgba[1] = (float)((c >> 8) & 0xff);
        s_last_rgba[2] = (float)(c & 0xff);
        s_last_rgba[3] = (float)(c >> 24);
    }
    if (s_lay.a >= 0 && (!limit || (unsigned)s_lay.a + 4 <= limit))
        memcpy(&s_last_rgba[3], v + s_lay.a, 4);
    o[3] = s_last_rgba[0];
    o[4] = s_last_rgba[1];
    o[5] = s_last_rgba[2];
    o[7] = s_last_rgba[3];
    if (s_lay.st0 >= 0)
        memcpy(&o[9], v + s_lay.st0, 8);
    else
        o[9] = o[10] = 0.0f;
    /* ST1 disabled: TMU1 inherits TMU0's coordinates (Guide, porting note on
     * GR_HINT_STWHINT). */
    if (s_lay.st1 >= 0)
        memcpy(&o[12], v + s_lay.st1, 8);
    else {
        o[12] = o[9];
        o[13] = o[10];
    }
}

static float s_v[3][GR_VERTEX_BYTES / 4];

static void tri(void)
{
    grDrawTriangle((uint32_t)(uintptr_t)s_v[0], (uint32_t)(uintptr_t)s_v[1],
                   (uint32_t)(uintptr_t)s_v[2]);
}

static unsigned s_mode_seen;

/* One primitive list, by an accessor that yields vertex i. */
static void draw_list(uint32_t mode, uint32_t count,
                      const uint8_t *(*at)(const void *, uint32_t),
                      const void *src, unsigned limit)
{
    uint32_t i;

    if (mode == M_TRIS) {
        for (i = 0; i + 2 < count; i += 3) {
            repack(at(src, i), limit, s_v[0]);
            repack(at(src, i + 1), limit, s_v[1]);
            repack(at(src, i + 2), limit, s_v[2]);
            tri();
        }
    } else if (mode == M_FAN) {
        if (count < 3)
            return;
        for (i = 1; i + 1 < count; i++) {
            repack(at(src, 0), limit, s_v[0]);
            repack(at(src, i), limit, s_v[1]);
            repack(at(src, i + 1), limit, s_v[2]);
            tri();
        }
    } else {
        vgl_log_capped(&s_mode_seen, 8, "glide3: draw mode %u (%u vertices)"
                       " is not one Arctic was seen to use; not drawn",
                       mode, count);
    }
}

static const uint8_t *at_ptrs(const void *src, uint32_t i)
{
    return ((const uint8_t *const *)src)[i];
}

static uint32_t s_stride;
static const uint8_t *at_contig(const void *src, uint32_t i)
{
    return (const uint8_t *)src + i * s_stride;
}

static void __stdcall g3_grDrawTriangle(const void *a, const void *b, const void *c)
{
    repack(a, 0, s_v[0]);
    repack(b, 0, s_v[1]);
    repack(c, 0, s_v[2]);
    tri();
}

static void __stdcall g3_grDrawVertexArray(uint32_t mode, uint32_t count, void *ptrs)
{
    if (ptrs)
        draw_list(mode, count, at_ptrs, ptrs, 0);
}

static void __stdcall g3_grDrawVertexArrayContiguous(uint32_t mode, uint32_t count,
                                                     void *v, uint32_t stride)
{
    if (!v)
        return;
    s_stride = stride;
    draw_list(mode, count, at_contig, v, stride);
}

/* ---- textures ----------------------------------------------------------- */

/* Glide 3's log2 LOD and aspect into the Glide 2 values the core holds. The
 * core's LOD 0 is 256 texels and 8 is one (vgl.h); its aspect 0 is 8:1 wide,
 * 3 square, 6 1:8 tall (tex.c, level_dims). Glide 3's LOD is log2 of the
 * larger dimension (Guide Table 9.2) and its aspect log2(width/height),
 * positive when s is the larger dimension (Guide ch. 9, GrTexInfo). */
static uint32_t lod2(int32_t lod3)    { return (uint32_t)(8 - lod3); }
static uint32_t aspect2(int32_t asp3) { return (uint32_t)(3 - asp3); }

/* GrTexInfo is the same five 32-bit fields in both APIs (Guide ch. 9 lists
 * smallLodLog2, largeLodLog2, aspectRatioLog2, format, data; glide2.h has the
 * offsets). The converted copy is what the Glide 2 entry, and a capture, see. */
static const void *texinfo2(const void *info3, uint8_t out[GR_TEXINFO_BYTES])
{
    int32_t f[5];

    memcpy(f, info3, sizeof f);
    f[0] = (int32_t)lod2(f[0]);
    f[1] = (int32_t)lod2(f[1]);
    f[2] = (int32_t)aspect2(f[2]);
    memcpy(out, f, sizeof f);
    return out;
}

static void __stdcall g3_grTexSource(uint32_t tmu, uint32_t start, uint32_t eo,
                                     const void *info)
{
    uint8_t t[GR_TEXINFO_BYTES];

    if (info)
        grTexSource(tmu, start, eo, (uint32_t)(uintptr_t)texinfo2(info, t));
}

static void __stdcall g3_grTexDownloadMipMap(uint32_t tmu, uint32_t start,
                                             uint32_t eo, const void *info)
{
    uint8_t t[GR_TEXINFO_BYTES];

    if (info)
        grTexDownloadMipMap(tmu, start, eo, (uint32_t)(uintptr_t)texinfo2(info, t));
}

static uint32_t __stdcall g3_grTexTextureMemRequired(uint32_t eo, const void *info)
{
    uint8_t t[GR_TEXINFO_BYTES];

    return info ? (uint32_t)grTexTextureMemRequired(
                      eo, (uint32_t)(uintptr_t)texinfo2(info, t)) : 0;
}

static void __stdcall g3_grTexDownloadMipMapLevelPartial(
    uint32_t tmu, uint32_t start, int32_t this_lod, int32_t large_lod,
    int32_t aspect, uint32_t format, uint32_t eo, void *data,
    uint32_t from, uint32_t to)
{
    grTexDownloadMipMapLevelPartial(tmu, start, lod2(this_lod), lod2(large_lod),
                                    aspect2(aspect), format, eo,
                                    (uint32_t)(uintptr_t)data, from, to);
}

/* A whole level is the partial download of all of its rows; its height is
 * the core's level geometry (tex.c, level_dims) for the converted values. */
static void __stdcall g3_grTexDownloadMipMapLevel(
    uint32_t tmu, uint32_t start, int32_t this_lod, int32_t large_lod,
    int32_t aspect, uint32_t format, uint32_t eo, void *data)
{
    uint32_t l = lod2(this_lod), a = aspect2(aspect);
    int major = 256 >> (l > VGL_MAX_LOD ? VGL_MAX_LOD : l);
    int h = a <= 3 ? major : major >> (a - 3);

    if (h < 1)
        h = 1;
    grTexDownloadMipMapLevelPartial(tmu, start, l, lod2(large_lod), a, format,
                                    eo, (uint32_t)(uintptr_t)data, 0,
                                    (uint32_t)(h - 1));
}

/* Glide 3 has no tmu argument: "All TMUs share the same NCC table or color
 * palette" (Guide ch. 10, porting note). The core already holds ONE palette
 * for both units, and its NCC tables likewise; naming TMU0 reaches both. */
static void __stdcall g3_grTexDownloadTable(uint32_t type, void *data)
{
    grTexDownloadTable(0, type, (uint32_t)(uintptr_t)data);
}

/* ---- fog ---------------------------------------------------------------- */

/* Glide 3 kept four fog modes: GR_FOG_DISABLE, GR_FOG_WITH_TABLE_ON_Q
 * ("formerly GR_FOG_WITH_TABLE"), GR_FOG_ADD2 and GR_FOG_MULT2, plus
 * GR_FOG_WITH_TABLE_ON_FOGCOORD_EXT where the extension exists (Guide ch. 8
 * and its porting note). Arctic sends 0 and 1 and downloads a fog table; it
 * never asks for extensions (it imports no grGetString) and never enables a
 * fog-coordinate vertex parameter, so its 1 can only be table fog on Q. The
 * core holds the table mode by its Glide 2 value, 2 (native.c). A mode the
 * game was not seen to send is passed on unchanged and logged by the core. */
#define FOG3_TABLE_ON_Q 1u
#define FOG2_TABLE      2u
static void __stdcall g3_grFogMode(uint32_t mode)
{
    grFogMode((mode & 0xffu) == FOG3_TABLE_ON_Q
              ? (mode & ~0xffu) | FOG2_TABLE : mode);
}

/* ---- texture memory ----------------------------------------------------- */

/* What each unit offers the game. NOT the Voodoo2 cabinets' 4 MB the Glide 2
 * entries report: Arctic's track textures do not fit in it ("Texture ... not
 * loaded", then a null pointer in snow.exe), and its
 * board was a Voodoo3, whose memory is one pool of up to 16 MB shared by the
 * frame buffer and textures ("The amount of texture memory is only limited by
 * the maximum amount of Avenger frame buffer memory", Voodoo3 Specification
 * 1.0, Memory Architecture): about 13.6 MB at 640x480 with three colour
 * buffers and one aux. Each unit here gets the whole of its allocation, 16 MB
 * between the two: at least what the board had, as separate ranges so the
 * game's per-unit allocator can never alias them. More memory changes how
 * often the game evicts, not what it draws; the boot census's run at this size
 * logged no texture it could not load. */
static uint32_t tmu_bytes(void)
{
    return vgl.tmu[0].mem_bytes;
}

static uint32_t __stdcall g3_grTexMinAddress(uint32_t tmu)
{
    (void)tmu;
    return 0;
}

/* The highest address a texture may START at, as Glide 2's grTexMaxAddress
 * means it (the core reports its 4 MB the same way: size - 8). */
static uint32_t __stdcall g3_grTexMaxAddress(uint32_t tmu)
{
    (void)tmu;
    return tmu_bytes() - 8u;
}

/* ---- queries, gamma, LFB, the rest -------------------------------------- */

static unsigned s_get_seen;

static uint32_t __stdcall g3_grGet(uint32_t pname, uint32_t plength, uint32_t *params)
{
    uint32_t v[4] = { 0, 0, 0, 0 }, n = 1, i;

    switch (pname) {
    case Q_FOG_ENTRIES:
        v[0] = GR_FOG_TABLE_BYTES;      /* one byte per entry; glide2.h */
        break;
    case Q_GAMMA_ENTRIES:
        /* The game builds a table of this many entries and hands it to
         * grLoadGammaTable, which keeps it (below). Voodoo3's gamma CLUT is
         * indexed by an 8-bit channel value (Voodoo3 Specification, "Video
         * Output Gamma Correction"; dacAddr/dacData), so 256 asks the game
         * for the table the hardware would have held. */
        v[0] = 256;
        break;
    case Q_MEMORY_TMU:
        v[0] = tmu_bytes();
        break;
    case Q_NUM_TMU:
        v[0] = VGL_TMUS;
        break;
    case Q_VIEWPORT:
        v[2] = (uint32_t)(vgl_cfg.lfb_width > 0 ? vgl_cfg.lfb_width : vgl.w);
        v[3] = (uint32_t)(vgl_cfg.lfb_width > 0 ? vgl_cfg.raster_height : vgl.h);
        n = 4;
        break;
    default:
        vgl_log_capped(&s_get_seen, 16, "glide3: grGet(%#x, %u) is not a query"
                       " this translator answers; 0 bytes written", pname,
                       plength);
        return 0;
    }
    if (!params || plength < n * 4)
        return 0;
    for (i = 0; i < n; i++)
        params[i] = v[i];
    return n * 4;
}

/* The table the presenters apply (vgl.h). It starts as the identity: "By
 * default, Glide does not perform gamma correction (i.e., a linear ramp is
 * used)" (Guide, Gamma Correction). */
vgl_gamma_t vgl_gamma;
static int s_gamma_init;

static void gamma_identity(void)
{
    int c, i;

    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++)
            vgl_gamma.lut[c][i] = (uint8_t)i;
    s_gamma_init = 1;
}

/* "If nEntries is less than the size of the hardware-dependent gamma table,
 * the first part of the table is overwritten by the new values; if nEntries
 * is greater than the gamma table size, the excess elements are discarded"
 * (Guide, grLoadGammaTable). The size is the 256 grGet reports. An entry is
 * an 8-bit channel value, as the DAC's 24-bit entries hold (Voodoo3
 * Specification, dacData); anything larger saturates.
 *
 * Arctic loads one table per frame while it fades: gobj.dll fills entry i
 * with i * 255 * k / (n - 1) for a brightness k it eases toward a target
 * (gobj.dll 0x10002510, the factor at 0x1000e2a0), and a constant table to
 * blank the screen (0x10002480). Its fades to and from black are this
 * table and nothing else. */
static unsigned s_gamma_seen;
static void __stdcall g3_grLoadGammaTable(uint32_t n, const uint32_t *r,
                                          const uint32_t *g, const uint32_t *b)
{
    const uint32_t *src[3];
    uint32_t i, v;
    int c, identity = 1;

    if (!r || !g || !b || !n)
        return;
    if (!s_gamma_init)
        gamma_identity();
    vgl_log_capped(&s_gamma_seen, 1, "glide3: grLoadGammaTable(%u): r[0]=%u"
                   " r[mid]=%u r[last]=%u g[mid]=%u b[mid]=%u; applied as each"
                   " frame is presented", n, r[0], r[n / 2], r[n - 1],
                   g[n / 2], b[n / 2]);
    src[0] = r; src[1] = g; src[2] = b;
    if (n > 256)
        n = 256;
    for (c = 0; c < 3; c++)
        for (i = 0; i < n; i++) {
            v = src[c][i];
            vgl_gamma.lut[c][i] = (uint8_t)(v > 255u ? 255u : v);
        }
    for (c = 0; c < 3 && identity; c++)
        for (i = 0; i < 256; i++)
            if (vgl_gamma.lut[c][i] != i) {
                identity = 0;
                break;
            }
    vgl_gamma.on = !identity;
    vgl_gamma.gen++;
}

/* "Cgamma = [(Cfb/255)^(1/gamma)]*255", gamma in [0.0..20.0] (Guide, Gamma
 * Correction). Arctic calls it once, with 1.0 for all three: the identity. */
static void __stdcall g3_guGammaCorrectionRGB(float r, float g, float b)
{
    uint32_t t[3][256];
    float gam[3];
    int c, i;

    gam[0] = r; gam[1] = g; gam[2] = b;
    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++)
            t[c][i] = gam[c] > 0.0f
                ? (uint32_t)(powf((float)i / 255.0f, 1.0f / gam[c]) * 255.0f
                             + 0.5f)
                : (uint32_t)i;
    g3_grLoadGammaTable(256, t[0], t[1], t[2]);
}

static void __stdcall g3_grFinish(void)
{
}

/* With a field, the side strips belong to the picture only where geometry
 * reached them. Arctic clears to a colour (blue behind its full-screen 2D
 * pages, which cover exactly the field), and a clear alone counts as drawing
 * the strips (device.c), so they would show it; this swap counts only
 * vertices beyond the field, and the provider blacks strips nothing reached. */
static void __stdcall g3_grBufferSwap(uint32_t interval)
{
    if (s_origin_x > 0.0f)
        vgl.margin_drawn = s_outside;
    s_outside = 0;
    grBufferSwap(interval);
}

/* The surface, then the field it was given (s_origin_x). */
static uint32_t __stdcall g3_grSstWinOpen(uint32_t hwnd, uint32_t res,
                                          uint32_t refresh, uint32_t format,
                                          uint32_t origin, uint32_t ncolor,
                                          uint32_t naux)
{
    uint32_t r = (uint32_t)grSstWinOpen(hwnd, res, refresh, format, origin,
                                        ncolor, naux);

    s_origin_x = vgl_cfg.lfb_width > 0 ? (float)vgl.field_x0 : 0.0f;
    if (vgl_cfg.lfb_width > 0)
        vgl_log(0, "glide3: the game's %dx%d picture is drawn at x=%d in the"
                   " %d-wide raster", vgl_cfg.lfb_width, vgl_cfg.raster_height,
                vgl.field_x0, vgl_cfg.raster_width);
    return r;
}

/* A rectangle into the frame buffer, through this provider's own LFB lock, so
 * the capture records it as the lock and unlock it is. Arctic's one caller is
 * its performance graph (snow.exe 0x490cc6, Perf_RenderBlit, shown with
 * GAME_PERF_MONITOR=1): the back buffer, format 0, no pixel pipeline, a stride
 * of two bytes a pixel. Format 0 is RGB 565, the Glide 2 value: the Guide's
 * porting notes change this entry's arguments, not its formats (Table 11.5).
 * Only that format is copied; any other is refused and logged, as the Guide
 * allows ("will return FXFALSE if an invalid or unsupported operation is
 * attempted"). The origin is the upper left (Guide, grLfbWriteRegion), and a
 * negative stride walks the source upward (Figure 11.2). */
#define LFB_SRC_565 0u
static unsigned s_lfbw_seen;
static int __stdcall g3_grLfbWriteRegion(uint32_t dst, uint32_t x, uint32_t y,
                                         uint32_t fmt, uint32_t w, uint32_t h,
                                         uint32_t pipe, int32_t stride, void *data)
{
    uint8_t info[GR_LFBINFO_BYTES];
    uint8_t *base;
    uint32_t pitch, r, cw;

    if (fmt != LFB_SRC_565 || !data) {
        vgl_log_capped(&s_lfbw_seen, 4, "glide3: grLfbWriteRegion(buffer %u,"
                       " format %u, %ux%u) is not a format this translator"
                       " writes; refused", dst, fmt, w, h);
        return 0;
    }
    if (x >= (uint32_t)vgl.w || y >= (uint32_t)vgl.h || !w || !h)
        return 1;
    memset(info, 0, sizeof info);
    *(uint32_t *)(info + GR_LFBINFO_SIZE) = GR_LFBINFO_BYTES;
    if (!grLfbLock(GR_LFB_WRITE_ONLY, dst, VGL_LFBWRITE_565,
                   GR_ORIGIN_UPPER_LEFT, pipe,
                   (uint32_t)(uintptr_t)info))
        return 0;
    memcpy(&base, info + GR_LFBINFO_PTR, sizeof base);
    memcpy(&pitch, info + GR_LFBINFO_STRIDE, sizeof pitch);
    cw = w < (uint32_t)vgl.w - x ? w : (uint32_t)vgl.w - x;
    for (r = 0; r < h && y + r < (uint32_t)vgl.h; r++)
        memcpy(base + (size_t)(y + r) * pitch + (size_t)x * 2u,
               (const uint8_t *)data + (intptr_t)r * stride, (size_t)cw * 2u);
    grLfbUnlock(GR_LFB_WRITE_ONLY, dst);
    return 1;
}

/* ---- the table ---------------------------------------------------------- */

static const struct {
    const char *name;
    int argbytes;
    void *fn;
} k_g3[] = {
    { "grAlphaBlendFunction", 16, (void *)grAlphaBlendFunction },
    { "grAlphaCombine", 20, (void *)grAlphaCombine },
    { "grAlphaTestFunction", 4, (void *)grAlphaTestFunction },
    { "grAlphaTestReferenceValue", 4, (void *)grAlphaTestReferenceValue },
    { "grBufferClear", 12, (void *)grBufferClear },
    { "grBufferSwap", 4, (void *)g3_grBufferSwap },
    { "grChromakeyMode", 4, (void *)grChromakeyMode },
    { "grChromakeyValue", 4, (void *)grChromakeyValue },
    { "grColorCombine", 20, (void *)grColorCombine },
    { "grConstantColorValue", 4, (void *)grConstantColorValue },
    { "grCullMode", 4, (void *)grCullMode },
    { "grDepthBiasLevel", 4, (void *)grDepthBiasLevel },
    { "grDepthBufferFunction", 4, (void *)grDepthBufferFunction },
    { "grDepthBufferMode", 4, (void *)grDepthBufferMode },
    { "grDepthMask", 4, (void *)grDepthMask },
    { "grDrawTriangle", 12, (void *)g3_grDrawTriangle },
    { "grDrawVertexArray", 12, (void *)g3_grDrawVertexArray },
    { "grDrawVertexArrayContiguous", 16, (void *)g3_grDrawVertexArrayContiguous },
    { "grFinish", 0, (void *)g3_grFinish },
    { "grFogColorValue", 4, (void *)grFogColorValue },
    { "grFogMode", 4, (void *)g3_grFogMode },
    { "grFogTable", 4, (void *)grFogTable },
    { "grGet", 12, (void *)g3_grGet },
    { "grGlideInit", 0, (void *)grGlideInit },
    { "grGlideShutdown", 0, (void *)grGlideShutdown },
    { "grLfbReadRegion", 28, (void *)grLfbReadRegion },
    { "grLfbWriteRegion", 36, (void *)g3_grLfbWriteRegion },
    { "grLoadGammaTable", 16, (void *)g3_grLoadGammaTable },
    { "grSstSelect", 4, (void *)grSstSelect },
    { "grSstWinOpen", 28, (void *)g3_grSstWinOpen },
    { "grTexClampMode", 12, (void *)grTexClampMode },
    { "grTexCombine", 28, (void *)grTexCombine },
    { "grTexDownloadMipMap", 16, (void *)g3_grTexDownloadMipMap },
    { "grTexDownloadMipMapLevel", 32, (void *)g3_grTexDownloadMipMapLevel },
    { "grTexDownloadMipMapLevelPartial", 40, (void *)g3_grTexDownloadMipMapLevelPartial },
    { "grTexDownloadTable", 8, (void *)g3_grTexDownloadTable },
    { "grTexFilterMode", 12, (void *)grTexFilterMode },
    { "grTexLodBiasValue", 8, (void *)grTexLodBiasValue },
    { "grTexMaxAddress", 4, (void *)g3_grTexMaxAddress },
    { "grTexMinAddress", 4, (void *)g3_grTexMinAddress },
    { "grTexMipMapMode", 12, (void *)grTexMipMapMode },
    { "grTexSource", 16, (void *)g3_grTexSource },
    { "grTexTextureMemRequired", 8, (void *)g3_grTexTextureMemRequired },
    { "grVertexLayout", 12, (void *)g3_grVertexLayout },
    { "guGammaCorrectionRGB", 12, (void *)g3_guGammaCorrectionRGB },
};

/* The host binds each glide3x.dll import by its undecorated name and stack
 * bytes. Both must match, or the call would leave the stack unbalanced. */
__declspec(dllexport) void *__stdcall vgl3Lookup(const char *name, int argbytes)
{
    unsigned i;

    if (!vgl_ready)
        vgl_init();
    for (i = 0; name && i < sizeof k_g3 / sizeof k_g3[0]; i++)
        if (!strcmp(k_g3[i].name, name))
            return k_g3[i].argbytes == argbytes ? k_g3[i].fn : NULL;
    return NULL;
}
