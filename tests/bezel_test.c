/* bezel_test.c -- bezel art with no game and no window.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * PNGs assembled here, in every colour type and bit depth with every scanline
 * filter, stored in uncompressed deflate blocks so the test needs no
 * compressor; the screen-hole finder on synthetic frames; the layout
 * arithmetic both presenters share. The bezels themselves are third-party and
 * never enter the repository.
 *
 *   bezel-test.exe --dump art.png out.rgba
 *       decodes one file, prints `w h hole_x hole_y hole_w hole_h`, and writes
 *       its straight RGBA, for comparing against another decoder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "zlib.h"
#include "../src/bezel.h"
#include "../renderer/core/layout.h"

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* ---- a PNG writer ------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} buf_t;

static void put(buf_t *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = (uint8_t *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put_be32(buf_t *b, uint32_t v)
{
    uint8_t q[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    put(b, q, 4);
}

static void chunk(buf_t *b, const char *type, const uint8_t *d, size_t n)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    put_be32(b, (uint32_t)n);
    put(b, type, 4);
    if (n)
        put(b, d, n);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (n)
        crc = crc32(crc, d, (uInt)n);
    put_be32(b, (uint32_t)crc);
}

/* A zlib stream of stored blocks. */
static void zstore(buf_t *z, const uint8_t *d, size_t n)
{
    static const uint8_t head[2] = { 0x78, 0x01 };
    size_t at = 0;
    put(z, head, 2);
    do {
        size_t len = n - at > 65535 ? 65535 : n - at;
        uint8_t h[5];
        h[0] = at + len == n;
        h[1] = (uint8_t)len; h[2] = (uint8_t)(len >> 8);
        h[3] = (uint8_t)~len; h[4] = (uint8_t)(~len >> 8);
        put(z, h, 5);
        put(z, d + at, len);
        at += len;
    } while (at < n);
    put_be32(z, (uint32_t)adler32(adler32(0L, Z_NULL, 0), d, (uInt)n));
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

typedef struct {
    int w, h, ctype, depth, filter;     /* filter -1: row y uses y % 5 */
    int npal, trns;                      /* palette entries; tRNS chunk */
    int interlace, bad_crc;
} spec_t;

static int channels_of(int ctype)
{
    static const int n[7] = { 1, 0, 3, 1, 2, 0, 4 };
    return n[ctype];
}

static unsigned lcg(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* Builds the file, and the RGBA a correct decoder must produce. */
static buf_t make_png(const spec_t *sp, uint8_t **expect)
{
    buf_t f = { 0 }, z = { 0 };
    int ch = channels_of(sp->ctype), maxv = (1 << sp->depth) - 1;
    size_t rowbytes = ((size_t)sp->w * ch * sp->depth + 7) / 8;
    unsigned bpp = (unsigned)((ch * sp->depth + 7) / 8), seed = 12345u;
    uint8_t *raw = (uint8_t *)calloc((size_t)sp->h, rowbytes);
    uint8_t *filt = (uint8_t *)calloc((size_t)sp->h, rowbytes + 1);
    uint8_t pal[256][4], ihdr[13];
    unsigned key[3] = { 0, 0, 0 };
    int x, y, c, i;

    *expect = (uint8_t *)malloc((size_t)sp->w * sp->h * 4);
    for (i = 0; i < 256; i++) {
        pal[i][0] = (uint8_t)(i * 7); pal[i][1] = (uint8_t)(255 - i);
        pal[i][2] = (uint8_t)(i * 3); pal[i][3] = (uint8_t)(sp->trns ? i : 255);
    }
    if (sp->trns && sp->ctype == 0) key[0] = (unsigned)(maxv / 2);
    if (sp->trns && sp->ctype == 2) { key[0] = 1; key[1] = 2; key[2] = 3; }

    for (y = 0; y < sp->h; y++)
        for (x = 0; x < sp->w; x++) {
            unsigned v[4];
            uint8_t *e = *expect + ((size_t)y * sp->w + x) * 4;
            for (c = 0; c < ch; c++) {
                v[c] = lcg(&seed) % (unsigned)(maxv + 1);
                if (sp->ctype == 3) v[c] %= (unsigned)sp->npal;
            }
            /* Put a few transparent keys in the picture. */
            if (sp->trns && (x + y) % 7 == 0) {
                if (sp->ctype == 0) v[0] = key[0];
                if (sp->ctype == 2) { v[0] = key[0]; v[1] = key[1]; v[2] = key[2]; }
            }
            for (c = 0; c < ch; c++) {
                size_t s = (size_t)x * ch + c;
                uint8_t *row = raw + (size_t)y * rowbytes;
                if (sp->depth == 16) {
                    row[2 * s] = (uint8_t)(v[c] >> 8);
                    row[2 * s + 1] = (uint8_t)v[c];
                } else if (sp->depth == 8) {
                    row[s] = (uint8_t)v[c];
                } else {
                    int per = 8 / sp->depth;
                    row[s / per] |= (uint8_t)(v[c] << (8 - sp->depth * (int)(s % per + 1)));
                }
            }
#define TO8(V) (uint8_t)(sp->depth == 16 ? (V) >> 8 : sp->depth == 8 ? (V) \
                         : (V) * 255u / (unsigned)maxv)
            switch (sp->ctype) {
            case 0:
                e[0] = e[1] = e[2] = TO8(v[0]);
                e[3] = sp->trns && v[0] == key[0] ? 0 : 255;
                break;
            case 2:
                e[0] = TO8(v[0]); e[1] = TO8(v[1]); e[2] = TO8(v[2]);
                e[3] = sp->trns && v[0] == key[0] && v[1] == key[1] &&
                       v[2] == key[2] ? 0 : 255;
                break;
            case 3:
                memcpy(e, pal[v[0]], 4);
                break;
            case 4:
                e[0] = e[1] = e[2] = TO8(v[0]); e[3] = TO8(v[1]);
                break;
            default:
                e[0] = TO8(v[0]); e[1] = TO8(v[1]); e[2] = TO8(v[2]);
                e[3] = TO8(v[3]);
                break;
            }
#undef TO8
        }

    for (y = 0; y < sp->h; y++) {
        const uint8_t *cur = raw + (size_t)y * rowbytes;
        const uint8_t *up = y ? cur - rowbytes : NULL;
        uint8_t *out = filt + (size_t)y * (rowbytes + 1);
        int ft = sp->filter < 0 ? y % 5 : sp->filter;
        size_t k;
        out[0] = (uint8_t)ft;
        for (k = 0; k < rowbytes; k++) {
            int a = k >= bpp ? cur[k - bpp] : 0, b = up ? up[k] : 0;
            int cc = k >= bpp && up ? up[k - bpp] : 0, p = 0;
            if (ft == 1) p = a;
            else if (ft == 2) p = b;
            else if (ft == 3) p = (a + b) >> 1;
            else if (ft == 4) p = paeth(a, b, cc);
            out[1 + k] = (uint8_t)(cur[k] - p);
        }
    }

    put(&f, "\x89PNG\r\n\x1a\n", 8);
    ihdr[0] = (uint8_t)(sp->w >> 24); ihdr[1] = (uint8_t)(sp->w >> 16);
    ihdr[2] = (uint8_t)(sp->w >> 8);  ihdr[3] = (uint8_t)sp->w;
    ihdr[4] = (uint8_t)(sp->h >> 24); ihdr[5] = (uint8_t)(sp->h >> 16);
    ihdr[6] = (uint8_t)(sp->h >> 8);  ihdr[7] = (uint8_t)sp->h;
    ihdr[8] = (uint8_t)sp->depth; ihdr[9] = (uint8_t)sp->ctype;
    ihdr[10] = ihdr[11] = 0; ihdr[12] = (uint8_t)sp->interlace;
    chunk(&f, "IHDR", ihdr, 13);
    chunk(&f, "tEXt", (const uint8_t *)"Comment\0ancillary, skipped", 26);
    if (sp->ctype == 3) {
        uint8_t p3[768], t[256];
        for (i = 0; i < sp->npal; i++) {
            p3[3 * i] = pal[i][0]; p3[3 * i + 1] = pal[i][1];
            p3[3 * i + 2] = pal[i][2]; t[i] = pal[i][3];
        }
        chunk(&f, "PLTE", p3, (size_t)sp->npal * 3);
        if (sp->trns)
            chunk(&f, "tRNS", t, (size_t)sp->npal);
    } else if (sp->trns) {
        uint8_t t[6] = { (uint8_t)(key[0] >> 8), (uint8_t)key[0],
                         (uint8_t)(key[1] >> 8), (uint8_t)key[1],
                         (uint8_t)(key[2] >> 8), (uint8_t)key[2] };
        chunk(&f, "tRNS", t, sp->ctype == 0 ? 2 : 6);
    }
    zstore(&z, filt, (size_t)sp->h * (rowbytes + 1));
    /* Two IDATs, split mid-stream: a decoder must join them. */
    chunk(&f, "IDAT", z.p, z.n / 2);
    chunk(&f, "IDAT", z.p + z.n / 2, z.n - z.n / 2);
    chunk(&f, "IEND", NULL, 0);
    if (sp->bad_crc)
        f.p[f.n - 30] ^= 0x40;          /* inside the second IDAT */
    free(z.p);
    free(raw);
    free(filt);
    return f;
}

static void decode_case(const spec_t *sp, const char *name)
{
    uint8_t *expect, *got = NULL;
    int w = 0, h = 0, ok;
    char why[160], what[200];
    buf_t f = make_png(sp, &expect);

    ok = bezel_png_decode(f.p, f.n, &got, &w, &h, why, sizeof why);
    snprintf(what, sizeof what, "%s decodes (%s)", name, ok ? "ok" : why);
    check(ok, what);
    if (ok) {
        snprintf(what, sizeof what, "%s is %dx%d", name, sp->w, sp->h);
        check(w == sp->w && h == sp->h, what);
        snprintf(what, sizeof what, "%s pixels are exact", name);
        check(!memcmp(got, expect, (size_t)w * h * 4), what);
    }
    free(got);
    free(expect);
    free(f.p);
}

static void refuse_case(const spec_t *sp, const char *name, const char *needle)
{
    uint8_t *expect, *got = NULL;
    int w, h, ok;
    char why[160] = "", what[260];
    buf_t f = make_png(sp, &expect);

    ok = bezel_png_decode(f.p, f.n, &got, &w, &h, why, sizeof why);
    snprintf(what, sizeof what, "%s is refused with a reason naming '%s' "
                                "(said: %s)", name, needle, why);
    check(!ok && strstr(why, needle) != NULL, what);
    free(got);
    free(expect);
    free(f.p);
}

/* ---- the screen hole ---------------------------------------------------- */

/* A frame: opaque rim, a soft glow (alpha 200) reaching from the rim into the
 * margin, an anti-aliased edge (alpha 64) one pixel inside the hole. */
static uint8_t *frame(int w, int h, int x0, int y0, int x1, int y1)
{
    uint8_t *p = (uint8_t *)malloc((size_t)w * h * 4);
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t a = 255;
            if (x >= x0 && x < x1 && y >= y0 && y < y1)
                a = (x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1) ? 64 : 0;
            else if (x >= x0 - 3 && x < x1 + 3 && y >= y0 - 3 && y < y1 + 3)
                a = 200;
            p[((size_t)y * w + x) * 4 + 0] = 10;
            p[((size_t)y * w + x) * 4 + 1] = 20;
            p[((size_t)y * w + x) * 4 + 2] = 30;
            p[((size_t)y * w + x) * 4 + 3] = a;
        }
    return p;
}

static void hole_cases(void)
{
    int x, y, w, h;
    char why[160];
    uint8_t *p = frame(160, 90, 20, 1, 140, 89);

    check(bezel_find_hole(p, 160, 90, &x, &y, &w, &h, why, sizeof why) &&
          x == 20 && y == 1 && w == 120 && h == 88,
          "hole: the alpha < 128 box, not the glow and not only the clear core");
    /* A transparent speck in the opaque rim is not part of the hole. */
    p[(5 * 160 + 3) * 4 + 3] = 0;
    check(bezel_find_hole(p, 160, 90, &x, &y, &w, &h, why, sizeof why) &&
          x == 20 && w == 120, "hole: a disconnected clear pixel is ignored");
    /* An opaque centre has no hole. */
    p[(45 * 160 + 80) * 4 + 3] = 255;
    check(!bezel_find_hole(p, 160, 90, &x, &y, &w, &h, why, sizeof why) &&
          strstr(why, "centre") != NULL, "hole: an opaque centre is refused");
    free(p);
    p = frame(160, 90, 70, 40, 90, 50);
    check(!bezel_find_hole(p, 160, 90, &x, &y, &w, &h, why, sizeof why) &&
          strstr(why, "too small") != NULL, "hole: a pinhole is not a screen");
    free(p);
    /* Top and bottom open is a pillarbox frame: side panels only. */
    p = frame(160, 90, 20, 0, 140, 90);
    check(bezel_find_hole(p, 160, 90, &x, &y, &w, &h, why, sizeof why) &&
          y == 0 && h == 90, "hole: a pillarbox frame (open top and bottom) is kept");
    free(p);
    /* Open at both sides, or on three edges: nothing frames the picture. */
    p = frame(160, 90, 0, 10, 160, 80);
    check(!bezel_find_hole(p, 160, 90, &x, &y, &w, &h, why, sizeof why) &&
          strstr(why, "edges") != NULL, "hole: open at both sides is refused");
    free(p);
    p = frame(160, 90, 0, 0, 140, 90);
    check(!bezel_find_hole(p, 160, 90, &x, &y, &w, &h, why, sizeof why) &&
          strstr(why, "edges") != NULL, "hole: open on three edges is refused");
    free(p);
}

/* ---- bezel_load: the file door and premultiplication -------------------- */

static void load_case(void)
{
    char path[MAX_PATH], why[160];
    uint8_t *fr = frame(64, 36, 8, 0, 56, 36), *rows;
    uint8_t ihdr[13] = { 0, 0, 0, 64, 0, 0, 0, 36, 8, 6, 0, 0, 0 };
    buf_t f = { 0 }, z = { 0 };
    bezel_t b;
    FILE *out;
    int i, y, ok, exact = 1;

    /* The frame as an 8-bit RGBA PNG, unfiltered. */
    rows = (uint8_t *)malloc(36 * (64 * 4 + 1));
    for (y = 0; y < 36; y++) {
        rows[y * (64 * 4 + 1)] = 0;
        memcpy(rows + y * (64 * 4 + 1) + 1, fr + y * 64 * 4, 64 * 4);
    }
    put(&f, "\x89PNG\r\n\x1a\n", 8);
    chunk(&f, "IHDR", ihdr, 13);
    zstore(&z, rows, 36 * (64 * 4 + 1));
    chunk(&f, "IDAT", z.p, z.n);
    chunk(&f, "IEND", NULL, 0);
    free(z.p);
    free(rows);

    snprintf(path, sizeof path, "bezel-test-%lu.png",
             (unsigned long)GetCurrentProcessId());
    out = fopen(path, "wb");
    check(out && fwrite(f.p, 1, f.n, out) == f.n, "load: temp file written");
    if (out) fclose(out);
    free(f.p);

    ok = bezel_load(path, &b, why, sizeof why);
    check(ok, "load: a frame PNG loads");
    if (ok) {
        check(b.w == 64 && b.h == 36 && b.hole_x == 8 && b.hole_y == 0 &&
              b.hole_w == 48 && b.hole_h == 36, "load: its hole is found");
        for (i = 0; i < 64 * 36; i++) {
            unsigned a = fr[i * 4 + 3];
            if (b.rgba[i * 4 + 0] != (uint8_t)((10 * a + 127) / 255) ||
                b.rgba[i * 4 + 1] != (uint8_t)((20 * a + 127) / 255) ||
                b.rgba[i * 4 + 2] != (uint8_t)((30 * a + 127) / 255) ||
                b.rgba[i * 4 + 3] != a)
                exact = 0;
        }
        check(exact, "load: colour is premultiplied by alpha, alpha kept");
        bezel_free(&b);
    }
    remove(path);
    free(fr);
    check(!bezel_load("bezel-test-no-such-file.png", &b, why, sizeof why) &&
          strstr(why, "opened") != NULL, "load: a missing file says so");
}

/* ---- the layout --------------------------------------------------------- */

static int rect_is(const vgl_rect_t *r, int x, int y, int w, int h)
{
    return r->x == x && r->y == y && r->w == w && r->h == h;
}

static void layout_cases(void)
{
    vgl_rect_t art, pic;

    /* A 1920x1080 frame with a 1433x1073 hole at 244,3, fullscreen on a
     * 5120x2160 monitor: the art fits by height, centred. */
    vgl_bezel_layout(5120, 2160, 1920, 1080, 244, 3, 1433, 1073, 4, 3, 1,
                     &art, &pic);
    check(rect_is(&art, 640, 0, 3840, 2160), "layout: 16:9 art on 21:9 is fitted");
    /* 4:3 covering: the hole is 2866 wide at this scale, so 4:3 is 2149.5
     * tall against a 2146 hole: width decides, the height overscans. */
    check(pic.x == 1128 && pic.w == 2866 && pic.y <= 6 &&
          pic.y + pic.h >= 2152, "layout: 4:3 covers the hole on both axes");
    check(abs(pic.w * 3 - pic.h * 4) <= 8, "layout: and stays 4:3");

    /* The same frame with a widescreen raster: the width is the hole's, the
     * picture is centred on it, the bars inside the hole are black. */
    vgl_bezel_layout(5120, 2160, 1920, 1080, 244, 3, 1433, 1073, 711, 400, 0,
                     &art, &pic);
    check(pic.x == 1128 && pic.w == 2866, "layout: widescreen spans the hole's width");
    check(abs(pic.h - 1612) <= 1 && abs((pic.y + pic.h / 2) - 1079) <= 1,
          "layout: and keeps its own shape, centred on the hole");

    /* A hole narrower than 4:3: covering means the height wins and the sides
     * pass under the rim. */
    vgl_bezel_layout(1600, 1200, 1600, 1200, 300, 100, 1000, 1000, 4, 3, 1,
                     &art, &pic);
    check(pic.y == 100 && pic.h == 1000 && pic.x == 133 && pic.w == 1334,
          "layout: a tall hole is covered by height");

    /* A window the art's own size: one art pixel per client pixel. */
    vgl_bezel_layout(1920, 1080, 1920, 1080, 244, 3, 1433, 1073, 4, 3, 1,
                     &art, &pic);
    check(rect_is(&art, 0, 0, 1920, 1080), "layout: 1:1 art fills the window");
    check(pic.x == 244 && pic.w == 1433 && pic.y <= 3 && pic.y + pic.h >= 1076,
          "layout: 1:1 picture covers the hole exactly");

    /* A 16:10 screen: letterboxed above and below the art. */
    vgl_bezel_layout(1920, 1200, 1920, 1080, 244, 3, 1433, 1073, 4, 3, 1,
                     &art, &pic);
    check(rect_is(&art, 0, 60, 1920, 1080), "layout: 16:9 art on 16:10 is letterboxed");

    /* Art the presenter could not draw: the picture fitted in its shape. */
    vgl_fit_layout(2148, 1208, 4, 3, &pic);
    check(rect_is(&pic, 269, 0, 1610, 1208), "fit: 4:3 in a 16:9 window, centred");
    vgl_fit_layout(1600, 1200, 711, 400, &pic);
    check(rect_is(&pic, 0, 150, 1600, 900), "fit: widescreen in a 4:3 window");

    /* The window for bezel art: hole as tall as the raster at the scale. */
    {
        int cw, ch;
        bezel_client_size(1920, 1080, 1073, 400, 3, &cw, &ch);
        check(cw == 2148 && ch == 1208, "client: 1920x1080 art, 1073-row hole, 400 rows at 3x");
        bezel_client_size(1920, 1080, 1080, 400, 1, &cw, &ch);
        check(cw == 711 && ch == 400, "client: a full-height hole at 1x is the raster's height");
    }

    /* Degenerate input changes nothing. */
    vgl_bezel_layout(0, 0, 1920, 1080, 244, 3, 1433, 1073, 4, 3, 1, &art, &pic);
    check(art.w == 0 && pic.w == 0, "layout: an empty client is left empty");
}

static int dump(const char *in, const char *out_path)
{
    FILE *f = fopen(in, "rb"), *o;
    long n;
    uint8_t *data, *px;
    int w, h, x, y, hw, hh;
    char why[160];

    if (!f) { fprintf(stderr, "cannot open %s\n", in); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; }
    fclose(f);
    if (!bezel_png_decode(data, (size_t)n, &px, &w, &h, why, sizeof why)) {
        fprintf(stderr, "%s: %s\n", in, why);
        return 1;
    }
    if (!bezel_find_hole(px, w, h, &x, &y, &hw, &hh, why, sizeof why)) {
        printf("%d %d no-hole (%s)\n", w, h, why);
    } else {
        printf("%d %d %d %d %d %d\n", w, h, x, y, hw, hh);
    }
    o = fopen(out_path, "wb");
    if (!o || fwrite(px, 1, (size_t)w * h * 4, o) != (size_t)w * h * 4) return 2;
    fclose(o);
    free(px);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct { int ctype, depth; } legal[] = {
        { 0, 1 }, { 0, 2 }, { 0, 4 }, { 0, 8 }, { 0, 16 },
        { 2, 8 }, { 2, 16 }, { 3, 1 }, { 3, 2 }, { 3, 4 }, { 3, 8 },
        { 4, 8 }, { 4, 16 }, { 6, 8 }, { 6, 16 },
    };
    unsigned i;
    int f;

    if (argc == 4 && !strcmp(argv[1], "--dump"))
        return dump(argv[2], argv[3]);

    for (i = 0; i < sizeof legal / sizeof legal[0]; i++) {
        int trns;
        for (trns = 0; trns <= 1; trns++) {
            spec_t sp;
            char name[64];
            if (trns && (legal[i].ctype == 4 || legal[i].ctype == 6))
                continue;               /* tRNS is not allowed with alpha */
            memset(&sp, 0, sizeof sp);
            sp.w = 37; sp.h = 11;       /* odd: partial bytes at row ends */
            sp.ctype = legal[i].ctype; sp.depth = legal[i].depth;
            sp.filter = -1;             /* every filter, row by row */
            sp.npal = sp.ctype == 3 ? (1 << (sp.depth < 8 ? sp.depth : 8)) : 0;
            sp.trns = trns;
            snprintf(name, sizeof name, "colour type %d, %d-bit%s",
                     sp.ctype, sp.depth, trns ? ", tRNS" : "");
            decode_case(&sp, name);
        }
    }
    for (f = 0; f <= 4; f++) {
        spec_t sp = { 70, 9, 6, 8, 0, 0, 0, 0, 0 };
        char name[48];
        sp.filter = f;
        snprintf(name, sizeof name, "RGBA with filter %d on every row", f);
        decode_case(&sp, name);
    }
    {
        spec_t big = { 300, 240, 6, 8, -1, 0, 0, 0, 0 };   /* > one stored block */
        spec_t inter = { 8, 8, 6, 8, 0, 0, 0, 1, 0 };
        spec_t crc = { 8, 8, 6, 8, 0, 0, 0, 0, 1 };
        decode_case(&big, "a 288000-byte image across several deflate blocks");
        refuse_case(&inter, "an interlaced file", "interlaced");
        refuse_case(&crc, "a damaged chunk", "CRC");
    }
    {
        uint8_t junk[8] = "GIF89a!";
        uint8_t *px;
        int w, h;
        char why[160];
        check(!bezel_png_decode(junk, sizeof junk, &px, &w, &h, why, sizeof why) &&
              strstr(why, "not a PNG"), "a file that is not a PNG says so");
    }
    hole_cases();
    load_case();
    layout_cases();

    printf("bezel: %d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
