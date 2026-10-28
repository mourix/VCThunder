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
 * the sharper level, which is what select_level's strict `<` does on the
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

/* Catmull-Rom from nine bilinear taps (the two middle taps of each axis
 * merged into one, their weights being both positive), so the sampler's own
 * address mode holds at the texture's edges exactly as for the plain tap. */
float3 catmull_rom_tex(Texture2D t, SamplerState s, float2 uv, float2 size,
                       float level)
{
    float2 p = uv * size;
    float2 p1 = floor(p - 0.5) + 0.5;
    float2 f = p - p1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 p0 = (p1 - 1.0) / size, p3 = (p1 + 2.0) / size;
    float2 p12 = (p1 + w2 / w12) / size;
    float3 c = 0.0;
    c += (t.SampleLevel(s, float2(p0.x,  p0.y), level).rgb * w0.x +
          t.SampleLevel(s, float2(p12.x, p0.y), level).rgb * w12.x +
          t.SampleLevel(s, float2(p3.x,  p0.y), level).rgb * w3.x) * w0.y;
    c += (t.SampleLevel(s, float2(p0.x,  p12.y), level).rgb * w0.x +
          t.SampleLevel(s, float2(p12.x, p12.y), level).rgb * w12.x +
          t.SampleLevel(s, float2(p3.x,  p12.y), level).rgb * w3.x) * w12.y;
    c += (t.SampleLevel(s, float2(p0.x,  p3.y), level).rgb * w0.x +
          t.SampleLevel(s, float2(p12.x, p3.y), level).rgb * w12.x +
          t.SampleLevel(s, float2(p3.x,  p3.y), level).rgb * w3.x) * w3.y;
    return saturate(c);
}

TexResult sample_unit(uint unit, float2 st, float lod, bool flat)
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
    /* Magnified 3D only, and only its colour: alpha stays bilinear, so
     * coverage, the alpha test and blending see what they saw. The weight
     * rises from bilinear at 1:1 to Catmull-Rom at 2x, so no contour marks
     * where a surface crosses 1:1. Flat-w primitives are 2D and keep their
     * texels; the 2D layer has its own magnifier. */
    float m = float(level) - (lod + bias);
    if (pd[base + 2].y != 0 && !flat && m > 0.0) {
        float2 size = float2(max(b.z >> level, 1u), max(b.w >> level, 1u));
        float3 cr = unit == 0
            ? catmull_rom_tex(tex0, smp0, uv, size, float(level))
            : catmull_rom_tex(tex1, smp1, uv, size, float(level));
        c.rgb = lerp(c.rgb, cr, saturate(m));
    }
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
        bool flat = ddx(i.oow) == 0.0 && ddy(i.oow) == 0.0;
        TexResult t0 = sample_unit(0, st0, lod, flat);
        if (f0.y != 0) {
            float2 st1 = i.stw.zw * invw;
            TexResult t1 = sample_unit(1, st1, lod, flat);
            /* Inside the chain: the measured plain fractional LOD. Past
             * either end the answer is one present level outright, and which
             * unit carries it is a parity question; this weight is 0 for the
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
    /* Alpha reaches only the 2D layer, where it is coverage: a draw that
     * replaces (ONE/ZERO) covers whatever its own alpha says. */
    if (pd[18].z != 0)
        o.color.a = 1.0;
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

/* A premultiplied layer laid over the target, texel for pixel: pd[0].xy is
 * the source offset. The pipeline blends ONE / ONE_MINUS_SRC_ALPHA. */
float4 over_ps(CopyOut i) : SV_Target0
{
    int2 p = int2(i.pos.xy) + asint(pd[0].xy);
    return tex0.Load(int3(p, 0));
}

/* The display's gamma table, as the DAC applies it on the way out: tex0 the
 * display, texel for pixel; tex1 the table, 256x1, entry i of each channel at
 * x = i (gpu.c, gamma_apply). The display is 8 bits a channel, so the index is
 * exact. */
float4 gamma_ps(CopyOut i) : SV_Target0
{
    float4 c = tex0.Load(int3(int2(i.pos.xy), 0));
    uint3 k = uint3(c.rgb * 255.0 + 0.5);
    return float4(tex1.Load(int3(k.r, 0, 0)).r, tex1.Load(int3(k.g, 0, 0)).g,
                  tex1.Load(int3(k.b, 0, 0)).b, 1.0);
}

/* Under a bezel, the presented picture and the art over it: one filtered
 * sample, the art premultiplied with its mips; the pipeline blends or not. */
float4 bezel_ps(CopyOut i) : SV_Target0
{
    return tex0.Sample(smp0, i.uv);
}

Texture2D tex2 : register(t2, space2);
SamplerState smp2 : register(s2, space2);

/* The magnified layer laid over the target. pd[0].xy is the offset into the
 * magnified pictures, pd[0].z the scale; tex0 is the magnified layer, tex1
 * the layer, tex2 the magnified native picture (target and layer composed at
 * 1x). A native pixel the layer covers completely hides the target, as at
 * 1x, so a sample inside one is the magnified native picture, which never
 * sees what lies under it: under an opaque page is whatever an earlier
 * screen left there. Elsewhere the magnified layer goes over the target at
 * the render scale. The pipeline blends ONE / ONE_MINUS_SRC_ALPHA. */
float4 compose_ps(CopyOut i) : SV_Target0
{
    int2 p = int2(i.pos.xy) + asint(pd[0].xy);
    float4 t = tex1.Load(int3(p / asint(pd[0].z), 0));
    if (t.a >= 1.0)
        return float4(tex2.Load(int3(p, 0)).rgb, 1.0);
    return tex0.Load(int3(p, 0));
}

/* The 2D layer magnified to the render scale, premultiplied in and out.
 * pd[0].x is the scale; pd[1] the valid field (x0, y0, x1, y1) in native
 * pixels. Inside the field a sample is bilinear with its taps held inside
 * the field, so nothing beyond the picture bleeds into its edge; outside it
 * a sample replicates its native pixel. */
float4 upscale_bilinear_ps(CopyOut i) : SV_Target0
{
    float n = float(pd[0].x);
    int4 f = asint(pd[1]);
    float2 u = (floor(i.pos.xy) + 0.5) / n;
    if (u.x < float(f.x) || u.y < float(f.y) ||
        u.x >= float(f.z) || u.y >= float(f.w))
        return tex0.Load(int3(int2(u), 0));
    float2 c = clamp(u - 0.5, float2(f.xy), float2(f.zw - 1));
    int2 a = int2(floor(c));
    int2 b = min(a + 1, f.zw - 1);
    float2 t = c - float2(a);
    float4 c00 = tex0.Load(int3(a.x, a.y, 0));
    float4 c10 = tex0.Load(int3(b.x, a.y, 0));
    float4 c01 = tex0.Load(int3(a.x, b.y, 0));
    float4 c11 = tex0.Load(int3(b.x, b.y, 0));
    return lerp(lerp(c00, c10, t.x), lerp(c01, c11, t.x), t.y);
}

/* The 2D layer's sharper magnifiers, beside upscale_bilinear_ps and with its
 * conventions: pd[0].x is the scale, pd[1] the valid field in native pixels,
 * taps are held inside the field and a sample outside it replicates its
 * native pixel. pd[0].z set means the picture is opaque: its alpha is
 * coverage, not picture. Premultiplied in and out. */
float4 mag_at(int2 p, int4 f)
{
    float4 c = tex0.Load(int3(clamp(p, f.xy, f.zw - 1), 0));
    if (pd[0].z != 0u)
        c.a = 1.0;
    return c;
}

float4 mag_out(float4 c)
{
    c = saturate(c);
    c.rgb = min(c.rgb, c.a);
    return c;
}

/* The native position of this sample, or false outside the field. */
bool mag_inside(float2 pos, out float2 u, out int4 f)
{
    f = asint(pd[1]);
    u = (floor(pos) + 0.5) / float(pd[0].x);
    return u.x >= float(f.x) && u.y >= float(f.y) &&
           u.x < float(f.z) && u.y < float(f.w);
}

float4 mag_replicate(float2 u)
{
    float4 c = tex0.Load(int3(int2(u), 0));
    if (pd[0].z != 0u)
        c.a = 1.0;
    return c;
}

/* Catmull-Rom weights for the taps at -1, 0, 1, 2 from a fraction t. */
float4 catmull_rom_w(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t,
                   1.5 * t3 - 2.5 * t2 + 1.0,
                  -1.5 * t3 + 2.0 * t2 + 0.5 * t,
                   0.5 * t3 - 0.5 * t2);
}

float4 upscale_catmull_ps(CopyOut i) : SV_Target0
{
    float2 u;
    int4 f;
    if (!mag_inside(i.pos.xy, u, f))
        return mag_replicate(u);
    float2 c = u - 0.5;
    int2 a = int2(floor(c));
    float4 wx = catmull_rom_w(c.x - float(a.x));
    float4 wy = catmull_rom_w(c.y - float(a.y));
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    [unroll] for (int y = 0; y < 4; y++) {
        float4 row = float4(0.0, 0.0, 0.0, 0.0);
        [unroll] for (int x = 0; x < 4; x++)
            row += wx[x] * mag_at(a + int2(x - 1, y - 1), f);
        sum += wy[y] * row;
    }
    return mag_out(sum);
}

/* AMD FidelityFX Super Resolution 1, condensed to the 2D layer's needs: one
 * static picture, any integer scale, 32-bit floats, full RGBA. From AMD's
 * ffx_fsr1.h v1.20210629 (https://github.com/GPUOpen-Effects/FidelityFX-FSR).
 * EASU upscales along the local edge with a windowed Lanczos-2 approximation;
 * RCAS then sharpens at 0.2 stops, as far as the neighbourhood allows without
 * clipping. Everything from here to the end of fsr_rcas_ps is under AMD's
 * licence:
 *
 * Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE. */
float fsr_luma(float4 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

/* One bilinear corner's share of the edge direction and length, from the
 * '+' around its centre c: a above, b left, d right, e below. */
void easu_edge(inout float2 dir, inout float len, float w,
               float a, float b, float c, float d, float e)
{
    float dx = d - b, dy = e - a;
    float mx = max(abs(d - c), abs(c - b));
    float my = max(abs(e - c), abs(c - a));
    float lx = mx > 0.0 ? saturate(abs(dx) / mx) : 0.0;
    float ly = my > 0.0 ? saturate(abs(dy) / my) : 0.0;
    dir += float2(dx, dy) * w;
    len += (lx * lx + ly * ly) * w;
}

void easu_tap(inout float4 sum, inout float wsum, float2 off, float2 dir,
              float2 len2, float lob, float clp, float4 c)
{
    float2 v = float2(off.x * dir.x + off.y * dir.y,
                      off.y * dir.x - off.x * dir.y) * len2;
    float d2 = min(dot(v, v), clp);
    float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;
    float w = (25.0 / 16.0 * wb * wb - (25.0 / 16.0 - 1.0)) * (wa * wa);
    sum += c * w;
    wsum += w;
}

float4 fsr_easu_ps(CopyOut i) : SV_Target0
{
    float2 u;
    int4 f;
    if (!mag_inside(i.pos.xy, u, f))
        return mag_replicate(u);
    float2 pp = u - 0.5;
    int2 p = int2(floor(pp));
    pp -= float2(p);

    /*   b c
     * e F g h
     * I j k l
     *   n o    F is the native pixel up and left of the sample. */
    float4 b = mag_at(p + int2(0, -1), f), c = mag_at(p + int2(1, -1), f);
    float4 e = mag_at(p + int2(-1, 0), f), F = mag_at(p, f);
    float4 g = mag_at(p + int2(1, 0), f), h = mag_at(p + int2(2, 0), f);
    float4 I = mag_at(p + int2(-1, 1), f), j = mag_at(p + int2(0, 1), f);
    float4 k = mag_at(p + int2(1, 1), f), l = mag_at(p + int2(2, 1), f);
    float4 n = mag_at(p + int2(0, 2), f), o = mag_at(p + int2(1, 2), f);
    float bL = fsr_luma(b), cL = fsr_luma(c), eL = fsr_luma(e);
    float fL = fsr_luma(F), gL = fsr_luma(g), hL = fsr_luma(h);
    float iL = fsr_luma(I), jL = fsr_luma(j), kL = fsr_luma(k);
    float lL = fsr_luma(l), nL = fsr_luma(n), oL = fsr_luma(o);

    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    easu_edge(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_edge(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easu_edge(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easu_edge(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    float r2 = dot(dir, dir);
    dir = r2 < 1.0 / 32768.0 ? float2(1.0, 0.0) : dir * rsqrt(r2);
    len = 0.5 * len;
    len *= len;
    /* The kernel stretches along an edge, up to sqrt(2) on a diagonal, and
     * narrows across it; its window shifts with how much edge there is. */
    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;
    easu_tap(sum, wsum, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(sum, wsum, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(sum, wsum, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, I);
    easu_tap(sum, wsum, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(sum, wsum, float2(0.0, 0.0) - pp, dir, len2, lob, clp, F);
    easu_tap(sum, wsum, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(sum, wsum, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(sum, wsum, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(sum, wsum, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(sum, wsum, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(sum, wsum, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easu_tap(sum, wsum, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    /* No ringing: held to the range of the four nearest pixels. */
    float4 mn = min(min(F, g), min(j, k)), mx = max(max(F, g), max(j, k));
    return mag_out(clamp(sum / wsum, mn, mx));
}

/* RCAS over EASU's output (tex0, at the render scale). The field is pd[1]
 * times the scale; outside it a sample passes through. */
float4 fsr_rcas_ps(CopyOut i) : SV_Target0
{
    const float limit = 0.25 - 1.0 / 16.0;
    const float sharpness = 0.8705506;          /* 0.2 stops: exp2(-0.2) */
    int2 p = int2(i.pos.xy);
    int4 g = asint(pd[1]) * int(pd[0].x);
    float4 e = tex0.Load(int3(p, 0));
    if (p.x < g.x || p.y < g.y || p.x >= g.z || p.y >= g.w)
        return e;
    float4 b = tex0.Load(int3(clamp(p + int2(0, -1), g.xy, g.zw - 1), 0));
    float4 d = tex0.Load(int3(clamp(p + int2(-1, 0), g.xy, g.zw - 1), 0));
    float4 f = tex0.Load(int3(clamp(p + int2(1, 0), g.xy, g.zw - 1), 0));
    float4 h = tex0.Load(int3(clamp(p + int2(0, 1), g.xy, g.zw - 1), 0));

    /* Less sharpening where the centre stands out alone: likely grain. */
    float bL = fsr_luma(b), dL = fsr_luma(d), eL = fsr_luma(e);
    float fL = fsr_luma(f), hL = fsr_luma(h);
    float range = max(max(max(bL, dL), max(eL, fL)), hL) -
                  min(min(min(bL, dL), min(eL, fL)), hL);
    float nz = range > 0.0
        ? saturate(abs(0.25 * (bL + dL + fL + hL) - eL) / range) : 0.0;
    nz = 1.0 - 0.5 * nz;

    /* The strongest negative lobe that keeps every channel in 0..1; a
     * channel that does not vary sets no limit. */
    float4 mn = min(min(b, d), min(f, h)), mx = max(max(b, d), max(f, h));
    float4 hit = max(-min(mn, e) / (4.0 * mx),
                     (1.0 - max(mx, e)) / (4.0 * mn - 4.0));
    hit = max(mx, e) - min(mn, e) > 1.0 / 1024.0 ? hit : -1.0;
    float lobe = max(-limit, min(max(max(hit.x, hit.y), max(hit.z, hit.w)),
                                 0.0)) * sharpness * nz;
    return mag_out((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
}

/* An empty stretch of a layer: transparent, with pd[0].y as its depth. */
ClearOut clear_layer_ps(CopyOut i)
{
    ClearOut o;
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    o.depth = float(pd[0].y & 65535) / 65535.0;
    return o;
}

float4 clear_layer_color_ps(CopyOut i) : SV_Target0
{
    return float4(0.0, 0.0, 0.0, 0.0);
}

/* The 2D layer's depth: the scaled depth taken at each native pixel's
 * centre sub-pixel, as depth_resolve_ps takes it, written as depth. */
ClearOut layer_depth_ps(CopyOut i)
{
    ClearOut o;
    uint scale = pd[0].x;
    int2 p = int2(i.pos.xy) * int(scale) + int(scale / 2);
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    o.depth = tex0.Load(int3(p, 0)).r;
    return o;
}

/* The depth buffer at native resolution, for a caller that locks it. Each
 * native pixel takes the sub-pixel at the centre of its scale x scale block,
 * the one a 1x surface would have rasterised, and loads it unfiltered: a depth
 * word is compared for equality, so it must come back exact. */
float4 depth_resolve_ps(CopyOut i) : SV_Target0
{
    uint scale = pd[0].x;
    int2 p = int2(i.pos.xy) * int(scale) + int(scale / 2);
    return float4(tex0.Load(int3(p, 0)).r, 0.0, 0.0, 1.0);
}


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
