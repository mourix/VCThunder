/* png.c -- write PNG files with stored deflate blocks, and no dependency.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"

static uint32_t s_crc[256];
static int      s_crc_ready;

static void crc_init(void)
{
    uint32_t c;
    int n, k;

    for (n = 0; n < 256; n++) {
        c = (uint32_t)n;
        for (k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc[n] = c;
    }
    s_crc_ready = 1;
}

static uint32_t crc_run(uint32_t c, const unsigned char *p, size_t n)
{
    while (n--)
        c = s_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}

static void put_be32(FILE *f, uint32_t v)
{
    fputc((int)((v >> 24) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)(v & 0xFF), f);
}

static void chunk(FILE *f, const char *type, const unsigned char *data,
                  size_t n)
{
    uint32_t c;

    put_be32(f, (uint32_t)n);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    c = crc_run(0xFFFFFFFFu, (const unsigned char *)type, 4);
    c = crc_run(c, data, n) ^ 0xFFFFFFFFu;
    put_be32(f, c);
}

int png_write565(const char *path, const uint16_t *px, unsigned w, unsigned h,
                 unsigned stride_px)
{
    FILE *f;
    unsigned char ihdr[13], *raw, *z;
    size_t rawn, zn, off = 0, i;
    uint32_t s1 = 1, s2 = 0;
    unsigned y, x;

    if (!w || !h || !px)
        return 0;
    if (!s_crc_ready)
        crc_init();
    f = fopen(path, "wb");
    if (!f)
        return 0;

    rawn = (size_t)h * (1u + (size_t)w * 3u);
    raw = (unsigned char *)malloc(rawn);
    if (!raw) { fclose(f); return 0; }
    for (y = 0; y < h; y++) {
        raw[off++] = 0;                         /* filter: none */
        for (x = 0; x < w; x++) {
            uint16_t p = px[(size_t)y * stride_px + x];
            unsigned r = (p >> 11) & 0x1Fu, g = (p >> 5) & 0x3Fu, b = p & 0x1Fu;
            /* Replicate the high bits into the low ones, so full scale in is
             * full scale out and the mapping is exactly reversible. */
            raw[off++] = (unsigned char)((r << 3) | (r >> 2));
            raw[off++] = (unsigned char)((g << 2) | (g >> 4));
            raw[off++] = (unsigned char)((b << 3) | (b >> 2));
        }
    }
    for (i = 0; i < rawn; i++) {                /* adler32 of the raw stream */
        s1 = (s1 + raw[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }

    zn = 2u + 4u + rawn + (rawn / 65535u + 1u) * 5u;
    z = (unsigned char *)malloc(zn);
    if (!z) { free(raw); fclose(f); return 0; }
    off = 0;
    z[off++] = 0x78; z[off++] = 0x01;           /* zlib, no preset dictionary */
    i = 0;
    while (i < rawn) {                          /* stored deflate blocks */
        size_t n = (rawn - i > 65535u) ? 65535u : rawn - i;
        z[off++] = (unsigned char)((i + n >= rawn) ? 1 : 0);
        z[off++] = (unsigned char)(n & 0xFF);
        z[off++] = (unsigned char)((n >> 8) & 0xFF);
        z[off++] = (unsigned char)(~n & 0xFF);
        z[off++] = (unsigned char)((~n >> 8) & 0xFF);
        memcpy(z + off, raw + i, n);
        off += n;
        i += n;
    }
    z[off++] = (unsigned char)((s2 >> 8) & 0xFF); z[off++] = (unsigned char)(s2 & 0xFF);
    z[off++] = (unsigned char)((s1 >> 8) & 0xFF); z[off++] = (unsigned char)(s1 & 0xFF);

    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
    ihdr[2] = (unsigned char)(w >> 8);  ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
    ihdr[6] = (unsigned char)(h >> 8);  ihdr[7] = (unsigned char)h;
    ihdr[8] = 8;    /* 8 bits per sample */
    ihdr[9] = 2;    /* truecolour        */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, sizeof ihdr);
    chunk(f, "IDAT", z, off);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    free(raw);
    return 1;
}
