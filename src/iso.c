/* iso.c -- files out of a CD image, read-only.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The C form of the project's Python ISO 9660 reader, which the maintainer
 * keeps as the oracle. ISO 9660 is ECMA-119, published; Joliet's UCS-2 names
 * are its supplementary descriptor with one of three escape sequences. NO GAME
 * MATERIAL IS IN THIS FILE. */
#include "iso.h"

#include <windows.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u
#define VD_START 16u
#define BATCH 16u                   /* sectors read at a time */
#define MAX_DEPTH 32

static const uint8_t k_sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0 };

struct iso {
    fat_image_t img;
    uint32_t stride;                /* bytes per sector in the image */
    int raw;                        /* sync, header and ECC around the data */
    int joliet;
    iso_node_t root;
    char layout[64];
    uint8_t *buf;                   /* BATCH sectors as the image holds them */
};

static void set_err(char *dst, size_t sz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void set_err(char *dst, size_t sz, const char *fmt, ...)
{
    va_list ap;
    if (!dst || !sz)
        return;
    va_start(ap, fmt);
    vsnprintf(dst, sz, fmt, ap);
    va_end(ap);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* ---- sectors ---------------------------------------------------------------- */

/* n <= BATCH sectors' 2048 user bytes each, from lba, into out. */
static int read_sectors(iso_t *v, uint32_t lba, uint32_t n, uint8_t *out,
                        char *err, size_t errsz)
{
    uint32_t i;

    if (v->stride == SECTOR) {
        if (!v->img.read(v->img.ctx, (uint64_t)lba * SECTOR, out,
                         (size_t)n * SECTOR)) {
            set_err(err, errsz, "sector %u could not be read", lba);
            return 0;
        }
        return 1;
    }
    if (!v->img.read(v->img.ctx, (uint64_t)lba * v->stride, v->buf,
                     (size_t)n * v->stride)) {
        set_err(err, errsz, "sector %u could not be read", lba);
        return 0;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *s = v->buf + (size_t)i * v->stride, *data = s;
        if (v->raw) {
            if (memcmp(s, k_sync, sizeof k_sync)) {
                set_err(err, errsz, "sector %u has no sync pattern", lba + i);
                return 0;
            }
            if (s[15] == 1) {
                data = s + 16;
            } else if (s[15] == 2) {
                if (s[18] & 0x20) {
                    set_err(err, errsz, "sector %u is mode 2 form 2, which "
                            "holds no file data", lba + i);
                    return 0;
                }
                data = s + 24;
            } else {
                set_err(err, errsz, "sector %u is mode %u", lba + i, s[15]);
                return 0;
            }
        }
        memcpy(out + (size_t)i * SECTOR, data, SECTOR);
    }
    return 1;
}

/* The layout, from the bytes where sector 16 would be in each: a sync
 * pattern for raw sectors, "CD001" for cooked ones. */
static int find_layout(iso_t *v)
{
    static const uint32_t strides[] = { 2352, 2448, 2048 };
    uint8_t b[16];
    unsigned i;

    for (i = 0; i < sizeof strides / sizeof strides[0]; i++) {
        uint32_t st = strides[i];
        if (!v->img.read(v->img.ctx, (uint64_t)VD_START * st, b, sizeof b))
            continue;
        if (st != SECTOR && !memcmp(b, k_sync, sizeof k_sync)) {
            v->stride = st;
            v->raw = 1;
            snprintf(v->layout, sizeof v->layout, "raw mode %u, %u-byte %s",
                     b[15], st, st == 2448 ? "frames" : "sectors");
            return 1;
        }
    }
    for (i = 0; i < sizeof strides / sizeof strides[0]; i++) {
        uint32_t st = strides[i];
        if (v->img.read(v->img.ctx, (uint64_t)VD_START * st, b, sizeof b) &&
            !memcmp(b + 1, "CD001", 5)) {
            v->stride = st;
            v->raw = 0;
            snprintf(v->layout, sizeof v->layout, "cooked 2048-byte blocks%s",
                     st == SECTOR ? "" : " in larger frames");
            return 1;
        }
    }
    return 0;
}

/* ---- the volume ---------------------------------------------------------------- */

/* The directory record at buf[at]: 0 when it runs off its sector or is
 * interleaved, which this reader refuses. */
static int record(const uint8_t *buf, size_t at, size_t end, iso_node_t *n,
                  const uint8_t **name, unsigned *nlen, uint8_t *flags,
                  char *err, size_t errsz)
{
    unsigned ln = buf[at];
    if (ln < 34 || at + ln > end) {
        set_err(err, errsz, "a directory record runs off its sector");
        return 0;
    }
    *nlen = buf[at + 32];
    if (33 + *nlen > ln) {
        set_err(err, errsz, "a directory record's name runs off it");
        return 0;
    }
    if (buf[at + 26] || buf[at + 27]) {
        set_err(err, errsz, "an interleaved file, which this reader refuses");
        return 0;
    }
    n->extent = le32(buf + at + 2);
    n->size = le32(buf + at + 10);
    *flags = buf[at + 25];
    n->dir = (*flags & 0x02) != 0;
    *name = buf + at + 33;
    return 1;
}

int iso_open(const fat_image_t *img, iso_t **out, char *err, size_t errsz)
{
    uint8_t vd[SECTOR], *primary = NULL, *joliet = NULL;
    uint8_t pv[SECTOR], jv[SECTOR];
    const uint8_t *nm;
    unsigned nlen, k;
    uint8_t flags;
    iso_t *v;

    *out = NULL;
    if (err && errsz)
        err[0] = 0;
    v = calloc(1, sizeof *v);
    if (!v)
        return -1;
    v->img = *img;
    if (!find_layout(v)) {
        free(v);
        return 0;
    }
    v->buf = malloc((size_t)BATCH * v->stride);
    if (!v->buf)
        goto fail;
    for (k = 0; k < 64; k++) {
        if (!read_sectors(v, VD_START + k, 1, vd, err, errsz))
            goto fail;
        if (memcmp(vd + 1, "CD001", 5)) {
            set_err(err, errsz, "sector %u is not a volume descriptor",
                    VD_START + k);
            goto fail;
        }
        if (vd[0] == 255)
            break;
        if (vd[0] == 1 && !primary) {
            memcpy(pv, vd, SECTOR);
            primary = pv;
        } else if (vd[0] == 2 && !joliet && vd[88] == '%' && vd[89] == '/' &&
                   (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            memcpy(jv, vd, SECTOR);
            joliet = jv;
        }
    }
    if (k == 64) {
        set_err(err, errsz, "no volume descriptor set terminator");
        goto fail;
    }
    if (!primary && !joliet) {
        set_err(err, errsz, "no primary volume descriptor");
        goto fail;
    }
    v->joliet = joliet != NULL;
    if (joliet)
        primary = joliet;
    if ((primary[128] | primary[129] << 8) != SECTOR) {
        set_err(err, errsz, "logical blocks are not 2048 bytes");
        goto fail;
    }
    if (!record(primary, 156, 156 + 34, &v->root, &nm, &nlen, &flags, err,
                errsz))
        goto fail;
    v->root.dir = 1;
    if (v->joliet)
        strncat(v->layout, ", Joliet names",
                sizeof v->layout - strlen(v->layout) - 1);
    *out = v;
    return 1;
fail:
    free(v->buf);
    free(v);
    return -1;
}

void iso_close(iso_t *v)
{
    if (!v)
        return;
    free(v->buf);
    free(v);
}

const char *iso_layout(const iso_t *v)
{
    return v->layout;
}

/* A record's name: Joliet's UCS-2 big-endian, or the primary's d-characters,
 * without ";1" (and the primary's trailing '.' of a name with no
 * extension). */
static int name_of(const iso_t *v, const uint8_t *raw, unsigned nlen,
                   wchar_t *out, size_t outsz)
{
    size_t i, n = 0;

    if (v->joliet) {
        for (i = 0; i + 1 < nlen && n + 1 < outsz; i += 2)
            out[n++] = (wchar_t)(raw[i] << 8 | raw[i + 1]);
    } else {
        for (i = 0; i < nlen && n + 1 < outsz; i++)
            out[n++] = raw[i];
    }
    out[n] = 0;
    for (i = 0; i < n; i++)
        if (out[i] == L';') {
            out[i] = 0;
            n = i;
            break;
        }
    if (!v->joliet && n && out[n - 1] == L'.')
        out[--n] = 0;
    return n > 0;
}

/* Each entry of a directory but "." and "..", to fn. */
typedef int (*entry_fn)(void *ctx, const wchar_t *name, const iso_node_t *n);

static int each_entry(iso_t *v, const iso_node_t *d, entry_fn fn, void *ctx,
                      char *err, size_t errsz)
{
    uint8_t sec[SECTOR];
    uint32_t k, count = (d->size + SECTOR - 1) / SECTOR;

    for (k = 0; k < count; k++) {
        size_t at = 0;
        if (!read_sectors(v, d->extent + k, 1, sec, err, errsz))
            return -1;
        while (at < SECTOR && sec[at]) {
            iso_node_t n;
            const uint8_t *nm;
            unsigned nlen;
            uint8_t flags;
            wchar_t name[256];
            int rc;

            if (!record(sec, at, SECTOR, &n, &nm, &nlen, &flags, err, errsz))
                return -1;
            at += sec[at];
            if (nlen == 1 && (nm[0] == 0 || nm[0] == 1))
                continue;
            if (!name_of(v, nm, nlen, name, sizeof name / sizeof name[0])) {
                set_err(err, errsz, "a directory record has no name");
                return -1;
            }
            if (flags & 0x80) {
                set_err(err, errsz, "a file spread over several extents, "
                        "which this reader refuses");
                return -1;
            }
            if ((rc = fn(ctx, name, &n)) != 0)
                return rc;
        }
    }
    return 0;
}

typedef struct {
    const wchar_t *want;
    iso_node_t hit;
    int found;
} find_t;

static int find_entry(void *ctx, const wchar_t *name, const iso_node_t *n)
{
    find_t *f = ctx;
    if (_wcsicmp(name, f->want))
        return 0;
    f->hit = *n;
    f->found = 1;
    return 1;
}

int iso_lookup(iso_t *v, const char *path, iso_node_t *n, char *err,
               size_t errsz)
{
    wchar_t w[1024], *part, *next;
    iso_node_t d = v->root;

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) {
        set_err(err, errsz, "the path is too long");
        return -1;
    }
    for (part = w; part && *part; part = next) {
        find_t f;
        int rc;
        next = wcspbrk(part, L"/\\");
        if (next)
            *next++ = 0;
        if (!*part)
            continue;
        if (!d.dir)
            return 0;
        memset(&f, 0, sizeof f);
        f.want = part;
        rc = each_entry(v, &d, find_entry, &f, err, errsz);
        if (rc < 0)
            return -1;
        if (!f.found)
            return 0;
        d = f.hit;
    }
    *n = d;
    return 1;
}

typedef struct {
    iso_t *v;
    iso_visit_t visit;
    void *ctx;
    wchar_t path[1024];
    uint32_t chain[MAX_DEPTH];
    int depth;
    char *err;
    size_t errsz;
} walk_t;

static int walk_dir(walk_t *w, const iso_node_t *d);

static int walk_entry(void *ctx, const wchar_t *name, const iso_node_t *n)
{
    walk_t *w = ctx;
    size_t at = wcslen(w->path);
    int rc;

    if (at + wcslen(name) + 2 >= sizeof w->path / sizeof w->path[0]) {
        set_err(w->err, w->errsz, "a path on the disc is too long");
        return -1;
    }
    wcscpy(w->path + at, name);
    if (n->dir) {
        wcscat(w->path, L"\\");
        rc = walk_dir(w, n);
    } else {
        rc = w->visit(w->ctx, w->path, n);
    }
    w->path[at] = 0;
    return rc;
}

static int walk_dir(walk_t *w, const iso_node_t *d)
{
    int i, rc;

    if (w->depth >= MAX_DEPTH) {
        set_err(w->err, w->errsz, "directories nested too deeply");
        return -1;
    }
    for (i = 0; i < w->depth; i++)
        if (w->chain[i] == d->extent) {
            set_err(w->err, w->errsz, "a directory contains itself");
            return -1;
        }
    w->chain[w->depth++] = d->extent;
    rc = each_entry(w->v, d, walk_entry, w, w->err, w->errsz);
    w->depth--;
    return rc;
}

int iso_walk(iso_t *v, const iso_node_t *dir, iso_visit_t visit, void *ctx,
             char *err, size_t errsz)
{
    walk_t *w = calloc(1, sizeof *w);
    int rc;

    if (!w)
        return -1;
    w->v = v;
    w->visit = visit;
    w->ctx = ctx;
    w->err = err;
    w->errsz = errsz;
    rc = walk_dir(w, dir);
    free(w);
    return rc;
}

int iso_read_file(iso_t *v, const iso_node_t *n, fat_sink_t sink, void *ctx,
                  char *err, size_t errsz)
{
    uint32_t left = n->size, lba = n->extent;
    uint32_t count = n->size / SECTOR + (n->size % SECTOR != 0);
    uint8_t *data;
    int rc = 0;

    if (n->dir) {
        set_err(err, errsz, "a directory, not a file");
        return -1;
    }
    if (v->img.plan && count) {
        /* A raw image's file can pass 4 GiB; a plan entry is 32-bit. */
        uint64_t start[5], s = (uint64_t)lba * v->stride;
        uint64_t l = (uint64_t)count * v->stride;
        uint32_t len[5];
        size_t k = 0;

        while (l && k < 5) {
            len[k] = l > 0x40000000u ? 0x40000000u : (uint32_t)l;
            start[k] = s;
            s += len[k];
            l -= len[k++];
        }
        v->img.plan(v->img.ctx, start, len, k);
    }
    data = malloc((size_t)BATCH * SECTOR);
    if (!data)
        return -1;
    while (left) {
        uint32_t k = count > BATCH ? BATCH : count;
        uint32_t take = k * SECTOR < left ? k * SECTOR : left;
        if (!read_sectors(v, lba, k, data, err, errsz)) {
            rc = -1;
            break;
        }
        if ((rc = sink(ctx, data, take)) != 0)
            break;
        left -= take;
        lba += k;
        count -= k;
    }
    free(data);
    return rc;
}

/* ---- an image on the host --------------------------------------------------- */

static int file_read(void *ctx, uint64_t pos, void *buf, size_t n)
{
    OVERLAPPED ov;
    DWORD got = 0;
    uint8_t *p = buf;

    while (n) {
        DWORD want = n > (1u << 30) ? 1u << 30 : (DWORD)n;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)pos;
        ov.OffsetHigh = (DWORD)(pos >> 32);
        if (!ReadFile((HANDLE)ctx, p, want, &got, &ov) || !got)
            return 0;
        p += got;
        pos += got;
        n -= got;
    }
    return 1;
}

static int ends_with(const wchar_t *s, const wchar_t *ext)
{
    size_t n = wcslen(s), e = wcslen(ext);
    return n >= e && !_wcsicmp(s + n - e, ext);
}

int iso_file_name(const wchar_t *path)
{
    return ends_with(path, L".cue") || ends_with(path, L".toc") ||
           ends_with(path, L".iso") || ends_with(path, L".bin");
}

/* The image a sheet names, beside the sheet: the first FILE "x" (cue) or
 * FILE/DATAFILE "x" (toc). A data disc's filesystem is in its first track. */
static int sheet_image(const wchar_t *sheet, wchar_t *out, size_t outsz)
{
    char text[65536], *line;
    wchar_t name[MAX_PATH];
    const wchar_t *slash;
    HANDLE f;
    DWORD got = 0;

    f = CreateFileW(sheet, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (GetFileSize(f, NULL) >= sizeof text ||
        !ReadFile(f, text, sizeof text - 1, &got, NULL)) {
        CloseHandle(f);
        return 0;
    }
    CloseHandle(f);
    text[got] = 0;
    for (line = text; line && *line; line = strpbrk(line, "\r\n")) {
        char *q, *e;
        while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t')
            line++;
        if (_strnicmp(line, "FILE", 4) && _strnicmp(line, "DATAFILE", 8))
            continue;
        q = line + (toupper((unsigned char)line[0]) == 'D' ? 8 : 4);
        if (*q != ' ' && *q != '\t')
            continue;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '"' || !(e = strchr(q + 1, '"')))
            continue;
        *e = 0;
        if (!MultiByteToWideChar(CP_ACP, 0, q + 1, -1, name, MAX_PATH))
            return 0;
        slash = wcsrchr(sheet, L'\\');
        if (wcschr(name, L':') || name[0] == L'\\' || !slash)
            _snwprintf(out, outsz, L"%ls", name);
        else
            _snwprintf(out, outsz, L"%.*ls\\%ls", (int)(slash - sheet), sheet,
                       name);
        out[outsz - 1] = 0;
        return 1;
    }
    return 0;
}

int iso_file_open(const wchar_t *path, fat_image_t *img, char *err,
                  size_t errsz)
{
    wchar_t image[MAX_PATH * 2];
    HANDLE f;

    memset(img, 0, sizeof *img);
    if (!iso_file_name(path))
        return 0;
    if (ends_with(path, L".cue") || ends_with(path, L".toc")) {
        if (!sheet_image(path, image, sizeof image / sizeof image[0])) {
            set_err(err, errsz, "the sheet names no image file");
            return 0;
        }
    } else {
        _snwprintf(image, sizeof image / sizeof image[0], L"%ls", path);
        image[sizeof image / sizeof image[0] - 1] = 0;
    }
    f = CreateFileW(image, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_FLAG_RANDOM_ACCESS, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        char n[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, image, -1, n, sizeof n, NULL, NULL);
        set_err(err, errsz, "%s cannot be opened (Windows error %lu)", n,
                GetLastError());
        return 0;
    }
    img->read = file_read;
    img->ctx = f;
    return 1;
}

void iso_file_close(fat_image_t *img)
{
    if (img->ctx)
        CloseHandle((HANDLE)img->ctx);
    memset(img, 0, sizeof *img);
}
