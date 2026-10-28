/* chd.c -- read a MAME CHD v5 without MAME.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A port of the project's Python CHD reader, which the maintainer keeps as the
 * oracle this file is checked against, file by file. A 124-byte big-endian
 * header names four compressors, the hunk size and the unit size; a
 * compressed map then says, per hunk, which codec it used, where its payload
 * is, how long it is, and a CRC-16 of the DECODED hunk. Only the hunks that
 * are read are decoded.
 *
 * THREE THINGS IN THE MAP ARE EASY TO GET WRONG and decode into something
 * plausible rather than into an error: `mapbytes` counts the payload AFTER
 * the 16-byte map header; the map's CRC-16 seed is 0xFFFF; and a
 * COMPRESSION_SELF entry's 48-bit field is a HUNK NUMBER, not an offset. The
 * file's own two oracles catch all three: every hunk's CRC-16, and the map's.
 *
 * zlib hunks are raw deflate (zlib's inflate); lzma hunks are headerless LZMA1
 * (7-Zip's public-domain LzmaDec.c); huff and flac are decoded here. Both
 * third-party decoders are fetched by `make deps`, never committed.
 *
 * A CD CHD is the same container with 2448-byte units, one CD frame each:
 * 2352 bytes of sector and 96 of subcode. Its three codecs (cdlz, cdzl,
 * cdfl) carry the sectors and the subcode as two streams, and cdlz and cdzl
 * strip each sector's sync and ECC when they can be rebuilt; see "CD frames"
 * below. */
#include "chd.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib.h"
#include "LzmaDec.h"

#define HEADER_LEN 124

/* Map entry types as they appear in the decoded map... */
enum { T_0, T_1, T_2, T_3, T_NONE, T_SELF, T_PARENT,
       /* ...and the pseudo-types that only appear in the compressed stream. */
       T_RLE_SMALL, T_RLE_LARGE, T_SELF_0, T_SELF_1,
       T_PARENT_SELF, T_PARENT_0, T_PARENT_1 };

#define TAG(a, b, c, d) \
    ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))
#define TAG_ZLIB TAG('z', 'l', 'i', 'b')
#define TAG_LZMA TAG('l', 'z', 'm', 'a')
#define TAG_HUFF TAG('h', 'u', 'f', 'f')
#define TAG_FLAC TAG('f', 'l', 'a', 'c')
#define TAG_CDLZ TAG('c', 'd', 'l', 'z')
#define TAG_CDZL TAG('c', 'd', 'z', 'l')
#define TAG_CDFL TAG('c', 'd', 'f', 'l')

#define CD_FRAME 2448u          /* one unit of a CD CHD */
#define CD_SECTOR 2352u
#define CD_SUB 96u

/* resolve()'s answer for a hunk an uncompressed CHD never wrote: it reads
 * as zeroes and has no payload. */
#define HUNK_ZERO UINT32_MAX

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}
static uint64_t be48(const uint8_t *p)
{
    return (uint64_t)be16(p) << 32 | be32(p + 2);
}
static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

/* ---- CRC-16/CCITT, seed 0xFFFF in every use here ------------------------ */

static uint16_t s_crc_t8[256];
static INIT_ONCE s_crc_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK crc_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    unsigned i, k;
    (void)once; (void)p; (void)ctx;
    for (i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (k = 0; k < 8; k++)
            c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
        s_crc_t8[i] = c;
    }
    return TRUE;
}

uint16_t chd_crc16(const void *data, size_t n, uint16_t seed)
{
    const uint8_t *p = data;
    uint16_t c = seed;
    InitOnceExecuteOnce(&s_crc_once, crc_init, NULL, NULL);
    while (n--)
        c = (uint16_t)((c << 8) ^ s_crc_t8[(c >> 8) ^ *p++]);
    return c;
}

/* ---- the bitstream: big-endian, MSB first, zeroes past the end ----------
 *
 * Reading past the end is not an error: the huffman trees are written without
 * padding, so the last symbol of a hunk can need bits the encoder never wrote.
 * `err` is raised only by a unary run that never ends, which a corrupt stream
 * would otherwise turn into an infinite loop. */
typedef struct {
    const uint8_t *d;
    size_t p, len;
    uint64_t b;     /* the low n bits are unread */
    unsigned n;
    int err;
} bits_t;

static void bits_init(bits_t *r, const uint8_t *d, size_t len)
{
    memset(r, 0, sizeof *r);
    r->d = d;
    r->len = len;
}

static void bits_fill(bits_t *r, unsigned k)
{
    while (r->n < k) {
        r->b = (r->b << 8) | (r->p < r->len ? r->d[r->p] : 0);
        r->p++;
        r->n += 8;
    }
}

/* k <= 56 */
static uint64_t bits_read(bits_t *r, unsigned k)
{
    uint64_t v;
    if (!k)
        return 0;
    bits_fill(r, k);
    r->n -= k;
    v = (r->b >> r->n) & ((1ull << k) - 1);
    r->b &= (1ull << r->n) - 1;
    return v;
}

static int32_t bits_signed(bits_t *r, unsigned k)
{
    uint64_t v;
    if (!k)
        return 0;
    v = bits_read(r, k);
    return (int32_t)(v & (1ull << (k - 1)) ? (int64_t)v - (1ll << k)
                                            : (int64_t)v);
}

static uint32_t bits_unary(bits_t *r)
{
    uint32_t c = 0;
    for (;;) {
        if (r->n == 0) {
            if (r->p > r->len + 8) {
                r->err = 1;
                return 0;
            }
            r->b = r->p < r->len ? r->d[r->p] : 0;
            r->p++;
            r->n = 8;
        }
        if (r->b) {
            unsigned k = 64u - (unsigned)__builtin_clzll(r->b);
            c += r->n - k;
            r->n = k - 1;
            r->b &= (1ull << r->n) - 1;
            return c;
        }
        c += r->n;
        r->b = 0;
        r->n = 0;
    }
}

/* The bits left over in the current byte. */
static void bits_align(bits_t *r)
{
    r->b = 0;
    r->n = 0;
}

/* ---- huffman -------------------------------------------------------------
 *
 * The container's canonical code assigns from the LONGEST length down, which is
 * not the order most descriptions use. Codes of 8 bits or fewer decode through
 * a 256-entry table; longer ones are consecutive within their length, in
 * symbol order, so each length is one range check. */
typedef struct {
    unsigned numcodes, maxbits;
    uint8_t lengths[256];
    uint8_t prim_len[256];      /* 0: no code of 8 bits or fewer starts here */
    uint16_t prim_sym[256];
    uint32_t first[33], count[33];
    uint16_t base[33];
    uint16_t syms[256];
} huff_t;

static void huff_new(huff_t *h, unsigned numcodes, unsigned maxbits)
{
    memset(h, 0, sizeof *h);
    h->numcodes = numcodes;
    h->maxbits = maxbits;
}

static int huff_assign(huff_t *h)
{
    uint32_t hist[33] = { 0 }, starts[33] = { 0 }, cur = 0, fill[33];
    unsigned L, sym, at = 0;

    for (sym = 0; sym < h->numcodes; sym++) {
        if (h->lengths[sym] > h->maxbits)
            return 0;
        hist[h->lengths[sym]]++;
    }
    for (L = 32; L > 0; L--) {
        uint32_t nxt = (cur + hist[L]) >> 1;
        if (L != 1 && nxt * 2 != cur + hist[L])
            return 0;
        starts[L] = cur;
        cur = nxt;
    }
    for (L = 1; L <= 32; L++) {
        h->first[L] = starts[L];
        h->count[L] = hist[L];
        h->base[L] = (uint16_t)at;
        fill[L] = at;
        at += hist[L];
    }
    memset(h->prim_len, 0, sizeof h->prim_len);
    for (sym = 0; sym < h->numcodes; sym++) {
        L = h->lengths[sym];
        if (!L)
            continue;
        if (L <= 8) {
            uint32_t code = starts[L]++, i, b = code << (8 - L);
            for (i = b; i < b + (1u << (8 - L)); i++) {
                h->prim_len[i] = (uint8_t)L;
                h->prim_sym[i] = (uint16_t)sym;
            }
        }
        h->syms[fill[L]++] = (uint16_t)sym;
    }
    return 1;
}

static int huff_decode(const huff_t *h, bits_t *r)
{
    unsigned i = 0, L;
    bits_fill(r, 16);
    i = (unsigned)(r->b >> (r->n - 8)) & 0xFF;
    if (h->prim_len[i]) {
        r->n -= h->prim_len[i];
        r->b &= (1ull << r->n) - 1;
        return h->prim_sym[i];
    }
    for (L = 9; L <= h->maxbits; L++) {
        uint32_t code = (uint32_t)(r->b >> (r->n - L)) & ((1u << L) - 1);
        if (code - h->first[L] < h->count[L]) {
            r->n -= L;
            r->b &= (1ull << r->n) - 1;
            return h->syms[h->base[L] + code - h->first[L]];
        }
    }
    return -1;
}

static int huff_import_rle(huff_t *h, bits_t *r)
{
    unsigned numbits = h->maxbits >= 16 ? 5 : (h->maxbits >= 8 ? 4 : 3);
    unsigned cur = 0;
    while (cur < h->numcodes) {
        unsigned nb = (unsigned)bits_read(r, numbits);
        if (nb != 1) {
            h->lengths[cur++] = (uint8_t)nb;
        } else if ((nb = (unsigned)bits_read(r, numbits)) == 1) {
            h->lengths[cur++] = 1;      /* a doubled 1 is one 1 */
        } else {
            unsigned rep = (unsigned)bits_read(r, numbits) + 3;
            while (rep-- && cur < h->numcodes)
                h->lengths[cur++] = (uint8_t)nb;
        }
    }
    return huff_assign(h);
}

/* The huff codec's tree: code lengths, RLE'd, themselves huffman-coded by a
 * small 24-code tree. */
static int huff_import_huffman(huff_t *h, bits_t *r)
{
    huff_t small;
    unsigned i, start, count = 0, temp, rlefullbits = 0, cur = 0, last = 0;

    huff_new(&small, 24, 6);
    small.lengths[0] = (uint8_t)bits_read(r, 3);
    start = (unsigned)bits_read(r, 3) + 1;
    for (i = 1; i < 24; i++) {
        if (i < start || count == 7) {
            small.lengths[i] = 0;
        } else {
            count = (unsigned)bits_read(r, 3);
            small.lengths[i] = (uint8_t)(count == 7 ? 0 : count);
        }
    }
    if (!huff_assign(&small))
        return 0;

    for (temp = h->numcodes - 9; temp; temp >>= 1)
        rlefullbits++;
    while (cur < h->numcodes) {
        int v = huff_decode(&small, r);
        if (v < 0)
            return 0;
        if (v != 0) {
            last = (unsigned)v - 1;
            h->lengths[cur++] = (uint8_t)last;
        } else {
            unsigned n = (unsigned)bits_read(r, 3) + 2;
            if (n == 9)
                n += (unsigned)bits_read(r, rlefullbits);
            while (n-- && cur < h->numcodes)
                h->lengths[cur++] = (uint8_t)last;
        }
    }
    return huff_assign(h);
}

int chd_huff_decode(const uint8_t *raw, size_t rawlen, uint8_t *out,
                    size_t outlen)
{
    huff_t tree;
    bits_t r;
    size_t i;

    bits_init(&r, raw, rawlen);
    huff_new(&tree, 256, 16);
    if (!huff_import_huffman(&tree, &r))
        return 0;
    for (i = 0; i < outlen; i++) {
        int sym = huff_decode(&tree, &r);
        if (sym < 0)
            return 0;
        out[i] = (uint8_t)sym;
    }
    return 1;
}

/* ---- FLAC ----------------------------------------------------------------
 *
 * CHD's flac codec is a bare stream of FLAC frames: one leading byte, 'L' or
 * 'B', says the endianness of the 16-bit samples, and there is no STREAMINFO
 * and no metadata. On a disk image the "samples" are file data read as two
 * channels of 16-bit audio, which is exactly as lossless as the other codecs.
 * The frame layer of the published FLAC format is all it needs. */

static int flac_residual(bits_t *r, int32_t *out, unsigned blocksize,
                         unsigned order)
{
    unsigned method = (unsigned)bits_read(r, 2), pbits, escape, porder, nparts;
    unsigned part, i = order;

    if (method > 1)
        return 0;
    pbits = method == 0 ? 4 : 5;
    escape = method == 0 ? 15 : 31;
    porder = (unsigned)bits_read(r, 4);
    nparts = 1u << porder;
    if (blocksize % nparts)
        return 0;
    for (part = 0; part < nparts; part++) {
        unsigned count = blocksize >> porder, param, k;
        if (part == 0) {
            if (count < order)
                return 0;
            count -= order;
        }
        if (i + count > blocksize)
            return 0;
        param = (unsigned)bits_read(r, pbits);
        if (param == escape) {
            unsigned nbits = (unsigned)bits_read(r, 5);
            for (k = 0; k < count; k++)
                out[i++] = bits_signed(r, nbits);
        } else {
            for (k = 0; k < count; k++) {
                uint64_t v = (uint64_t)bits_unary(r) << param;
                v |= bits_read(r, param);
                out[i++] = (int32_t)((int64_t)(v >> 1) ^ -(int64_t)(v & 1));
            }
        }
        if (r->err)
            return 0;
    }
    return 1;
}

static int flac_subframe(bits_t *r, int32_t *out, unsigned blocksize,
                         unsigned bps)
{
    unsigned stype, wasted = 0, i, j, order;

    if (bits_read(r, 1))
        return 0;
    stype = (unsigned)bits_read(r, 6);
    if (bits_read(r, 1)) {
        wasted = bits_unary(r) + 1;
        if (wasted > bps)
            return 0;
        bps -= wasted;
    }
    if (stype == 0) {                                   /* constant */
        int32_t v = bits_signed(r, bps);
        for (i = 0; i < blocksize; i++)
            out[i] = v;
    } else if (stype == 1) {                            /* verbatim */
        for (i = 0; i < blocksize; i++)
            out[i] = bits_signed(r, bps);
    } else if (stype >= 8 && stype <= 12) {             /* fixed, order 0..4 */
        order = stype - 8;
        if (order > blocksize)
            return 0;
        for (i = 0; i < order; i++)
            out[i] = bits_signed(r, bps);
        if (!flac_residual(r, out, blocksize, order))
            return 0;
        for (i = order; i < blocksize; i++) {
            switch (order) {
            case 1: out[i] += out[i - 1]; break;
            case 2: out[i] += 2 * out[i - 1] - out[i - 2]; break;
            case 3: out[i] += 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3];
                    break;
            case 4: out[i] += 4 * out[i - 1] - 6 * out[i - 2]
                              + 4 * out[i - 3] - out[i - 4];
                    break;
            }
        }
    } else if (stype >= 32) {                           /* LPC, order 1..32 */
        int32_t coef[32];
        unsigned prec;
        int shift;
        order = stype - 31;
        if (order > blocksize)
            return 0;
        for (i = 0; i < order; i++)
            out[i] = bits_signed(r, bps);
        prec = (unsigned)bits_read(r, 4) + 1;
        if (prec == 16)
            return 0;
        shift = bits_signed(r, 5);
        if (shift < 0)
            return 0;
        for (i = 0; i < order; i++)
            coef[i] = bits_signed(r, prec);
        if (!flac_residual(r, out, blocksize, order))
            return 0;
        for (i = order; i < blocksize; i++) {
            int64_t s = 0;
            for (j = 0; j < order; j++)
                s += (int64_t)coef[j] * out[i - 1 - j];
            out[i] += (int32_t)(s >> shift);
        }
    } else {
        return 0;                                       /* reserved */
    }
    if (wasted)
        for (i = 0; i < blocksize; i++)
            out[i] = (int32_t)((uint32_t)out[i] << wasted);
    return !r->err;
}

static unsigned flac_blocksize(unsigned code)
{
    static const unsigned table[16] = {
        0, 192, 576, 1152, 2304, 4608, 0, 0,
        256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
    return table[code & 15];
}

static unsigned flac_samplesize(unsigned code)
{
    static const unsigned table[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    return table[code & 7] ? table[code & 7] : 16;
}

/* FLAC frames from r until outlen bytes of 16-bit stereo are out, in the
 * given byte order. Leaves r just past the last frame's CRC. */
static int flac_frames(bits_t *r, uint8_t *out, size_t outlen, int big,
                       int32_t *a, int32_t *b)
{
    unsigned nsamples = (unsigned)(outlen / 4), done = 0, i;

    while (done < nsamples) {
        unsigned bs_code, sr_code, ch, ss_code, b0, blocksize, bps;

        if (bits_read(r, 14) != 0x3FFE)
            return 0;                                   /* lost frame sync */
        bits_read(r, 2);                               /* reserved, blocking */
        bs_code = (unsigned)bits_read(r, 4);
        sr_code = (unsigned)bits_read(r, 4);
        ch = (unsigned)bits_read(r, 4);
        ss_code = (unsigned)bits_read(r, 3);
        bits_read(r, 1);                               /* reserved */
        b0 = (unsigned)bits_read(r, 8);                /* UTF-8 frame number */
        if (b0 >= 0xC0) {
            unsigned m = 0x20;
            while (b0 & m) {
                bits_read(r, 8);
                m >>= 1;
            }
        }
        if (bs_code == 6)
            blocksize = (unsigned)bits_read(r, 8) + 1;
        else if (bs_code == 7)
            blocksize = (unsigned)bits_read(r, 16) + 1;
        else
            blocksize = flac_blocksize(bs_code) ? flac_blocksize(bs_code)
                                                : nsamples;
        if (sr_code == 12)
            bits_read(r, 8);
        else if (sr_code == 13 || sr_code == 14)
            bits_read(r, 16);
        bps = flac_samplesize(ss_code);
        bits_read(r, 8);                               /* header CRC-8 */
        if ((ch < 8 ? ch + 1 : 2) != 2)
            return 0;
        if (blocksize > nsamples - done)
            return 0;
        if (!flac_subframe(r, a, blocksize, bps + (ch == 9)) ||
            !flac_subframe(r, b, blocksize, bps + (ch == 8 || ch == 10)))
            return 0;
        bits_align(r);
        bits_read(r, 16);                              /* frame CRC-16 */
        for (i = 0; i < blocksize; i++) {
            int32_t l = a[i], rt = b[i];
            if (ch == 8) {                              /* left / side */
                rt = a[i] - b[i];
            } else if (ch == 9) {                       /* right / side */
                l = a[i] + b[i];
            } else if (ch == 10) {                      /* mid / side */
                int32_t mid = (int32_t)((uint32_t)a[i] << 1) | (b[i] & 1);
                l = (mid + b[i]) >> 1;
                rt = (mid - b[i]) >> 1;
            }
            if (l < -32768 || l > 32767 || rt < -32768 || rt > 32767)
                return 0;
            {
                uint8_t *o = out + (size_t)(done + i) * 4;
                uint16_t ul = (uint16_t)l, ur = (uint16_t)rt;
                if (big) {
                    o[0] = (uint8_t)(ul >> 8); o[1] = (uint8_t)ul;
                    o[2] = (uint8_t)(ur >> 8); o[3] = (uint8_t)ur;
                } else {
                    o[0] = (uint8_t)ul; o[1] = (uint8_t)(ul >> 8);
                    o[2] = (uint8_t)ur; o[3] = (uint8_t)(ur >> 8);
                }
            }
        }
        done += blocksize;
    }
    return 1;
}

static int flac_decode(const uint8_t *raw, size_t rawlen, uint8_t *out,
                       size_t outlen, int32_t *a, int32_t *b)
{
    bits_t r;

    if (rawlen < 1 || (raw[0] != 'L' && raw[0] != 'B'))
        return 0;
    bits_init(&r, raw + 1, rawlen - 1);
    return flac_frames(&r, out, outlen, raw[0] == 'B', a, b);
}

int chd_flac_decode(const uint8_t *raw, size_t rawlen, uint8_t *out,
                    size_t outlen)
{
    unsigned n = (unsigned)(outlen / 4);
    int32_t *a = malloc(sizeof *a * (n ? n : 1));
    int32_t *b = malloc(sizeof *b * (n ? n : 1));
    int ok = a && b && flac_decode(raw, rawlen, out, outlen, a, b);
    free(a);
    free(b);
    return ok;
}

/* ---- CD frames -------------------------------------------------------------
 *
 * Measured against chdman's own output, never read out of its source: a
 * cdlz or cdzl hunk is a bitmap of the frames whose sector was stripped (bit
 * i of byte i/8 is frame i), the
 * compressed length of the sector stream in two big-endian bytes, the
 * sectors of every frame back to back in lzma or deflate, then their subcode
 * in deflate. A stripped sector has its sync pattern and its 276 ECC bytes
 * zeroed, and both are rebuilt here; cdfl has no bitmap and no length: its
 * FLAC frames (big-endian samples) are followed by the subcode's deflate
 * stream. The hunk's CRC-16 is over the rebuilt frames, so a wrong rebuild
 * fails the hunk, never passes it. */

static uint8_t s_gf_exp[512], s_gf_log[256];
static INIT_ONCE s_gf_once = INIT_ONCE_STATIC_INIT;

/* GF(2^8) over x^8 + x^4 + x^3 + x^2 + 1, ECMA-130 Annex A. */
static BOOL CALLBACK gf_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    unsigned i, x = 1;
    (void)once; (void)p; (void)ctx;
    for (i = 0; i < 255; i++) {
        s_gf_exp[i] = (uint8_t)x;
        s_gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    for (i = 255; i < 512; i++)
        s_gf_exp[i] = s_gf_exp[i - 255];
    return TRUE;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return a && b ? s_gf_exp[s_gf_log[a] + s_gf_log[b]] : 0;
}

/* The two check symbols of one RSPC vector: n data symbols v[] (with
 * stride), then p0 and p1 such that the sum of the vector and its sum
 * weighted by a^(len-1-i) are both zero (ECMA-130 Annex A). */
static void rspc(const uint8_t *s, const unsigned *at, unsigned n,
                 uint8_t *p0, uint8_t *p1)
{
    uint8_t a = 0, b = 0, q;
    unsigned i, len = n + 2;
    for (i = 0; i < n; i++) {
        a ^= s[at[i]];
        b ^= gf_mul(s[at[i]], s_gf_exp[(len - 1 - i) % 255]);
    }
    /* p0 (a^1 + 1) = a + b; a^1 + 1 = 3. */
    q = (uint8_t)(a ^ b);
    q = q ? s_gf_exp[(s_gf_log[q] + 255 - s_gf_log[3]) % 255] : 0;
    *p0 = q;
    *p1 = (uint8_t)(a ^ q);
}

/* A stripped sector's sync and P/Q parity, back. The parity covers bytes
 * 12..2075 as 16-bit words in two byte planes; a mode 2 sector computes it
 * with its four header bytes taken as zero (measured: 496 of 496 sampled
 * sectors of a mode 2 disc agree with that and none with the header). */
static void cd_rebuild(uint8_t *sec)
{
    static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0 };
    uint8_t hdr[4];
    unsigned at[43], plane, k, m;

    InitOnceExecuteOnce(&s_gf_once, gf_init, NULL, NULL);
    memcpy(sec, sync, sizeof sync);
    memcpy(hdr, sec + 12, 4);
    if (sec[15] == 2)
        memset(sec + 12, 0, 4);
    for (plane = 0; plane < 2; plane++) {
        uint8_t *w = sec + 12 + plane;          /* word k's byte: w[2k] */
        for (k = 0; k < 43; k++) {              /* P: 43 columns of 24 */
            for (m = 0; m < 24; m++)
                at[m] = 2 * (43 * m + k);
            rspc(w, at, 24, &w[2 * (1032 + k)], &w[2 * (1075 + k)]);
        }
        for (k = 0; k < 26; k++) {              /* Q: 26 diagonals of 43 */
            for (m = 0; m < 43; m++)
                at[m] = 2 * ((44 * m + 43 * k) % 1118);
            rspc(w, at, 43, &w[2 * (1118 + k)], &w[2 * (1144 + k)]);
        }
    }
    memcpy(sec + 12, hdr, 4);
}

/* ---- the container -------------------------------------------------------*/

/* What one decoding thread owns: its own file handle (two threads seeking one
 * handle read each other's bytes) and its own codec state. */
typedef struct {
    HANDLE fh;
    z_stream z;
    int z_ready;
    CLzmaDec lz;
    int lz_ready;
    uint8_t *raw;
    size_t rawcap;
    int32_t *fa, *fb;
    uint8_t *cd;                /* a CD hunk's sectors, then its subcode */
} codec_t;

/* One cache slot. A slot that is PENDING belongs to a decoder and is pinned. */
enum { S_FREE, S_PENDING, S_READY, S_FAILED };
typedef struct {
    uint32_t hunk;
    int state;
    int pins;
    int prev, next;     /* LRU list, oldest at s_head */
    uint8_t *data;
} slot_t;

struct chd {
    wchar_t path[MAX_PATH * 2];
    uint32_t compressors[4];
    uint8_t rawsha1[20], sha1[20];
    uint64_t logicalbytes, mapoffset;
    uint32_t hunkbytes, unitbytes, hunkcount;
    int compressed;

    uint8_t *comps;
    uint64_t *offsets;
    uint32_t *lengths;
    uint16_t *crcs;

    /* cache, guarded by lock */
    SRWLOCK lock;
    CONDITION_VARIABLE ready_cv, work_cv;
    slot_t *slots;
    uint8_t *slab;
    int nslots;
    int lru_head, lru_tail;
    int32_t *where;     /* hunk -> slot, or -1 */
    int *queue;         /* slots waiting for a decoder, a ring */
    int q_head, q_count;
    int stop;
    uint64_t decoded;
    char err[256];

    /* the reader's own codec, and the decoders' */
    codec_t reader;
    unsigned jobs;
    HANDLE *threads;
    struct worker *workers;

    /* the plan: hunks in the order they will be read */
    uint32_t *plan;
    size_t plan_n, plan_cap, plan_at, plan_sub;
    size_t window;
};

static void set_err(char *dst, size_t sz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void set_err(char *dst, size_t sz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, sz, fmt, ap);
    va_end(ap);
}

static int pread_all(HANDLE fh, uint64_t off, void *buf, uint32_t n)
{
    uint8_t *p = buf;
    while (n) {
        OVERLAPPED ov;
        DWORD got = 0;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        if (!ReadFile(fh, p, n, &got, &ov) || got == 0)
            return 0;
        p += got;
        off += got;
        n -= got;
    }
    return 1;
}

static HANDLE open_read(const wchar_t *path)
{
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void *lz_alloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void lz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc s_lz_alloc = { lz_alloc, lz_free };

/* The decoders' state for hunks of hunkbytes, with no file. */
static int codec_setup(codec_t *k, uint32_t hunkbytes)
{
    /* LZMA in a CHD has no header: the properties are the ones chdman's
     * encoder derives from the hunk size, lc=3 lp=0 pb=2 and a dictionary of
     * the hunk, at least 4 KiB. */
    uint32_t dict = hunkbytes > 4096 ? hunkbytes : 4096;
    uint8_t props[LZMA_PROPS_SIZE] = {
        (2 * 5 + 0) * 9 + 3,
        (uint8_t)dict, (uint8_t)(dict >> 8), (uint8_t)(dict >> 16),
        (uint8_t)(dict >> 24) };

    if (inflateInit2(&k->z, -15) != Z_OK)
        return 0;
    k->z_ready = 1;
    LzmaDec_Construct(&k->lz);
    if (LzmaDec_Allocate(&k->lz, props, LZMA_PROPS_SIZE, &s_lz_alloc) != SZ_OK)
        return 0;
    k->lz_ready = 1;
    k->fa = malloc(sizeof *k->fa * (hunkbytes / 4 + 1));
    k->fb = malloc(sizeof *k->fb * (hunkbytes / 4 + 1));
    k->cd = malloc(hunkbytes);
    return k->fa && k->fb && k->cd;
}

static int codec_init(codec_t *k, const chd_t *c)
{
    memset(k, 0, sizeof *k);
    k->fh = open_read(c->path);
    if (k->fh == INVALID_HANDLE_VALUE)
        return 0;
    return codec_setup(k, c->hunkbytes);
}

static void codec_free(codec_t *k)
{
    if (k->fh && k->fh != INVALID_HANDLE_VALUE)
        CloseHandle(k->fh);
    if (k->z_ready)
        inflateEnd(&k->z);
    if (k->lz_ready)
        LzmaDec_Free(&k->lz, &s_lz_alloc);
    free(k->raw);
    free(k->fa);
    free(k->fb);
    free(k->cd);
    memset(k, 0, sizeof *k);
}

/* Follow COMPRESSION_SELF to the hunk that holds the bytes. Its field is a
 * HUNK NUMBER; a chain of them is legal, a loop is not. */
static uint32_t resolve(const chd_t *c, uint32_t h, char *err, size_t errsz)
{
    uint32_t start = h;
    int seen = 0;
    while (c->comps[h] == T_SELF) {
        if (!c->compressed && c->offsets[h] == 0)
            return HUNK_ZERO;
        if (++seen > 64 || c->offsets[h] >= c->hunkcount) {
            set_err(err, errsz, "the self-reference chain from hunk %u is bad",
                    start);
            return HUNK_ZERO - 1;
        }
        h = (uint32_t)c->offsets[h];
    }
    return h;
}

/* Raw deflate of exactly outlen bytes. */
static int inflate_exact(codec_t *k, const uint8_t *in, size_t inlen,
                         uint8_t *out, size_t outlen)
{
    inflateReset(&k->z);
    k->z.next_in = (uint8_t *)in;
    k->z.avail_in = (uInt)inlen;
    k->z.next_out = out;
    k->z.avail_out = (uInt)outlen;
    return inflate(&k->z, Z_FINISH) == Z_STREAM_END &&
           k->z.total_out == outlen;
}

/* One cdlz, cdzl or cdfl hunk into out: frames of sector + subcode. */
static int decode_cd(const chd_t *c, codec_t *k, uint32_t tag, uint32_t len,
                     uint8_t *out)
{
    unsigned frames = c->hunkbytes / CD_FRAME, nb = (frames + 7) / 8, i;
    size_t secs = (size_t)frames * CD_SECTOR, at, sublen;
    uint8_t *sub = k->cd + secs;
    const uint8_t *raw = k->raw;

    if (tag == TAG_CDFL) {
        bits_t r;
        bits_init(&r, raw, len);
        if (!flac_frames(&r, k->cd, secs, 1, k->fa, k->fb))
            return 0;
        at = r.p - r.n / 8;                     /* past the last frame */
        nb = 0;
    } else {
        size_t clen;
        if (len < nb + 2)
            return 0;
        clen = be16(raw + nb);
        at = nb + 2 + clen;
        if (at > len)
            return 0;
        if (tag == TAG_CDZL) {
            if (!inflate_exact(k, raw + nb + 2, clen, k->cd, secs))
                return 0;
        } else {
            SizeT outlen = secs, inlen = clen;
            ELzmaStatus status;
            LzmaDec_Init(&k->lz);
            if (LzmaDec_DecodeToBuf(&k->lz, k->cd, &outlen, raw + nb + 2,
                                    &inlen, LZMA_FINISH_ANY, &status) != SZ_OK ||
                outlen != secs)
                return 0;
        }
    }
    if (at > len)
        return 0;
    sublen = (size_t)frames * CD_SUB;
    if (!inflate_exact(k, raw + at, len - at, sub, sublen))
        return 0;
    for (i = 0; i < frames; i++) {
        uint8_t *f = out + (size_t)i * CD_FRAME;
        memcpy(f, k->cd + (size_t)i * CD_SECTOR, CD_SECTOR);
        memcpy(f + CD_SECTOR, sub + (size_t)i * CD_SUB, CD_SUB);
        if (nb && (raw[i / 8] >> (i % 8) & 1))
            cd_rebuild(f);
    }
    return 1;
}

void chd_cd_rebuild(uint8_t *sector)
{
    cd_rebuild(sector);
}

int chd_cd_decode(const char codec[4], const uint8_t *raw, size_t rawlen,
                  uint8_t *out, unsigned frames)
{
    chd_t c;
    codec_t k;
    uint32_t tag = TAG(codec[0], codec[1], codec[2], codec[3]);
    int ok;

    memset(&c, 0, sizeof c);
    memset(&k, 0, sizeof k);
    c.hunkbytes = frames * CD_FRAME;
    ok = codec_setup(&k, c.hunkbytes) && rawlen <= UINT32_MAX;
    if (ok) {
        k.raw = (uint8_t *)raw;
        ok = decode_cd(&c, &k, tag, (uint32_t)rawlen, out);
        k.raw = NULL;
    }
    codec_free(&k);
    return ok;
}

/* Decode hunk h (already resolved, never SELF) into out. Thread-safe given
 * its own codec_t. */
static int decode_hunk(const chd_t *c, codec_t *k, uint32_t h, uint8_t *out,
                       char *err, size_t errsz)
{
    uint32_t len = c->lengths[h], tag;
    int t = c->comps[h];

    if (t == T_PARENT) {
        set_err(err, errsz, "hunk %u needs the parent CHD", h);
        return 0;
    }
    if (len > k->rawcap) {
        free(k->raw);
        k->raw = malloc(len);
        k->rawcap = k->raw ? len : 0;
        if (!k->raw) {
            set_err(err, errsz, "out of memory reading hunk %u", h);
            return 0;
        }
    }
    if (len && !pread_all(k->fh, c->offsets[h], k->raw, len)) {
        set_err(err, errsz, "hunk %u: the file ends inside its payload", h);
        return 0;
    }
    if (t == T_NONE) {
        if (len != c->hunkbytes) {
            set_err(err, errsz, "hunk %u: stored %u of %u bytes", h, len,
                    c->hunkbytes);
            return 0;
        }
        memcpy(out, k->raw, len);
    } else {
        tag = c->compressors[t];
        if (tag == TAG_ZLIB) {
            inflateReset(&k->z);
            k->z.next_in = k->raw;
            k->z.avail_in = len;
            k->z.next_out = out;
            k->z.avail_out = c->hunkbytes;
            if (inflate(&k->z, Z_FINISH) != Z_STREAM_END ||
                k->z.total_out != c->hunkbytes) {
                set_err(err, errsz, "hunk %u: the zlib stream does not decode "
                        "to one hunk", h);
                return 0;
            }
        } else if (tag == TAG_LZMA) {
            SizeT outlen = c->hunkbytes, inlen = len;
            ELzmaStatus status;
            LzmaDec_Init(&k->lz);
            if (LzmaDec_DecodeToBuf(&k->lz, out, &outlen, k->raw, &inlen,
                                    LZMA_FINISH_ANY, &status) != SZ_OK ||
                outlen != c->hunkbytes) {
                set_err(err, errsz, "hunk %u: the lzma stream does not decode "
                        "to one hunk", h);
                return 0;
            }
        } else if (tag == TAG_HUFF) {
            if (!chd_huff_decode(k->raw, len, out, c->hunkbytes)) {
                set_err(err, errsz, "hunk %u: undecodable huff stream", h);
                return 0;
            }
        } else if (tag == TAG_FLAC) {
            if (!flac_decode(k->raw, len, out, c->hunkbytes, k->fa, k->fb)) {
                set_err(err, errsz, "hunk %u: undecodable flac stream", h);
                return 0;
            }
        } else if (tag == TAG_CDLZ || tag == TAG_CDZL || tag == TAG_CDFL) {
            if (!decode_cd(c, k, tag, len, out)) {
                set_err(err, errsz, "hunk %u: undecodable %c%c%c%c stream", h,
                        (int)(tag >> 24), (int)(tag >> 16 & 0xFF),
                        (int)(tag >> 8 & 0xFF), (int)(tag & 0xFF));
                return 0;
            }
        } else {
            set_err(err, errsz, "hunk %u uses compressor slot %d, which this "
                    "CHD leaves empty", h, t);
            return 0;
        }
    }
    /* An uncompressed CHD's map carries no CRCs; checking a stored zero
     * against a real one would fail every hunk of a good file. */
    if (c->compressed && chd_crc16(out, c->hunkbytes, 0xFFFF) != c->crcs[h]) {
        set_err(err, errsz, "hunk %u: CRC-16 mismatch; the CHD is damaged, "
                "and nothing read from it can be used", h);
        return 0;
    }
    return 1;
}

/* ---- the map ------------------------------------------------------------- */

static int read_map(chd_t *c, char *err, size_t errsz)
{
    uint32_t n = c->hunkcount, h;
    uint8_t mh[16], *raw = NULL;
    uint32_t mapbytes;
    uint64_t cur, lastself = 0, lastparent = 0;
    unsigned lengthbits, selfbits, parentbits;
    uint16_t mapcrc, got;
    huff_t tree;
    bits_t r;
    int lastcomp = 0, ok = 0;
    uint32_t rep = 0;

    c->comps = calloc(n ? n : 1, 1);
    c->offsets = calloc(n ? n : 1, sizeof *c->offsets);
    c->lengths = calloc(n ? n : 1, sizeof *c->lengths);
    c->crcs = calloc(n ? n : 1, sizeof *c->crcs);
    if (!c->comps || !c->offsets || !c->lengths || !c->crcs) {
        set_err(err, errsz, "out of memory for a map of %u hunks", n);
        return 0;
    }

    if (!c->compressed) {
        raw = malloc((size_t)n * 4 + 1);
        if (!raw || !pread_all(c->reader.fh, c->mapoffset, raw, n * 4)) {
            set_err(err, errsz, "cannot read the hunk map");
            free(raw);
            return 0;
        }
        for (h = 0; h < n; h++) {
            uint32_t off = be32(raw + (size_t)h * 4);
            c->comps[h] = off ? T_NONE : T_SELF;
            c->offsets[h] = (uint64_t)off * c->hunkbytes;
            c->lengths[h] = c->hunkbytes;
        }
        free(raw);
        return 1;
    }

    if (!pread_all(c->reader.fh, c->mapoffset, mh, sizeof mh)) {
        set_err(err, errsz, "cannot read the map header");
        return 0;
    }
    mapbytes = be32(mh);
    cur = be48(mh + 4);
    mapcrc = (uint16_t)be16(mh + 10);
    lengthbits = mh[12];
    selfbits = mh[13];
    parentbits = mh[14];
    if (lengthbits > 32 || selfbits > 48 || parentbits > 48) {
        set_err(err, errsz, "implausible map field widths");
        return 0;
    }
    /* The payload starts right after these 16 bytes: mapbytes counts it. */
    raw = malloc(mapbytes ? mapbytes : 1);
    if (!raw || !pread_all(c->reader.fh, c->mapoffset + 16, raw, mapbytes)) {
        set_err(err, errsz, "cannot read the hunk map");
        goto out;
    }
    bits_init(&r, raw, mapbytes);
    huff_new(&tree, 16, 8);
    if (!huff_import_rle(&tree, &r)) {
        set_err(err, errsz, "the hunk map's huffman tree is not valid");
        goto out;
    }
    for (h = 0; h < n; h++) {
        int v;
        if (rep > 0) {
            c->comps[h] = (uint8_t)lastcomp;
            rep--;
            continue;
        }
        v = huff_decode(&tree, &r);
        if (v < 0) {
            set_err(err, errsz, "undecodable hunk map entry at hunk %u", h);
            goto out;
        }
        if (v == T_RLE_SMALL) {
            int x = huff_decode(&tree, &r);
            c->comps[h] = (uint8_t)lastcomp;
            rep = 2 + (uint32_t)(x < 0 ? 0 : x);
        } else if (v == T_RLE_LARGE) {
            int hi = huff_decode(&tree, &r), lo = huff_decode(&tree, &r);
            c->comps[h] = (uint8_t)lastcomp;
            rep = 2 + 16 + ((uint32_t)(hi < 0 ? 0 : hi) << 4) +
                  (uint32_t)(lo < 0 ? 0 : lo);
        } else {
            lastcomp = v;
            c->comps[h] = (uint8_t)v;
        }
    }

    for (h = 0; h < n; h++) {
        int t = c->comps[h];
        uint64_t offset = cur;
        uint32_t length = 0;
        uint16_t crc = 0;
        if (t <= T_3) {
            length = (uint32_t)bits_read(&r, lengthbits);
            cur += length;
            crc = (uint16_t)bits_read(&r, 16);
        } else if (t == T_NONE) {
            length = c->hunkbytes;
            cur += length;
            crc = (uint16_t)bits_read(&r, 16);
        } else if (t == T_SELF) {
            offset = lastself = bits_read(&r, selfbits);
        } else if (t == T_PARENT) {
            offset = lastparent = bits_read(&r, parentbits);
        } else if (t == T_SELF_0 || t == T_SELF_1) {
            if (t == T_SELF_1)
                lastself++;
            t = T_SELF;
            offset = lastself;
        } else if (t == T_PARENT_SELF) {
            t = T_PARENT;
            lastparent = offset = (uint64_t)h * c->hunkbytes / c->unitbytes;
        } else if (t == T_PARENT_0 || t == T_PARENT_1) {
            if (t == T_PARENT_1)
                lastparent += c->hunkbytes / c->unitbytes;
            t = T_PARENT;
            offset = lastparent;
        } else {
            set_err(err, errsz, "unknown map entry type %d at hunk %u", t, h);
            goto out;
        }
        c->comps[h] = (uint8_t)t;
        c->offsets[h] = offset;
        c->lengths[h] = length;
        c->crcs[h] = crc;
    }

    /* The map carries a CRC-16 of itself, checked before it is trusted: a
     * map read wrongly still yields plausible offsets, and the failure would
     * otherwise look like a codec bug in whichever hunk came first. */
    got = 0xFFFF;
    for (h = 0; h < n; h++) {
        uint8_t e[12];
        e[0] = c->comps[h];
        e[1] = (uint8_t)(c->lengths[h] >> 16);
        e[2] = (uint8_t)(c->lengths[h] >> 8);
        e[3] = (uint8_t)c->lengths[h];
        e[4] = (uint8_t)(c->offsets[h] >> 40);
        e[5] = (uint8_t)(c->offsets[h] >> 32);
        e[6] = (uint8_t)(c->offsets[h] >> 24);
        e[7] = (uint8_t)(c->offsets[h] >> 16);
        e[8] = (uint8_t)(c->offsets[h] >> 8);
        e[9] = (uint8_t)c->offsets[h];
        e[10] = (uint8_t)(c->crcs[h] >> 8);
        e[11] = (uint8_t)c->crcs[h];
        got = chd_crc16(e, sizeof e, got);
    }
    if (got != mapcrc) {
        set_err(err, errsz, "the hunk map does not match its own CRC-16 "
                "(0x%04x computed, 0x%04x stored)", got, mapcrc);
        goto out;
    }
    ok = 1;
out:
    free(raw);
    return ok;
}

/* ---- the cache ------------------------------------------------------------
 *
 * IT MUST BE AN LRU, NOT A QUEUE: a disk image is mostly
 * one hunk of zeroes with a million references to it. Here the references are
 * resolved before the cache is asked, so that hunk is one slot, touched on
 * every use. Everything below runs under c->lock. */

static void lru_unlink(chd_t *c, int s)
{
    slot_t *x = &c->slots[s];
    if (x->prev >= 0) c->slots[x->prev].next = x->next; else c->lru_head = x->next;
    if (x->next >= 0) c->slots[x->next].prev = x->prev; else c->lru_tail = x->prev;
    x->prev = x->next = -1;
}

static void lru_push(chd_t *c, int s)
{
    slot_t *x = &c->slots[s];
    x->prev = c->lru_tail;
    x->next = -1;
    if (c->lru_tail >= 0) c->slots[c->lru_tail].next = s; else c->lru_head = s;
    c->lru_tail = s;
}

static void lru_touch(chd_t *c, int s)
{
    lru_unlink(c, s);
    lru_push(c, s);
}

/* The oldest slot nobody holds, emptied and given to hunk h, PENDING with
 * one pin. -1 when every slot is held. */
static int slot_take(chd_t *c, uint32_t h)
{
    int s;
    for (s = c->lru_head; s >= 0; s = c->slots[s].next)
        if (c->slots[s].pins == 0 && c->slots[s].state != S_PENDING)
            break;
    if (s < 0)
        return -1;
    if (c->slots[s].state != S_FREE)
        c->where[c->slots[s].hunk] = -1;
    c->slots[s].hunk = h;
    c->slots[s].state = S_PENDING;
    c->slots[s].pins = 1;
    c->where[h] = s;
    lru_touch(c, s);
    return s;
}

static void slot_done(chd_t *c, int s, int ok)
{
    c->slots[s].state = ok ? S_READY : S_FAILED;
    c->slots[s].pins--;
    if (ok)
        c->decoded++;
    WakeAllConditionVariable(&c->ready_cv);
}

/* A decoder thread: its codec, and the file it decodes. */
typedef struct worker {
    codec_t k;
    chd_t *c;
} worker_t;

static DWORD WINAPI worker_main(LPVOID arg)
{
    worker_t *w = arg;
    codec_t *k = &w->k;
    chd_t *c = w->c;
    char err[256];

    AcquireSRWLockExclusive(&c->lock);
    for (;;) {
        int s, ok;
        uint32_t h;
        while (!c->q_count && !c->stop)
            SleepConditionVariableSRW(&c->work_cv, &c->lock, INFINITE, 0);
        if (c->stop)
            break;
        s = c->queue[c->q_head];
        c->q_head = (c->q_head + 1) % c->nslots;
        c->q_count--;
        h = c->slots[s].hunk;
        ReleaseSRWLockExclusive(&c->lock);
        err[0] = 0;
        ok = decode_hunk(c, k, h, c->slots[s].data, err, sizeof err);
        AcquireSRWLockExclusive(&c->lock);
        if (!ok && !c->err[0])
            snprintf(c->err, sizeof c->err, "%s", err);
        slot_done(c, s, ok);
    }
    ReleaseSRWLockExclusive(&c->lock);
    return 0;
}

/* Queue the planned hunks up to `upto` for the decoders. Under the lock. */
static void plan_submit(chd_t *c, size_t upto)
{
    char err[64];
    if (upto > c->plan_n)
        upto = c->plan_n;
    while (c->plan_sub < upto) {
        uint32_t t = resolve(c, c->plan[c->plan_sub], err, sizeof err);
        int s;
        if (t >= HUNK_ZERO - 1 || c->where[t] >= 0) {
            c->plan_sub++;
            continue;
        }
        s = slot_take(c, t);
        if (s < 0)
            return;                     /* everything is held: try later */
        c->queue[(c->q_head + c->q_count) % c->nslots] = s;
        c->q_count++;
        c->plan_sub++;
        WakeConditionVariable(&c->work_cv);
    }
}

/* Hunk h's bytes, pinned. Returns the slot; -2 for a hunk that reads as
 * zeroes and is in no slot; -1 with c->err set. */
static int hunk_get(chd_t *c, uint32_t h)
{
    uint32_t t;
    int s, ok;
    char err[256];

    err[0] = 0;
    t = resolve(c, h, err, sizeof err);
    AcquireSRWLockExclusive(&c->lock);
    if (t == HUNK_ZERO - 1) {
        snprintf(c->err, sizeof c->err, "%s", err);
        ReleaseSRWLockExclusive(&c->lock);
        return -1;
    }
    if (t == HUNK_ZERO) {
        ReleaseSRWLockExclusive(&c->lock);
        return -2;
    }
    /* Where the plan says the reader is, keep the decoders a window ahead. */
    if (c->threads && c->plan_n) {
        size_t i = c->plan_at;
        while (i < c->plan_n && c->plan[i] != h && i < c->plan_at + 4)
            i++;
        if (i < c->plan_n && c->plan[i] == h) {
            c->plan_at = i;
            if (c->plan_sub < i)
                c->plan_sub = i;
            plan_submit(c, i + c->window);
        }
    }
    for (;;) {
        s = c->where[t];
        if (s >= 0 && c->slots[s].state == S_READY) {
            c->slots[s].pins++;
            lru_touch(c, s);
            ReleaseSRWLockExclusive(&c->lock);
            return s;
        }
        if (s >= 0 && c->slots[s].state == S_PENDING) {
            SleepConditionVariableSRW(&c->ready_cv, &c->lock, INFINITE, 0);
            continue;
        }
        if (s >= 0 && c->slots[s].state == S_FAILED) {
            ReleaseSRWLockExclusive(&c->lock);
            return -1;
        }
        s = slot_take(c, t);
        if (s < 0) {
            SleepConditionVariableSRW(&c->ready_cv, &c->lock, INFINITE, 0);
            continue;
        }
        break;
    }
    ReleaseSRWLockExclusive(&c->lock);
    ok = decode_hunk(c, &c->reader, t, c->slots[s].data, err, sizeof err);
    AcquireSRWLockExclusive(&c->lock);
    if (!ok && !c->err[0])
        snprintf(c->err, sizeof c->err, "%s", err);
    c->slots[s].pins++;
    slot_done(c, s, ok);
    if (!ok)
        c->slots[s].pins--;
    ReleaseSRWLockExclusive(&c->lock);
    return ok ? s : -1;
}

static void hunk_put(chd_t *c, int s)
{
    AcquireSRWLockExclusive(&c->lock);
    c->slots[s].pins--;
    WakeAllConditionVariable(&c->ready_cv);
    ReleaseSRWLockExclusive(&c->lock);
}

/* ---- public ----------------------------------------------------------------*/

static unsigned default_jobs(void)
{
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!n)
        n = 1;
    return n > 32 ? 32 : (unsigned)n;
}

int chd_open(const wchar_t *path, unsigned jobs, chd_t **out,
             char *err, size_t errsz)
{
    uint8_t hdr[HEADER_LEN];
    chd_t *c;
    uint64_t hunks;
    unsigned i;
    size_t s;
    int rc = CHD_ERROR;

    *out = NULL;
    c = calloc(1, sizeof *c);
    if (!c) {
        set_err(err, errsz, "out of memory");
        return CHD_ERROR;
    }
    InitializeSRWLock(&c->lock);
    InitializeConditionVariable(&c->ready_cv);
    InitializeConditionVariable(&c->work_cv);
    c->lru_head = c->lru_tail = -1;
    if (wcslen(path) >= sizeof c->path / sizeof c->path[0]) {
        set_err(err, errsz, "the path is too long");
        goto fail;
    }
    wcscpy(c->path, path);
    c->reader.fh = open_read(path);
    if (c->reader.fh == INVALID_HANDLE_VALUE) {
        set_err(err, errsz, "cannot open the file (Windows error %lu)",
                GetLastError());
        goto fail;
    }
    if (!pread_all(c->reader.fh, 0, hdr, sizeof hdr) ||
        memcmp(hdr, "MComprHD", 8) != 0) {
        set_err(err, errsz, "not a CHD file");
        rc = CHD_UNSUPPORTED;
        goto fail;
    }
    if (be32(hdr + 12) != 5) {
        set_err(err, errsz, "CHD version %u; only version 5 is read here, and "
                "MAME's `chdman copy` converts an older one", be32(hdr + 12));
        rc = CHD_UNSUPPORTED;
        goto fail;
    }
    for (i = 0; i < 4; i++) {
        uint32_t t = be32(hdr + 16 + i * 4);
        if (t && t != TAG_ZLIB && t != TAG_LZMA && t != TAG_HUFF &&
            t != TAG_FLAC && t != TAG_CDLZ && t != TAG_CDZL && t != TAG_CDFL) {
            set_err(err, errsz, "this CHD uses a codec (%c%c%c%c) that is "
                    "not read here", (int)(t >> 24), (int)(t >> 16 & 0xFF),
                    (int)(t >> 8 & 0xFF), (int)(t & 0xFF));
            rc = CHD_UNSUPPORTED;
            goto fail;
        }
        c->compressors[i] = t;
    }
    c->logicalbytes = be64(hdr + 32);
    c->mapoffset = be64(hdr + 40);
    c->hunkbytes = be32(hdr + 56);
    c->unitbytes = be32(hdr + 60);
    memcpy(c->rawsha1, hdr + 64, sizeof c->rawsha1);
    memcpy(c->sha1, hdr + 84, sizeof c->sha1);
    for (i = 104; i < 124 && !hdr[i]; i++)
        ;
    if (i < 124) {
        set_err(err, errsz, "this CHD has a parent; only a whole CHD is "
                "read here, and `chdman copy` makes one");
        rc = CHD_UNSUPPORTED;
        goto fail;
    }
    if (!c->hunkbytes || c->hunkbytes % 4 || c->hunkbytes > (1u << 24) ||
        !c->unitbytes) {
        set_err(err, errsz, "implausible hunk size %u", c->hunkbytes);
        goto fail;
    }
    /* The CD codecs split a hunk into whole frames. The sector stream's
     * length field widens with the hunk, not the stream, and a hunk of 64 KiB
     * or more has never been measured here; such a file is declined rather
     * than guessed at. */
    for (i = 0; i < 4; i++) {
        uint32_t t = c->compressors[i];
        if ((t == TAG_CDLZ || t == TAG_CDZL || t == TAG_CDFL) &&
            (c->unitbytes != CD_FRAME || c->hunkbytes % CD_FRAME ||
             c->hunkbytes >= 65536)) {
            set_err(err, errsz, "a CD CHD laid out as this reader has never "
                    "measured (hunk %u, unit %u)", c->hunkbytes, c->unitbytes);
            rc = CHD_UNSUPPORTED;
            goto fail;
        }
    }
    hunks = (c->logicalbytes + c->hunkbytes - 1) / c->hunkbytes;
    if (hunks >= HUNK_ZERO - 1) {
        set_err(err, errsz, "implausibly large image");
        goto fail;
    }
    c->hunkcount = (uint32_t)hunks;
    c->compressed = c->compressors[0] != 0;
    if (!read_map(c, err, errsz))
        goto fail;
    /* The header and map were read through a bare handle; the reader's
     * codec opens its own like every decoder's. */
    CloseHandle(c->reader.fh);
    c->reader.fh = NULL;
    if (!codec_init(&c->reader, c)) {
        set_err(err, errsz, "cannot set up the decoders");
        goto fail;
    }

    /* About 32 MB of decoded hunks, and a planned window a quarter of it. */
    c->nslots = (int)((32u << 20) / c->hunkbytes);
    if (c->nslots < 64)
        c->nslots = 64;
    if (c->nslots > 16384)
        c->nslots = 16384;
    c->window = (size_t)c->nslots / 4;
    c->slots = calloc((size_t)c->nslots, sizeof *c->slots);
    c->slab = malloc((size_t)c->nslots * c->hunkbytes);
    c->queue = calloc((size_t)c->nslots, sizeof *c->queue);
    c->where = malloc(sizeof *c->where * (c->hunkcount ? c->hunkcount : 1));
    if (!c->slots || !c->slab || !c->queue || !c->where) {
        set_err(err, errsz, "out of memory for the hunk cache");
        goto fail;
    }
    for (s = 0; s < c->hunkcount; s++)
        c->where[s] = -1;
    for (i = 0; i < (unsigned)c->nslots; i++) {
        c->slots[i].data = c->slab + (size_t)i * c->hunkbytes;
        c->slots[i].prev = c->slots[i].next = -1;
        lru_push(c, (int)i);
    }

    c->jobs = jobs ? jobs : default_jobs();
    if (c->jobs >= 2) {
        worker_t *w = calloc(c->jobs, sizeof *w);
        c->threads = calloc(c->jobs, sizeof *c->threads);
        c->workers = w;
        if (!w || !c->threads) {
            set_err(err, errsz, "out of memory for the decoders");
            goto fail;
        }
        for (i = 0; i < c->jobs; i++) {
            w[i].c = c;
            if (!codec_init(&w[i].k, c)) {
                set_err(err, errsz, "cannot set up the decoders");
                goto fail;
            }
            c->threads[i] = CreateThread(NULL, 0, worker_main, &w[i], 0, NULL);
            if (!c->threads[i]) {
                set_err(err, errsz, "cannot start a decoder thread");
                goto fail;
            }
        }
    }
    *out = c;
    return CHD_OK;
fail:
    chd_close(c);
    return rc;
}

void chd_close(chd_t *c)
{
    unsigned i;
    if (!c)
        return;
    if (c->threads) {
        AcquireSRWLockExclusive(&c->lock);
        c->stop = 1;
        WakeAllConditionVariable(&c->work_cv);
        ReleaseSRWLockExclusive(&c->lock);
        for (i = 0; i < c->jobs; i++) {
            if (c->threads[i]) {
                WaitForSingleObject(c->threads[i], INFINITE);
                CloseHandle(c->threads[i]);
            }
        }
    }
    if (c->workers)
        for (i = 0; i < c->jobs; i++)
            codec_free(&c->workers[i].k);
    codec_free(&c->reader);
    free(c->threads);
    free(c->workers);
    free(c->comps);
    free(c->offsets);
    free(c->lengths);
    free(c->crcs);
    free(c->slots);
    free(c->slab);
    free(c->queue);
    free(c->where);
    free(c->plan);
    free(c);
}

uint64_t chd_size(const chd_t *c) { return c->logicalbytes; }
uint32_t chd_hunk_bytes(const chd_t *c) { return c->hunkbytes; }
unsigned chd_jobs(const chd_t *c) { return c->jobs; }
uint64_t chd_hunks_decoded(const chd_t *c) { return c->decoded; }
const char *chd_error(const chd_t *c) { return c->err; }
void chd_raw_sha1(const chd_t *c, uint8_t out[20])
{
    memcpy(out, c->rawsha1, 20);
}
void chd_sha1(const chd_t *c, uint8_t out[20])
{
    memcpy(out, c->sha1, 20);
}

int chd_read(chd_t *c, uint64_t pos, void *buf, size_t n, size_t *got)
{
    uint8_t *o = buf;
    *got = 0;
    if (pos >= c->logicalbytes)
        return CHD_OK;
    if (n > c->logicalbytes - pos)
        n = (size_t)(c->logicalbytes - pos);
    while (n) {
        uint32_t h = (uint32_t)(pos / c->hunkbytes);
        uint32_t off = (uint32_t)(pos % c->hunkbytes);
        size_t take = c->hunkbytes - off;
        int s;
        if (take > n)
            take = n;
        s = hunk_get(c, h);
        if (s == -2) {
            memset(o, 0, take);
            o += take;
            pos += take;
            n -= take;
            *got += take;
            continue;
        }
        if (s < 0) {
            if (!c->err[0])
                snprintf(c->err, sizeof c->err, "hunk %u could not be read", h);
            return CHD_ERROR;
        }
        memcpy(o, c->slots[s].data + off, take);
        hunk_put(c, s);
        o += take;
        pos += take;
        n -= take;
        *got += take;
    }
    return CHD_OK;
}

void chd_plan(chd_t *c, const uint64_t *start, const uint32_t *len, size_t n)
{
    size_t i;
    AcquireSRWLockExclusive(&c->lock);
    c->plan_n = c->plan_at = c->plan_sub = 0;
    for (i = 0; i < n; i++) {
        uint64_t h, a, b;
        if (!len[i])
            continue;
        a = start[i] / c->hunkbytes;
        b = (start[i] + len[i] - 1) / c->hunkbytes;
        for (h = a; h <= b && h < c->hunkcount; h++) {
            if (c->plan_n && c->plan[c->plan_n - 1] == (uint32_t)h)
                continue;
            if (c->plan_n == c->plan_cap) {
                size_t cap = c->plan_cap ? c->plan_cap * 2 : 4096;
                uint32_t *p = realloc(c->plan, cap * sizeof *p);
                if (!p) {
                    c->plan_n = 0;          /* no plan is slower, not wrong */
                    ReleaseSRWLockExclusive(&c->lock);
                    return;
                }
                c->plan = p;
                c->plan_cap = cap;
            }
            c->plan[c->plan_n++] = (uint32_t)h;
        }
    }
    ReleaseSRWLockExclusive(&c->lock);
}
