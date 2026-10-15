/* bezel.c -- bezel art: a PNG with a transparent screen hole.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The decoder follows the PNG specification (W3C, Third Edition): chunk
 * layout and CRC, the five colour types and their legal bit depths, PLTE and
 * tRNS, the five scanline filters. The image data is a zlib stream, inflated
 * by the zlib the CHD importer already links (`make deps`).
 *
 * The hole is read from the art itself. Measured on 1920x1080 and 4096x2304
 * cabinet frames: alpha < 128 gives the opening to within a pixel, alpha == 0
 * falls 13 to 17 px short of it (rounded CRT corners are partly transparent),
 * and alpha < 255 runs into soft glows. */
#include "bezel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib.h"

#define BEZEL_MAX_PIXELS (4096u * 4096u)
#define BEZEL_MAX_FILE   (256u << 20)

static int fail(char *why, size_t n, const char *msg)
{
    if (why && n)
        snprintf(why, n, "%s", msg);
    return 0;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* Reverse one scanline's filter in place. `prev` is the reconstructed line
 * above, or NULL for the first. */
static int unfilter(uint8_t *line, const uint8_t *prev, size_t n, int ftype,
                    unsigned bpp)
{
    size_t i;

    switch (ftype) {
    case 0:
        return 1;
    case 1:
        for (i = bpp; i < n; i++)
            line[i] = (uint8_t)(line[i] + line[i - bpp]);
        return 1;
    case 2:
        if (prev)
            for (i = 0; i < n; i++)
                line[i] = (uint8_t)(line[i] + prev[i]);
        return 1;
    case 3:
        for (i = 0; i < n; i++) {
            int a = i >= bpp ? line[i - bpp] : 0, b = prev ? prev[i] : 0;
            line[i] = (uint8_t)(line[i] + ((a + b) >> 1));
        }
        return 1;
    case 4:
        for (i = 0; i < n; i++) {
            int a = i >= bpp ? line[i - bpp] : 0, b = prev ? prev[i] : 0;
            int c = i >= bpp && prev ? prev[i - bpp] : 0;
            line[i] = (uint8_t)(line[i] + paeth(a, b, c));
        }
        return 1;
    }
    return 0;
}

/* Sample `x` of a scanline at `depth` bits per sample. */
static unsigned sample(const uint8_t *line, size_t x, int depth)
{
    switch (depth) {
    case 1:  return (line[x >> 3] >> (7 - (x & 7))) & 1u;
    case 2:  return (line[x >> 2] >> (6 - 2 * (x & 3))) & 3u;
    case 4:  return (line[x >> 1] >> (4 - 4 * (x & 1))) & 15u;
    case 8:  return line[x];
    default: return (unsigned)line[2 * x] << 8 | line[2 * x + 1];
    }
}

static uint8_t to8(unsigned v, int depth)
{
    if (depth == 16) return (uint8_t)(v >> 8);
    if (depth == 8)  return (uint8_t)v;
    return (uint8_t)(v * 255u / ((1u << depth) - 1u));
}

int bezel_png_decode(const uint8_t *data, size_t size, uint8_t **out,
                     int *w_out, int *h_out, char *why, size_t why_size)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, channels = 0, have_ihdr = 0, done = 0;
    uint8_t plte[256][4];
    unsigned nplte = 0;
    int key_set = 0;
    unsigned key[3] = { 0, 0, 0 };
    size_t pos = 8, rowbytes = 0, rawsize = 0;
    unsigned bpp = 1;
    uint8_t *raw = NULL, *rgba = NULL;
    z_stream z;
    int z_open = 0, z_end = 0, ok = 0;
    uint32_t y;

    *out = NULL;
    memset(&z, 0, sizeof z);
    memset(plte, 0, sizeof plte);
    if (size < 8 || memcmp(data, sig, 8) != 0)
        return fail(why, why_size, "not a PNG file");

    while (!done) {
        uint32_t len, type, crc;
        const uint8_t *body;

        if (size - pos < 12) {
            fail(why, why_size, "truncated: the file ends inside a chunk");
            goto out;
        }
        len = be32(data + pos);
        type = be32(data + pos + 4);
        if (len > size - pos - 12) {
            fail(why, why_size, "truncated: a chunk runs past the end of the file");
            goto out;
        }
        body = data + pos + 8;
        crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0), data + pos + 4, len + 4);
        if (crc != be32(body + len)) {
            fail(why, why_size, "a chunk's CRC does not match: the file is damaged");
            goto out;
        }
        pos += 12 + (size_t)len;

        if (!have_ihdr && type != 0x49484452u) {
            fail(why, why_size, "the first chunk is not IHDR");
            goto out;
        }
        if (type == 0x49484452u) {                      /* IHDR */
            static const int legal[7][17] = {
                [0] = { [1] = 1, [2] = 1, [4] = 1, [8] = 1, [16] = 1 },
                [2] = { [8] = 1, [16] = 1 },
                [3] = { [1] = 1, [2] = 1, [4] = 1, [8] = 1 },
                [4] = { [8] = 1, [16] = 1 },
                [6] = { [8] = 1, [16] = 1 },
            };
            static const int nch[7] = { 1, 0, 3, 1, 2, 0, 4 };
            if (have_ihdr || len != 13) {
                fail(why, why_size, "a malformed IHDR chunk");
                goto out;
            }
            have_ihdr = 1;
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            ctype = body[9];
            if (ctype > 6 || depth > 16 || !legal[ctype][depth] ||
                body[10] != 0 || body[11] != 0) {
                fail(why, why_size, "an unsupported colour type, bit depth or method");
                goto out;
            }
            if (body[12] != 0) {
                fail(why, why_size, "interlaced PNGs are not supported: save it "
                                    "without interlacing");
                goto out;
            }
            if (!w || !h || w > BEZEL_MAX_SIDE || h > BEZEL_MAX_SIDE ||
                (uint64_t)w * h > BEZEL_MAX_PIXELS) {
                fail(why, why_size, "the image is empty, wider or taller than "
                                    "8192 pixels, or larger than 4096x4096 in "
                                    "area");
                goto out;
            }
            channels = nch[ctype];
            rowbytes = ((size_t)w * channels * depth + 7) / 8;
            bpp = (unsigned)((channels * depth + 7) / 8);
            rawsize = (size_t)h * (rowbytes + 1);
            raw = (uint8_t *)malloc(rawsize);
            if (!raw || inflateInit(&z) != Z_OK) {
                fail(why, why_size, "out of memory");
                goto out;
            }
            z_open = 1;
            z.next_out = raw;
            z.avail_out = (uInt)rawsize;
        } else if (type == 0x504c5445u) {               /* PLTE */
            unsigned i;
            if (len % 3 || len == 0 || len > 768) {
                fail(why, why_size, "a malformed PLTE chunk");
                goto out;
            }
            nplte = len / 3;
            for (i = 0; i < nplte; i++) {
                plte[i][0] = body[3 * i];
                plte[i][1] = body[3 * i + 1];
                plte[i][2] = body[3 * i + 2];
                plte[i][3] = 255;
            }
        } else if (type == 0x74524e53u) {               /* tRNS */
            unsigned i;
            if (ctype == 3) {
                for (i = 0; i < len && i < 256; i++)
                    plte[i][3] = body[i];
            } else if (ctype == 0 && len == 2) {
                key[0] = (unsigned)body[0] << 8 | body[1];
                key_set = 1;
            } else if (ctype == 2 && len == 6) {
                key[0] = (unsigned)body[0] << 8 | body[1];
                key[1] = (unsigned)body[2] << 8 | body[3];
                key[2] = (unsigned)body[4] << 8 | body[5];
                key_set = 1;
            }
        } else if (type == 0x49444154u) {               /* IDAT */
            int r;
            if (z_end) {
                fail(why, why_size, "image data continues after its end");
                goto out;
            }
            z.next_in = (Bytef *)body;
            z.avail_in = len;
            while (z.avail_in) {
                r = inflate(&z, Z_NO_FLUSH);
                if (r == Z_STREAM_END) { z_end = 1; break; }
                if (r != Z_OK) {
                    fail(why, why_size, "the image data does not decompress");
                    goto out;
                }
                if (!z.avail_out) {
                    fail(why, why_size, "the image data is longer than the image");
                    goto out;
                }
            }
        } else if (type == 0x49454e44u) {               /* IEND */
            done = 1;
        } else if (!(type & 0x20000000u)) {
            /* A critical chunk this decoder does not know: the picture may
             * depend on it, so refuse rather than draw something else. */
            fail(why, why_size, "an unknown critical chunk");
            goto out;
        }
    }
    if (!z_end || z.avail_out != 0) {
        fail(why, why_size, "the image data is shorter than the image");
        goto out;
    }
    if (ctype == 3 && !nplte) {
        fail(why, why_size, "a palette image without a palette");
        goto out;
    }

    rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) {
        fail(why, why_size, "out of memory");
        goto out;
    }
    for (y = 0; y < h; y++) {
        uint8_t *line = raw + (size_t)y * (rowbytes + 1);
        /* The line above, already reconstructed: lines are unfiltered in
         * place, top to bottom. */
        const uint8_t *prev = y ? line - rowbytes : NULL;
        uint8_t *px = rgba + (size_t)y * w * 4;
        size_t x;

        if (!unfilter(line + 1, prev, rowbytes, line[0], bpp)) {
            fail(why, why_size, "an unknown scanline filter");
            goto out;
        }
        line++;                         /* past the filter byte */
        for (x = 0; x < w; x++, px += 4) {
            unsigned s0, s1, s2;
            switch (ctype) {
            case 0:
                s0 = sample(line, x, depth);
                px[0] = px[1] = px[2] = to8(s0, depth);
                px[3] = key_set && s0 == key[0] ? 0 : 255;
                break;
            case 2:
                s0 = sample(line, 3 * x, depth);
                s1 = sample(line, 3 * x + 1, depth);
                s2 = sample(line, 3 * x + 2, depth);
                px[0] = to8(s0, depth); px[1] = to8(s1, depth);
                px[2] = to8(s2, depth);
                px[3] = key_set && s0 == key[0] && s1 == key[1] &&
                        s2 == key[2] ? 0 : 255;
                break;
            case 3:
                s0 = sample(line, x, depth);
                if (s0 >= nplte) {
                    fail(why, why_size, "a pixel indexes past the palette");
                    goto out;
                }
                memcpy(px, plte[s0], 4);
                break;
            case 4:
                px[0] = px[1] = px[2] = to8(sample(line, 2 * x, depth), depth);
                px[3] = to8(sample(line, 2 * x + 1, depth), depth);
                break;
            default:
                px[0] = to8(sample(line, 4 * x, depth), depth);
                px[1] = to8(sample(line, 4 * x + 1, depth), depth);
                px[2] = to8(sample(line, 4 * x + 2, depth), depth);
                px[3] = to8(sample(line, 4 * x + 3, depth), depth);
                break;
            }
        }
    }
    *out = rgba;
    *w_out = (int)w;
    *h_out = (int)h;
    rgba = NULL;
    ok = 1;

out:
    if (z_open)
        inflateEnd(&z);
    free(raw);
    free(rgba);
    return ok;
}

int bezel_find_hole(const uint8_t *rgba, int w, int h,
                    int *hx, int *hy, int *hw, int *hh,
                    char *why, size_t why_size)
{
    uint8_t *seen;
    int *stack;
    size_t top = 0, cap;
    int x0, y0, x1, y1;

#define CLEAR(X, Y) (rgba[((size_t)(Y) * w + (X)) * 4 + 3] < 128)
#define SEEN(X, Y)  (seen[(size_t)(Y) * w + (X)])

    if (w < 4 || h < 4)
        return fail(why, why_size, "the image is too small to frame anything");
    if (!CLEAR(w / 2, h / 2))
        return fail(why, why_size, "the centre of the image is not transparent, "
                                   "so it has no screen hole");
    seen = (uint8_t *)calloc((size_t)w * h, 1);
    cap = (size_t)h * 2 + 64;
    stack = (int *)malloc(cap * 2 * sizeof *stack);
    if (!seen || !stack) {
        free(seen);
        free(stack);
        return fail(why, why_size, "out of memory");
    }
    x0 = x1 = w / 2;
    y0 = y1 = h / 2;
    stack[top * 2] = w / 2;
    stack[top * 2 + 1] = h / 2;
    top++;
    /* Scanline fill: each seed grows into its whole run, and the rows above
     * and below are seeded once per run of clear pixels under it. */
    while (top) {
        int sx, sy, l, r, dy;
        top--;
        sx = stack[top * 2];
        sy = stack[top * 2 + 1];
        if (SEEN(sx, sy))
            continue;
        for (l = sx; l > 0 && CLEAR(l - 1, sy) && !SEEN(l - 1, sy); l--)
            ;
        for (r = sx; r < w - 1 && CLEAR(r + 1, sy) && !SEEN(r + 1, sy); r++)
            ;
        memset(&SEEN(l, sy), 1, (size_t)(r - l + 1));
        if (l < x0) x0 = l;
        if (r > x1) x1 = r;
        if (sy < y0) y0 = sy;
        if (sy > y1) y1 = sy;
        for (dy = -1; dy <= 1; dy += 2) {
            int ny = sy + dy, x, in_run = 0;
            if (ny < 0 || ny >= h)
                continue;
            for (x = l; x <= r; x++) {
                int open = CLEAR(x, ny) && !SEEN(x, ny);
                if (open && !in_run) {
                    if (top == cap) {
                        int *more;
                        cap *= 2;
                        more = (int *)realloc(stack, cap * 2 * sizeof *stack);
                        if (!more) {
                            free(seen);
                            free(stack);
                            return fail(why, why_size, "out of memory");
                        }
                        stack = more;
                    }
                    stack[top * 2] = x;
                    stack[top * 2 + 1] = ny;
                    top++;
                }
                in_run = open;
            }
        }
    }
    free(seen);
    free(stack);
#undef CLEAR
#undef SEEN

    *hx = x0;
    *hy = y0;
    *hw = x1 - x0 + 1;
    *hh = y1 - y0 + 1;
    if ((int64_t)*hw * 4 < w || (int64_t)*hh * 4 < h)
        return fail(why, why_size, "the transparent area around the centre is "
                                   "too small to be a screen");
    if ((x0 == 0 && x1 == w - 1) ||
        (x0 == 0) + (y0 == 0) + (x1 == w - 1) + (y1 == h - 1) >= 3)
        return fail(why, why_size, "the transparent area around the centre "
                                   "reaches the edges of the image, so nothing "
                                   "frames it");
    return 1;
}

void bezel_client_size(int art_w, int art_h, int hole_h, int raster_h,
                       int scale, int *w, int *h)
{
    *h = (int)(((int64_t)art_h * raster_h * scale + hole_h / 2) / hole_h);
    *w = (int)(((int64_t)*h * art_w + art_h / 2) / art_h);
}

int bezel_load(const char *path, bezel_t *out, char *why, size_t why_size)
{
    FILE *f;
    long n;
    uint8_t *file, *px;
    size_t i, count;

    memset(out, 0, sizeof *out);
    f = fopen(path, "rb");
    if (!f)
        return fail(why, why_size, "the file cannot be opened");
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
        (unsigned long)n > BEZEL_MAX_FILE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return fail(why, why_size, "the file is empty, unreadable or too large");
    }
    file = (uint8_t *)malloc((size_t)n);
    if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(file);
        return fail(why, why_size, "the file cannot be read");
    }
    fclose(f);
    if (!bezel_png_decode(file, (size_t)n, &out->rgba, &out->w, &out->h,
                          why, why_size)) {
        free(file);
        return 0;
    }
    free(file);
    if (!bezel_find_hole(out->rgba, out->w, out->h, &out->hole_x, &out->hole_y,
                         &out->hole_w, &out->hole_h, why, why_size)) {
        bezel_free(out);
        return 0;
    }
    /* Premultiplied, because the alpha is coverage: it is filtered when the
     * art is scaled, and straight colour filtered beside a transparent texel
     * darkens the rim. */
    count = (size_t)out->w * out->h;
    for (i = 0, px = out->rgba; i < count; i++, px += 4) {
        unsigned a = px[3];
        px[0] = (uint8_t)((px[0] * a + 127) / 255);
        px[1] = (uint8_t)((px[1] * a + 127) / 255);
        px[2] = (uint8_t)((px[2] * a + 127) / 255);
    }
    return 1;
}

void bezel_free(bezel_t *b)
{
    free(b->rgba);
    memset(b, 0, sizeof *b);
}
