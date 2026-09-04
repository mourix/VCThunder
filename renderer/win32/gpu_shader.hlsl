/* gpu_shader.hlsl -- SDL GPU implementation of vcglide's measured pipeline.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * This is project-authored HLSL compiled to DXBC by D3DCompiler during
 * vglInit, before the host maps the game image. SDL_GPU consumes that DXBC on
 * its Direct3D 12 backend. The C side deliberately sends decoded RGBA texture
 * levels, so Glide texture-memory formats continue to have one authority:
 * core/tex.c.
 */

/* Consecutive triangles which share resolved GPU state are submitted as one
 * draw. 64 is a command-amortisation capacity, not a Glide constant: the
 * measured Offroad race trace has runs as long as 240 triangles, while its
 * mean strict run is 6.52. The C side pushes only the vectors actually used. */
#define GPU_BATCH_TRIANGLES 64

cbuffer VSData : register(b0, space1)
{
    float4 vd[GPU_BATCH_TRIANGLES * 12];
};

cbuffer VSSurface : register(b1, space1)
{
    float4 vs_surface;
};

cbuffer PSData : register(b0, space3)
{
    uint4 pd[20];
};

Texture2D tex0 : register(t0, space2);
SamplerState smp0 : register(s0, space2);
Texture2D tex1 : register(t1, space2);
SamplerState smp1 : register(s1, space2);

struct MainOut
{
    float4 pos   : SV_Position;
    float4 color : TEXCOORD0;
    float  oow   : TEXCOORD1;
    float4 stw   : TEXCOORD2;
    nointerpolation uint3 dynamic_state : TEXCOORD3;
};

MainOut main_vs(uint id : SV_VertexID)
{
    MainOut o;
    float4 a = vd[id * 4 + 0];
    float4 b = vd[id * 4 + 1];
    float4 c = vd[id * 4 + 2];
    float4 d = vd[id * 4 + 3];
    float2 surface = vs_surface.xy;

    o.pos = float4(a.x * (2.0 / surface.x) - 1.0,
                   1.0 - a.y * (2.0 / surface.y), 0.0, 1.0);
    o.color = float4(a.z, a.w, b.x, b.y);
    o.oow = b.z;
    o.stw = float4(b.w, c.x, c.y, c.z);
    o.dynamic_state = asuint(d.xyz);
    return o;
}

struct CopyOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

CopyOut copy_vs(uint id : SV_VertexID)
{
    CopyOut o;
    float2 xy;
    if (id == 0) xy = float2(vd[0].x, vd[0].y);
    else if (id == 1) xy = float2(vd[0].z, vd[0].y);
    else if (id == 2) xy = float2(vd[0].x, vd[0].w);
    else xy = float2(vd[0].z, vd[0].w);
    o.uv = float2(id == 1 || id == 3, id >= 2);
    o.pos = float4(xy.x * 2.0 - 1.0, 1.0 - xy.y * 2.0, 0.0, 1.0);
    return o;
}

uint mul8(uint a, uint b)
{
    uint p = a * b;
    return (p + 128 + ((p + 128) >> 8)) >> 8;
}

int combine_i(uint fn, uint fac, int other, int local, int local_a, uint inv)
{
    int a = 0, b = 0;
    if (fn == 1) b = local;
    else if (fn == 2) b = local_a;
    else if (fn == 3) a = other;
    else if (fn == 4) { a = other; b = local; }
    else if (fn == 5) { a = other; b = local_a; }
    else if (fn == 6) a = other - local;
    else if (fn == 7) { a = other - local; b = local; }
    else if (fn == 8) { a = other - local; b = local_a; }
    else if (fn == 9) { a = -local; b = local; }
    else if (fn == 10) { a = -local; b = local_a; }
    int scaled = a >= 0 ? int(mul8(fac, uint(a))) : -int(mul8(fac, uint(-a)));
    int outv = clamp(scaled + b, 0, 255);
    return inv != 0 ? 255 - outv : outv;
}

uint factor_i(uint fac, int local, int local_a, int other_a,
              int sel4, int sel5)
{
    int v = 0;
    uint sel = fac & 7;
    if (sel == 1) v = local;
    else if (sel == 2) v = other_a;
    else if (sel == 3) v = local_a;
    else if (sel == 4) v = sel4;
    else if (sel == 5) v = sel5;
    return uint((fac & 8) != 0 ? 255 - v : v);
}

bool cmp_i(uint fn, uint src, uint dst)
{
    if (fn == 0) return false;
    if (fn == 1) return src < dst;
    if (fn == 2) return src == dst;
    if (fn == 3) return src <= dst;
    if (fn == 4) return src > dst;
    if (fn == 5) return src != dst;
    if (fn == 6) return src >= dst;
    return true;
}

uint first_level(uint mask)
{
    [unroll] for (uint i = 0; i <= 8; ++i)
        if ((mask & (1u << i)) != 0) return i;
    return 0;
}

/* The nearest level this unit HOLDS to a requested one, with the tie going to
 * the sharper level -- which is what select_level's strict `<` does on the
 * reference side. A forced level may be absent from one half of a split
 * chain, which is the whole reason this is not `want`. */
uint forced_level(uint mask, uint want)
{
    uint best = 99, level = first_level(mask);
    [unroll] for (uint i = 0; i <= 8; ++i) {
        if ((mask & (1u << i)) != 0) {
            uint d = i > want ? i - want : want - i;
            if (d < best) { best = d; level = i; }
        }
    }
    return level;
}

uint nearest_level(float lod, uint mask, uint large_lod, uint small_lod,
                   bool mipmap, out uint frac)
{
    /* FORCELOD: always the sharpest level the unit holds, or, above one, the
     * diagnostic level N-1. Mipmapping being disabled asks for the same thing.
     * tex.c select_level is the authority and this mirrors it. */
    uint forcelod = pd[18].x;
    if (forcelod != 0 || !mipmap) {
        frac = 0;
        return forced_level(mask, min(forcelod > 1u ? forcelod - 1u
                                                    : first_level(mask), 8u));
    }
    lod = clamp(lod, float(large_lod), float(small_lod));
    float best = 1.0e30;
    uint level = first_level(mask);
    [unroll] for (uint i = 0; i <= 8; ++i) {
        if ((mask & (1u << i)) != 0) {
            float d = abs(lod - float(i));
            if (d < best) { best = d; level = i; }
        }
    }
    frac = uint(clamp(best, 0.0, 1.0) * 255.0 + 0.5);
    return level;
}

struct TexResult { uint4 c; uint frac; };

TexResult sample_unit(uint unit, float2 st, float lod)
{
    TexResult r;
    uint base = unit == 0 ? 8 : 11;
    uint4 a = pd[base + 0]; /* valid, level mask, large, small */
    uint4 b = pd[base + 1]; /* parity, mip mode, lod0 w, lod0 h */
    float bias = asfloat(pd[base + 2].x);
    if (a.x == 0 || a.y == 0) {
        r.c = uint4(255, 255, 255, 255); r.frac = 0; return r;
    }
    uint frac;
    uint level = nearest_level(lod + bias, a.y, a.z, a.w, b.y != 0, frac);
    float2 uv = st / float2(max(b.z, 1u), max(b.w, 1u));
    float4 c = unit == 0 ? tex0.SampleLevel(smp0, uv, float(level))
                         : tex1.SampleLevel(smp1, uv, float(level));
    r.c = uint4(clamp(c * 255.0 + 0.5, 0.0, 255.0));
    r.frac = frac;
    return r;
}

uint wdepth_i(float oow)
{
    if (!(oow > 0.0)) return 65535;
    if (oow >= 1.0) return 0;
    uint bits = asuint(oow);
    int k = 126 - int((bits >> 23) & 255);
    if (k < 0) return 0;
    if (k > 15) return 65535;
    int mant = 4095 - int((bits & 0x007fffff) >> 11);
    return uint((k << 12) | max(mant, 0));
}

uint fog_byte(uint idx)
{
    uint word = pd[14 + idx / 16][(idx / 4) & 3];
    return (word >> ((idx & 3) * 8)) & 255;
}

uint fog_factor_i(float oow)
{
    if (!(oow > 0.0)) return fog_byte(63);
    if (oow >= 1.0) return fog_byte(0);
    uint bits = asuint(oow);
    int k = 126 - int((bits >> 23) & 255);
    if (k < 0) return fog_byte(0);
    if (k > 15) return fog_byte(63);
    float t = 1.0 - float(bits & 0x007fffff) / 8388608.0;
    float pos = float(k) * 4.0 + t * 4.0;
    uint i = min(uint(pos), 63u);
    uint n = min(i + 1, 63u);
    uint f = uint(clamp((pos - float(i)) * 256.0, 0.0, 255.0));
    int d = int(fog_byte(n)) - int(fog_byte(i));
    return uint(clamp(int(fog_byte(i)) + ((d * int(f)) >> 8), 0, 255));
}

struct MainPSOut
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

MainPSOut main_ps(MainOut i)
{
    MainPSOut o;
    uint4 f0 = pd[0]; /* need tex, need tmu1, itrgb option, lodfrac option */
    uint4 f1 = pd[1]; /* lodf+1, alpha function, alpha ref, alpha test */
    uint4 cc0 = pd[2];
    uint4 ac0 = pd[3];
    uint4 mix0 = pd[4];
    uint4 tc0 = pd[5];
    uint constant_color = i.dynamic_state.x;
    uint chroma_on = pd[6].y, chroma_color = pd[6].z, fog_on = pd[6].w;
    uint fog_color = pd[7].x;
    int depth_bias = asint(i.dynamic_state.z);
    f1.z = i.dynamic_state.y;
    uint4 iter = uint4(clamp(i.color + 0.5, 0.0, 255.0));
    uint4 tex = uint4(255, 255, 255, 255);

    if (f0.x != 0) {
        float invw = i.oow > 0.0 ? rcp(i.oow) : 0.0;
        float2 st0 = i.stw.xy * invw;
        float2 dxv = ddx(st0), dyv = ddy(st0);
        /* LOD from the maximum x/y texture-coordinate derivative. LODGRAD 0
         * retains the x-only diagnostic, as raster.c does. */
        float footprint = max(abs(dxv.x), abs(dxv.y));
        if (pd[7].w != 0)
            footprint = max(footprint, max(abs(dyv.x), abs(dyv.y)));
        float lod = log2(max(footprint, 1.0e-20));
        TexResult t0 = sample_unit(0, st0, lod);
        if (f0.y != 0) {
            float2 st1 = i.stw.zw * invw;
            TexResult t1 = sample_unit(1, st1, lod);
            /* Inside the chain: the measured plain fractional LOD. Past
             * either end the answer is one present level outright, and which
             * unit carries it is a parity question -- this weight is 0 for the
             * unit holding the EVEN half and 255 for the ODD one. frac()
             * answers 0 at both ends, which is right only when that end's
             * level is even, and unclamped it is a sawtooth under
             * magnification. pd[8] is TMU0's declared range; both halves of a
             * split chain are downloaded from one GrTexInfo. */
            uint lo = pd[8].z, hi = pd[8].w;
            uint lf;
            if (pd[7].z != 0 && lod <= float(lo))
                lf = (lo & 1u) != 0u ? 255u : 0u;
            else if (pd[7].z != 0 && lod >= float(hi))
                lf = (hi & 1u) != 0u ? 255u : 0u;
            else
                lf = uint(frac(lod) * 255.0);
            if (f0.w != 0) {
                lf = t0.frac;
                if (pd[9].x == 2) lf = 255 - lf; /* ODD half on TMU0 */
            }
            if (f1.x != 0) lf = f1.x - 1;
            uint4 fac;
            fac.r = factor_i(mix0.w, t0.c.r, t0.c.a, t1.c.a, 255, lf);
            fac.g = factor_i(mix0.w, t0.c.g, t0.c.a, t1.c.a, 255, lf);
            fac.b = factor_i(mix0.w, t0.c.b, t0.c.a, t1.c.a, 255, lf);
            fac.a = factor_i(tc0.y, t0.c.a, t0.c.a, t1.c.a, 255, lf);
            tex.r = combine_i(mix0.z, fac.r, t1.c.r, t0.c.r, t0.c.a, tc0.z);
            tex.g = combine_i(mix0.z, fac.g, t1.c.g, t0.c.g, t0.c.a, tc0.z);
            tex.b = combine_i(mix0.z, fac.b, t1.c.b, t0.c.b, t0.c.a, tc0.z);
            tex.a = combine_i(tc0.x, fac.a, t1.c.a, t0.c.a, t0.c.a, tc0.w);
        } else if (mix0.z == 1 && tc0.x == 1) {
            tex = t0.c;
        } else {
            uint4 fac;
            fac.r = factor_i(mix0.w, t0.c.r, t0.c.a, 0, 255, t0.frac);
            fac.g = factor_i(mix0.w, t0.c.g, t0.c.a, 0, 255, t0.frac);
            fac.b = factor_i(mix0.w, t0.c.b, t0.c.a, 0, 255, t0.frac);
            fac.a = factor_i(tc0.y, t0.c.a, t0.c.a, 0, 255, t0.frac);
            tex.r = combine_i(mix0.z, fac.r, 0, t0.c.r, t0.c.a, tc0.z);
            tex.g = combine_i(mix0.z, fac.g, 0, t0.c.g, t0.c.a, tc0.z);
            tex.b = combine_i(mix0.z, fac.b, 0, t0.c.b, t0.c.a, tc0.z);
            tex.a = combine_i(tc0.x, fac.a, 0, t0.c.a, t0.c.a, tc0.w);
        }
    }

    uint4 k = uint4((constant_color >> 16) & 255,
                    (constant_color >> 8) & 255,
                    constant_color & 255, constant_color >> 24);
    uint3 localc = cc0.z == 1 ? k.rgb : iter.rgb;
    /* grAlphaControlsITRGBLighting overrides Clocal outright: MSB of Atexture
     * 0 -> iterated RGB, 1 -> grConstantColorValue (the published semantic,
     * option 1).  Option 3 is that inverted, option 2 zeroes the iterated
     * colour.  Kept identical to raster.c's copy in renderer/core. */
    if (f0.z == 1)
        localc = (tex.a & 128) != 0 ? k.rgb : iter.rgb;
    else if (f0.z != 0 && (tex.a & 128) == 0)
        localc = f0.z == 3 ? k.rgb : uint3(0, 0, 0);
    uint alocal = ac0.w == 1 ? k.a : iter.a;
    uint3 otherc = cc0.w == 1 ? tex.rgb :
                   cc0.w == 2 ? k.rgb :
                   cc0.w == 0 ? iter.rgb : uint3(0, 0, 0);
    if (chroma_on != 0 && otherc.r == ((chroma_color >> 16) & 255) &&
        otherc.g == ((chroma_color >> 8) & 255) &&
        otherc.b == (chroma_color & 255)) discard;
    uint aother = mix0.x == 1 ? tex.a : mix0.x == 2 ? k.a :
                  mix0.x == 0 ? iter.a : 0;
    uint4 fac;
    fac.r = factor_i(cc0.y, localc.r, alocal, aother, tex.a, tex.r);
    fac.g = factor_i(cc0.y, localc.g, alocal, aother, tex.a, tex.g);
    fac.b = factor_i(cc0.y, localc.b, alocal, aother, tex.a, tex.b);
    fac.a = factor_i(ac0.z, alocal, alocal, aother, tex.a, tex.a);
    uint4 src;
    src.r = combine_i(cc0.x, fac.r, otherc.r, localc.r, alocal, ac0.x);
    src.g = combine_i(cc0.x, fac.g, otherc.g, localc.g, alocal, ac0.x);
    src.b = combine_i(cc0.x, fac.b, otherc.b, localc.b, alocal, ac0.x);
    src.a = combine_i(ac0.y, fac.a, aother, alocal, alocal, mix0.y);

    if (fog_on != 0) {
        uint ff = fog_factor_i(i.oow);
        uint3 fc = uint3((fog_color >> 16) & 255,
                         (fog_color >> 8) & 255, fog_color & 255);
        src.rgb = uint3((int3(src.rgb) +
            int3((int3(fc) - int3(src.rgb)) * int(ff) / 255)));
    }
    if (f1.w != 0 && !cmp_i(f1.y, src.a, f1.z)) discard;

    int zd = int(wdepth_i(i.oow)) + depth_bias;
    o.depth = float(clamp(zd, 0, 65535)) / 65535.0;
    o.color = float4(src) / 255.0;
    return o;
}

float4 overlay_ps(CopyOut i) : SV_Target0
{
    float4 c = tex0.SampleLevel(smp0, i.uv, 0.0);
    if (c.a < 0.5) discard;
    return float4(c.rgb, 1.0);
}

struct ClearOut
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

ClearOut clear_ps(CopyOut i)
{
    ClearOut o;
    /* The interpolant stays in the signature so this stage pairs with copy_vs;
     * the clear value itself is uniform across the rectangle. */
    uint c = pd[0].x;
    o.color = float4(float((c >> 16) & 255), float((c >> 8) & 255),
                     float(c & 255), 255.0) / 255.0;
    o.depth = float(pd[0].y & 65535) / 65535.0;
    return o;
}
