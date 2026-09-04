/* tex.c -- texture memory, mip chains, formats, and two texture units.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Split chains use one full source buffer while destination offsets include
 * only selected mip parity. Levels are packed largest-first with eight-byte
 * alignment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vgl.h"
#include "vcglide.h"

/* A Voodoo2's texture memory, per unit. The board in the cabinet carried
 * 4 MB per TMU; twice that is allocated so that a capture taken through a
 * provider which reported more cannot walk off the end of ours, and every
 * access is bounds-checked anyway. What is REPORTED to the game through
 * grTexMinAddress/grTexMaxAddress is the 4 MB, because that is what decides
 * how the game's own _tmem_* allocator behaves and a wrong answer there is a
 * change to the game's behaviour rather than to ours. */
#define VGL_TMU_REPORTED   (4u * 1024u * 1024u)
#define VGL_TMU_ALLOCATED  (8u * 1024u * 1024u)

/* Retain exact texture-write ranges behind each TMU generation. Cache entries
 * use the generation as a watermark and invalidate only overlapping mip data;
 * expired watermarks invalidate conservatively. */
#define VGL_TEX_WRITE_LOG 4096
typedef struct {
    unsigned version;
    uint32_t first, last;               /* half-open TMU byte range */
} tex_write_t;

static tex_write_t s_writes[VGL_TMUS][VGL_TEX_WRITE_LOG];
static unsigned s_write_head[VGL_TMUS], s_write_count[VGL_TMUS];

/* ---- the LOD table ------------------------------------------------------
 *
 * A level's dimensions come from its LOD and the chain's aspect ratio: the
 * LARGER dimension is 256 >> lod, and the aspect index says how many halvings
 * the other one takes. Index 3 is 1x1, below it the width leads, above it the
 * height does. */
static void level_dims(int lod, int aspect, int *w, int *h)
{
    int major = 256 >> (lod < 0 ? 0 : (lod > VGL_MAX_LOD ? VGL_MAX_LOD : lod));
    int minor;

    if (aspect <= 3) {
        minor = major >> (3 - aspect);
        *w = major;
        *h = minor < 1 ? 1 : minor;
    } else {
        minor = major >> (aspect - 3);
        *w = minor < 1 ? 1 : minor;
        *h = major;
    }
}

static int format_bytes(int format)
{
    return format >= 8 ? 2 : 1;
}

uint32_t vgl_tex_level_bytes(int lod, int aspect, int format)
{
    int w, h;
    uint32_t bytes;

    level_dims(lod, aspect, &w, &h);
    bytes = (uint32_t)w * (uint32_t)h * (uint32_t)format_bytes(format);
    return (bytes + 7u) & ~7u;
}

/* GrTexInfo, read at the offsets glide2.h records. Poked by byte arithmetic
 * on purpose: this provider interoperates with a layout the CALLER owns and
 * does not redeclare it as a struct. */
static void texinfo_read(const void *info, int *small, int *large,
                         int *aspect, int *format, const void **data)
{
    const uint8_t *p = (const uint8_t *)info;

    memcpy(small,  p + GR_TEXINFO_SMALLLOD, 4);
    memcpy(large,  p + GR_TEXINFO_LARGELOD, 4);
    memcpy(aspect, p + GR_TEXINFO_ASPECT,   4);
    memcpy(format, p + GR_TEXINFO_FORMAT,   4);
    if (data)
        memcpy(data, p + GR_TEXINFO_DATA, 4);
}

/* Sum caller-buffer extent or masked texture-memory occupancy. */
static uint32_t chain_sum(int small, int large, int aspect, int format,
                          int mask)
{
    uint32_t total = 0;
    int lod;

    if (large > small) {                /* large_lod is the numerically SMALLER */
        int t = large; large = small; small = t;
    }
    for (lod = large; lod <= small; lod++) {
        if (mask) {
            int even = ((lod & 1) == 0);
            if (even && !(mask & GR_MIPMAPLEVELMASK_EVEN))
                continue;
            if (!even && !(mask & GR_MIPMAPLEVELMASK_ODD))
                continue;
        }
        total += vgl_tex_level_bytes(lod, aspect, format);
    }
    return total;
}

uint32_t vgl_tex_mem_required(int even_odd, const void *info)
{
    int small, large, aspect, format;

    texinfo_read(info, &small, &large, &aspect, &format, NULL);
    return chain_sum(small, large, aspect, format,
                     even_odd ? even_odd : GR_MIPMAPLEVELMASK_BOTH);
}

uint32_t vgl_tex_chain_extent(const void *info)
{
    int small, large, aspect, format;

    texinfo_read(info, &small, &large, &aspect, &format, NULL);
    return chain_sum(small, large, aspect, format, GR_MIPMAPLEVELMASK_BOTH);
}

/* Filled below, next to the decoder it is built from; declared here because
 * grTexSource is what notices that it has gone stale. */
static void build_decode_tables(vgl_tmu_t *u);

static void texture_write(int tmu, uint32_t first, uint32_t last)
{
    vgl_tmu_t *u = &vgl.tmu[tmu];
    tex_write_t *write;

    if (last <= first)
        return;
    u->mem_ver++;
    if (!u->mem_ver) {
        /* Four billion writes are not a practical session, but wrapping a
         * cache watermark silently would be a correctness bug. Start a new
         * journal epoch; old cache versions then take the conservative path. */
        u->mem_ver = 1;
        s_write_head[tmu] = s_write_count[tmu] = 0;
    }
    write = &s_writes[tmu][s_write_head[tmu]];
    write->version = u->mem_ver;
    write->first = first;
    write->last = last;
    s_write_head[tmu] = (s_write_head[tmu] + 1) % VGL_TEX_WRITE_LOG;
    if (s_write_count[tmu] < VGL_TEX_WRITE_LOG)
        s_write_count[tmu]++;
}

uint32_t vgl_tex_dirty_mips(int tmu, unsigned since_ver)
{
    const vgl_tmu_t *u;
    uint32_t present = 0, dirty = 0;
    unsigned count, first_index, n;
    int lod;

    if (tmu < 0 || tmu >= VGL_TMUS)
        return 0;
    u = &vgl.tmu[tmu];
    for (lod = 0; lod <= VGL_MAX_LOD; lod++)
        if (u->lod_off[lod] >= 0)
            present |= 1u << lod;
    if (!present || since_ver == u->mem_ver)
        return 0;

    count = s_write_count[tmu];
    if (!count || since_ver > u->mem_ver)
        return present;
    first_index = (s_write_head[tmu] + VGL_TEX_WRITE_LOG - count) %
                  VGL_TEX_WRITE_LOG;
    /* If the first version needed by this cache entry predates the oldest
     * retained write, an overwritten range might overlap any mip. */
    if (since_ver < s_writes[tmu][first_index].version - 1u)
        return present;

    for (n = 0; n < count; n++) {
        const tex_write_t *write =
            &s_writes[tmu][(first_index + n) % VGL_TEX_WRITE_LOG];
        if (write->version <= since_ver)
            continue;
        for (lod = 0; lod <= VGL_MAX_LOD; lod++) {
            uint32_t level_first, level_last;
            if (!(present & (1u << lod)) || (dirty & (1u << lod)))
                continue;
            level_first = u->base + (uint32_t)u->lod_off[lod];
            level_last = level_first +
                (uint32_t)u->lod_w[lod] * (uint32_t)u->lod_h[lod] * u->bpp;
            if (write->first < level_last && write->last > level_first)
                dirty |= 1u << lod;
        }
        if (dirty == present)
            break;
    }
    return dirty;
}

/* ---- texture memory ----------------------------------------------------- */

void vgl_tex_init(void)
{
    int t;

    for (t = 0; t < VGL_TMUS; t++) {
        vgl_tmu_t *u = &vgl.tmu[t];

        if (!u->mem) {
            u->mem = (uint8_t *)calloc(1, VGL_TMU_ALLOCATED);
            u->mem_bytes = u->mem ? VGL_TMU_ALLOCATED : 0;
            if (!u->mem)
                VGL_LOG_CAPPED(3, "tex: TMU%d: cannot allocate %u bytes of "
                                  "texture memory: every texture on this unit "
                                  "will sample white", t, VGL_TMU_ALLOCATED);
        }
        u->min_addr = 0;
        u->max_addr = VGL_TMU_REPORTED - 8u;
        u->lut_fmt = -1;                /* no decode tables yet */
        u->mem_ver = 0;
        s_write_head[t] = s_write_count[t] = 0;
    }
}

void vgl_tex_free(void)
{
    int t;

    for (t = 0; t < VGL_TMUS; t++) {
        free(vgl.tmu[t].mem);
        vgl.tmu[t].mem = NULL;
        vgl.tmu[t].mem_bytes = 0;
    }
}

void vgl_tex_download(int tmu, uint32_t start, int even_odd, const void *info)
{
    int small, large, aspect, format, lod;
    const void *data = NULL;
    const uint8_t *src;
    uint32_t dst_off = 0, src_off = 0, dirty_first = ~0u, dirty_last = 0;
    vgl_tmu_t *u;

    if (tmu < 0 || tmu >= VGL_TMUS)
        return;
    u = &vgl.tmu[tmu];
    texinfo_read(info, &small, &large, &aspect, &format, &data);
    if (large > small) { int t = large; large = small; small = t; }
    src = (const uint8_t *)data;
    if (!src || !u->mem)
        return;
    if (!even_odd)
        even_odd = GR_MIPMAPLEVELMASK_BOTH;

    for (lod = large; lod <= small; lod++) {
        uint32_t bytes = vgl_tex_level_bytes(lod, aspect, format);
        int even = ((lod & 1) == 0);
        int wanted = even ? (even_odd & GR_MIPMAPLEVELMASK_EVEN)
                          : (even_odd & GR_MIPMAPLEVELMASK_ODD);

        if (wanted) {
            if (start + dst_off + bytes <= u->mem_bytes) {
                memcpy(u->mem + start + dst_off, src + src_off, bytes);
                if (dirty_first == ~0u)
                    dirty_first = start + dst_off;
                dirty_last = start + dst_off + bytes;
            } else
                /* Capped: this is inside a per-level loop inside a call
                 * Offroad makes 408 times in 60 frames. */
                VGL_LOG_CAPPED(4, "tex: TMU%d download of %u bytes at 0x%X "
                                  "runs past %u bytes of texture memory: "
                                  "dropped",
                               tmu, bytes, start + dst_off, u->mem_bytes);
            dst_off += bytes;
        }
        /* Advance across every source level, including masked levels. */
        src_off += bytes;
    }
    if (dirty_first != ~0u)
        texture_write(tmu, dirty_first, dirty_last);
}

/* grTexDownloadMipMapLevelPartial. Neither game calls it (0 of 4,131,737
 * records in the Hydro capture and 0 in Offroad's), so this is the shape of
 * the answer rather than a tested path, and it says so the first time it runs. */
void vgl_tex_download_partial(int tmu, uint32_t start, int this_lod,
                              int large_lod, int aspect, int format,
                              int even_odd, const void *data, int from, int to)
{
    uint32_t off = 0, row_bytes;
    int w, h, l;
    vgl_tmu_t *u;
    static int announced;

    if (tmu < 0 || tmu >= VGL_TMUS || !data)
        return;
    if (!announced) {
        announced = 1;
        vgl_log(0, "tex: grTexDownloadMipMapLevelPartial called; neither "
                   "game does so in any capture taken; this path has never "
                   "been exercised");
    }
    u = &vgl.tmu[tmu];
    if (!even_odd)
        even_odd = GR_MIPMAPLEVELMASK_BOTH;
    for (l = large_lod; l < this_lod; l++) {
        int even = ((l & 1) == 0);
        if (even ? (even_odd & GR_MIPMAPLEVELMASK_EVEN)
                 : (even_odd & GR_MIPMAPLEVELMASK_ODD))
            off += vgl_tex_level_bytes(l, aspect, format);
    }
    level_dims(this_lod, aspect, &w, &h);
    row_bytes = (uint32_t)w * (uint32_t)format_bytes(format);
    if (from < 0) from = 0;
    if (to >= h) to = h - 1;
    if (to < from)
        return;
    if (u->mem && start + off + (uint32_t)(to + 1) * row_bytes <= u->mem_bytes) {
        memcpy(u->mem + start + off + (uint32_t)from * row_bytes, data,
               (size_t)(to - from + 1) * row_bytes);
        texture_write(tmu, start + off + (uint32_t)from * row_bytes,
                      start + off + (uint32_t)(to + 1) * row_bytes);
    }
}

void vgl_tex_source(int tmu, uint32_t start, int even_odd, const void *info)
{
    int small, large, aspect, format, lod;
    uint32_t off = 0;
    vgl_tmu_t *u;

    if (tmu < 0 || tmu >= VGL_TMUS)
        return;
    u = &vgl.tmu[tmu];
    texinfo_read(info, &small, &large, &aspect, &format, NULL);
    if (large > small) { int t = large; large = small; small = t; }
    if (!even_odd)
        even_odd = GR_MIPMAPLEVELMASK_BOTH;

    if (even_odd != GR_MIPMAPLEVELMASK_BOTH)
        vgl_split_parity[large & 1]++;
    u->base      = start;
    u->even_odd  = even_odd;
    u->small_lod = small;
    u->large_lod = large;
    u->aspect    = aspect;
    u->format    = format;
    u->valid     = 1;

    for (lod = 0; lod <= VGL_MAX_LOD; lod++) {
        int w, h;
        u->lod_off[lod] = -1;
        level_dims(lod, aspect, &w, &h);
        u->lod_w[lod] = (uint16_t)w;
        u->lod_h[lod] = (uint16_t)h;
    }
    for (lod = large; lod <= small; lod++) {
        int even = ((lod & 1) == 0);
        int present = even ? (even_odd & GR_MIPMAPLEVELMASK_EVEN)
                           : (even_odd & GR_MIPMAPLEVELMASK_ODD);
        if (present) {
            u->lod_off[lod] = (int32_t)off;
            off += vgl_tex_level_bytes(lod, aspect, format);
        }
    }

    /* The answer to "which level does this unit use for lod x", precomputed.
     * See vgl.h: the nearest present level to any lod in [f, f+1) is either
     * the largest present level at or below f or the smallest above it, so
     * two tables filled here replace a scan of the chain per texture sample.
     * lvl_first is the same question the forced-LOD path asks. */
    u->bpp = (uint8_t)format_bytes(format);
    if (u->lut_fmt != format || u->lut_pal_ver != u->pal_ver)
        build_decode_tables(u);
    u->lvl_first = -1;
    for (lod = large; lod <= small; lod++)
        if (u->lod_off[lod] >= 0) { u->lvl_first = (int8_t)lod; break; }
    {
        int f, le = -1;
        for (f = 0; f <= VGL_MAX_LOD; f++) {
            if (u->lod_off[f] >= 0)
                le = f;
            u->lvl_le[f] = (int8_t)le;
        }
        {
            int gt = -1;
            for (f = VGL_MAX_LOD; f >= 0; f--) {
                u->lvl_gt[f] = (int8_t)gt;
                if (u->lod_off[f] >= 0)
                    gt = f;
            }
        }
    }
}

void vgl_tex_table(int tmu, int type, const void *data)
{
    vgl_tmu_t *u;

    if (tmu < 0 || tmu >= VGL_TMUS || !data)
        return;
    u = &vgl.tmu[tmu];
    if (type == GR_TEXTABLE_PALETTE) {
        memcpy(u->palette, data, GR_PALETTE_BYTES);
        u->palette_valid = 1;
        u->pal_ver++;
        /* Rebuild active decode tables immediately because palette updates may
         * occur between draws without a new grTexSource call. */
        if (u->lut_fmt >= 0)
            build_decode_tables(u);
    } else {
        /* An NCC table. Neither game downloads one (Hydro's single
         * grTexDownloadTable is a palette and its two grTexNCCTable calls
         * select a table nothing ever filled), so YIQ decoding is not
         * implemented and says so where it would be used. */
        static int announced;
        if (!announced) {
            announced = 1;
            vgl_log(0, "tex: NCC table %d downloaded on TMU%d; no capture "
                       "of either game contains one and YIQ decoding is not "
                       "implemented", type, tmu);
        }
    }
}

/* Decode observed direct and palette formats. YIQ remains unsupported because
 * neither title supplies the required NCC table. */
static inline uint32_t argb(int a, int r, int g, int b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
           (uint32_t)b;
}

static inline int x5(int v) { return (v << 3) | (v >> 2); }
static inline int x6(int v) { return (v << 2) | (v >> 4); }
static inline int x4(int v) { return v * 17; }
static inline int x3(int v) { return (v << 5) | (v << 2) | (v >> 1); }
static inline int x2(int v) { return v * 85; }

static uint32_t decode_texel(const vgl_tmu_t *u, const uint8_t *p)
{
    switch (u->format) {
    case VGL_TEXFMT_RGB_332:
        return argb(255, x3(p[0] >> 5), x3((p[0] >> 2) & 7), x2(p[0] & 3));
    case VGL_TEXFMT_ALPHA_8:
        return argb(p[0], p[0], p[0], p[0]);
    case VGL_TEXFMT_INTENSITY_8:
        return argb(255, p[0], p[0], p[0]);
    case VGL_TEXFMT_ALPHA_INTENSITY_44: {
        int a = x4(p[0] >> 4), i = x4(p[0] & 15);
        return argb(a, i, i, i);
    }
    case VGL_TEXFMT_P_8: {
        uint32_t c = u->palette_valid ? u->palette[p[0]] : 0x00ffffffu;
        return argb(255, (int)((c >> 16) & 0xff), (int)((c >> 8) & 0xff),
                    (int)(c & 0xff));
    }
    case VGL_TEXFMT_ARGB_8332: {
        int a = p[1];
        return argb(a, x3(p[0] >> 5), x3((p[0] >> 2) & 7), x2(p[0] & 3));
    }
    case VGL_TEXFMT_RGB_565: {
        unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
        return argb(255, x5((int)(v >> 11)), x6((int)((v >> 5) & 0x3f)),
                    x5((int)(v & 0x1f)));
    }
    case VGL_TEXFMT_ARGB_1555: {
        unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
        return argb((v & 0x8000u) ? 255 : 0, x5((int)((v >> 10) & 0x1f)),
                    x5((int)((v >> 5) & 0x1f)), x5((int)(v & 0x1f)));
    }
    case VGL_TEXFMT_ARGB_4444: {
        unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
        return argb(x4((int)(v >> 12)), x4((int)((v >> 8) & 15)),
                    x4((int)((v >> 4) & 15)), x4((int)(v & 15)));
    }
    case VGL_TEXFMT_ALPHA_INTENSITY_88:
        return argb(p[1], p[0], p[0], p[0]);
    case VGL_TEXFMT_AP_88: {
        uint32_t c = u->palette_valid ? u->palette[p[0]] : 0x00ffffffu;
        return argb(p[1], (int)((c >> 16) & 0xff), (int)((c >> 8) & 0xff),
                    (int)(c & 0xff));
    }
    default: {
        static int announced;
        if (!announced) {
            announced = 1;
            vgl_log(0, "tex: format %d is not decoded (YIQ needs an NCC table "
                       "no capture contains): sampling opaque white",
                    u->format);
        }
        return 0xffffffffu;
    }
    }
}

/* Export one selected level as tightly described RGBA8 for an accelerated
 * backend. Texture memory layout and format decoding stay owned by this file;
 * the SDL GPU path receives pixels, not a second interpretation of Glide
 * memory. Missing parity levels return false and are never sampled there. */
int vgl_tex_export_rgba(int tmu, int lod, uint8_t *dst, unsigned pitch)
{
    const vgl_tmu_t *u;
    const uint8_t *src;
    int x, y, w, h;
    uint64_t end;

    if (tmu < 0 || tmu >= VGL_TMUS || lod < 0 || lod > VGL_MAX_LOD || !dst)
        return 0;
    u = &vgl.tmu[tmu];
    if (!u->valid || !u->mem || u->lod_off[lod] < 0)
        return 0;
    w = u->lod_w[lod];
    h = u->lod_h[lod];
    if (pitch < (unsigned)w * 4u)
        return 0;
    end = (uint64_t)u->base + (uint32_t)u->lod_off[lod] +
          (uint64_t)w * (uint64_t)h * u->bpp;
    if (end > u->mem_bytes)
        return 0;
    src = u->mem + u->base + (uint32_t)u->lod_off[lod];
    for (y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            uint32_t c = decode_texel(u, src +
                ((size_t)y * (size_t)w + (size_t)x) * u->bpp);
            row[x * 4 + 0] = (uint8_t)(c >> 16);
            row[x * 4 + 1] = (uint8_t)(c >> 8);
            row[x * 4 + 2] = (uint8_t)c;
            row[x * 4 + 3] = (uint8_t)(c >> 24);
        }
    }
    return 1;
}

/* Build decode tables from decode_texel, which remains authoritative. Validate
 * one-byte, separable two-byte, and explicit-alpha compositions over all input
 * pairs; unsupported compositions retain arithmetic decoding. */
enum { DEC_ARITH = 0, DEC_LUT1, DEC_LUT2A, DEC_LUT2 };

static int8_t s_fmt_kind[16];           /* 0 = not yet classified */

static int classify_format(const vgl_tmu_t *u)
{
    uint8_t p[2];
    int i, j;

    if (format_bytes(u->format) == 1) {
        /* One byte, if the second one truly does not matter. */
        for (i = 0; i < 256; i++) {
            uint32_t a, b;
            p[0] = (uint8_t)i; p[1] = 0x00; a = decode_texel(u, p);
            p[1] = 0xff;                  b = decode_texel(u, p);
            if (a != b)
                return DEC_ARITH;
        }
        return DEC_LUT1;
    }
    {
        uint32_t lo[256], hi[256];
        int ok2 = 1, ok2a = 1;

        for (i = 0; i < 256; i++) {
            p[0] = (uint8_t)i; p[1] = 0; lo[i] = decode_texel(u, p);
            p[0] = 0; p[1] = (uint8_t)i;  hi[i] = decode_texel(u, p);
        }
        for (i = 0; i < 256 && (ok2 || ok2a); i++)
            for (j = 0; j < 256; j++) {
                uint32_t want;
                p[0] = (uint8_t)i; p[1] = (uint8_t)j;
                want = decode_texel(u, p);
                if (want != (lo[i] | hi[j]))
                    ok2 = 0;
                if (want != ((lo[i] & 0x00ffffffu) | ((uint32_t)j << 24)))
                    ok2a = 0;
                if (!ok2 && !ok2a)
                    return DEC_ARITH;
            }
        return ok2 ? DEC_LUT2 : DEC_LUT2A;
    }
}

/* Rebuilt when the format changes or a palette download lands under it, which
 * between them happen a few hundred times in a 1,600-frame capture against
 * several million texels a frame. */
static void build_decode_tables(vgl_tmu_t *u)
{
    uint8_t p[2];
    int i, kind;

    if (u->format < 0 || u->format > 15) {
        u->dec_kind = DEC_ARITH;
        u->lut_fmt = u->format;
        u->lut_pal_ver = u->pal_ver;
        return;
    }
    kind = s_fmt_kind[u->format];
    if (!kind) {
        kind = classify_format(u);
        /* P_8 and AP_88 decode THROUGH the palette, so a classification made
         * with one palette has to hold for any other. It does: the palette is
         * indexed by p0 in both, which is the byte the tables are keyed on. */
        s_fmt_kind[u->format] = (int8_t)kind;
    }
    for (i = 0; i < 256; i++) {
        p[0] = (uint8_t)i; p[1] = 0;    u->dec_lo[i] = decode_texel(u, p);
        p[0] = 0; p[1] = (uint8_t)i;    u->dec_hi[i] = decode_texel(u, p);
    }
    u->dec_kind = kind;
    u->lut_fmt = u->format;
    u->lut_pal_ver = u->pal_ver;
}

/* ---- sampling ----------------------------------------------------------- */

static inline int wrap_coord(int v, int n, int clamp)
{
    if (clamp)
        return v < 0 ? 0 : (v >= n ? n - 1 : v);
    v &= (n - 1);                       /* every level is a power of two */
    return v;
}

static uint32_t fetch(const vgl_tmu_t *u, int lod, int x, int y)
{
    const uint8_t *base;
    int w = u->lod_w[lod], h = u->lod_h[lod];
    uint32_t off;

    x = wrap_coord(x, w, u->clamp_s);
    y = wrap_coord(y, h, u->clamp_t);
    off = u->base + (uint32_t)u->lod_off[lod] +
          ((uint32_t)y * (uint32_t)w + (uint32_t)x) * (uint32_t)u->bpp;
    if (off + (uint32_t)u->bpp > u->mem_bytes)
        return 0xffffffffu;
    base = u->mem + off;
    return decode_texel(u, base);
}

/* Select an available mip level for this unit. frac_out is the distance from
 * the chosen unit-local level, retained for LODFRAC option 1's diagnostic;
 * the measured default derives its factor from global LOD instead. */
static int select_level(const vgl_tmu_t *u, float lod, int *frac_out)
{
    int f, le, gt, best;
    float d_le, d_gt, best_d;

    if (vgl_opt.forcelod || u->mipmap_mode == VGL_MIPMAP_DISABLE) {
        int forced = vgl_opt.forcelod > 1 ? vgl_opt.forcelod - 1
                                          : u->lvl_first;

        *frac_out = 0;
        /* The sharpest level this unit actually HOLDS, which on a split chain
         * is not necessarily large_lod: the odd half of a chain declared
         * 0..8 starts at level 1. */
        if (forced < 0) forced = 0;
        if (forced > VGL_MAX_LOD) forced = VGL_MAX_LOD;
        if (u->lod_off[forced] >= 0)
            return forced;
        /* A forced level may be absent from one half of a split chain. Pick
         * its nearest present neighbour; the LODF pin then chooses which
         * unit is visible. Values above one are an analysis instrument only:
         * 2 requests LOD1, 3 LOD2, and so on. */
        {
            int forced_le = u->lvl_le[forced];
            int forced_gt = u->lvl_gt[forced];
            if (forced_le < 0) return forced_gt;
            if (forced_gt < 0) return forced_le;
            return forced - forced_le <= forced_gt - forced
                ? forced_le : forced_gt;
        }
    }
    if (lod < (float)u->large_lod) lod = (float)u->large_lod;
    if (lod > (float)u->small_lod) lod = (float)u->small_lod;

    /* Two candidates and a comparison rather than a scan of the chain. The
     * tie goes to the SHARPER level, which is what an ascending scan with a
     * strict `<` gives: same level, same fraction, no search. */
    f = vgl_ifloor(lod);
    if (f < 0) f = 0; else if (f > VGL_MAX_LOD) f = VGL_MAX_LOD;
    le = u->lvl_le[f];
    gt = u->lvl_gt[f];
    if (le < 0 && gt < 0) {
        *frac_out = 0;
        return -1;
    }
    d_le = le >= 0 ? lod - (float)le : 1e30f;
    d_gt = gt >= 0 ? (float)gt - lod : 1e30f;
    if (le >= 0 && !(d_gt < d_le)) { best = le; best_d = d_le; }
    else                           { best = gt; best_d = d_gt; }

    if (best_d > 1.0f) best_d = 1.0f;
    *frac_out = (int)(best_d * 255.0f + 0.5f);
    return best;
}

/* Record requested LOD and selected level distributions. */
/* grTexSource of a SPLIT chain, by the parity of its sharpest present level.
 * An odd one holds that level on the ODD unit, so the cross-fade weight that
 * selects it is 255 and not 0. */
uint64_t vgl_split_parity[2];

uint64_t vgl_lod_hist[VGL_MAX_LOD + 3];
uint64_t vgl_level_hist[VGL_MAX_LOD + 1];

/* 1 / (1 << lod), exactly: every one of these is representable, so
 * multiplying by it is the same float as dividing by the power of two. */
static const float inv_pow2[VGL_MAX_LOD + 1] = {
    1.0f, 1.0f / 2, 1.0f / 4, 1.0f / 8, 1.0f / 16,
    1.0f / 32, 1.0f / 64, 1.0f / 128, 1.0f / 256
};

uint32_t vgl_tex_sample(int t, float s, float tc, float lod, int *lod_frac,
                        vgl_stats_t *st)
{
    const vgl_tmu_t *u = &vgl.tmu[t];
    int lvl, shift, w, h;
    float fs, ft;

    *lod_frac = 0;
    if (!u->valid || !u->mem)
        return 0xffffffffu;

    lod += u->lod_bias;
    {
        int b = (int)(lod + 1.0f);      /* bucket 0 holds everything <= -1 */
        if (b < 0) b = 0;
        if (b > VGL_MAX_LOD + 2) b = VGL_MAX_LOD + 2;
        st->lod[b]++;
    }
    lvl = select_level(u, lod, lod_frac);
    if (lvl >= 0)
        st->level[lvl]++;
    if (lvl < 0)
        return 0xffffffffu;

    /* Texture coordinates are expressed in LOD-0 texels. Scale by the selected
     * LOD index, not by the chain's largest present level. */
    /* A power of two, so the reciprocal is exact and the multiply is the
     * division: two float divisions per texture sample, up to two samples per
     * pixel, is a divider stall on a third of the pixels in a frame. */
    shift = lvl;
    fs = s * inv_pow2[shift];
    ft = tc * inv_pow2[shift];
    w = u->lod_w[lvl];
    h = u->lod_h[lvl];

    if (u->mag_filter == VGL_TEXFILTER_POINT &&
        u->min_filter == VGL_TEXFILTER_POINT) {
        return fetch(u, lvl, vgl_ifloor(fs), vgl_ifloor(ft));
    } else {
        /* Glide's s,t coordinates address texel CENTRES at integer values.
         * Subtracting half a texel shifts a texture by a visible fraction of
         * a large polygon: on Offroad's opening snow road, one half texel is
         * roughly fifteen screen pixels.  Keep the old D3D-style offset as a
         * diagnostic switch while the comparator/Oracle A/B is retained. */
        /* Glide's integer texture coordinates address texel corners.  The
         * half-texel offset is independently visible on Offroad's opening
         * road: removing it displaced the image and collapsed its oracle
         * correlation from 0.9988 to 0.794. */
        float x = fs - 0.5f, y = ft - 0.5f;
        int x0 = vgl_ifloor(x), y0 = vgl_ifloor(y);
        int fx = (int)((x - (float)x0) * 256.0f);
        int fy = (int)((y - (float)y0) * 256.0f);
        uint32_t t00, t10, t01, t11;
        uint32_t rb0, ag0, rb1, ag1;
        /* ONE BOUNDS CHECK FOR THE LEVEL RATHER THAN FOUR FOR ITS TEXELS.
         * wrap_coord already confines both coordinates to the level, so if
         * the level itself fits in texture memory then all four taps do, and
         * the address arithmetic collapses to two row pointers. A level that
         * does NOT fit is the caller's error and keeps the old per-texel
         * path, which answers white for exactly the texels that overrun. */
        uint32_t lvl_off = u->base + (uint32_t)u->lod_off[lvl];
        unsigned bpp = u->bpp;

        if (u->mem && (uint64_t)lvl_off +
                      (uint64_t)w * (uint64_t)h * bpp <= u->mem_bytes) {
            const uint8_t *lb = u->mem + lvl_off;
            int xa = wrap_coord(x0,     w, u->clamp_s);
            int xb = wrap_coord(x0 + 1, w, u->clamp_s);
            int ya = wrap_coord(y0,     h, u->clamp_t);
            int yb = wrap_coord(y0 + 1, h, u->clamp_t);
            const uint8_t *r0 = lb + (uint32_t)ya * (uint32_t)w * bpp;
            const uint8_t *r1 = lb + (uint32_t)yb * (uint32_t)w * bpp;
            const uint8_t *pa = r0 + (uint32_t)xa * bpp;
            const uint8_t *pb = r0 + (uint32_t)xb * bpp;
            const uint8_t *pc = r1 + (uint32_t)xa * bpp;
            const uint8_t *pd = r1 + (uint32_t)xb * bpp;
            const uint32_t *lo = u->dec_lo, *hi = u->dec_hi;

            /* ONE decision per sample instead of one per texel: the format
             * cannot change between the four taps of a bilinear filter. */
            switch (u->dec_kind) {
            case DEC_LUT1:
                t00 = lo[pa[0]]; t10 = lo[pb[0]];
                t01 = lo[pc[0]]; t11 = lo[pd[0]];
                break;
            case DEC_LUT2A:
                t00 = (lo[pa[0]] & 0x00ffffffu) | ((uint32_t)pa[1] << 24);
                t10 = (lo[pb[0]] & 0x00ffffffu) | ((uint32_t)pb[1] << 24);
                t01 = (lo[pc[0]] & 0x00ffffffu) | ((uint32_t)pc[1] << 24);
                t11 = (lo[pd[0]] & 0x00ffffffu) | ((uint32_t)pd[1] << 24);
                break;
            case DEC_LUT2:
                t00 = lo[pa[0]] | hi[pa[1]]; t10 = lo[pb[0]] | hi[pb[1]];
                t01 = lo[pc[0]] | hi[pc[1]]; t11 = lo[pd[0]] | hi[pd[1]];
                break;
            default:
                t00 = decode_texel(u, pa); t10 = decode_texel(u, pb);
                t01 = decode_texel(u, pc); t11 = decode_texel(u, pd);
                break;
            }
        } else {
            t00 = fetch(u, lvl, x0,     y0);
            t10 = fetch(u, lvl, x0 + 1, y0);
            t01 = fetch(u, lvl, x0,     y0 + 1);
            t11 = fetch(u, lvl, x0 + 1, y0 + 1);
        }

        if (fx < 0) fx = 0; else if (fx > 255) fx = 255;
        if (fy < 0) fy = 0; else if (fy > 255) fy = 255;

        /* Interpolate two packed 8-bit channels per 32-bit register. Each
         * weighted lane remains below 2^16, preventing cross-lane carry. */
        rb0 = ((t00 & 0x00ff00ffu) * (unsigned)(256 - fx) +
               (t10 & 0x00ff00ffu) * (unsigned)fx) >> 8;
        ag0 = (((t00 >> 8) & 0x00ff00ffu) * (unsigned)(256 - fx) +
               ((t10 >> 8) & 0x00ff00ffu) * (unsigned)fx) >> 8;
        rb1 = ((t01 & 0x00ff00ffu) * (unsigned)(256 - fx) +
               (t11 & 0x00ff00ffu) * (unsigned)fx) >> 8;
        ag1 = (((t01 >> 8) & 0x00ff00ffu) * (unsigned)(256 - fx) +
               ((t11 >> 8) & 0x00ff00ffu) * (unsigned)fx) >> 8;
        rb0 &= 0x00ff00ffu; ag0 &= 0x00ff00ffu;
        rb1 &= 0x00ff00ffu; ag1 &= 0x00ff00ffu;

        rb0 = (rb0 * (unsigned)(256 - fy) + rb1 * (unsigned)fy) >> 8;
        ag0 = (ag0 * (unsigned)(256 - fy) + ag1 * (unsigned)fy) >> 8;

        (void)w; (void)h;
        return ((ag0 & 0x00ff00ffu) << 8) | (rb0 & 0x00ff00ffu);
    }
}
