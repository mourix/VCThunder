/* import_test.c -- the CHD importer's pieces, with no game and no CHD.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A disk built in memory, shaped like the cabinets' (a primary FAT16, then
 * logical volumes in an extended partition whose EBR entries are relative to
 * two different bases), read through src/fat.c; hand-assembled FLAC frames
 * through src/chd.c's decoder; and CRC-16's published check value. The real
 * CHDs are the maintainer's, compared with the Python oracle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../src/chd.h"
#include "../src/fat.h"

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

int main(void)
{
    check(chd_crc16("123456789", 9, 0xFFFF) == 0x29B1,
          "CRC-16/CCITT check value");
    check(chd_crc16("", 0, 0xFFFF) == 0xFFFF, "CRC-16 of nothing is its seed");
    test_fat();
    test_flac();
    if (failures) {
        fprintf(stderr, "import-test: %d of %d check(s) failed\n", failures,
                checks);
        return 1;
    }
    printf("import-test: %d checks, all passed\n", checks);
    return 0;
}
