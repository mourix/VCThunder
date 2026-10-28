/* import_test.c -- the CHD importer's pieces, with no game and no CHD.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A disk built in memory, shaped like the cabinets' (a primary FAT16, then
 * logical volumes in an extended partition whose EBR entries are relative to
 * two different bases), read through src/fat.c; hand-assembled FLAC frames
 * through src/chd.c's decoder; CRC-16's published check value; and an NTFS
 * volume shaped like Arctic Thunder's disk (bare, no partition table) read
 * through src/ntfs.c. The real CHDs are the maintainer's, compared with the
 * Python oracle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../src/chd.h"
#include "../src/fat.h"
#include "../src/iso.h"
#include "../src/ntfs.h"

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* ---- a disk in memory ----------------------------------------------------- */

#define SEC 512u
#define DISK_SECTORS 12000u
static uint8_t s_disk[DISK_SECTORS * SEC];
static unsigned s_plans, s_plan_ranges;

static int mem_read(void *ctx, uint64_t pos, void *buf, size_t n)
{
    (void)ctx;
    if (pos + n > sizeof s_disk)
        return 0;
    memcpy(buf, s_disk + pos, n);
    return 1;
}

static void mem_plan(void *ctx, const uint64_t *start, const uint32_t *len,
                     size_t n)
{
    (void)ctx; (void)start; (void)len;
    s_plans++;
    s_plan_ranges = (unsigned)n;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

static void part_entry(uint8_t *sector, int slot, uint8_t type, uint32_t lba,
                       uint32_t count)
{
    uint8_t *e = sector + 446 + slot * 16;
    e[4] = type;
    put32(e + 8, lba);
    put32(e + 12, count);
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

/* A FAT volume of `total` sectors at `lba`, one sector per cluster, two FATs
 * of `spf` sectors and a 32-sector root. Returns the volume base pointer. */
typedef struct {
    uint8_t *base;
    unsigned bits, spf, root_sectors, first_data;
} vol_t;

static vol_t make_vol(uint32_t lba, uint32_t total, unsigned bits, unsigned spf)
{
    vol_t v;
    uint8_t *b = s_disk + (size_t)lba * SEC;
    v.base = b;
    v.bits = bits;
    v.spf = spf;
    v.root_sectors = 32;
    v.first_data = 1 + 2 * spf + v.root_sectors;
    put16(b + 0x0B, SEC);
    b[0x0D] = 1;
    put16(b + 0x0E, 1);
    b[0x10] = 2;
    put16(b + 0x11, 512);
    put16(b + 0x13, total);
    put16(b + 0x16, spf);
    memcpy(b + 0x2B, "TESTVOL    ", 11);
    return v;
}

static void fat_set(vol_t *v, unsigned cl, unsigned val)
{
    uint8_t *fat = v->base + SEC;
    if (v->bits == 16) {
        put16(fat + cl * 2, val);
    } else {
        unsigned off = cl + cl / 2, cur = fat[off] | fat[off + 1] << 8;
        cur = cl & 1 ? (cur & 0x000F) | (val << 4) : (cur & 0xF000) | (val & 0xFFF);
        put16(fat + off, cur);
    }
}

static uint8_t *cluster(vol_t *v, unsigned cl)
{
    return v->base + ((size_t)v->first_data + cl - 2) * SEC;
}

static uint8_t *dirent(uint8_t *dir, int i, const char *name11, uint8_t attr,
                       unsigned cl, uint32_t size)
{
    uint8_t *e = dir + i * 32;
    memcpy(e, name11, 11);
    e[0x0B] = attr;
    put16(e + 0x1A, cl);
    put32(e + 0x1C, size);
    return e;
}

/* One LFN part holding up to 13 characters of `name` from `from`. */
static void lfn_entry(uint8_t *dir, int i, int seq, const char *name, int from)
{
    static const int at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    uint8_t *e = dir + i * 32;
    int j, len = (int)strlen(name), ended = 0;
    memset(e, 0, 32);
    e[0] = (uint8_t)seq;
    e[0x0B] = 0x0F;
    for (j = 0; j < 13; j++) {
        int k = from + j;
        unsigned ch = k < len ? (unsigned char)name[k] : (ended++ ? 0xFFFF : 0);
        put16(e + at[j], ch);
    }
}

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(seed * 131 + i * 7 + (i >> 9));
}

/* Collected by the walk. */
static struct { wchar_t path[256]; uint32_t cl, size; } s_files[16];
static int s_nfiles;

static int visit(void *ctx, const wchar_t *path, uint32_t cl, uint32_t size)
{
    (void)ctx;
    if (s_nfiles < 16) {
        wcsncpy(s_files[s_nfiles].path, path, 255);
        s_files[s_nfiles].cl = cl;
        s_files[s_nfiles].size = size;
        s_nfiles++;
    }
    return 0;
}

static uint8_t s_out[64 * 1024];
static size_t s_outn;

static int sink(void *ctx, const void *buf, size_t n)
{
    (void)ctx;
    if (s_outn + n > sizeof s_out)
        return 9;
    memcpy(s_out + s_outn, buf, n);
    s_outn += n;
    return 0;
}

static const uint8_t *find_expected(const wchar_t *path, size_t *len);
static uint8_t s_exe[3000], s_long[700], s_nvr[1100];

static const uint8_t *find_expected(const wchar_t *path, size_t *len)
{
    if (!wcscmp(path, L"HYDRO.EXE")) { *len = sizeof s_exe; return s_exe; }
    if (!wcscmp(path, L"SUB\\A Long File Name.dat")) {
        *len = sizeof s_long;
        return s_long;
    }
    if (!wcscmp(path, L"AUDT1.NVR")) { *len = sizeof s_nvr; return s_nvr; }
    return NULL;
}

static void build_disk(void)
{
    /* MBR: a primary FAT16 at 63, and an extended partition at 6000. */
    const uint32_t ext = 6000, ebr2 = 2000, ebr3 = 3500;
    vol_t v16, v12a, v12b, v12c;
    uint8_t *root, *sub;
    unsigned i;

    memset(s_disk, 0, sizeof s_disk);
    part_entry(s_disk, 0, 0x06, 63, 4400);
    part_entry(s_disk, 1, 0x05, ext, 6000);
    /* EBR 1, at the extended partition's start: a logical volume 63 sectors
     * past THIS EBR, and the next EBR 2000 sectors past the EXTENDED start. */
    part_entry(s_disk + (size_t)ext * SEC, 0, 0x01, 63, 600);
    part_entry(s_disk + (size_t)ext * SEC, 1, 0x05, ebr2, 1000);
    /* EBR 2: its volume 63 past itself, and EBR 3 at 3500 past the EXTENDED
     * start. EBR 1 sits at the extended start, so only from here on does
     * reading a link against the wrong base give a different answer. */
    part_entry(s_disk + (size_t)(ext + ebr2) * SEC, 0, 0x01, 63, 600);
    part_entry(s_disk + (size_t)(ext + ebr2) * SEC, 1, 0x05, ebr3, 1000);
    part_entry(s_disk + (size_t)(ext + ebr3) * SEC, 0, 0x01, 63, 600);

    /* The FAT16: 4400 sectors is 4300-odd clusters, which is FAT16 by count. */
    v16 = make_vol(63, 4400, 16, 18);
    fat_set(&v16, 0, 0xFFF8);
    fat_set(&v16, 1, 0xFFFF);
    fill(s_exe, sizeof s_exe, 1);
    fill(s_long, sizeof s_long, 2);
    /* HYDRO.EXE fragmented: clusters 2,3,4 then 10,11,12 (3000 bytes). */
    {
        static const unsigned chain[6] = { 2, 3, 4, 10, 11, 12 };
        for (i = 0; i < 6; i++) {
            fat_set(&v16, chain[i], i < 5 ? chain[i + 1] : 0xFFFF);
            memcpy(cluster(&v16, chain[i]), s_exe + i * SEC,
                   i < 5 ? SEC : sizeof s_exe - 5 * SEC);
        }
    }
    root = v16.base + (1 + 2 * v16.spf) * SEC;
    dirent(root, 0, "TESTVOL    ", 0x08, 0, 0);             /* a label */
    dirent(root, 1, "HYDRO   EXE", 0x20, 2, sizeof s_exe);
    dirent(root, 2, "GONE    TXT", 0x20, 20, 10)[0] = 0xE5;  /* deleted */
    dirent(root, 3, "SUB        ", 0x10, 5, 0);
    fat_set(&v16, 5, 0xFFFF);
    sub = cluster(&v16, 5);
    dirent(sub, 0, ".          ", 0x10, 5, 0);
    dirent(sub, 1, "..         ", 0x10, 0, 0);
    lfn_entry(sub, 2, 0x42, "A Long File Name.dat", 13);
    lfn_entry(sub, 3, 0x01, "A Long File Name.dat", 0);
    dirent(sub, 4, "ALONGF~1DAT", 0x20, 6, sizeof s_long);
    fat_set(&v16, 6, 7);
    fat_set(&v16, 7, 0xFFFF);
    memcpy(cluster(&v16, 6), s_long, SEC);
    memcpy(cluster(&v16, 7), s_long + SEC, sizeof s_long - SEC);

    /* Logical 1: FAT12, a file whose chain loops back on itself. */
    v12a = make_vol(ext + 63, 600, 12, 2);
    fat_set(&v12a, 0, 0xFF8);
    fat_set(&v12a, 1, 0xFFF);
    fat_set(&v12a, 2, 3);
    fat_set(&v12a, 3, 2);
    root = v12a.base + (1 + 2 * v12a.spf) * SEC;
    dirent(root, 0, "LOOP    BIN", 0x20, 2, 100000);

    /* Logical 2: FAT12, odd-numbered clusters to exercise the nibble packing. */
    v12b = make_vol(ext + ebr2 + 63, 600, 12, 2);
    fat_set(&v12b, 0, 0xFF8);
    fat_set(&v12b, 1, 0xFFF);
    fill(s_nvr, sizeof s_nvr, 3);
    fat_set(&v12b, 3, 5);
    fat_set(&v12b, 5, 6);
    fat_set(&v12b, 6, 0xFFF);
    memcpy(cluster(&v12b, 3), s_nvr, SEC);
    memcpy(cluster(&v12b, 5), s_nvr + SEC, SEC);
    memcpy(cluster(&v12b, 6), s_nvr + 2 * SEC, sizeof s_nvr - 2 * SEC);
    root = v12b.base + (1 + 2 * v12b.spf) * SEC;
    dirent(root, 0, "AUDT1   NVR", 0x20, 3, sizeof s_nvr);

    /* Logical 3: an empty FAT12, there to be found at the right place. */
    v12c = make_vol(ext + ebr3 + 63, 600, 12, 2);
    fat_set(&v12c, 0, 0xFF8);
    fat_set(&v12c, 1, 0xFFF);
}

static void test_fat(void)
{
    fat_image_t img = { mem_read, mem_plan, NULL };
    fat_part_t parts[FAT_MAX_PARTS];
    char err[256];
    int n, p, i;

    build_disk();
    n = fat_partitions(&img, parts, FAT_MAX_PARTS);
    check(n == 4, "four partitions: the primary and three logical volumes");
    check(n == 4 && parts[0].type == 0x06 && parts[0].lba == 63,
          "the primary FAT16 at 63");
    check(n == 4 && parts[1].type == 0x01 && parts[1].lba == 6000 + 63,
          "logical 1 relative to its own EBR");
    check(n == 4 && parts[2].type == 0x01 && parts[2].lba == 6000 + 2000 + 63,
          "logical 2: next EBR relative to the extended start, its volume "
          "relative to that EBR");
    check(n == 4 && parts[3].type == 0x01 && parts[3].lba == 6000 + 3500 + 63,
          "logical 3: a link from a later EBR is still relative to the "
          "extended start");

    for (p = 0; p < n; p++) {
        fat_t *v;
        s_nfiles = 0;
        if (fat_open(&img, parts[p].lba, &v, err, sizeof err)) {
            check(0, "a synthetic volume opens");
            continue;
        }
        check(fat_bits(v) == (p == 0 ? 16u : 12u), "FAT width by cluster count");
        check(fat_walk(v, visit, NULL, err, sizeof err) == 0, "the walk completes");
        for (i = 0; i < s_nfiles; i++) {
            size_t len;
            const uint8_t *want = find_expected(s_files[i].path, &len);
            int rc;
            s_outn = 0;
            s_plans = 0;
            rc = fat_read_file(v, s_files[i].cl, s_files[i].size, sink, NULL,
                               err, sizeof err);
            if (!wcscmp(s_files[i].path, L"LOOP.BIN")) {
                check(rc == -1 && strstr(err, "loop"),
                      "a looping chain is refused, not followed");
                continue;
            }
            check(want != NULL, "every file walked is one that was written");
            check(rc == 0 && want && s_outn == len && !memcmp(s_out, want, len),
                  "a file reads back byte for byte");
            if (!wcscmp(s_files[i].path, L"HYDRO.EXE"))
                check(s_plans == 1 && s_plan_ranges == 2,
                      "a fragmented chain is planned as its two runs");
        }
        if (p == 0)
            check(s_nfiles == 2, "the label and the deleted entry are not files");
        fat_close(v);
    }
}

/* ---- FLAC, assembled bit by bit -------------------------------------------- */

typedef struct {
    uint8_t buf[256];
    unsigned bits;
} bw_t;

static void bw_put(bw_t *w, uint32_t v, unsigned k)
{
    while (k--) {
        unsigned bit = (v >> k) & 1;
        if (bit)
            w->buf[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}

static void bw_align(bw_t *w)
{
    w->bits = (w->bits + 7) & ~7u;
}

/* A frame of 4 samples per channel, channel assignment `ch`, with two
 * subframes written by the caller. */
static void frame_head(bw_t *w, unsigned ch)
{
    bw_put(w, 0x3FFE, 14);
    bw_put(w, 0, 2);
    bw_put(w, 6, 4);        /* blocksize: 8 bits of (n - 1) follow */
    bw_put(w, 0, 4);
    bw_put(w, ch, 4);
    bw_put(w, 4, 3);        /* 16-bit samples */
    bw_put(w, 0, 1);
    bw_put(w, 0, 8);        /* frame number 0 */
    bw_put(w, 3, 8);        /* blocksize - 1 */
    bw_put(w, 0, 8);        /* header CRC-8, not checked */
}

static void verbatim(bw_t *w, const int *v, unsigned bps)
{
    int i;
    bw_put(w, 0, 1);
    bw_put(w, 1, 6);
    bw_put(w, 0, 1);
    for (i = 0; i < 4; i++)
        bw_put(w, (uint32_t)v[i] & ((1u << bps) - 1), bps);
}

static void test_flac(void)
{
    static const int left[4] = { 1, -2, 300, -32768 };
    static const int right[4] = { 32767, 0, -5, 77 };
    uint8_t out[16];
    bw_t w;
    int i, ok;

    /* Independent channels, little-endian: plain interleaving. */
    memset(&w, 0, sizeof w);
    bw_put(&w, 'L', 8);
    frame_head(&w, 1);
    verbatim(&w, left, 16);
    verbatim(&w, right, 16);
    bw_align(&w);
    bw_put(&w, 0, 16);
    ok = chd_flac_decode(w.buf, w.bits / 8, out, sizeof out);
    for (i = 0; ok && i < 4; i++)
        ok = (int16_t)(out[i * 4] | out[i * 4 + 1] << 8) == left[i] &&
             (int16_t)(out[i * 4 + 2] | out[i * 4 + 3] << 8) == right[i];
    check(ok, "flac: two verbatim channels, little-endian");

    /* Mid/side, big-endian: the side channel is one bit wider. */
    {
        int mid[4], side[4];
        for (i = 0; i < 4; i++) {
            side[i] = left[i] - right[i];
            mid[i] = (left[i] + right[i]) >> 1;
        }
        memset(&w, 0, sizeof w);
        bw_put(&w, 'B', 8);
        frame_head(&w, 10);
        verbatim(&w, mid, 16);
        verbatim(&w, side, 17);
        bw_align(&w);
        bw_put(&w, 0, 16);
        ok = chd_flac_decode(w.buf, w.bits / 8, out, sizeof out);
        for (i = 0; ok && i < 4; i++)
            ok = (int16_t)(out[i * 4] << 8 | out[i * 4 + 1]) == left[i] &&
                 (int16_t)(out[i * 4 + 2] << 8 | out[i * 4 + 3]) == right[i];
        check(ok, "flac: mid/side, big-endian");
    }

    /* A constant subframe, and a frame with no sync is refused. */
    memset(&w, 0, sizeof w);
    bw_put(&w, 'L', 8);
    frame_head(&w, 1);
    bw_put(&w, 0, 1); bw_put(&w, 0, 6); bw_put(&w, 0, 1);
    bw_put(&w, (uint32_t)-9 & 0xFFFF, 16);
    verbatim(&w, right, 16);
    bw_align(&w);
    bw_put(&w, 0, 16);
    ok = chd_flac_decode(w.buf, w.bits / 8, out, sizeof out);
    check(ok && (int16_t)(out[8] | out[9] << 8) == -9, "flac: a constant subframe");
    w.buf[1] ^= 0x80;
    check(!chd_flac_decode(w.buf, w.bits / 8, out, sizeof out),
          "flac: a lost frame sync is refused");
}

/* ---- an NTFS volume in memory ---------------------------------------------- *
 *
 * 512-byte sectors, 1 KB clusters, 1 KB MFT records and index blocks, at byte
 * 0 of the image as on Arctic Thunder's disk. What it exercises: the update
 * sequence of every record and index block (and a torn one refused), $MFT's
 * own $DATA in two extents, a file whose $DATA is split over two records by
 * an $ATTRIBUTE_LIST, a sparse run, an initialized size short of the file's,
 * resident data, a directory index with subnodes before an entry and after
 * the last, DOS aliases and the "." entry left out, compression refused, and
 * an index that revisits a node refused. */

#define NS_DOS_TEST 2
#define NC 1024u                        /* cluster, record and index block */
#define N_CLUSTERS 1024u
static uint8_t s_vol[N_CLUSTERS * NC];

static uint8_t *clu(unsigned lcn) { return s_vol + lcn * NC; }

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* Record n lives in MFT extent 0 (LCN 16..31) or 1 (LCN 100..115). */
static uint8_t *mft_rec(unsigned n)
{
    return n < 16 ? clu(16 + n) : clu(100 + n - 16);
}

/* "FILE" or "INDX" with its update sequence array at `usa`; returns the
 * offset where content may start. */
static void head(uint8_t *b, const char *sig, unsigned usa)
{
    memcpy(b, sig, 4);
    put16(b + 4, usa);
    put16(b + 6, NC / SEC + 1);
}

/* Write the update sequence: each sector's last two bytes into the array,
 * the sequence number in their place. */
static void seal(uint8_t *b)
{
    unsigned usa = b[4] | b[5] << 8, i;
    put16(b + usa, 0x4D2A);
    for (i = 1; i <= NC / SEC; i++) {
        uint8_t *end = b + i * SEC;
        b[usa + 2 * i] = end[-2];
        b[usa + 2 * i + 1] = end[-1];
        end[-2] = 0x2A;
        end[-1] = 0x4D;
    }
}

typedef struct {
    uint8_t *r;
    unsigned at;
} rec_t;

static rec_t rec_begin(unsigned n, unsigned flags)
{
    rec_t w;
    w.r = mft_rec(n);
    memset(w.r, 0, NC);
    head(w.r, "FILE", 0x30);
    put16(w.r + 0x14, 0x38);
    put16(w.r + 0x16, flags);
    w.at = 0x38;
    return w;
}

static void rec_end(rec_t *w)
{
    put32(w->r + w->at, 0xFFFFFFFFu);
    seal(w->r);
}

static void attr_name(uint8_t *a, unsigned off, const char *name)
{
    unsigned i, n = name ? (unsigned)strlen(name) : 0;
    a[9] = (uint8_t)n;
    put16(a + 10, off);
    for (i = 0; i < n; i++)
        put16(a + off + 2 * i, (unsigned char)name[i]);
}

static void resident(rec_t *w, uint32_t type, const char *name,
                     const void *val, unsigned len)
{
    uint8_t *a = w->r + w->at;
    unsigned nlen = name ? (unsigned)strlen(name) : 0;
    unsigned voff = (0x18 + 2 * nlen + 7) & ~7u;
    unsigned total = (voff + len + 7) & ~7u;

    put32(a, type);
    put32(a + 4, total);
    attr_name(a, 0x18, name);
    put32(a + 0x10, len);
    put16(a + 0x14, voff);
    memcpy(a + voff, val, len);
    w->at += total;
}

static void nonresident(rec_t *w, uint32_t type, const char *name,
                        uint16_t flags, uint64_t vcn0, uint64_t vcn1,
                        const uint8_t *runs, unsigned runs_len, uint64_t alloc,
                        uint64_t size, uint64_t init)
{
    uint8_t *a = w->r + w->at;
    unsigned nlen = name ? (unsigned)strlen(name) : 0;
    unsigned roff = (0x40 + 2 * nlen + 7) & ~7u;
    unsigned total = (roff + runs_len + 1 + 7) & ~7u;

    put32(a, type);
    put32(a + 4, total);
    a[8] = 1;
    attr_name(a, 0x40, name);
    put16(a + 12, flags);
    put64(a + 0x10, vcn0);
    put64(a + 0x18, vcn1);
    put16(a + 0x20, roff);
    put64(a + 0x28, alloc);
    put64(a + 0x30, size);
    put64(a + 0x38, init);
    memcpy(a + roff, runs, runs_len);
    w->at += total;
}

/* One index entry naming `name` in record `ref`; returns its length. */
static unsigned entry(uint8_t *e, unsigned ref, const char *name, int ns,
                      int is_dir, unsigned flags, uint64_t sub)
{
    unsigned i, n = name ? (unsigned)strlen(name) : 0;
    unsigned klen = name ? 0x42 + 2 * n : 0;
    unsigned len = (16 + klen + (flags & 1 ? 8 : 0) + 7) & ~7u;

    memset(e, 0, len);
    put64(e, ref);
    put16(e + 8, len);
    put16(e + 10, klen);
    put16(e + 12, flags);
    if (name) {
        uint8_t *k = e + 16;
        put32(k + 0x38, is_dir ? 0x10000000u : 0);
        k[0x40] = (uint8_t)n;
        k[0x41] = (uint8_t)ns;
        for (i = 0; i < n; i++)
            put16(k + 0x42 + 2 * i, (unsigned char)name[i]);
    }
    if (flags & 1)
        put64(e + len - 8, sub);
    return len;
}

/* A node header at `h` over entries already written from h + 0x10. */
static void node(uint8_t *h, unsigned used, unsigned large)
{
    put32(h, 0x10);
    put32(h + 4, 0x10 + used);
    put32(h + 8, 0x10 + used);
    h[12] = (uint8_t)large;
}

static uint8_t s_big[5000], s_small[600];

static void indx_block(unsigned lcn, unsigned vcn, int loop)
{
    uint8_t *b = clu(lcn);
    unsigned used = 0;

    memset(b, 0, NC);
    head(b, "INDX", 0x28);
    put64(b + 0x10, vcn);
    if (vcn == 0) {
        used += entry(b + 0x28 + used, 21, "big.bin", 1, 0, 0, 0);
        used += entry(b + 0x28 + used, 0, NULL, 0, 0, 2, 0);
    } else {
        used += entry(b + 0x28 + used, 23, "ZETA~1.BIN", NS_DOS_TEST, 0, 0, 0);
        used += entry(b + 0x28 + used, 23, "zeta.bin", 1, 0, 0, 0);
        used += entry(b + 0x28 + used, 25, "packed.bin", 3, 0, 0, 0);
        used += entry(b + 0x28 + used, 0, NULL, 0, 0, loop ? 3 : 2, 1);
    }
    node(b + 0x18, used, 0);
    seal(b);
}

static void build_ntfs(int loop)
{
    static const uint8_t mft_runs[] = { 0x11, 16, 16, 0x11, 16, 84, 0 };
    /* Pkg's second block is BEFORE its first: a negative LCN delta. */
    static const uint8_t pkg_runs[] = { 0x21, 1, 200, 0, 0x11, 1, 0xCE, 0 };
    static const uint8_t big_runs0[] = { 0x21, 2, 0x90, 1, 0x01, 1, 0 };
    static const uint8_t big_runs1[] = { 0x21, 2, 0xF4, 1, 0 };
    static const uint8_t packed_runs[] = { 0x11, 1, 0x7F, 0 };
    uint8_t ir[512], al[128];
    unsigned used;
    rec_t w;

    memset(s_vol, 0, sizeof s_vol);
    /* The boot sector. */
    memcpy(s_vol + 3, "NTFS    ", 8);
    put16(s_vol + 0x0B, SEC);
    s_vol[0x0D] = NC / SEC;
    put64(s_vol + 0x28, N_CLUSTERS * (NC / SEC));
    put64(s_vol + 0x30, 16);
    s_vol[0x40] = 1;                    /* one cluster per record */
    s_vol[0x44] = 1;                    /* one cluster per index block */
    s_vol[510] = 0x55;
    s_vol[511] = 0xAA;

    /* $MFT: 32 records in two extents of its run list. */
    w = rec_begin(0, 1);
    nonresident(&w, 0x80, NULL, 0, 0, 31, mft_runs, sizeof mft_runs - 1,
                32 * NC, 32 * NC, 32 * NC);
    rec_end(&w);

    /* The root: ".", and the directory "Pkg". */
    memset(ir, 0, sizeof ir);
    put32(ir, 0x30);
    put32(ir + 8, NC);
    ir[12] = 1;
    used = entry(ir + 0x20, 5, ".", 3, 1, 0, 0);
    used += entry(ir + 0x20 + used, 20, "Pkg", 1, 1, 0, 0);
    used += entry(ir + 0x20 + used, 20, "PKG", NS_DOS_TEST, 1, 0, 0);
    used += entry(ir + 0x20 + used, 0, NULL, 0, 0, 2, 0);
    node(ir + 0x10, used, 0);
    w = rec_begin(5, 3);
    resident(&w, 0x90, "$I30", ir, 0x20 + used);
    rec_end(&w);

    /* Pkg: big.bin in the subnode before Small.TXT, zeta.bin and packed.bin
     * in the one after the last entry; its blocks at VCN 0 (LCN 200) and 1
     * (LCN 150). */
    memset(ir, 0, sizeof ir);
    put32(ir, 0x30);
    put32(ir + 8, NC);
    ir[12] = 1;
    used = entry(ir + 0x20, 22, "Small.TXT", 1, 0, 1, 0);
    used += entry(ir + 0x20 + used, 0, NULL, 0, 0, 3, 1);
    node(ir + 0x10, used, 1);
    w = rec_begin(20, 3);
    resident(&w, 0x90, "$I30", ir, 0x20 + used);
    nonresident(&w, 0xA0, "$I30", 0, 0, 1, pkg_runs, sizeof pkg_runs - 1,
                2 * NC, 2 * NC, 2 * NC);
    rec_end(&w);
    indx_block(200, 0, 0);
    indx_block(150, 1, loop);

    /* big.bin: 5000 bytes, 4500 initialized; VCN 0-1 at LCN 400, VCN 2
     * sparse (record 21), VCN 3-4 at LCN 500 (record 24, by the list). */
    memset(al, 0, sizeof al);
    put32(al, 0x80);
    put16(al + 4, 32);
    put64(al + 16, 21);
    put32(al + 32, 0x80);
    put16(al + 36, 32);
    put64(al + 40, 3);
    put64(al + 48, 24);
    w = rec_begin(21, 1);
    resident(&w, 0x20, NULL, al, 64);
    nonresident(&w, 0x80, NULL, 0, 0, 2, big_runs0, sizeof big_runs0 - 1,
                5 * NC, sizeof s_big, 4500);
    rec_end(&w);
    w = rec_begin(24, 1);
    put64(w.r + 0x20, 21);              /* its base record */
    nonresident(&w, 0x80, NULL, 0, 3, 4, big_runs1, sizeof big_runs1 - 1,
                5 * NC, sizeof s_big, 4500);
    rec_end(&w);
    fill(clu(400), 2 * NC, 7);
    fill(clu(500), 2 * NC, 9);
    memset(s_big, 0, sizeof s_big);
    memcpy(s_big, clu(400), 2 * NC);
    memcpy(s_big + 3 * NC, clu(500), 4500 - 3 * NC);

    /* Small.TXT: resident, across the record's first sector boundary, whose
     * last two bytes only the update sequence array restores. */
    fill(s_small, sizeof s_small, 3);
    memcpy(s_small, "hello ntfs", 10);
    w = rec_begin(22, 1);
    resident(&w, 0x80, NULL, s_small, sizeof s_small);
    rec_end(&w);
    w = rec_begin(23, 1);
    resident(&w, 0x80, NULL, "z", 1);
    rec_end(&w);
    w = rec_begin(25, 1);
    nonresident(&w, 0x80, NULL, 0x0001, 0, 0, packed_runs,
                sizeof packed_runs - 1, NC, 100, 100);
    rec_end(&w);
}

static int vol_read(void *ctx, uint64_t pos, void *buf, size_t n)
{
    (void)ctx;
    if (pos + n > sizeof s_vol)
        return 0;
    memcpy(buf, s_vol + pos, n);
    return 1;
}

static struct { wchar_t path[64]; uint64_t rec; } s_seen[16];
static int s_nseen;

static int ntfs_visit(void *ctx, const wchar_t *path, uint64_t rec)
{
    (void)ctx;
    if (s_nseen < 16) {
        wcsncpy(s_seen[s_nseen].path, path, 63);
        s_seen[s_nseen].rec = rec;
    }
    s_nseen++;
    return 0;
}

static void test_ntfs(void)
{
    fat_image_t img = { vol_read, mem_plan, NULL };
    uint64_t vols[NTFS_MAX_VOLUMES], rec = 0, size = 0;
    char err[256];
    ntfs_t *v;
    int i;

    build_ntfs(0);
    check(ntfs_volumes(&img, vols, NTFS_MAX_VOLUMES) == 1 && vols[0] == 0,
          "ntfs: a bare volume at byte 0, though the sector ends 55 AA");
    if (ntfs_open(&img, 0, &v, err, sizeof err)) {
        fprintf(stderr, "  ntfs_open: %s\n", err);
        check(0, "ntfs: the volume opens");
        return;
    }
    check(ntfs_record_count(v) == 32, "ntfs: $MFT's size, over both extents");
    check(ntfs_lookup(v, "pkg/SMALL.txt", &rec, err, sizeof err) == 1 &&
          rec == 22, "ntfs: lookup ignores case");
    check(ntfs_lookup(v, "Pkg\\nothing", &rec, err, sizeof err) == 0,
          "ntfs: an absent name is 0, not an error");
    check(ntfs_lookup(v, "Pkg/zeta.bin", &rec, err, sizeof err) == 1 &&
          rec == 23, "ntfs: a name in the subnode after the last entry");

    s_nseen = 0;
    check(ntfs_walk(v, NTFS_ROOT, ntfs_visit, NULL, err, sizeof err) == 0,
          "ntfs: the walk completes");
    check(s_nseen == 4, "ntfs: four files; '.', and the DOS aliases, left out");
    if (s_nseen == 4) {
        static const wchar_t *const want[4] = {
            L"Pkg\\big.bin", L"Pkg\\Small.TXT", L"Pkg\\zeta.bin",
            L"Pkg\\packed.bin" };
        for (i = 0; i < 4; i++)
            check(!wcscmp(s_seen[i].path, want[i]),
                  "ntfs: index order, subnode before its entry and END's last");
    }

    check(ntfs_file_size(v, 21, &size, err, sizeof err) == 0 && size == 5000,
          "ntfs: a split file's size");
    s_outn = 0;
    s_plans = 0;
    check(ntfs_read_file(v, 21, sink, NULL, err, sizeof err) == 0 &&
          s_outn == sizeof s_big && !memcmp(s_out, s_big, sizeof s_big),
          "ntfs: two records' extents, a sparse run, zeroes past initialized");
    check(s_plans == 1 && s_plan_ranges == 2,
          "ntfs: the file's two allocated runs are planned before the copy");
    s_outn = 0;
    check(ntfs_read_file(v, 22, sink, NULL, err, sizeof err) == 0 &&
          s_outn == sizeof s_small && !memcmp(s_out, s_small, sizeof s_small),
          "ntfs: resident data, across a sector's fixed-up tail");
    check(ntfs_read_file(v, 25, sink, NULL, err, sizeof err) == -1 &&
          strstr(err, "compressed"), "ntfs: compressed data is refused");

    /* A torn record: one sector's tail no longer carries the sequence. */
    mft_rec(22)[2 * SEC - 1] ^= 0xFF;
    check(ntfs_read_file(v, 22, sink, NULL, err, sizeof err) == -1 &&
          strstr(err, "update sequence"), "ntfs: a torn record is refused");
    ntfs_close(v);

    /* An index whose last node names itself again. */
    build_ntfs(1);
    if (!ntfs_open(&img, 0, &v, err, sizeof err)) {
        s_nseen = 0;
        check(ntfs_walk(v, NTFS_ROOT, ntfs_visit, NULL, err, sizeof err) == -1 &&
              strstr(err, "revisits"), "ntfs: an index loop is refused");
        ntfs_close(v);
    }

    /* The update sequence's block is 512 bytes on any sector size: the same
     * volume, said to have 1024-byte sectors, still reads. */
    build_ntfs(0);
    put16(s_vol + 0x0B, 1024);
    s_vol[0x0D] = NC / 1024;
    put64(s_vol + 0x28, N_CLUSTERS * (NC / 1024));
    if (!ntfs_open(&img, 0, &v, err, sizeof err)) {
        s_outn = 0;
        check(ntfs_read_file(v, 22, sink, NULL, err, sizeof err) == 0 &&
              s_outn == sizeof s_small && !memcmp(s_out, s_small, sizeof s_small),
              "ntfs: 1024-byte sectors, a 512-byte update sequence");
        ntfs_close(v);
    } else {
        fprintf(stderr, "  ntfs_open: %s\n", err);
        check(0, "ntfs: a volume of 1024-byte sectors opens");
    }
}


/* ---- CD frames ------------------------------------------------------------
 *
 * The parity is checked by its own definition, not by a second copy of the
 * encoder: every P and Q vector of a rebuilt sector must have both syndromes
 * zero (ECMA-130 Annex A), computed here by shift-and-add multiplication. */

static uint8_t gf_mul_slow(uint8_t a, uint8_t b)
{
    unsigned r = 0, x = a;
    while (b) {
        if (b & 1)
            r ^= x;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
        b >>= 1;
    }
    return (uint8_t)r;
}

/* 1 when every RSPC vector of the sector checks; the header is taken as
 * zero for a mode 2 sector, as the parity was computed. */
static int rspc_ok(const uint8_t *sector)
{
    uint8_t s[2352];
    unsigned plane, k, m;

    memcpy(s, sector, sizeof s);
    if (s[15] == 2)
        memset(s + 12, 0, 4);
    for (plane = 0; plane < 2; plane++) {
        const uint8_t *w = s + 12 + plane;
        for (k = 0; k < 43; k++) {              /* P: 26-symbol vectors */
            uint8_t a = 0, b = 0;
            for (m = 0; m < 26; m++) {
                uint8_t v = w[2 * (43 * m + k)];
                a ^= v;
                b = gf_mul_slow(b, 2) ^ v;      /* Horner: a^(25-m) */
            }
            if (a || b)
                return 0;
        }
        for (k = 0; k < 26; k++) {              /* Q: 45-symbol vectors */
            uint8_t a = 0, b = 0;
            for (m = 0; m < 45; m++) {
                unsigned at = m < 43 ? (44 * m + 43 * k) % 1118
                                     : (m == 43 ? 1118 + k : 1144 + k);
                uint8_t v = w[2 * at];
                a ^= v;
                b = gf_mul_slow(b, 2) ^ v;
            }
            if (a || b)
                return 0;
        }
    }
    return 1;
}

static void cd_sector(uint8_t *s, unsigned mode, unsigned seed)
{
    fill(s, 2352, seed);
    s[12] = 0x00; s[13] = 0x02; s[14] = (uint8_t)seed; s[15] = (uint8_t)mode;
    if (mode == 2)
        memset(s + 16, 0, 8);                   /* form 1 subheader */
    chd_cd_rebuild(s);
}

/* A raw deflate stream of stored blocks: no compressor needed. */
static size_t stored(uint8_t *out, const uint8_t *in, size_t n)
{
    size_t at = 0;
    do {
        size_t k = n > 65535 ? 65535 : n;
        out[at++] = n == k;                     /* BFINAL, BTYPE 00 */
        put16(out + at, (unsigned)k);
        put16(out + at + 2, (unsigned)(~k & 0xFFFF));
        memcpy(out + at + 4, in, k);
        at += 4 + k;
        in += k;
        n -= k;
    } while (n);
    return at;
}

static void test_cd(void)
{
    static uint8_t frames[2][2448], secs[2 * 2352], sub[2 * 96];
    static uint8_t raw[8000], out[2 * 2448];
    uint8_t s[2352];
    size_t at, clen;
    int f;

    cd_sector(s, 1, 11);
    check(rspc_ok(s), "a rebuilt mode 1 sector's P and Q vectors all check");
    check(!memcmp(s, "\0\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\0", 12),
          "the rebuild writes the sync pattern");
    s[700] ^= 0x40;
    check(!rspc_ok(s), "the checker sees one flipped data byte");
    cd_sector(s, 2, 12);
    check(rspc_ok(s), "a mode 2 sector's parity is over a zeroed header");
    check(s[12] == 0 && s[13] == 2 && s[14] == 12 && s[15] == 2,
          "the rebuild puts the mode 2 header back");

    /* A cdzl hunk of two frames, frame 1 stripped: bitmap bit 1 of byte 0. */
    for (f = 0; f < 2; f++) {
        cd_sector(frames[f], 1, 20 + f);
        fill(frames[f] + 2352, 96, 30 + f);
        memcpy(secs + f * 2352, frames[f], 2352);
        memcpy(sub + f * 96, frames[f] + 2352, 96);
    }
    memset(secs + 2352, 0, 12);
    memset(secs + 2352 + 2076, 0, 276);
    raw[0] = 0x02;
    clen = stored(raw + 3, secs, sizeof secs);
    raw[1] = (uint8_t)(clen >> 8);
    raw[2] = (uint8_t)clen;
    at = 3 + clen;
    at += stored(raw + at, sub, sizeof sub);
    check(chd_cd_decode("cdzl", raw, at, out, 2) &&
          !memcmp(out, frames, sizeof out),
          "a cdzl hunk: sectors, subcode, and the stripped frame rebuilt");
    raw[0] = 0x01;
    check(chd_cd_decode("cdzl", raw, at, out, 2) &&
          memcmp(out, frames, sizeof out),
          "the bitmap's bit i is frame i: rebuilding frame 0 instead differs");
    raw[0] = 0x02;
    check(!chd_cd_decode("cdzl", raw, at - 1, out, 2),
          "a subcode stream cut short is refused");
}

/* ---- ISO 9660 ------------------------------------------------------------ */

#define ISO_SECTORS 27
static uint8_t s_iso[ISO_SECTORS * 2048], s_isoraw[ISO_SECTORS * 2448];
static uint8_t s_snow[3000];
static size_t s_isosz;

static int iso_mem_read(void *ctx, uint64_t pos, void *buf, size_t n)
{
    (void)ctx;
    if (pos > s_isosz || n > s_isosz - pos)
        return 0;
    memcpy(buf, s_isoraw + pos, n);
    return 1;
}

static void put32both(uint8_t *p, uint32_t v)
{
    put32(p, v);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)v;
}

/* A directory record at d; name in UCS-2 BE when joliet. Returns its length. */
static unsigned iso_rec(uint8_t *d, uint32_t extent, uint32_t size,
                        uint8_t flags, const char *name, int joliet)
{
    unsigned nlen = name ? (unsigned)strlen(name) * (joliet ? 2 : 1) : 1, i;
    unsigned len = 33 + nlen + ((33 + nlen) & 1);
    memset(d, 0, len);
    d[0] = (uint8_t)len;
    put32both(d + 2, extent);
    put32both(d + 10, size);
    d[25] = flags;
    d[32] = (uint8_t)nlen;
    if (!name)
        d[33] = 0;
    else
        for (i = 0; name[i]; i++) {
            if (joliet) {
                d[33 + 2 * i] = 0;
                d[34 + 2 * i] = (uint8_t)name[i];
            } else {
                d[33 + i] = (uint8_t)name[i];
            }
        }
    return len;
}

static uint8_t *iso_sec(unsigned n) { return s_iso + n * 2048; }

/* A directory sector: ".", "..", then up to two entries. */
static void iso_dir(unsigned at, unsigned parent, const char *n1,
                    uint32_t e1, uint32_t z1, uint8_t f1, const char *n2,
                    uint32_t e2, uint32_t z2, uint8_t f2)
{
    uint8_t *d = iso_sec(at);
    unsigned o = iso_rec(d, at, 2048, 2, NULL, 1), dotdot = o;
    o += iso_rec(d + o, parent, 2048, 2, NULL, 1);
    d[dotdot + 33] = 1;                         /* ".." is the name byte 1 */
    if (n1)
        o += iso_rec(d + o, e1, z1, f1, n1, 1);
    if (n2)
        iso_rec(d + o, e2, z2, f2, n2, 1);
}

static void build_iso(void)
{
    uint8_t *v;
    memset(s_iso, 0, sizeof s_iso);
    fill(s_snow, sizeof s_snow, 77);
    v = iso_sec(16);                            /* primary */
    v[0] = 1; memcpy(v + 1, "CD001", 5); v[6] = 1;
    put16(v + 128, 2048);
    iso_rec(v + 156, 20, 2048, 2, NULL, 0);
    v = iso_sec(17);                            /* Joliet */
    v[0] = 2; memcpy(v + 1, "CD001", 5); v[6] = 1;
    memcpy(v + 88, "%/E", 3);
    put16(v + 128, 2048);
    iso_rec(v + 156, 21, 2048, 2, NULL, 1);
    v = iso_sec(18);                            /* terminator */
    v[0] = 255; memcpy(v + 1, "CD001", 5); v[6] = 1;
    iso_dir(21, 21, "Installation Directory", 22, 2048, 2, NULL, 0, 0, 0);
    iso_dir(22, 21, "Arctic Thunder", 23, 2048, 2, NULL, 0, 0, 0);
    iso_dir(23, 22, "snow.exe", 24, sizeof s_snow, 0, "version", 26, 1, 0);
    memcpy(iso_sec(24), s_snow, sizeof s_snow);
    iso_sec(26)[0] = '8';
}

/* The cooked image as raw sectors of `stride` bytes, mode 2 form 1. */
static void wrap_iso(unsigned stride)
{
    unsigned i;
    if (stride == 2048) {
        memcpy(s_isoraw, s_iso, sizeof s_iso);
        s_isosz = sizeof s_iso;
        return;
    }
    memset(s_isoraw, 0, sizeof s_isoraw);
    for (i = 0; i < ISO_SECTORS; i++) {
        uint8_t *s = s_isoraw + i * stride;
        memcpy(s, "\0\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\0", 12);
        s[15] = 2;
        memcpy(s + 24, iso_sec(i), 2048);
    }
    s_isosz = (size_t)ISO_SECTORS * stride;
}

static struct { wchar_t path[128]; uint32_t size; } s_isofiles[8];
static int s_nisofiles;

static int iso_visit(void *ctx, const wchar_t *path, const iso_node_t *n)
{
    (void)ctx;
    if (s_nisofiles < 8) {
        wcsncpy(s_isofiles[s_nisofiles].path, path, 127);
        s_isofiles[s_nisofiles++].size = n->size;
    }
    return 0;
}

static uint64_t s_plan_bytes;

static void iso_plan(void *ctx, const uint64_t *start, const uint32_t *len,
                     size_t n)
{
    (void)ctx; (void)start;
    while (n--)
        s_plan_bytes += len[n];
}

static void test_iso_layout(unsigned stride, const char *what)
{
    fat_image_t img = { iso_mem_read, iso_plan, NULL };
    char err[256], line[160];
    iso_node_t n;
    iso_t *v;

    wrap_iso(stride);
    snprintf(line, sizeof line, "%s: opens", what);
    check(iso_open(&img, &v, err, sizeof err) == 1, line);
    if (!v)
        return;
    snprintf(line, sizeof line, "%s: a path found without regard to case",
             what);
    check(iso_lookup(v, "installation directory/ARCTIC THUNDER\\Snow.EXE",
                     &n, err, sizeof err) == 1 && !n.dir &&
          n.size == sizeof s_snow, line);
    s_outn = 0;
    snprintf(line, sizeof line, "%s: a two-sector file reads back whole",
             what);
    check(iso_read_file(v, &n, sink, NULL, err, sizeof err) == 0 &&
          s_outn == sizeof s_snow && !memcmp(s_out, s_snow, s_outn), line);
    {
        /* Files near 4 GiB, which the disc does not hold: the plan comes
         * before the read that fails, and covers every sector. */
        static const uint32_t sizes[] = { 0xF0000000u, 0xFFFFFFFFu };
        iso_node_t big = n;
        unsigned i;

        for (i = 0; i < 2; i++) {
            uint64_t count = sizes[i] / 2048u + (sizes[i] % 2048u != 0);

            big.size = sizes[i];
            s_plan_bytes = 0;
            s_outn = 0;
            snprintf(line, sizeof line, "%s: a %#x-byte file is planned "
                     "whole, past 32 bits", what, sizes[i]);
            check(iso_read_file(v, &big, sink, NULL, err, sizeof err) != 0 &&
                  s_plan_bytes == count * stride, line);
        }
    }
    snprintf(line, sizeof line, "%s: an absent path is 0, not an error",
             what);
    check(iso_lookup(v, "Installation Directory/nothing", &n, err,
                     sizeof err) == 0, line);
    s_nisofiles = 0;
    snprintf(line, sizeof line, "%s: the walk names both files by their "
             "Joliet names", what);
    {
        iso_node_t root;
        check(iso_lookup(v, "", &root, err, sizeof err) == 1 &&
              iso_walk(v, &root, iso_visit, NULL, err, sizeof err) == 0 &&
              s_nisofiles == 2 &&
              !wcscmp(s_isofiles[0].path,
                      L"Installation Directory\\Arctic Thunder\\snow.exe") &&
              !wcscmp(s_isofiles[1].path,
                      L"Installation Directory\\Arctic Thunder\\version"),
              line);
    }
    iso_close(v);
}

static void test_iso(void)
{
    fat_image_t img = { iso_mem_read, NULL, NULL };
    char err[256];
    iso_node_t n;
    iso_t *v;

    build_iso();
    test_iso_layout(2048, "a cooked image");
    test_iso_layout(2352, "raw mode 2 sectors");
    test_iso_layout(2448, "a CD CHD's 2448-byte frames");

    wrap_iso(2352);
    s_isoraw[24 * 2352 + 18] |= 0x20;           /* snow.exe's first: form 2 */
    check(iso_open(&img, &v, err, sizeof err) == 1 &&
          iso_lookup(v, "Installation Directory/Arctic Thunder/snow.exe", &n,
                     err, sizeof err) == 1 &&
          iso_read_file(v, &n, sink, NULL, err, sizeof err) == -1,
          "a mode 2 form 2 sector is refused, never read as data");
    iso_close(v);
    s_isoraw[24 * 2352 + 18] &= (uint8_t)~0x20;
    memset(s_isoraw + 24 * 2352, 0, 12);
    check(iso_open(&img, &v, err, sizeof err) == 1 &&
          iso_lookup(v, "Installation Directory/Arctic Thunder/snow.exe", &n,
                     err, sizeof err) == 1 &&
          iso_read_file(v, &n, sink, NULL, err, sizeof err) == -1,
          "a raw sector with no sync pattern is refused");
    iso_close(v);

    build_iso();
    {
        /* snow.exe's record, after "." and "..", spread over extents */
        uint8_t *d = iso_sec(23);
        unsigned o = d[0];
        o += d[o];
        d[o + 25] |= 0x80;
    }
    wrap_iso(2048);
    check(iso_open(&img, &v, err, sizeof err) == 1 &&
          iso_lookup(v, "Installation Directory/Arctic Thunder/snow.exe", &n,
                     err, sizeof err) == -1,
          "a file over several extents is refused");
    iso_close(v);

    memset(s_isoraw, 0, sizeof s_isoraw);
    s_isosz = sizeof s_iso;
    check(iso_open(&img, &v, err, sizeof err) == 0 && !v,
          "an image with no volume descriptor is not a CD, not an error");
}

int main(void)
{
    check(chd_crc16("123456789", 9, 0xFFFF) == 0x29B1,
          "CRC-16/CCITT check value");
    check(chd_crc16("", 0, 0xFFFF) == 0xFFFF, "CRC-16 of nothing is its seed");
    test_fat();
    test_flac();
    test_ntfs();
    test_cd();
    test_iso();
    if (failures) {
        fprintf(stderr, "import-test: %d of %d check(s) failed\n", failures,
                checks);
        return 1;
    }
    printf("import-test: %d checks, all passed\n", checks);
    return 0;
}
