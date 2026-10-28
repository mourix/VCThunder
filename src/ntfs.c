/* ntfs.c -- files out of an NTFS volume, read-only.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A port of the project's Python NTFS reader, which the maintainer keeps as
 * its oracle and which is itself checked against an extraction made by
 * another tool. What it reads: the boot sector, MFT records with their
 * update-sequence fixups, attribute lists, run lists, resident and non-resident unnamed $DATA, and directory
 * indexes walked from each $INDEX_ROOT down through $INDEX_ALLOCATION, so
 * only live index nodes are ever read. */
#include "ntfs.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512u
#define STRIDE 512u        /* the update sequence's, whatever the sector */
#define NTFS_TYPE 0x07

#define AT_ATTRIBUTE_LIST   0x20u
#define AT_DATA             0x80u
#define AT_INDEX_ROOT       0x90u
#define AT_INDEX_ALLOCATION 0xA0u
#define AT_END              0xFFFFFFFFu

#define ATTR_COMPRESSED 0x0001u
#define ATTR_ENCRYPTED  0x4000u
#define REC_IN_USE      0x0001u
#define REC_DIRECTORY   0x0002u
#define NS_DOS          2           /* the 8.3 alias; the long name has its own entry */
#define DUP_DIRECTORY   0x10000000u /* $FILE_NAME's flags: the entry is a directory */
#define ENTRY_NODE      0x01u       /* a subnode's VCN ends the entry */
#define ENTRY_END       0x02u       /* the last entry of a node, with no key */

#define MAX_EXTENSIONS 32           /* records one file's attributes span */
#define MAX_ATTRS 256
#define PATH_W 1024
#define CHUNK (1u << 16)

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}
static uint64_t ref_of(uint64_t r) { return r & 0xFFFFFFFFFFFFull; }

static void say(char *err, size_t errsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void say(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* ---- the volume ------------------------------------------------------------ */

typedef struct {
    uint64_t vcn, lcn, len;
    int sparse;
} run_t;

typedef struct {
    run_t *r;
    size_t n, cap;
} runs_t;

struct ntfs {
    fat_image_t img;
    uint64_t base, clusters, mft_size;
    uint32_t sector, cluster, rec_size, idx_size;
    runs_t mft;
    char *err;                      /* where messages go: the caller's buffer */
    size_t errsz;
    char idle[256];                 /* ... or this, between calls */
};

static int is_ntfs(const uint8_t *s) { return !memcmp(s + 3, "NTFS    ", 8); }

int ntfs_volumes(const fat_image_t *img, uint64_t *out, int max)
{
    uint8_t s[SECTOR];
    int i, n = 0;

    if (max < 1 || !img->read(img->ctx, 0, s, SECTOR))
        return 0;
    if (is_ntfs(s)) {
        out[0] = 0;
        return 1;
    }
    if (s[510] != 0x55 || s[511] != 0xAA)
        return 0;
    for (i = 0; i < 4 && n < max; i++) {
        const uint8_t *e = s + 446 + i * 16;
        if (e[4] == NTFS_TYPE && le32(e + 12))
            out[n++] = (uint64_t)le32(e + 8) * SECTOR;
    }
    return n;
}

uint32_t ntfs_cluster_bytes(const ntfs_t *v) { return v->cluster; }
uint64_t ntfs_record_count(const ntfs_t *v)
{
    return v->mft_size / v->rec_size;
}

static int raw(ntfs_t *v, uint64_t pos, void *buf, size_t n)
{
    if (!v->img.read(v->img.ctx, v->base + pos, buf, n)) {
        say(v->err, v->errsz, "the disk could not be read at volume byte "
            "%llu", (unsigned long long)pos);
        return 0;
    }
    return 1;
}

static int runs_add(runs_t *rs, uint64_t vcn, uint64_t lcn, uint64_t len,
                    int sparse)
{
    if (rs->n == rs->cap) {
        size_t cap = rs->cap ? rs->cap * 2 : 16;
        run_t *r = realloc(rs->r, cap * sizeof *r);
        if (!r)
            return 0;
        rs->r = r;
        rs->cap = cap;
    }
    rs->r[rs->n].vcn = vcn;
    rs->r[rs->n].lcn = lcn;
    rs->r[rs->n].len = len;
    rs->r[rs->n].sparse = sparse;
    rs->n++;
    return 1;
}

/* A mapping-pairs array: each run is a header byte (low nibble: bytes of
 * length, high nibble: bytes of a signed LCN delta, 0 for a sparse run). */
static int decode_runs(ntfs_t *v, const uint8_t *p, size_t len, uint64_t vcn,
                       runs_t *out)
{
    int64_t lcn = 0;
    size_t at = 0;

    while (at < len && p[at]) {
        unsigned nlen = p[at] & 0x0F, noff = p[at] >> 4, i;
        uint64_t length = 0;
        int64_t delta = 0;

        at++;
        if (!nlen || nlen > 8 || noff > 8 || at + nlen + noff > len) {
            say(v->err, v->errsz, "a run list runs off its attribute");
            return 0;
        }
        for (i = 0; i < nlen; i++)
            length |= (uint64_t)p[at + i] << (8 * i);
        at += nlen;
        if (noff) {
            uint64_t u = 0;
            for (i = 0; i < noff; i++)
                u |= (uint64_t)p[at + i] << (8 * i);
            if (noff < 8 && (u >> (8 * noff - 1)) & 1)
                u |= ~0ull << (8 * noff);
            delta = (int64_t)u;
            lcn += delta;
            if (!runs_add(out, vcn, (uint64_t)lcn, length, 0))
                return 0;
        } else if (!runs_add(out, vcn, 0, length, 1)) {
            return 0;
        }
        at += noff;
        vcn += length;
    }
    return 1;
}

/* n bytes at byte `pos` of a stream; sparse runs read as zeroes. */
static int read_runs(ntfs_t *v, const runs_t *rs, uint64_t pos, uint8_t *buf,
                     size_t n)
{
    while (n) {
        uint64_t vcn = pos / v->cluster, into = pos % v->cluster, take;
        const run_t *r = NULL;
        size_t i;

        for (i = 0; i < rs->n && !r; i++)
            if (rs->r[i].vcn <= vcn && vcn < rs->r[i].vcn + rs->r[i].len)
                r = &rs->r[i];
        if (!r) {
            say(v->err, v->errsz, "byte %llu is outside its run list",
                (unsigned long long)pos);
            return 0;
        }
        take = (r->vcn + r->len - vcn) * v->cluster - into;
        if (take > n)
            take = n;
        if (r->sparse) {
            memset(buf, 0, (size_t)take);
        } else {
            uint64_t lcn = r->lcn + vcn - r->vcn;
            if (lcn + (into + take - 1) / v->cluster >= v->clusters) {
                say(v->err, v->errsz, "a run points past the volume (LCN "
                    "%llu)", (unsigned long long)lcn);
                return 0;
            }
            if (!raw(v, lcn * v->cluster + into, buf, (size_t)take))
                return 0;
        }
        pos += take;
        buf += take;
        n -= (size_t)take;
    }
    return 1;
}

/* The update sequence: the last two bytes of every 512-byte block were
 * replaced by the sequence number on write and the array holds the real ones.
 * The block is 512 bytes on any sector size. A block whose tail does not match
 * is a torn write. */
static int fixup(ntfs_t *v, uint8_t *buf, size_t len, const char *sig,
                 const char *what, uint64_t n)
{
    unsigned usa, count, i;

    if (memcmp(buf, sig, 4)) {
        say(v->err, v->errsz, "%s %llu has no %s signature", what,
            (unsigned long long)n, sig);
        return 0;
    }
    usa = le16(buf + 4);
    count = le16(buf + 6);
    if (count - 1 != len / STRIDE || usa + 2u * count > len) {
        say(v->err, v->errsz, "%s %llu: an update sequence of %u for %u "
            "bytes", what, (unsigned long long)n, count, (unsigned)len);
        return 0;
    }
    for (i = 1; i < count; i++) {
        uint8_t *end = buf + i * STRIDE;
        if (end[-2] != buf[usa] || end[-1] != buf[usa + 1]) {
            say(v->err, v->errsz, "%s %llu: block %u fails its update "
                "sequence check", what, (unsigned long long)n, i - 1);
            return 0;
        }
        end[-2] = buf[usa + 2 * i];
        end[-1] = buf[usa + 2 * i + 1];
    }
    return 1;
}

static int record(ntfs_t *v, uint64_t n, uint8_t *buf)
{
    return read_runs(v, &v->mft, n * v->rec_size, buf, v->rec_size) &&
           fixup(v, buf, v->rec_size, "FILE", "MFT record", n);
}

/* ---- attributes ------------------------------------------------------------- */

typedef struct {
    uint32_t type;
    uint16_t flags;
    int nonres;
    const uint8_t *name;            /* UTF-16LE, in a record buffer */
    unsigned name_len;
    const uint8_t *value;           /* resident */
    uint32_t vlen;
    uint64_t vcn0, size, init;      /* non-resident */
    const uint8_t *map;             /* its mapping pairs */
    size_t map_len;
} attr_t;

/* A file's attributes, over its base record and any extension records its
 * $ATTRIBUTE_LIST names. The attributes point into the buffers held here. */
typedef struct {
    uint16_t flags;
    uint8_t *rec[MAX_EXTENSIONS];
    unsigned nrec;
    uint8_t *list;                  /* a non-resident attribute list */
    attr_t a[MAX_ATTRS];
    unsigned na;
} file_t;

static void file_free(file_t *f)
{
    unsigned i;
    for (i = 0; i < f->nrec; i++)
        free(f->rec[i]);
    free(f->list);
    f->nrec = 0;
    f->list = NULL;
}

static int parse_attrs(ntfs_t *v, file_t *f, const uint8_t *rec, uint64_t n)
{
    uint32_t at = le16(rec + 0x14);

    while (at + 4 <= v->rec_size) {
        attr_t *a;
        uint32_t len, type = le32(rec + at);

        if (type == AT_END)
            return 1;
        if (at + 16 > v->rec_size || (len = le32(rec + at + 4)) < 16 ||
            at + len > v->rec_size) {
            say(v->err, v->errsz, "MFT record %llu: attribute 0x%X overruns "
                "its record", (unsigned long long)n, type);
            return 0;
        }
        if (f->na == MAX_ATTRS) {
            say(v->err, v->errsz, "MFT record %llu has more than %u "
                "attributes", (unsigned long long)n, MAX_ATTRS);
            return 0;
        }
        a = &f->a[f->na++];
        memset(a, 0, sizeof *a);
        a->type = type;
        a->nonres = rec[at + 8];
        a->name_len = rec[at + 9];
        a->flags = le16(rec + at + 12);
        if (le16(rec + at + 10) + 2u * a->name_len > len) {
            say(v->err, v->errsz, "MFT record %llu: an attribute's name "
                "overruns it", (unsigned long long)n);
            return 0;
        }
        a->name = rec + at + le16(rec + at + 10);
        if (!a->nonres) {
            uint32_t vlen = le32(rec + at + 16), voff = le16(rec + at + 20);
            if (voff + (uint64_t)vlen > len) {
                say(v->err, v->errsz, "MFT record %llu: resident 0x%X "
                    "overruns itself", (unsigned long long)n, type);
                return 0;
            }
            a->value = rec + at + voff;
            a->vlen = vlen;
            a->size = vlen;
        } else {
            uint32_t roff;
            if (len < 0x40 || (roff = le16(rec + at + 0x20)) > len) {
                say(v->err, v->errsz, "MFT record %llu: a non-resident "
                    "attribute is too short", (unsigned long long)n);
                return 0;
            }
            a->vcn0 = le64(rec + at + 0x10);
            a->size = le64(rec + at + 0x30);
            a->init = le64(rec + at + 0x38);
            a->map = rec + at + roff;
            a->map_len = len - roff;
        }
        at += len;
    }
    say(v->err, v->errsz, "MFT record %llu has no end marker",
        (unsigned long long)n);
    return 0;
}

static int load_record(ntfs_t *v, file_t *f, uint64_t n)
{
    uint8_t *buf;

    if (f->nrec == MAX_EXTENSIONS) {
        say(v->err, v->errsz, "a file spans more than %u MFT records",
            MAX_EXTENSIONS);
        return 0;
    }
    if (!(buf = malloc(v->rec_size))) {
        say(v->err, v->errsz, "out of memory");
        return 0;
    }
    f->rec[f->nrec++] = buf;
    return record(v, n, buf) && parse_attrs(v, f, buf, n);
}

static int stream_runs(ntfs_t *v, const file_t *f, const attr_t *first,
                       runs_t *out);

static int file_open(ntfs_t *v, uint64_t n, file_t *f)
{
    unsigned i, base_attrs;
    uint64_t others[MAX_EXTENSIONS];
    unsigned nothers = 0;
    const uint8_t *list = NULL;
    size_t list_len = 0;

    memset(f, 0, sizeof *f);
    if (!load_record(v, f, n))
        return 0;
    f->flags = le16(f->rec[0] + 0x16);
    if (!(f->flags & REC_IN_USE)) {
        say(v->err, v->errsz, "MFT record %llu is not in use",
            (unsigned long long)n);
        return 0;
    }
    base_attrs = f->na;
    for (i = 0; i < base_attrs; i++) {
        const attr_t *a = &f->a[i];
        if (a->type != AT_ATTRIBUTE_LIST)
            continue;
        if (!a->nonres) {
            list = a->value;
            list_len = a->vlen;
        } else {
            runs_t rs = { 0 };
            if (a->size > (1u << 20) ||
                !(f->list = malloc((size_t)a->size + 1)) ||
                !decode_runs(v, a->map, a->map_len, a->vcn0, &rs) ||
                !read_runs(v, &rs, 0, f->list, (size_t)a->size)) {
                free(rs.r);
                if (!v->err[0])
                    say(v->err, v->errsz, "MFT record %llu: its attribute "
                        "list cannot be read", (unsigned long long)n);
                return 0;
            }
            free(rs.r);
            list = f->list;
            list_len = (size_t)a->size;
        }
        break;
    }
    if (list) {
        size_t at = 0;
        while (at + 26 <= list_len) {
            unsigned ln = le16(list + at + 4), j;
            uint64_t r = ref_of(le64(list + at + 16));
            if (ln < 26) {
                say(v->err, v->errsz, "MFT record %llu: an attribute list "
                    "entry of %u bytes", (unsigned long long)n, ln);
                return 0;
            }
            for (j = 0; j < nothers && others[j] != r; j++)
                ;
            if (r != n && j == nothers) {
                if (nothers == MAX_EXTENSIONS) {
                    say(v->err, v->errsz, "a file spans more than %u MFT "
                        "records", MAX_EXTENSIONS);
                    return 0;
                }
                others[nothers++] = r;
            }
            at += ln;
        }
        /* In record order, as the oracle reads them. */
        for (i = 0; i < nothers; i++) {
            unsigned j, k = i;
            for (j = i + 1; j < nothers; j++)
                if (others[j] < others[k])
                    k = j;
            if (k != i) {
                uint64_t t = others[i];
                others[i] = others[k];
                others[k] = t;
            }
            if (!load_record(v, f, others[i]))
                return 0;
        }
    }
    return 1;
}

static int name_is(const attr_t *a, const char *ascii)
{
    unsigned i, n = (unsigned)strlen(ascii);
    if (a->name_len != n)
        return 0;
    for (i = 0; i < n; i++)
        if (le16(a->name + 2 * i) != (unsigned char)ascii[i])
            return 0;
    return 1;
}

/* The extents of one stream, merged in VCN order; they must join up. */
static int stream_runs(ntfs_t *v, const file_t *f, const attr_t *first,
                       runs_t *out)
{
    const attr_t *ext[MAX_ATTRS];
    unsigned i, n = 0, j;
    uint64_t next = 0;

    for (i = 0; i < f->na; i++) {
        const attr_t *a = &f->a[i];
        if (a->type == first->type && a->nonres &&
            a->name_len == first->name_len &&
            !memcmp(a->name, first->name, 2u * a->name_len))
            ext[n++] = a;
    }
    for (i = 0; i < n; i++)             /* by first VCN */
        for (j = i + 1; j < n; j++)
            if (ext[j]->vcn0 < ext[i]->vcn0) {
                const attr_t *t = ext[i];
                ext[i] = ext[j];
                ext[j] = t;
            }
    for (i = 0; i < n; i++)
        if (!decode_runs(v, ext[i]->map, ext[i]->map_len, ext[i]->vcn0, out))
            return 0;
    for (i = 0; i < out->n; i++) {
        if (out->r[i].vcn != next) {
            say(v->err, v->errsz, "a stream's extents do not join up");
            return 0;
        }
        next += out->r[i].len;
    }
    return 1;
}

/* ---- opening ------------------------------------------------------------------ */

static uint32_t unit(const ntfs_t *v, int8_t c)
{
    return c > 0 ? v->cluster * (uint32_t)c : 1u << -c;
}

int ntfs_open(const fat_image_t *img, uint64_t offset, ntfs_t **out,
              char *err, size_t errsz)
{
    uint8_t boot[SECTOR];
    ntfs_t *v = calloc(1, sizeof *v);
    file_t *f = NULL;
    unsigned spc, i;
    const attr_t *data = NULL;
    runs_t rs = { 0 };

    *out = NULL;
    err[0] = 0;
    if (!v) {
        say(err, errsz, "out of memory");
        return -1;
    }
    v->img = *img;
    v->base = offset;
    v->err = err;
    v->errsz = errsz;
    if (!raw(v, 0, boot, SECTOR))
        goto fail;
    if (!is_ntfs(boot)) {
        say(err, errsz, "not an NTFS boot sector");
        goto fail;
    }
    v->sector = le16(boot + 0x0B);
    spc = boot[0x0D];
    if ((v->sector != 512 && v->sector != 1024 && v->sector != 2048 &&
         v->sector != 4096) || !spc || (spc & (spc - 1))) {
        say(err, errsz, "bytes per sector %u, sectors per cluster %u",
            v->sector, spc);
        goto fail;
    }
    v->cluster = v->sector * spc;
    v->clusters = le64(boot + 0x28) * v->sector / v->cluster;
    v->rec_size = unit(v, (int8_t)boot[0x40]);
    v->idx_size = unit(v, (int8_t)boot[0x44]);
    if (!v->rec_size || v->rec_size % STRIDE || v->rec_size > 65536 ||
        !v->idx_size || v->idx_size % STRIDE || v->idx_size > 65536) {
        say(err, errsz, "record sizes %u and %u are not whole 512-byte blocks",
            v->rec_size, v->idx_size);
        goto fail;
    }
    /* $MFT's own record is at the boot sector's LCN; its $DATA says where
     * every other record is. */
    if (!runs_add(&v->mft, 0, le64(boot + 0x30),
                  v->rec_size >= v->cluster ? v->rec_size / v->cluster : 1, 0))
        goto fail;
    if (!(f = malloc(sizeof *f)) || !file_open(v, 0, f))
        goto fail;
    for (i = 0; i < f->na; i++)
        if (f->a[i].type == AT_DATA && !f->a[i].name_len &&
            (!data || f->a[i].vcn0 < data->vcn0))
            data = &f->a[i];
    if (!data || !data->nonres) {
        say(err, errsz, "$MFT has no non-resident $DATA");
        goto fail;
    }
    if (!stream_runs(v, f, data, &rs))
        goto fail;
    v->mft_size = data->size;
    free(v->mft.r);
    v->mft = rs;
    rs.r = NULL;
    file_free(f);
    free(f);
    v->err = v->idle;
    v->errsz = sizeof v->idle;
    *out = v;
    return 0;
fail:
    if (f) {
        file_free(f);
        free(f);
    }
    free(rs.r);
    free(v->mft.r);
    free(v);
    if (!err[0])
        say(err, errsz, "out of memory");
    return -1;
}

void ntfs_close(ntfs_t *v)
{
    if (!v)
        return;
    free(v->mft.r);
    free(v);
}

/* ---- files --------------------------------------------------------------------- */

/* Each call reports into its own caller's buffer, and puts back the one it
 * found: a walk's visitor reads files, and the walk's own messages must still
 * reach the walk's caller. */
typedef struct {
    char *err;
    size_t errsz;
} errb_t;

static errb_t bind_err(ntfs_t *v, char *err, size_t errsz)
{
    errb_t was = { v->err, v->errsz };
    v->err = err;
    v->errsz = errsz;
    err[0] = 0;
    return was;
}

static void unbind_err(ntfs_t *v, errb_t was)
{
    v->err = was.err;
    v->errsz = was.errsz;
}

/* The unnamed $DATA's first extent, refused when compressed or encrypted. */
static const attr_t *data_attr(ntfs_t *v, const file_t *f, uint64_t n)
{
    const attr_t *first = NULL;
    unsigned i;

    for (i = 0; i < f->na; i++) {
        const attr_t *a = &f->a[i];
        if (a->type != AT_DATA || a->name_len)
            continue;
        if (a->flags & (ATTR_COMPRESSED | ATTR_ENCRYPTED)) {
            say(v->err, v->errsz, "MFT record %llu is compressed or "
                "encrypted, which this reader does not read",
                (unsigned long long)n);
            return NULL;
        }
        if (!first || (a->nonres && a->vcn0 < first->vcn0))
            first = a;
    }
    if (!first)
        say(v->err, v->errsz, "MFT record %llu has no $DATA",
            (unsigned long long)n);
    return first;
}

int ntfs_file_size(ntfs_t *v, uint64_t rec, uint64_t *size, char *err,
                   size_t errsz)
{
    file_t *f = malloc(sizeof *f);
    const attr_t *a;
    errb_t was = bind_err(v, err, errsz);
    int rc = -1;

    if (!f) {
        say(err, errsz, "out of memory");
    } else if (file_open(v, rec, f) && (a = data_attr(v, f, rec))) {
        *size = a->size;
        rc = 0;
    }
    if (f)
        file_free(f);
    free(f);
    unbind_err(v, was);
    return rc;
}

int ntfs_read_file(ntfs_t *v, uint64_t rec, fat_sink_t sink, void *ctx,
                   char *err, size_t errsz)
{
    file_t *f = malloc(sizeof *f);
    const attr_t *a;
    runs_t rs = { 0 };
    uint8_t *buf = NULL;
    errb_t was = bind_err(v, err, errsz);
    int rc = -1;

    if (!f) {
        say(err, errsz, "out of memory");
        unbind_err(v, was);
        return -1;
    }
    if (!file_open(v, rec, f) || !(a = data_attr(v, f, rec)))
        goto out;
    if (!a->nonres) {
        rc = a->vlen ? sink(ctx, a->value, a->vlen) : 0;
        goto out;
    }
    if (!stream_runs(v, f, a, &rs) || !(buf = malloc(CHUNK))) {
        if (!err[0])
            say(err, errsz, "out of memory");
        goto out;
    }
    if (v->img.plan && rs.n) {
        uint64_t *start = malloc(rs.n * 2 * sizeof *start);
        uint32_t *len = malloc(rs.n * 2 * sizeof *len);
        size_t i, k = 0;
        if (start && len) {
            for (i = 0; i < rs.n; i++) {
                uint64_t s = rs.r[i].lcn * v->cluster, l = rs.r[i].len * v->cluster;
                if (rs.r[i].sparse)
                    continue;
                while (l && k < rs.n * 2) {     /* a plan entry is 32-bit */
                    uint32_t piece = l > 0x40000000u ? 0x40000000u : (uint32_t)l;
                    start[k] = v->base + s;
                    len[k++] = piece;
                    s += piece;
                    l -= piece;
                }
            }
            v->img.plan(v->img.ctx, start, len, k);
        }
        free(start);
        free(len);
    }
    {
        uint64_t pos = 0, size = a->size, init = a->init;
        while (pos < size) {
            size_t n = size - pos < CHUNK ? (size_t)(size - pos) : CHUNK;
            size_t real = pos >= init ? 0 : init - pos < n ? (size_t)(init - pos) : n;
            if (real && !read_runs(v, &rs, pos, buf, real))
                goto out;
            memset(buf + real, 0, n - real);
            if ((rc = sink(ctx, buf, n)) != 0)
                goto out;
            pos += n;
        }
    }
    rc = 0;
out:
    free(buf);
    free(rs.r);
    file_free(f);
    free(f);
    unbind_err(v, was);
    return rc;
}

/* ---- directories ------------------------------------------------------------------ */

typedef struct {
    wchar_t *name;
    uint64_t rec;
    int is_dir;
} entry_t;

typedef struct {
    ntfs_t *v;
    uint64_t dir;
    entry_t *e;
    size_t n, cap;
    runs_t alloc;
    int has_alloc;
    uint64_t *seen;
    size_t nseen, seen_cap;
    uint32_t vcn_unit;
    unsigned depth;
} list_t;

static void list_free(list_t *l)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        free(l->e[i].name);
    free(l->e);
    free(l->alloc.r);
    free(l->seen);
}

static int descend(list_t *l, uint64_t vcn);

/* An entry flagged ENTRY_NODE carries the subnode that sorts before it, and
 * the END entry's sorts after them all: this is the index's own order, and
 * only live nodes are reached, never a block the bitmap has freed. */
static int visit_node(list_t *l, const uint8_t *buf, size_t buflen, size_t hdr)
{
    ntfs_t *v = l->v;
    size_t at, end;

    if (hdr + 8 > buflen)
        goto bad;
    at = hdr + le32(buf + hdr);
    end = hdr + le32(buf + hdr + 4);
    if (end > buflen)
        goto bad;
    while (at + 16 <= end) {
        uint64_t ref = le64(buf + at);
        unsigned ln = le16(buf + at + 8), klen = le16(buf + at + 10);
        unsigned fl = le16(buf + at + 12);

        if (ln < 16 || at + ln > end)
            goto bad;
        if ((fl & ENTRY_NODE) && !descend(l, le64(buf + at + ln - 8)))
            return 0;
        if (fl & ENTRY_END)
            return 1;
        if (klen < 0x42 || 16 + klen > ln)
            goto bad;
        {
            const uint8_t *key = buf + at + 16;
            unsigned nlen = key[0x40], ns = key[0x41], i;
            uint64_t r = ref_of(ref);

            if (0x42 + 2u * nlen > klen)
                goto bad;
            if (ns != NS_DOS && r != l->dir) {
                entry_t *e;
                if (l->n == l->cap) {
                    size_t cap = l->cap ? l->cap * 2 : 64;
                    entry_t *ne = realloc(l->e, cap * sizeof *ne);
                    if (!ne)
                        goto oom;
                    l->e = ne;
                    l->cap = cap;
                }
                e = &l->e[l->n];
                if (!(e->name = malloc((nlen + 1) * sizeof(wchar_t))))
                    goto oom;
                for (i = 0; i < nlen; i++)
                    e->name[i] = (wchar_t)le16(key + 0x42 + 2 * i);
                e->name[nlen] = 0;
                e->rec = r;
                e->is_dir = (le32(key + 0x38) & DUP_DIRECTORY) != 0;
                l->n++;
            }
        }
        at += ln;
    }
bad:
    say(v->err, v->errsz, "directory %llu: an index node is damaged",
        (unsigned long long)l->dir);
    return 0;
oom:
    say(v->err, v->errsz, "out of memory");
    return 0;
}

static int descend(list_t *l, uint64_t vcn)
{
    ntfs_t *v = l->v;
    uint8_t *buf;
    size_t i;
    int ok;

    if (!l->has_alloc) {
        say(v->err, v->errsz, "directory %llu names a subnode and has no "
            "$INDEX_ALLOCATION", (unsigned long long)l->dir);
        return 0;
    }
    for (i = 0; i < l->nseen; i++)
        if (l->seen[i] == vcn) {
            say(v->err, v->errsz, "directory %llu's index revisits VCN %llu",
                (unsigned long long)l->dir, (unsigned long long)vcn);
            return 0;
        }
    if (l->nseen == l->seen_cap) {
        size_t cap = l->seen_cap ? l->seen_cap * 2 : 16;
        uint64_t *s = realloc(l->seen, cap * sizeof *s);
        if (!s) {
            say(v->err, v->errsz, "out of memory");
            return 0;
        }
        l->seen = s;
        l->seen_cap = cap;
    }
    l->seen[l->nseen++] = vcn;
    if (++l->depth > 64) {
        say(v->err, v->errsz, "directory %llu's index is too deep",
            (unsigned long long)l->dir);
        return 0;
    }
    if (!(buf = malloc(v->idx_size))) {
        say(v->err, v->errsz, "out of memory");
        return 0;
    }
    ok = read_runs(v, &l->alloc, vcn * l->vcn_unit, buf, v->idx_size) &&
         fixup(v, buf, v->idx_size, "INDX", "directory index of record",
               l->dir) &&
         visit_node(l, buf, v->idx_size, 0x18);
    free(buf);
    l->depth--;
    return ok;
}

/* Directory `n`'s entries, DOS aliases left out, in index order. */
static int listdir(ntfs_t *v, uint64_t n, list_t *l)
{
    file_t *f = malloc(sizeof *f);
    const attr_t *root = NULL, *alloc = NULL;
    unsigned i;
    int ok = 0;

    memset(l, 0, sizeof *l);
    l->v = v;
    l->dir = n;
    /* A subnode's VCN counts clusters, or 512-byte blocks when an index
     * block is smaller than a cluster. */
    l->vcn_unit = v->idx_size >= v->cluster ? v->cluster : SECTOR;
    if (!f) {
        say(v->err, v->errsz, "out of memory");
        return 0;
    }
    if (!file_open(v, n, f))
        goto out;
    if (!(f->flags & REC_DIRECTORY)) {
        say(v->err, v->errsz, "MFT record %llu is not a directory",
            (unsigned long long)n);
        goto out;
    }
    for (i = 0; i < f->na; i++) {
        if (f->a[i].type == AT_INDEX_ROOT && !f->a[i].nonres &&
            name_is(&f->a[i], "$I30"))
            root = &f->a[i];
        if (f->a[i].type == AT_INDEX_ALLOCATION && f->a[i].nonres &&
            name_is(&f->a[i], "$I30") && (!alloc || f->a[i].vcn0 < alloc->vcn0))
            alloc = &f->a[i];
    }
    if (!root) {
        say(v->err, v->errsz, "directory %llu has no $I30 index",
            (unsigned long long)n);
        goto out;
    }
    if (alloc) {
        if (!stream_runs(v, f, alloc, &l->alloc))
            goto out;
        l->has_alloc = 1;
    }
    ok = visit_node(l, root->value, root->vlen, 0x10);
out:
    file_free(f);
    free(f);
    return ok;
}

static int lookup(ntfs_t *v, const char *p, uint64_t *at);

int ntfs_lookup(ntfs_t *v, const char *path, uint64_t *rec, char *err,
                size_t errsz)
{
    uint64_t n = NTFS_ROOT;
    const char *p = path;
    errb_t was = bind_err(v, err, errsz);
    int rc = lookup(v, p, &n);

    unbind_err(v, was);
    if (rc == 1)
        *rec = n;
    return rc;
}

static int lookup(ntfs_t *v, const char *p, uint64_t *at)
{
    uint64_t n = NTFS_ROOT;

    while (*p) {
        char comp[MAX_PATH];
        wchar_t want[MAX_PATH];
        size_t len = strcspn(p, "/\\"), i;
        list_t l;
        int hit = 0;

        if (!len) {
            p++;
            continue;
        }
        if (len >= sizeof comp)
            return 0;
        memcpy(comp, p, len);
        comp[len] = 0;
        p += len;
        if (!MultiByteToWideChar(CP_UTF8, 0, comp, -1, want, MAX_PATH))
            return 0;
        if (!listdir(v, n, &l)) {
            list_free(&l);
            return -1;
        }
        for (i = 0; i < l.n && !hit; i++)
            if (!_wcsicmp(l.e[i].name, want)) {
                n = l.e[i].rec;
                hit = 1;
            }
        list_free(&l);
        if (!hit)
            return 0;
    }
    *at = n;
    return 1;
}

static int walk(ntfs_t *v, uint64_t dir, wchar_t *path, size_t at,
                unsigned depth, ntfs_visit_t visit, void *ctx)
{
    list_t l;
    size_t i;
    int rc = 0;

    if (depth > 64) {
        say(v->err, v->errsz, "directories nest deeper than 64");
        return -1;
    }
    if (!listdir(v, dir, &l)) {
        list_free(&l);
        return -1;
    }
    for (i = 0; i < l.n && !rc; i++) {
        size_t len = wcslen(l.e[i].name);
        if (at + len + 2 >= PATH_W) {
            say(v->err, v->errsz, "a path is longer than %u characters",
                PATH_W);
            rc = -1;
            break;
        }
        memcpy(path + at, l.e[i].name, len * sizeof(wchar_t));
        path[at + len] = 0;
        if (l.e[i].is_dir) {
            path[at + len] = L'\\';
            path[at + len + 1] = 0;
            rc = walk(v, l.e[i].rec, path, at + len + 1, depth + 1, visit, ctx);
        } else {
            rc = visit(ctx, path, l.e[i].rec);
        }
        path[at] = 0;
    }
    list_free(&l);
    return rc;
}

int ntfs_walk(ntfs_t *v, uint64_t dir, ntfs_visit_t visit, void *ctx,
              char *err, size_t errsz)
{
    wchar_t path[PATH_W];
    errb_t was = bind_err(v, err, errsz);
    int rc;

    path[0] = 0;
    rc = walk(v, dir, path, 0, 0, visit, ctx);
    unbind_err(v, was);
    return rc;
}
