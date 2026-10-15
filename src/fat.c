/* fat.c -- the partitions and FAT volumes of a cabinet's disk, read-only.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A port of the Python ingest's read_partitions(), read_extended() and Fat,
 * which the maintainer keeps as its oracle. Sectors are addressed as 512 bytes
 * throughout, as there. Files reach 1 GB (Offroad's GD3.R2), so nothing is
 * held whole. */
#include "fat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512u
#define ATTR_VOLUME 0x08
#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F
#define PATH_MAX_W 1024

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void say(char *err, size_t errsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void say(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* ---- the partition table ------------------------------------------------- */

int fat_type_known(uint8_t type)
{
    return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B ||
           type == 0x0C || type == 0x0E;
}

const char *fat_type_name(uint8_t type)
{
    switch (type) {
    case 0x00: return "no MBR";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 <32M";
    case 0x06: return "FAT16";
    case 0x0B: return "FAT32";
    case 0x0C: return "FAT32 LBA";
    case 0x0E: return "FAT16 LBA";
    default:   return "not a FAT type";
    }
}

static int is_extended(uint8_t type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* THE CABINET DISKS USE EXTENDED PARTITIONS: Hydro's drive
 * C is a primary FAT16, and D and E are logical volumes inside a type-0x05
 * extended partition. In an EBR the two entries are relative to DIFFERENT
 * bases: entry 0 to this EBR, entry 1 to the extended partition's start. */
static int read_extended(const fat_image_t *img, uint32_t ext_base,
                         fat_part_t *out, int n, int max)
{
    uint32_t cur = ext_base, seen[64];
    int nseen = 0, i;

    while (cur && nseen < 64) {
        uint8_t ebr[SECTOR];
        for (i = 0; i < nseen; i++)
            if (seen[i] == cur)
                return n;               /* a malformed chain must not loop */
        seen[nseen++] = cur;
        if (!img->read(img->ctx, (uint64_t)cur * SECTOR, ebr, SECTOR) ||
            ebr[510] != 0x55 || ebr[511] != 0xAA)
            break;
        if (ebr[446 + 4] && le32(ebr + 446 + 12) && n < max) {
            out[n].type = ebr[446 + 4];
            out[n].lba = cur + le32(ebr + 446 + 8);
            n++;
        }
        cur = ebr[462 + 4] && le32(ebr + 462 + 12)
            ? ext_base + le32(ebr + 462 + 8) : 0;
    }
    return n;
}

int fat_partitions(const fat_image_t *img, fat_part_t *out, int max)
{
    uint8_t mbr[SECTOR];
    int i, n = 0;

    if (!img->read(img->ctx, 0, mbr, SECTOR) || mbr[510] != 0x55 ||
        mbr[511] != 0xAA)
        goto whole;                     /* a partitionless image is legal */
    for (i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        if (type == 0 || le32(e + 12) == 0)
            continue;
        if (is_extended(type)) {
            n = read_extended(img, le32(e + 8), out, n, max);
            continue;
        }
        if (n < max) {
            out[n].type = type;
            out[n].lba = le32(e + 8);
            n++;
        }
    }
    if (n)
        return n;
whole:
    out[0].type = 0;
    out[0].lba = 0;
    return 1;
}

/* ---- FAT ------------------------------------------------------------------ */

struct fat {
    const fat_image_t *img;
    uint64_t base;
    unsigned bps, spc, reserved, nfats, root_entries, bits;
    uint32_t spf, total, root_sectors, first_data, clusters, root_cluster;
    char label[16];
    uint8_t *fat;
    size_t fatlen;
};

static int sectors(const fat_t *v, uint64_t n, uint32_t count, void *buf)
{
    return v->img->read(v->img->ctx, v->base + n * SECTOR, buf,
                        (size_t)count * SECTOR);
}

static void trim_label(char *dst, const uint8_t *src, size_t n)
{
    size_t i, a = 0, b = n;
    while (a < n && (src[a] == ' ' || src[a] == 0))
        a++;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == 0))
        b--;
    for (i = a; i < b; i++)
        *dst++ = (char)(src[i] >= 0x20 && src[i] < 0x7F ? src[i] : '?');
    *dst = 0;
}

int fat_open(const fat_image_t *img, uint32_t lba, fat_t **out,
             char *err, size_t errsz)
{
    uint8_t b[SECTOR];
    fat_t *v;
    uint32_t total16, spf16;

    *out = NULL;
    v = calloc(1, sizeof *v);
    if (!v) {
        say(err, errsz, "out of memory");
        return -1;
    }
    v->img = img;
    v->base = (uint64_t)lba * SECTOR;
    if (!sectors(v, 0, 1, b)) {
        say(err, errsz, "cannot read its boot sector");
        goto fail;
    }
    v->bps = le16(b + 0x0B);
    v->spc = b[0x0D];
    v->reserved = le16(b + 0x0E);
    v->nfats = b[0x10];
    v->root_entries = le16(b + 0x11);
    total16 = le16(b + 0x13);
    spf16 = le16(b + 0x16);
    if (!v->bps || !v->spc) {
        say(err, errsz, "not a FAT volume (zero bytes/sector or /cluster)");
        goto fail;
    }
    v->spf = spf16 ? spf16 : le32(b + 0x24);
    v->total = total16 ? total16 : le32(b + 0x20);
    v->root_sectors = (v->root_entries * 32u + v->bps - 1) / v->bps;
    v->first_data = v->reserved + v->nfats * v->spf + v->root_sectors;
    if (!v->spf || v->total <= v->first_data) {
        say(err, errsz, "not a FAT volume (its layout does not add up)");
        goto fail;
    }
    v->clusters = (v->total - v->first_data) / v->spc;
    /* By CLUSTER COUNT, not by the partition type byte, which is a hint the
     * formatter wrote and is wrong often enough to matter. */
    v->bits = v->clusters < 4085 ? 12 : (v->clusters < 65525 ? 16 : 32);
    v->root_cluster = v->bits == 32 ? le32(b + 0x2C) : 0;
    trim_label(v->label, b + (v->bits == 32 ? 0x47 : 0x2B), 11);
    v->fatlen = (size_t)v->spf * SECTOR;
    v->fat = malloc(v->fatlen);
    if (!v->fat || !sectors(v, v->reserved, v->spf, v->fat)) {
        say(err, errsz, "cannot read its allocation table");
        goto fail;
    }
    *out = v;
    return 0;
fail:
    fat_close(v);
    return -1;
}

void fat_close(fat_t *v)
{
    if (!v)
        return;
    free(v->fat);
    free(v);
}

unsigned fat_bits(const fat_t *v) { return v->bits; }
const char *fat_label(const fat_t *v) { return v->label; }

/* The next cluster in a chain: 0 at its end, UINT32_MAX when the table does
 * not hold the entry. */
static uint32_t next_cluster(const fat_t *v, uint32_t cl)
{
    uint32_t x;
    if (v->bits == 16) {
        if ((size_t)cl * 2 + 2 > v->fatlen)
            return UINT32_MAX;
        x = le16(v->fat + (size_t)cl * 2);
        return x >= 0xFFF8 ? 0 : x;
    }
    if (v->bits == 32) {
        if ((size_t)cl * 4 + 4 > v->fatlen)
            return UINT32_MAX;
        x = le32(v->fat + (size_t)cl * 4) & 0x0FFFFFFF;
        return x >= 0x0FFFFFF8 ? 0 : x;
    }
    /* FAT12: 1.5 bytes per entry, nibble-packed by parity. */
    if ((size_t)cl + cl / 2 + 2 > v->fatlen)
        return UINT32_MAX;
    x = le16(v->fat + cl + cl / 2);
    x = cl & 1 ? x >> 4 : x & 0x0FFF;
    return x >= 0xFF8 ? 0 : x;
}

static uint64_t cluster_pos(const fat_t *v, uint32_t cl)
{
    return v->base + ((uint64_t)v->first_data + (uint64_t)(cl - 2) * v->spc) *
           SECTOR;
}

/* A chain as a list, at most `want` clusters (0: the whole chain). A loop or a
 * link outside the table is damage, and is reported rather than followed. */
static int chain(const fat_t *v, uint32_t cl, uint32_t want, uint32_t **out,
                 uint32_t *count, char *err, size_t errsz)
{
    /* Every entry the table can hold, so any cluster number next_cluster()
     * accepts has a bit: a revisit is a loop, whatever its length. */
    size_t entries = v->fatlen * 8 / v->bits;
    uint32_t n = 0, cap = 64, *list = malloc(cap * sizeof *list);
    uint8_t *seen = calloc(entries / 8 + 1, 1);

    if (!list || !seen) {
        free(list);
        free(seen);
        say(err, errsz, "out of memory");
        return -1;
    }
    while (cl >= 2 && (!want || n < want)) {
        if (cl >= entries || (seen[cl >> 3] & (1u << (cl & 7)))) {
            if (cl >= entries)
                say(err, errsz, "a cluster chain leaves the allocation table");
            else
                say(err, errsz, "a cluster chain loops (at cluster %u)", cl);
            goto fail;
        }
        seen[cl >> 3] |= (uint8_t)(1u << (cl & 7));
        if (n == cap) {
            uint32_t *p = realloc(list, (cap *= 2) * sizeof *list);
            if (!p) {
                say(err, errsz, "out of memory");
                goto fail;
            }
            list = p;
        }
        list[n++] = cl;
        cl = next_cluster(v, cl);
        if (cl == UINT32_MAX) {
            say(err, errsz, "a cluster chain leaves the allocation table");
            goto fail;
        }
    }
    free(seen);
    *out = list;
    *count = n;
    return 0;
fail:
    free(list);
    free(seen);
    return -1;
}

/* A directory's bytes: the fixed root of FAT12/16, or a cluster chain. */
static uint8_t *dir_bytes(const fat_t *v, uint32_t cl, int root, size_t *len,
                          char *err, size_t errsz)
{
    uint32_t *list = NULL, n = 0, i;
    size_t cbytes = (size_t)v->spc * SECTOR;
    uint8_t *blob;

    if (root && v->bits != 32) {
        *len = (size_t)v->root_sectors * SECTOR;
        blob = malloc(*len ? *len : 1);
        if (!blob || !sectors(v, v->reserved + (uint64_t)v->nfats * v->spf,
                              v->root_sectors, blob)) {
            free(blob);
            say(err, errsz, "cannot read the root directory");
            return NULL;
        }
        return blob;
    }
    if (chain(v, root ? v->root_cluster : cl, 0, &list, &n, err, errsz))
        return NULL;
    *len = (size_t)n * cbytes;
    blob = malloc(*len ? *len : 1);
    for (i = 0; blob && i < n; i++) {
        if (!v->img->read(v->img->ctx, cluster_pos(v, list[i]),
                          blob + (size_t)i * cbytes, cbytes)) {
            free(blob);
            blob = NULL;
        }
    }
    free(list);
    if (!blob)
        say(err, errsz, "cannot read a directory");
    return blob;
}

/* An 8.3 name, Latin-1, 0x05 standing for a leading 0xE5. */
static size_t short_name(const uint8_t *raw, wchar_t *dst)
{
    size_t n = 0, i, stem = 8, ext = 3;
    while (stem && raw[stem - 1] == ' ')
        stem--;
    while (ext && raw[8 + ext - 1] == ' ')
        ext--;
    for (i = 0; i < stem; i++)
        dst[n++] = (wchar_t)(i == 0 && raw[0] == 0x05 ? 0xE5 : raw[i]);
    if (ext) {
        dst[n++] = L'.';
        for (i = 0; i < ext; i++)
            dst[n++] = raw[8 + i];
    }
    dst[n] = 0;
    return n;
}

typedef struct {
    unsigned seq;
    wchar_t chars[14];
} lfn_part_t;

static int walk_dir(fat_t *v, const uint8_t *blob, size_t len, wchar_t *path,
                    size_t plen, fat_visit_t visit, void *ctx, int depth,
                    char *err, size_t errsz)
{
    lfn_part_t lfn[20];
    int nlfn = 0, rc;
    size_t i;

    if (depth > 32) {
        say(err, errsz, "directories nest implausibly deep");
        return -1;
    }
    for (i = 0; i + 32 <= len; i += 32) {
        const uint8_t *e = blob + i;
        uint8_t attr = e[0x0B];
        wchar_t name[300];
        size_t nlen = 0;
        uint32_t cl, size;
        int k;

        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5) {                     /* deleted */
            nlfn = 0;
            continue;
        }
        if (attr == ATTR_LFN) {
            static const int at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22,
                                        24, 28, 30 };
            if (nlfn < 20) {
                int j, m = 0;
                lfn[nlfn].seq = e[0] & 0x3F;
                for (j = 0; j < 13; j++) {
                    wchar_t ch = (wchar_t)le16(e + at[j]);
                    if (ch == 0x0000 || ch == 0xFFFF)
                        break;
                    lfn[nlfn].chars[m++] = ch;
                }
                lfn[nlfn].chars[m] = 0;
                nlfn++;
            }
            continue;
        }
        if ((attr & ATTR_VOLUME) && !(attr & ATTR_DIR)) {
            nlfn = 0;
            continue;
        }
        if (nlfn) {
            /* The parts arrive last first; they are joined in seq order. */
            unsigned want;
            for (want = 0; want <= 0x3F; want++)
                for (k = 0; k < nlfn; k++)
                    if (lfn[k].seq == want) {
                        size_t m = wcslen(lfn[k].chars);
                        if (nlen + m < sizeof name / sizeof name[0]) {
                            memcpy(name + nlen, lfn[k].chars, m * sizeof *name);
                            nlen += m;
                        }
                    }
            name[nlen] = 0;
        } else {
            nlen = short_name(e, name);
        }
        nlfn = 0;
        cl = le16(e + 0x1A) | (v->bits == 32 ? (uint32_t)le16(e + 0x14) << 16
                                             : 0);
        size = le32(e + 0x1C);
        if (!wcscmp(name, L".") || !wcscmp(name, L".."))
            continue;
        if (plen + nlen + 2 >= PATH_MAX_W) {
            say(err, errsz, "a path inside the disk is too long");
            return -1;
        }
        memcpy(path + plen, name, (nlen + 1) * sizeof *path);
        if (attr & ATTR_DIR) {
            if (cl >= 2) {
                size_t sublen;
                uint8_t *sub = dir_bytes(v, cl, 0, &sublen, err, errsz);
                if (!sub)
                    return -1;
                path[plen + nlen] = L'\\';
                path[plen + nlen + 1] = 0;
                rc = walk_dir(v, sub, sublen, path, plen + nlen + 1, visit,
                              ctx, depth + 1, err, errsz);
                free(sub);
                if (rc)
                    return rc;
            }
        } else {
            rc = visit(ctx, path, cl, size);
            if (rc)
                return rc;
        }
        path[plen] = 0;
    }
    return 0;
}

int fat_walk(fat_t *v, fat_visit_t visit, void *ctx, char *err, size_t errsz)
{
    wchar_t path[PATH_MAX_W];
    size_t len;
    uint8_t *root = dir_bytes(v, 0, 1, &len, err, errsz);
    int rc;
    if (!root)
        return -1;
    path[0] = 0;
    rc = walk_dir(v, root, len, path, 0, visit, ctx, 0, err, errsz);
    free(root);
    return rc;
}

int fat_read_file(fat_t *v, uint32_t cluster, uint32_t size, fat_sink_t sink,
                  void *ctx, char *err, size_t errsz)
{
    size_t cbytes = (size_t)v->spc * SECTOR, nrun = 0, r;
    uint32_t want, *list = NULL, n = 0, i;
    uint64_t *rstart = NULL, left = size;
    uint32_t *rlen = NULL;
    uint8_t *buf = NULL;
    int rc = -1;

    if (size == 0 || cluster < 2)
        return 0;
    want = (uint32_t)((size + cbytes - 1) / cbytes);
    if (chain(v, cluster, want, &list, &n, err, errsz))
        return -1;
    if (n < want) {
        say(err, errsz, "a file's cluster chain ends before its size does");
        goto out;
    }
    /* The chain as runs of adjacent clusters: one plan entry and one read
     * per run rather than per cluster. A run is capped so a read fits the
     * buffer. */
    rstart = malloc(n * sizeof *rstart);
    rlen = malloc(n * sizeof *rlen);
    buf = malloc(1u << 20);
    if (!rstart || !rlen || !buf) {
        say(err, errsz, "out of memory");
        goto out;
    }
    for (i = 0; i < n; i++) {
        uint64_t pos = cluster_pos(v, list[i]);
        if (nrun && rstart[nrun - 1] + rlen[nrun - 1] == pos &&
            rlen[nrun - 1] + cbytes <= (1u << 20)) {
            rlen[nrun - 1] += (uint32_t)cbytes;
        } else {
            rstart[nrun] = pos;
            rlen[nrun] = (uint32_t)(cbytes <= (1u << 20) ? cbytes : (1u << 20));
            nrun++;
        }
    }
    if (cbytes > (1u << 20)) {
        say(err, errsz, "clusters larger than 1 MB are not read here");
        goto out;
    }
    if (v->img->plan)
        v->img->plan(v->img->ctx, rstart, rlen, nrun);
    for (r = 0; r < nrun && left; r++) {
        size_t take = rlen[r] < left ? rlen[r] : (size_t)left;
        int s;
        if (!v->img->read(v->img->ctx, rstart[r], buf, take)) {
            say(err, errsz, "a read inside a file failed");
            goto out;
        }
        s = sink(ctx, buf, take);
        if (s) {
            rc = s;
            goto out;
        }
        left -= take;
    }
    rc = 0;
out:
    free(list);
    free(rstart);
    free(rlen);
    free(buf);
    return rc;
}
