/* raster.c -- triangle setup, span rasterisation, and pixel pipeline.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Named diagnostics retain the measured ITRGB and LOD-fraction alternatives.
 * Pipeline order is TMU1, TMU0, chroma key, combines, fog, tests, blend, and
 * write; depth may reject early without changing accepted pixels.
 */
#include <string.h>

#include "vgl.h"
#include "vcglide.h"

vgl_opt_t vgl_opt = { 0, 0, 1, 0, 1, -1 };

/* ---- small helpers ------------------------------------------------------ */

/* log2 to about a hundredth of a LOD: the exponent exactly, plus a cubic fit
 * over the mantissa. A LOD is only ever used to pick a level and to weigh a
 * cross-fade, and libm's log2f costs more than either is worth. */
static inline float fast_log2(float x)
{
    union { float f; uint32_t i; } u;
    float e, m;

    if (!(x > 0.0f))
        return -30.0f;
    u.f = x;
    e = (float)(int)((u.i >> 23) & 0xffu) - 127.0f;
    u.i = (u.i & 0x007fffffu) | 0x3f800000u;
    m = u.f;                            /* [1,2) */
    return e + ((-0.34484843f * m + 2.02466578f) * m - 1.67487759f);
}

/* The software surface is deliberately full colour. Glide still exposes its
 * documented 16-bit LFB formats at the API boundary (device.c), but triangle
 * output and destination blending stay at eight bits per channel. Calls to
 * grDitherMode are therefore accepted as state and have no effect. */
static inline uint32_t pack888(int r, int g, int b)
{
    return (uint32_t)((vgl_clamp8(r) << 16) |
                      (vgl_clamp8(g) << 8) | vgl_clamp8(b));
}

/* Implement floor and ceil without libm so renderer arithmetic remains on SSE
 * and cannot disturb the game's live x87 stack. */
static inline int ifloor(float x)
{
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}

static inline int iceil(float x)
{
    int i = (int)x;
    return (x > (float)i) ? i + 1 : i;
}

/* The W buffer. 1/w arrives in (0,1] and the value stored has to grow with
 * distance, so this is a four-bit octave index and a twelve-bit position
 * inside it: the shape of a hardware W buffer, chosen so that precision is
 * distributed the way W buffering exists to distribute it. It is not a claim
 * about the chip's encoding: nothing in a capture reveals that, and only the
 * ORDER of two depths can change a picture. */
static inline uint16_t wdepth(float oow)
{
    union { float f; uint32_t i; } u;
    int k, mant;

    if (!(oow > 0.0f))
        return 0xffff;
    if (oow >= 1.0f)
        return 0;
    u.f = oow;
    /* oow = m * 2^(e+1) with m in [0.5,1); k is the octave below 1. */
    k = 126 - (int)((u.i >> 23) & 0xffu);
    if (k > 15 || k < 0)
        return k < 0 ? (uint16_t)0 : (uint16_t)0xffff;
    /* The 23-bit mantissa runs 0..1 across the octave as the value runs
     * m=0.5..1, so the stored position is its complement, top 12 bits. */
    mant = 4095 - (int)((u.i & 0x007fffffu) >> 11);
    if (mant < 0) mant = 0;
    return (uint16_t)((k << 12) | mant);
}

/* Map reciprocal W into the 64-entry fog table with four interpolated entries
 * per octave. This derived mapping is monotonic but remains conformance-sensitive. */
static inline int fog_factor(float oow)
{
    union { float f; uint32_t i; } u;
    int k, idx, next, frac;
    float pos, t;

    if (!(oow > 0.0f))
        return vgl.fog_table[63];
    if (oow >= 1.0f)
        return vgl.fog_table[0];
    u.f = oow;
    k = 126 - (int)((u.i >> 23) & 0xffu);
    if (k < 0) return vgl.fog_table[0];
    if (k > 15) return vgl.fog_table[63];
    /* Within an octave, a larger mantissa is a larger 1/w (nearer), so the
     * position runs the other way. */
    t = 1.0f - (float)(u.i & 0x007fffffu) * (1.0f / 8388608.0f);
    pos = (float)k * 4.0f + t * 4.0f;
    idx = (int)pos;
    if (idx > 63) idx = 63;
    next = idx < 63 ? idx + 1 : 63;
    frac = (int)((pos - (float)idx) * 256.0f);
    if (frac < 0) frac = 0; else if (frac > 255) frac = 255;
    return vgl.fog_table[idx] +
           (((int)vgl.fog_table[next] - (int)vgl.fog_table[idx]) * frac >> 8);
}

/* factor/255 of a SIGNED delta. vgl_mul8 takes non-negative operands, and a
 * fog delta is a difference of two colours. */
static inline int lerp8(int fac, int delta)
{
    return delta >= 0 ? vgl_mul8(fac, delta) : -vgl_mul8(fac, -delta);
}

static inline int cmp_pass(int func, unsigned src, unsigned dst)
{
    switch (func) {
    case VGL_CMP_NEVER:    return 0;
    case VGL_CMP_LESS:     return src <  dst;
    case VGL_CMP_EQUAL:    return src == dst;
    case VGL_CMP_LEQUAL:   return src <= dst;
    case VGL_CMP_GREATER:  return src >  dst;
    case VGL_CMP_NOTEQUAL: return src != dst;
    case VGL_CMP_GEQUAL:   return src >= dst;
    default:               return 1;
    }
}

/* ---- the combine units --------------------------------------------------
 *
 * One function serves the colour unit, the alpha unit and both texture units,
 * because on the hardware it is one circuit: `factor * A + B` where the
 * FUNCTION chooses A from {0, other, other-local, -local} and B from
 * {0, local, local_alpha}, and an invert bit complements the result. Writing
 * it as ten cases would be ten places to get the same thing wrong. */
static inline int combine(int func, int fac, int other, int local,
                          int local_a, int invert)
{
    int a, b, out;

    switch (func) {
    case VGL_CF_ZERO:                                 a = 0;             b = 0;       break;
    case VGL_CF_LOCAL:                                a = 0;             b = local;   break;
    case VGL_CF_LOCAL_ALPHA:                          a = 0;             b = local_a; break;
    case VGL_CF_SCALE_OTHER:                          a = other;         b = 0;       break;
    case VGL_CF_SCALE_OTHER_ADD_LOCAL:                a = other;         b = local;   break;
    case VGL_CF_SCALE_OTHER_ADD_LOCAL_ALPHA:          a = other;         b = local_a; break;
    case VGL_CF_SCALE_OTHER_MINUS_LOCAL:              a = other - local; b = 0;       break;
    case VGL_CF_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL:    a = other - local; b = local;   break;
    case VGL_CF_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA:
                                                      a = other - local; b = local_a; break;
    case VGL_CF_SCALE_MINUS_LOCAL_ADD_LOCAL:          a = -local;        b = local;   break;
    case VGL_CF_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA:    a = -local;        b = local_a; break;
    default:                                          a = 0;             b = 0;       break;
    }
    out = (a >= 0 ? vgl_mul8(fac, a) : -vgl_mul8(fac, -a)) + b;
    out = vgl_clamp8(out);
    return invert ? 255 - out : out;
}

/* Resolve overloaded factor enumerants through explicit unit inputs. */
static inline int factor_of(int fac, int local, int local_a, int other_a,
                            int sel4, int sel5)
{
    int v;

    switch (fac & 7) {
    case 1: v = local;   break;
    case 2: v = other_a; break;
    case 3: v = local_a; break;
    case 4: v = sel4;    break;
    case 5: v = sel5;    break;
    default: v = 0;      break;
    }
    return (fac & VGL_FAC_ONE_MINUS) ? 255 - v : v;
}

/* ---- per-triangle setup ------------------------------------------------- */

/* Nine screen-linear interpolants: r, g, b, a, 1/w, and s/w, t/w for two
 * texture units. Colour is iterated in screen space rather than perspective
 * space, which is what the hardware does and is visible on large polygons. */
enum { IR, IG, IB, IA, IW, IS0, IT0, IS1, IT1, NINTERP };

typedef struct {
    float v0, dx, dy;
} plane_t;

typedef struct {
    plane_t  p[NINTERP];
    float    x0, y0;
    int      need_tex, need_tmu1;
    int      stw_shared;                /* TMU1's s,t are TMU0's */
    int      ninterp;                   /* how many of p[] the span steps */
} setup_t;

/* Snapshot immutable per-triangle device state once before the pixel loop to
 * avoid repeated global reloads across texture-sampler calls. */
typedef struct {
    uint32_t *cbuf;
    uint16_t *zbuf;
    unsigned  w;
    int depth_on, depth_func, depth_bias, depth_write;
    int alpha_test, alpha_func, alpha_ref;
    int fog_on, fog_r, fog_g, fog_b;
    int chroma_on, chroma_r, chroma_g, chroma_b;
    int blend_replace, blend_src, blend_dst;
    int write_color;
    int itrgb_on, itrgb_opt, lodfrac_opt, lodgrad_opt, lodf_pin;
    int lodclamp_opt;
    int lod_lo, lod_hi;                 /* the chain's servable LOD range */
    int cc_func, cc_factor, cc_local, cc_other, cc_invert;
    int ac_func, ac_factor, ac_local, ac_other, ac_invert;
    int t_c_func, t_c_factor, t_a_func, t_a_factor, t_c_invert, t_a_invert;
    int const_r, const_g, const_b, const_a;
} pipe_t;

static void pipe_resolve(pipe_t *pp)
{
    const vgl_tmu_t *u = &vgl.tmu[0];

    pp->cbuf = vgl.color[vgl.draw_buffer];
    pp->zbuf = vgl.depth;
    pp->w    = (unsigned)vgl.w;

    pp->depth_on    = vgl.depth_mode != VGL_DEPTH_DISABLE;
    pp->depth_func  = vgl.depth_func;
    pp->depth_bias  = vgl.depth_bias;
    pp->depth_write = pp->depth_on && vgl.depth_mask && vgl.depth != NULL;

    pp->alpha_test = vgl.alpha_func != VGL_CMP_ALWAYS;
    pp->alpha_func = vgl.alpha_func;
    pp->alpha_ref  = vgl.alpha_ref;

    pp->fog_on = (vgl.fog_mode & 7) == 2;
    pp->fog_r  = (int)((vgl.fog_color >> 16) & 0xff);
    pp->fog_g  = (int)((vgl.fog_color >> 8) & 0xff);
    pp->fog_b  = (int)(vgl.fog_color & 0xff);

    pp->chroma_on = vgl.chroma_mode;
    pp->chroma_r  = (int)((vgl.chroma_value >> 16) & 0xff);
    pp->chroma_g  = (int)((vgl.chroma_value >> 8) & 0xff);
    pp->chroma_b  = (int)(vgl.chroma_value & 0xff);

    pp->blend_src     = vgl.blend_rgb_src;
    pp->blend_dst     = vgl.blend_rgb_dst;
    pp->blend_replace = vgl.blend_rgb_src == VGL_BLEND_ONE &&
                        vgl.blend_rgb_dst == VGL_BLEND_ZERO;
    pp->write_color   = vgl.color_mask_rgb;

    pp->itrgb_on    = vgl.itrgb_lighting && vgl_opt.itrgb;
    pp->itrgb_opt   = vgl_opt.itrgb;
    pp->lodfrac_opt = vgl_opt.lodfrac;
    pp->lodgrad_opt = vgl_opt.lodgrad;
    pp->lodf_pin    = vgl_opt.lodf;
    pp->lodclamp_opt = vgl_opt.lodclamp;
    pp->lod_lo      = u->large_lod;
    pp->lod_hi      = u->small_lod;

    pp->cc_func = vgl.cc.func; pp->cc_factor = vgl.cc.factor;
    pp->cc_local = vgl.cc.local; pp->cc_other = vgl.cc.other;
    pp->cc_invert = vgl.cc.invert;
    pp->ac_func = vgl.ac.func; pp->ac_factor = vgl.ac.factor;
    pp->ac_local = vgl.ac.local; pp->ac_other = vgl.ac.other;
    pp->ac_invert = vgl.ac.invert;

    pp->t_c_func = u->c_func; pp->t_c_factor = u->c_factor;
    pp->t_a_func = u->a_func; pp->t_a_factor = u->a_factor;
    pp->t_c_invert = u->c_invert; pp->t_a_invert = u->a_invert;

    pp->const_r = (int)((vgl.constant_color >> 16) & 0xff);
    pp->const_g = (int)((vgl.constant_color >> 8) & 0xff);
    pp->const_b = (int)(vgl.constant_color & 0xff);
    pp->const_a = (int)((vgl.constant_color >> 24) & 0xff);

}

static void plane_make(plane_t *p, float a0, float a1, float a2,
                       float x0, float y0, float x1, float y1,
                       float x2, float y2, float inv_area)
{
    float d1 = a1 - a0, d2 = a2 - a0;
    float ex1 = x1 - x0, ey1 = y1 - y0, ex2 = x2 - x0, ey2 = y2 - y0;

    p->v0 = a0;
    p->dx = (d1 * ey2 - d2 * ey1) * inv_area;
    p->dy = (d2 * ex1 - d1 * ex2) * inv_area;
}

/* Does the current state read a texture at all? A frame of Hydro's HUD is
 * hundreds of untextured triangles and paying for two bilinear fetches on each
 * of them is pure loss. */
static int state_needs_texture(int *need_tmu1)
{
    int t0_uses_other;

    *need_tmu1 = 0;
    if (vgl.cc.other != VGL_OTHER_TEXTURE &&
        vgl.ac.other != VGL_OTHER_TEXTURE &&
        (vgl.cc.factor & 7) != VGL_FAC_TEXTURE_ALPHA &&
        (vgl.cc.factor & 7) != VGL_FAC_TEXTURE_RGB &&
        (vgl.ac.factor & 7) != VGL_FAC_TEXTURE_ALPHA &&
        !vgl.itrgb_lighting)
        return 0;

    /* TMU1 is only read when TMU0's own combine refers to `other`, which is
     * where TMU1's result arrives. */
    t0_uses_other = vgl.tmu[0].c_func >= VGL_CF_SCALE_OTHER &&
                    vgl.tmu[0].c_func <= VGL_CF_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA;
    *need_tmu1 = t0_uses_other && vgl.tmu[1].valid;
    return 1;
}

/* ---- the span loop ------------------------------------------------------ */

static void raster_span(const setup_t *su, const pipe_t *pp, vgl_stats_t *st,
                        int y, int xs, int xe)
{
    uint32_t *cp = pp->cbuf + (unsigned)y * pp->w + (unsigned)xs;
    uint16_t *zp = pp->zbuf ? pp->zbuf + (unsigned)y * pp->w + (unsigned)xs
                            : NULL;
    int x;
    float fx = (float)xs + 0.5f - su->x0, fy = (float)y + 0.5f - su->y0;
    float acc[NINTERP];
    float dx[NINTERP];
    float w_cur = 0.0f;
    int i, n = su->ninterp;
    /* Counters accumulate here and are added to the device once per span:
     * two 64-bit read-modify-writes per pixel are four instructions each on a
     * 32-bit target, and the totals they produce are identical. */
    uint64_t n_shaded = 0, n_written = 0;
    const int need_tex = su->need_tex, need_tmu1 = su->need_tmu1;
    const int depth_on = pp->depth_on, depth_write = pp->depth_write;

    if (!pp->cbuf)
        return;
    for (i = 0; i < n; i++) {
        acc[i] = su->p[i].v0 + su->p[i].dx * fx + su->p[i].dy * fy;
        dx[i]  = su->p[i].dx;
    }

    /* 1/w is only ever read to un-project s,t. An untextured span (the HUD,
     * the whole 2D layer, every flat-shaded polygon) pays a division per
     * pixel for a value nothing reads. */
    if (need_tex)
        w_cur = acc[IW] > 0.0f ? 1.0f / acc[IW] : 0.0f;

    for (x = xs; x < xe; x++, cp++, zp += (zp != NULL)) {
        float w_next = w_cur;
        int cr, cg, cb, ca;
        int tex_r = 255, tex_g = 255, tex_b = 255, tex_a = 255;
        int src_r, src_g, src_b, src_a;
        int c_local_r, c_local_g, c_local_b;
        uint16_t zsrc = 0;

        n_shaded++;

        /* Before the depth test, not after: a rejected pixel still has to
         * advance w for the next one, because the carried value is what a
         * non-positive 1/w falls back to. */
        if (need_tex) {
            float oow_next = acc[IW] + dx[IW];
            if (oow_next > 0.0f)
                w_next = 1.0f / oow_next;
        }

        /* Depth, early and as a rejection only. */
        if (depth_on) {
            int z = (int)wdepth(acc[IW]) + pp->depth_bias;
            if (z < 0) z = 0; else if (z > 0xffff) z = 0xffff;
            zsrc = (uint16_t)z;
            if (zp && !cmp_pass(pp->depth_func, zsrc, *zp))
                goto next;
        }

        cr = vgl_clamp8((int)(acc[IR] + 0.5f));
        cg = vgl_clamp8((int)(acc[IG] + 0.5f));
        cb = vgl_clamp8((int)(acc[IB] + 0.5f));
        ca = vgl_clamp8((int)(acc[IA] + 0.5f));

        if (need_tex) {
            float cw = w_cur;
            float cw_next = w_next;
            float s0 = acc[IS0] * cw, t0 = acc[IT0] * cw;
            float s0n = (acc[IS0] + dx[IS0]) * cw_next;
            float t0n = (acc[IT0] + dx[IT0]) * cw_next;
            float ds = s0n - s0, dt = t0n - t0;
            float lod;
            uint32_t c0, c1 = 0;
            int frac0 = 0, frac1 = 0;

            if (ds < 0.0f) ds = -ds;
            if (dt < 0.0f) dt = -dt;

            /* Estimate LOD from the maximum x/y texture-coordinate derivative.
             * Option 0 retains the x-only diagnostic. */
            if (pp->lodgrad_opt) {
                float oow_y = acc[IW] + su->p[IW].dy;
                float w_y = oow_y > 0.0f ? 1.0f / oow_y : w_cur;
                float ds_y = (acc[IS0] + su->p[IS0].dy) * w_y - s0;
                float dt_y = (acc[IT0] + su->p[IT0].dy) * w_y - t0;

                if (ds_y < 0.0f) ds_y = -ds_y;
                if (dt_y < 0.0f) dt_y = -dt_y;
                if (ds_y > ds) ds = ds_y;
                if (dt_y > dt) dt = dt_y;
            }
            lod = fast_log2(ds > dt ? ds : dt);

            c0 = vgl_tex_sample(0, s0, t0, lod, &frac0, st);
            if (need_tmu1) {
                float s1, t1;
                int lf;

                if (su->stw_shared) {
                    s1 = s0; t1 = t0;
                } else {
                    s1 = acc[IS1] * cw;
                    t1 = acc[IT1] * cw;
                }
                c1 = vgl_tex_sample(1, s1, t1, lod, &frac1, st);

                /* Sample TMU1 before TMU0. The measured default is the plain
                 * fractional LOD. Option 1 retains the old unit-local reading,
                 * complemented when TMU0 holds the odd half. */
                if (pp->lodfrac_opt) {
                    lf = frac0;
                    if (vgl.tmu[0].even_odd == GR_MIPMAPLEVELMASK_ODD)
                        lf = 255 - lf;
                } else if (pp->lodclamp_opt &&
                           lod <= (float)pp->lod_lo) {
                    /* Magnified past the sharpest level the chain holds. The
                     * answer is that level outright, and WHICH UNIT carries it
                     * is a parity question: this weight is 0 for the unit
                     * holding the EVEN half and 255 for the ODD one, so frac()
                     * -- which answers 0 at both ends -- is right only when
                     * that end's level is even. Unclamped, frac() of a
                     * negative LOD is a sawtooth instead, and fades a blurrier
                     * level in and out as the camera closes. */
                    lf = (pp->lod_lo & 1) ? 255 : 0;
                } else if (pp->lodclamp_opt &&
                           lod >= (float)pp->lod_hi) {
                    lf = (pp->lod_hi & 1) ? 255 : 0;   /* the blurriest end */
                } else {
                    /* Inside the chain: the measured plain fractional LOD,
                     * unchanged. */
                    lf = (int)((lod - (float)ifloor(lod)) * 255.0f);
                }
                if (pp->lodf_pin >= 0)
                    lf = pp->lodf_pin;          /* diagnostic pin; see vgl.h */
                {
                    int l_r = (int)((c0 >> 16) & 0xff);
                    int l_g = (int)((c0 >> 8) & 0xff);
                    int l_b = (int)(c0 & 0xff);
                    int l_a = (int)((c0 >> 24) & 0xff);
                    int o_r = (int)((c1 >> 16) & 0xff);
                    int o_g = (int)((c1 >> 8) & 0xff);
                    int o_b = (int)(c1 & 0xff);
                    int o_a = (int)((c1 >> 24) & 0xff);
                    int fr = factor_of(pp->t_c_factor, l_r, l_a, o_a, 255, lf);
                    int fg = factor_of(pp->t_c_factor, l_g, l_a, o_a, 255, lf);
                    int fb = factor_of(pp->t_c_factor, l_b, l_a, o_a, 255, lf);
                    int fa = factor_of(pp->t_a_factor, l_a, l_a, o_a, 255, lf);

                    tex_r = combine(pp->t_c_func, fr, o_r, l_r, l_a, pp->t_c_invert);
                    tex_g = combine(pp->t_c_func, fg, o_g, l_g, l_a, pp->t_c_invert);
                    tex_b = combine(pp->t_c_func, fb, o_b, l_b, l_a, pp->t_c_invert);
                    tex_a = combine(pp->t_a_func, fa, o_a, l_a, l_a, pp->t_a_invert);
                }
            } else {
                /* A single unit still passes through its own combine, because
                 * FUNCTION_LOCAL and FUNCTION_SCALE_OTHER are both in use and
                 * only one of them is the identity. */
                int l_r = (int)((c0 >> 16) & 0xff);
                int l_g = (int)((c0 >> 8) & 0xff);
                int l_b = (int)(c0 & 0xff);
                int l_a = (int)((c0 >> 24) & 0xff);

                if (pp->t_c_func == VGL_CF_LOCAL &&
                    pp->t_a_func == VGL_CF_LOCAL) {
                    tex_r = l_r; tex_g = l_g; tex_b = l_b; tex_a = l_a;
                } else {
                    /* With no second unit there is nothing on `other`, so the
                     * combine runs against zero rather than against a texel
                     * that is not there. */
                    int fr = factor_of(pp->t_c_factor, l_r, l_a, 0, 255, frac0);
                    int fg = factor_of(pp->t_c_factor, l_g, l_a, 0, 255, frac0);
                    int fb = factor_of(pp->t_c_factor, l_b, l_a, 0, 255, frac0);
                    int fa = factor_of(pp->t_a_factor, l_a, l_a, 0, 255, frac0);
                    tex_r = combine(pp->t_c_func, fr, 0, l_r, l_a, pp->t_c_invert);
                    tex_g = combine(pp->t_c_func, fg, 0, l_g, l_a, pp->t_c_invert);
                    tex_b = combine(pp->t_c_func, fb, 0, l_b, l_a, pp->t_c_invert);
                    tex_a = combine(pp->t_a_func, fa, 0, l_a, l_a, pp->t_a_invert);
                }
            }
        }

        /* ITRGB uses Atexture's MSB to select iterated or constant local colour.
         * Diagnostic options retain zero-gated and inverted readings. */
        if (pp->cc_local == VGL_LOCAL_CONSTANT) {
            c_local_r = pp->const_r;
            c_local_g = pp->const_g;
            c_local_b = pp->const_b;
        } else {
            c_local_r = cr; c_local_g = cg; c_local_b = cb;
        }
        if (pp->itrgb_on) {
            int msb = (tex_a & 0x80) != 0;

            if (pp->itrgb_opt == 1) {
                if (msb) {
                    c_local_r = pp->const_r;
                    c_local_g = pp->const_g;
                    c_local_b = pp->const_b;
                } else {
                    c_local_r = cr; c_local_g = cg; c_local_b = cb;
                }
            } else if (!msb) {
                if (pp->itrgb_opt == 3) {
                    c_local_r = pp->const_r;
                    c_local_g = pp->const_g;
                    c_local_b = pp->const_b;
                } else {
                    c_local_r = c_local_g = c_local_b = 0;
                }
            }
        }

        {
            int a_local = (pp->ac_local == VGL_LOCAL_CONSTANT)
                            ? pp->const_a : ca;
            int c_other_r, c_other_g, c_other_b, a_other;
            int fr, fg, fb, fa;

            switch (pp->cc_other) {
            case VGL_OTHER_TEXTURE:
                c_other_r = tex_r; c_other_g = tex_g; c_other_b = tex_b; break;
            case VGL_OTHER_CONSTANT:
                c_other_r = pp->const_r;
                c_other_g = pp->const_g;
                c_other_b = pp->const_b; break;
            case VGL_OTHER_ITERATED:
                c_other_r = cr; c_other_g = cg; c_other_b = cb; break;
            default:
                c_other_r = c_other_g = c_other_b = 0; break;
            }

            /* The SST-1/2 chroma-key unit compares the colour-combine
             * unit's `other` input before colour combination.  Comparing the
             * final, possibly fogged colour instead changes which texels are
             * discarded whenever the combine is not an identity. */
            if (pp->chroma_on &&
                c_other_r == pp->chroma_r &&
                c_other_g == pp->chroma_g &&
                c_other_b == pp->chroma_b)
                goto next;

            switch (pp->ac_other) {
            case VGL_OTHER_TEXTURE:  a_other = tex_a; break;
            case VGL_OTHER_CONSTANT: a_other = pp->const_a; break;
            case VGL_OTHER_ITERATED: a_other = ca; break;
            default:                 a_other = 0; break;
            }

            fr = factor_of(pp->cc_factor, c_local_r, a_local, a_other, tex_a, tex_r);
            fg = factor_of(pp->cc_factor, c_local_g, a_local, a_other, tex_a, tex_g);
            fb = factor_of(pp->cc_factor, c_local_b, a_local, a_other, tex_a, tex_b);
            fa = factor_of(pp->ac_factor, a_local,   a_local, a_other, tex_a, tex_a);

            src_r = combine(pp->cc_func, fr, c_other_r, c_local_r, a_local, pp->cc_invert);
            src_g = combine(pp->cc_func, fg, c_other_g, c_local_g, a_local, pp->cc_invert);
            src_b = combine(pp->cc_func, fb, c_other_b, c_local_b, a_local, pp->cc_invert);
            src_a = combine(pp->ac_func, fa, a_other,   a_local,   a_local, pp->ac_invert);
        }

        /* Fog, between the combine units and the tests, which is where the
         * hardware's fog unit sits. It changes colour and never alpha, so the
         * alpha test below cannot tell the difference. */
        if (pp->fog_on) {
            int f = fog_factor(acc[IW]);

            src_r = vgl_clamp8(src_r + lerp8(f, pp->fog_r - src_r));
            src_g = vgl_clamp8(src_g + lerp8(f, pp->fog_g - src_g));
            src_b = vgl_clamp8(src_b + lerp8(f, pp->fog_b - src_b));
        }

        if (pp->alpha_test &&
            !cmp_pass(pp->alpha_func, (unsigned)src_a,
                        (unsigned)pp->alpha_ref))
            goto next;

        /* Blend against the full-colour destination, which has no alpha.
         *
         * ONE/ZERO does not read the destination, and that is most of the
         * pixels in a frame: reading it anyway costs a load and an unpack on
         * every opaque pixel drawn. The unpack moved inside the else for the
         * same reason. */
        {
            int out_r, out_g, out_b;

            if (pp->blend_replace) {
                out_r = src_r; out_g = src_g; out_b = src_b;
            } else {
                uint32_t d = *cp;
                int dr = (int)((d >> 16) & 0xff),
                    dg = (int)((d >> 8) & 0xff), db = (int)(d & 0xff);
                int sf_r, sf_g, sf_b, df_r, df_g, df_b;

                switch (pp->blend_src) {
                case VGL_BLEND_ZERO:      sf_r = sf_g = sf_b = 0; break;
                case VGL_BLEND_SRC_ALPHA: sf_r = sf_g = sf_b = src_a; break;
                case VGL_BLEND_SRC_COLOR: sf_r = src_r; sf_g = src_g; sf_b = src_b; break;
                case VGL_BLEND_DST_ALPHA: sf_r = sf_g = sf_b = 255; break;
                case VGL_BLEND_ONE_MINUS_SRC_ALPHA: sf_r = sf_g = sf_b = 255 - src_a; break;
                case VGL_BLEND_ONE_MINUS_DST_ALPHA: sf_r = sf_g = sf_b = 0; break;
                case VGL_BLEND_DST_COLOR: sf_r = dr; sf_g = dg; sf_b = db; break;
                case VGL_BLEND_ONE_MINUS_DST_COLOR: sf_r = 255 - dr; sf_g = 255 - dg; sf_b = 255 - db; break;
                default:                  sf_r = sf_g = sf_b = 255; break;
                }
                switch (pp->blend_dst) {
                case VGL_BLEND_ZERO:      df_r = df_g = df_b = 0; break;
                case VGL_BLEND_SRC_ALPHA: df_r = df_g = df_b = src_a; break;
                case VGL_BLEND_SRC_COLOR: df_r = src_r; df_g = src_g; df_b = src_b; break;
                case VGL_BLEND_DST_ALPHA: df_r = df_g = df_b = 255; break;
                case VGL_BLEND_ONE_MINUS_SRC_ALPHA: df_r = df_g = df_b = 255 - src_a; break;
                case VGL_BLEND_ONE_MINUS_DST_ALPHA: df_r = df_g = df_b = 0; break;
                case VGL_BLEND_DST_COLOR: df_r = dr; df_g = dg; df_b = db; break;
                case VGL_BLEND_ONE_MINUS_DST_COLOR: df_r = 255 - dr; df_g = 255 - dg; df_b = 255 - db; break;
                default:                  df_r = df_g = df_b = 255; break;
                }
                out_r = vgl_mul8(src_r, sf_r) + vgl_mul8(dr, df_r);
                out_g = vgl_mul8(src_g, sf_g) + vgl_mul8(dg, df_g);
                out_b = vgl_mul8(src_b, sf_b) + vgl_mul8(db, df_b);
            }

            if (pp->write_color)
                *cp = pack888(out_r, out_g, out_b);
        }
        if (depth_write)
            *zp = zsrc;
        n_written++;

    next:
        for (i = 0; i < n; i++)
            acc[i] += dx[i];
        w_cur = w_next;
    }
    st->pixels  += n_shaded;
    st->written += n_written;
}

/* ---- the triangle ------------------------------------------------------- */

/* Everything a worker needs to rasterise SOME OF THE ROWS of one triangle.
 * It is read-only for the duration of the parallel section except for `st`,
 * of which each worker touches exactly its own entry. */
typedef struct {
    const setup_t *su;
    const pipe_t  *pipe;
    float x[3], y[3];
    int   clip_x0, clip_x1;
} trijob_t;

/* The workers' counters, and they live HERE rather than in the job because a
 * job is per triangle and there are two million of them: zeroing a per-job
 * block and summing it afterwards cost more than the counting did. They
 * accumulate across triangles and are folded into the device at the buffer
 * swap, which is also the only moment anything reads them. */
static vgl_stats_t s_wstat[VGL_PAR_MAX];

void vgl_raster_collect(void)
{
    int wk, k;

    for (wk = 0; wk < VGL_PAR_MAX; wk++) {
        vgl_stats_t *st = &s_wstat[wk];

        if (!st->pixels && !st->spans)
            continue;
        vgl.n_pixels += st->pixels;
        vgl.n_pixels_written += st->written;
        vgl.n_spans += st->spans;
        for (k = 0; k < VGL_MAX_LOD + 3; k++) vgl_lod_hist[k] += st->lod[k];
        for (k = 0; k < VGL_MAX_LOD + 1; k++) vgl_level_hist[k] += st->level[k];
        memset(st, 0, sizeof *st);
    }
}

/* A triangle's rows [y0, y1). Called once on the submitting thread for a
 * small triangle, and once per worker with a slice of the rows for a big one.
 * Nothing in here reads mutable device state (that is what pipe_t is for)
 * so two workers in it at once cannot interact. */
static void tri_rows(void *ctx, int worker, int y0, int y1)
{
    trijob_t *j = (trijob_t *)ctx;
    vgl_stats_t *st = &s_wstat[worker];
    int py;

    for (py = y0; py < y1; py++) {
        float sy = (float)py + 0.5f;
        float xs = (float)j->clip_x1, xe = (float)j->clip_x0;
        int e, span_s, span_e;

        /* Intersect the scanline with the three edges. Two crossings bound
         * the span; a horizontal edge contributes none and needs none. */
        for (e = 0; e < 3; e++) {
            int a = e, b = (e + 1) % 3;
            float ya = j->y[a], yb = j->y[b], t, cx;

            if ((sy < ya && sy < yb) || (sy >= ya && sy >= yb))
                continue;
            t = (sy - ya) / (yb - ya);
            cx = j->x[a] + (j->x[b] - j->x[a]) * t;
            if (cx < xs) xs = cx;
            if (cx > xe) xe = cx;
        }
        if (xe <= xs)
            continue;

        /* Pixel centres: a pixel is covered when its centre is inside, which
         * is the ordinary convention and makes a shared edge draw once. */
        span_s = iceil(xs - 0.5f);
        span_e = iceil(xe - 0.5f);
        if (span_s < j->clip_x0) span_s = j->clip_x0;
        if (span_e > j->clip_x1) span_e = j->clip_x1;
        if (span_e <= span_s)
            continue;
        st->spans++;
        raster_span(j->su, j->pipe, st, py, span_s, span_e);
    }
}

static void cpu_draw_triangle(const float *va, const float *vb, const float *vc)
{
    float x[3], y[3];
    const float *v[3];
    setup_t su;
    pipe_t pipe;
    trijob_t job;
    float area, inv_area;
    int i, ymin, ymax;

    if (!vgl.open || !vgl.color[vgl.draw_buffer])
        return;
    vgl.n_tris++;
    v[0] = va; v[1] = vb; v[2] = vc;
    for (i = 0; i < 3; i++) {
        x[i] = v[i][0];
        /* Map lower-left vertex coordinates to top-down sample positions. */
        y[i] = vgl.origin_lower_left ? ((float)vgl.h - v[i][1]) : v[i][1];
    }

    area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (area == 0.0f || !(area == area)) {
        vgl.n_tris_culled++;
        return;
    }
    if (area < 0.0f) {                  /* one winding from here on */
        const float *t = v[1]; v[1] = v[2]; v[2] = t;
        { float tf = x[1]; x[1] = x[2]; x[2] = tf; }
        { float tf = y[1]; y[1] = y[2]; y[2] = tf; }
        area = -area;
    }
    inv_area = 1.0f / area;

    su.x0 = x[0];
    su.y0 = y[0];
#define MK(slot, comp) plane_make(&su.p[slot], v[0][comp], v[1][comp], \
                                  v[2][comp], x[0], y[0], x[1], y[1], \
                                  x[2], y[2], inv_area)
    MK(IR, 3); MK(IG, 4); MK(IB, 5); MK(IA, 7); MK(IW, 8);
    MK(IS0, 9); MK(IT0, 10); MK(IS1, 12); MK(IT1, 13);
#undef MK

    su.need_tex = state_needs_texture(&su.need_tmu1);
    su.stw_shared = (v[0][9] == v[0][12] && v[0][10] == v[0][13] &&
                     v[1][9] == v[1][12] && v[1][10] == v[1][13] &&
                     v[2][9] == v[2][12] && v[2][10] == v[2][13]);
    /* The interpolants are ordered so that what a triangle needs is a PREFIX:
     * colour and 1/w always, TMU0's s,t when it is textured, TMU1's only when
     * the second unit is read with coordinates of its own. Stepping nine
     * planes down an untextured span is four floating-point adds per pixel
     * spent on values nothing reads. */
    su.ninterp = !su.need_tex ? IS0
               : (su.need_tmu1 && !su.stw_shared) ? NINTERP : IS1;
    pipe_resolve(&pipe);

    ymin = ifloor(y[0] < y[1] ? (y[0] < y[2] ? y[0] : y[2])
                              : (y[1] < y[2] ? y[1] : y[2]));
    ymax = iceil(y[0] > y[1] ? (y[0] > y[2] ? y[0] : y[2])
                             : (y[1] > y[2] ? y[1] : y[2]));
    if (ymin < vgl.clip_y0) ymin = vgl.clip_y0;
    if (ymax > vgl.clip_y1) ymax = vgl.clip_y1;

    job.su = &su; job.pipe = &pipe;
    for (i = 0; i < 3; i++) { job.x[i] = x[i]; job.y[i] = y[i]; }
    job.clip_x0 = vgl.clip_x0; job.clip_x1 = vgl.clip_x1;

    /* Parallelize tall triangles by independent row ranges. */
    if (ymax - ymin >= VGL_PAR_MIN_ROWS && vgl_par_workers() > 1)
        vgl_par_rows(tri_rows, &job, ymin, ymax);
    else
        tri_rows(&job, 0, ymin, ymax);
}

void vgl_draw_triangle(const float *va, const float *vb, const float *vc)
{
    vgl.frame_drawn = 1;
    /* Did this primitive reach outside the game's native field? That one bit
     * is what lets an LFB page know whether the side strips are its problem
     * (vgl.h). A bounding test on the three x's is enough: a triangle wholly
     * inside the field cannot paint a margin pixel. */
    if ((!vgl.margin_drawn || !vgl.margin_geom) &&
        (va[0] < (float)vgl.field_x0 || va[0] >= (float)vgl.field_x1 ||
         vb[0] < (float)vgl.field_x0 || vb[0] >= (float)vgl.field_x1 ||
         vc[0] < (float)vgl.field_x0 || vc[0] >= (float)vgl.field_x1)) {
        vgl.margin_drawn = 1;
        /* `margin_drawn` is also set by a buffer clear, so it cannot answer
         * "did GEOMETRY paint the strips this frame". Both flags together
         * can, and the pair still short-circuits once both are set. */
        vgl.margin_geom = 1;
    }
    if (vgl.gpu)
        vgl_gpu_draw_triangle(va, vb, vc);
    else
        cpu_draw_triangle(va, vb, vc);
}

void vgl_draw_vertex_list(int planar, int count, const float *verts)
{
    int i;

    (void)planar;                       /* a planar polygon is still a fan */
    if (count < 3 || !verts)
        return;
    for (i = 1; i + 1 < count; i++)
        vgl_draw_triangle(verts,
                          verts + (size_t)i * 15,
                          verts + (size_t)(i + 1) * 15);
}

/* grDrawLine / grAADrawLine. Hydro calls neither in 1,500 frames and Offroad
 * calls grDrawLine 0 times in its boot capture, so this is a one-pixel-wide
 * Bresenham through the same pixel pipeline as a degenerate triangle rather
 * than the hardware's line rasteriser, and it says so. */
void vgl_draw_line(const float *a, const float *b)
{
    static int announced;

    if (!announced) {
        announced = 1;
        vgl_log(0, "raster: grDrawLine used; no capture of either game "
                   "contains one; lines are drawn as thin quads");
    }
    if (a && b) {
        float q[3][15];
        memcpy(q[0], a, sizeof q[0]);
        memcpy(q[1], b, sizeof q[1]);
        memcpy(q[2], b, sizeof q[2]);
        q[2][1] += 1.0f;
        vgl_draw_triangle(q[0], q[1], q[2]);
    }
}
