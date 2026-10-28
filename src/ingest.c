/* ingest.c -- import a game from the user's own MAME CHD.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The C form of the Python ingest, which the maintainer keeps as the oracle
 * and compares with this file by file. NO GAME MATERIAL IS IN THIS FILE:
 * this reads the user's own lawful dump and writes it to the user's own disk.
 *
 * The order is the point, and each step is a rule paid for once already:
 *   1. the executable is found and hashed INSIDE the CHD, and a build this
 *      host has no tables for is refused before anything is written;
 *   2. every FAT partition, primary and logical, is merged flat into one
 *      folder per title, because fs_map strips the drive letter;
 *   3. a partition that could not be read FAILS the import, because the
 *      executable's hash says nothing about the rest of the disk;
 *   4. the files are written to <title>.part and renamed into place only once
 *      the executable written out has verified, so nothing half-done is ever
 *      left where the launcher looks.
 * A win32 title's disk is one NTFS volume instead (Arctic Thunder's), and
 * nothing is merged: its install package's game folder becomes data/<id>
 * and the operator system's folder data/<id>-diagnostics, both verified by
 * every file the host pins before either is put in place.
 * An update title's CD (Ultimate Arctic Thunder's) holds only what the update
 * changed: its package is laid over the base title's imported folders, as
 * the cabinet's own update laid it over the installed game, and the result is
 * verified the same way. The CD is a CD CHD or the user's own cue sheet and
 * image; which it is decides nothing, the snow.exe on it does. */
#include "ingest.h"
#include "arctic.h"
#include "chd.h"
#include "fat.h"
#include "hash.h"
#include "iso.h"
#include "ntfs.h"

#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WPATH 1024

/* ---- narration ------------------------------------------------------------ */

static void tell(const ingest_ui_t *ui, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void tell(const ingest_ui_t *ui, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (!ui || !ui->say)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ui->say(ui->ctx, line);
}

static void why_set(char *why, size_t whysz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void why_set(char *why, size_t whysz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, whysz, fmt, ap);
    va_end(ap);
}

static int cancelled(const ingest_ui_t *ui)
{
    return ui && ui->cancel && *ui->cancel;
}

/* ---- the image, as fat.c reads it ------------------------------------------ */

static int img_read(void *ctx, uint64_t pos, void *buf, size_t n)
{
    size_t got;
    return chd_read(ctx, pos, buf, n, &got) == CHD_OK && got == n;
}

static void img_plan(void *ctx, const uint64_t *start, const uint32_t *len,
                     size_t n)
{
    chd_plan(ctx, start, len, n);
}

/* ---- paths ------------------------------------------------------------------ */

static void narrow(const wchar_t *w, char *out, size_t outsz)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outsz, NULL, NULL))
        snprintf(out, outsz, "?");
}

static const wchar_t *leaf_w(const wchar_t *p)
{
    const wchar_t *a = wcsrchr(p, L'\\'), *b = wcsrchr(p, L'/');
    if (b > a)
        a = b;
    return a ? a + 1 : p;
}

/* Every directory on the way to `path`'s last component. */
static int make_parents(wchar_t *path)
{
    wchar_t *p;
    for (p = path; *p; p++) {
        if ((*p == L'\\' || *p == L'/') && p > path && p[-1] != L':') {
            wchar_t keep = *p;
            *p = 0;
            if (!CreateDirectoryW(path, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                *p = keep;
                return 0;
            }
            *p = keep;
        }
    }
    return 1;
}

static int is_dir(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Remove a directory tree this importer wrote. */
static void remove_tree(const wchar_t *dir)
{
    wchar_t pat[WPATH], sub[WPATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;

    if (_snwprintf(pat, WPATH, L"%ls\\*", dir) < 0)
        return;
    f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (_snwprintf(sub, WPATH, L"%ls\\%ls", dir, fd.cFileName) < 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(sub);
            } else {
                SetFileAttributesW(sub, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(sub);
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir);
}

/* `name` in `dir`, matched without regard to case: FAT is case-preserving and
 * the two dumps disagree. */
static int find_ci(const wchar_t *dir, const char *name, wchar_t *out)
{
    wchar_t pat[WPATH], want[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int hit = 0;

    MultiByteToWideChar(CP_UTF8, 0, name, -1, want, MAX_PATH);
    if (_snwprintf(pat, WPATH, L"%ls\\*", dir) < 0)
        return 0;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !_wcsicmp(fd.cFileName, want)) {
            _snwprintf(out, WPATH, L"%ls\\%ls", dir, fd.cFileName);
            hit = 1;
        }
    } while (!hit && FindNextFileW(f, &fd));
    FindClose(f);
    return hit;
}

static int hash_file(const wchar_t *path, char out[65])
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    static unsigned char buf[1 << 16];
    sha256_t s;
    DWORD got;
    int ok;

    out[0] = 0;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ok = sha256_begin(&s);
    while (ok && ReadFile(f, buf, sizeof buf, &got, NULL) && got)
        ok = sha256_update(&s, buf, got);
    CloseHandle(f);
    if (!ok) {
        sha256_end(&s, out);
        out[0] = 0;
        return 0;
    }
    return sha256_end(&s, out);
}

/* ---- the titles ---------------------------------------------------------------- */

#define MAX_TITLES 8

static ingest_title_t s_titles[MAX_TITLES];
static int s_ntitles;

/* Every ETS profile, then every win32 title, which has no profile. */
static void titles_init(void)
{
    int i;

    if (s_ntitles)
        return;
    for (i = 0; g_profiles[i] && s_ntitles < MAX_TITLES; i++) {
        ingest_title_t *t = &s_titles[s_ntitles++];
        t->id = g_profiles[i]->id;
        t->name = g_profiles[i]->name;
        t->exe = g_profiles[i]->exe;
        t->sha256 = g_profiles[i]->sha256;
        t->build = g_profiles[i]->build;
    }
    for (i = 0; g_arctic_titles[i] && s_ntitles < MAX_TITLES; i++) {
        const arctic_ids_t *a = g_arctic_titles[i]->ids;
        ingest_title_t *t = &s_titles[s_ntitles++];
        t->id = a->id;
        t->name = a->name;
        t->exe = a->file[ARCTIC_SNOW].name;
        t->sha256 = a->file[ARCTIC_SNOW].sha256;
        t->build = a->build;
        t->disk_dir = a->disk_dir;
        t->diag_dir = a->diag_dir;
        t->base = a->base;
        t->update_file = a->update_file;
        t->update_to = a->update_to;
    }
}

static const ingest_title_t *title_by_id(const char *id)
{
    int i;
    for (i = 0; id && i < s_ntitles; i++)
        if (!strcmp(s_titles[i].id, id))
            return &s_titles[i];
    return NULL;
}

/* Of the titles whose executable lies at the same path (Arctic Thunder and
 * its update keep snow.exe in the same folder), the one whose registered
 * build this is; else the first, so the refusal names a title. */
static const ingest_title_t *title_for(const ingest_title_t *first,
                                       const char *sha)
{
    int i;
    for (i = 0; i < s_ntitles; i++) {
        const ingest_title_t *t = &s_titles[i];
        if (t->disk_dir && first->disk_dir &&
            !strcmp(t->disk_dir, first->disk_dir) &&
            !_stricmp(t->exe, first->exe) && !_stricmp(t->sha256, sha))
            return t;
    }
    return first;
}

/* The folders a title's import writes under the data folder, each from where
 * on the disk: an ETS title's one, merged from every partition (disk NULL),
 * or a win32 title's game folder and operator system's folder. */
typedef struct {
    const char *disk;
    char name[64];
} dest_t;

static int dests_of(const ingest_title_t *t, dest_t out[2])
{
    out[0].disk = t->disk_dir;
    snprintf(out[0].name, sizeof out[0].name, "%s", t->id);
    if (!t->diag_dir)
        return 1;
    out[1].disk = t->diag_dir;
    snprintf(out[1].name, sizeof out[1].name, "%s-diagnostics", t->id);
    return 2;
}

/* ---- identifying a disk ----------------------------------------------------- */

typedef struct {
    fat_t *vol;
    const ingest_title_t *hit;
    char sha[65];
    int failed;
} ident_walk_t;

static int sink_hash(void *ctx, const void *buf, size_t n)
{
    return sha256_update(ctx, buf, n) ? 0 : 1;
}

static int visit_ident(void *ctx, const wchar_t *path, uint32_t cl,
                       uint32_t size)
{
    ident_walk_t *w = ctx;
    char leaf[MAX_PATH], err[256];
    sha256_t s;
    int i;

    narrow(leaf_w(path), leaf, sizeof leaf);
    for (i = 0; i < s_ntitles; i++) {
        if (s_titles[i].disk_dir || _stricmp(leaf, s_titles[i].exe))
            continue;
        if (!sha256_begin(&s) ||
            fat_read_file(w->vol, cl, size, sink_hash, &s, err, sizeof err)) {
            sha256_end(&s, w->sha);
            w->failed = 1;
            return 1;
        }
        sha256_end(&s, w->sha);
        w->hit = &s_titles[i];
        return 1;                       /* the first executable found decides */
    }
    return 0;
}

/* A win32 title on an NTFS volume: its executable, at the path its install
 * package puts it. 1 decided (a title or none), 0 the disk is damaged. */
static int identify_ntfs(const fat_image_t *img, const uint64_t *vols,
                         int nvols, ingest_ident_t *id, char *why,
                         size_t whysz)
{
    int i, k;

    for (k = 0; k < nvols; k++) {
        ntfs_t *v;
        char err[256], path[MAX_PATH];
        uint64_t rec;

        if (ntfs_open(img, vols[k], &v, err, sizeof err))
            continue;
        for (i = 0; i < s_ntitles; i++) {
            const ingest_title_t *t = &s_titles[i];
            sha256_t s;
            int rc;

            if (!t->disk_dir || t->base)
                continue;
            snprintf(path, sizeof path, "%s/%s", t->disk_dir, t->exe);
            rc = ntfs_lookup(v, path, &rec, err, sizeof err);
            if (rc == 0)
                continue;
            if (rc < 0 || !sha256_begin(&s) ||
                ntfs_read_file(v, rec, sink_hash, &s, err, sizeof err)) {
                if (rc > 0)
                    sha256_end(&s, id->sha256);
                ntfs_close(v);
                why_set(why, whysz, "The game's executable could not be read "
                        "out of this CHD; the file is damaged.");
                return 0;
            }
            sha256_end(&s, id->sha256);
            id->title = title_for(t, id->sha256);
            id->supported = !_stricmp(id->sha256, id->title->sha256);
            ntfs_close(v);
            return 1;
        }
        ntfs_close(v);
    }
    return 1;
}

/* The title and the build, from the executable read where it lies on the
 * disk: a second, nothing written. */
static int identify(const fat_image_t *img, ingest_ident_t *id, char *why,
                    size_t whysz)
{
    fat_part_t parts[FAT_MAX_PARTS];
    uint64_t vols[NTFS_MAX_VOLUMES];
    int nvols, n, i;

    titles_init();
    memset(id, 0, sizeof *id);
    nvols = ntfs_volumes(img, vols, NTFS_MAX_VOLUMES);
    if (nvols)
        return identify_ntfs(img, vols, nvols, id, why, whysz);
    n = fat_partitions(img, parts, FAT_MAX_PARTS);
    for (i = 0; i < n; i++) {
        ident_walk_t w;
        char err[256];
        if (parts[i].type && !fat_type_known(parts[i].type))
            continue;
        memset(&w, 0, sizeof w);
        if (fat_open(img, parts[i].lba, &w.vol, err, sizeof err))
            continue;
        fat_walk(w.vol, visit_ident, &w, err, sizeof err);
        fat_close(w.vol);
        if (w.failed) {
            why_set(why, whysz, "The game's executable could not be read out "
                    "of this CHD; the file is damaged.");
            return 0;
        }
        if (w.hit) {
            id->title = w.hit;
            memcpy(id->sha256, w.sha, sizeof id->sha256);
            id->supported = !_stricmp(w.sha, w.hit->sha256);
            return 1;
        }
    }
    return 1;
}

/* An update title's CD: its snow.exe where the update's package keeps it,
 * hashed on the disc. 1 decided (a title or none), 0 the disc is damaged. */
static int identify_disc(iso_t *v, ingest_ident_t *id, char *why,
                         size_t whysz)
{
    int i;

    titles_init();
    memset(id, 0, sizeof *id);
    for (i = 0; i < s_ntitles; i++) {
        const ingest_title_t *t = &s_titles[i];
        char path[MAX_PATH], err[256];
        iso_node_t n;
        sha256_t s;
        int rc;

        if (!t->base)
            continue;
        snprintf(path, sizeof path, "%s/%s", t->disk_dir, t->exe);
        rc = iso_lookup(v, path, &n, err, sizeof err);
        if (rc == 0 || (rc == 1 && n.dir))
            continue;
        if (rc < 0 || !sha256_begin(&s) ||
            iso_read_file(v, &n, sink_hash, &s, err, sizeof err)) {
            if (rc > 0)
                sha256_end(&s, id->sha256);
            why_set(why, whysz, "The game's executable could not be read off "
                    "this disc: %s.", err);
            return 0;
        }
        sha256_end(&s, id->sha256);
        id->title = title_for(t, id->sha256);
        id->supported = !_stricmp(id->sha256, id->title->sha256);
        return 1;
    }
    return 1;
}

/* ---- which disk MAME calls it ------------------------------------------------- */

static void hex20(const uint8_t b[20], char out[41])
{
    int i;
    for (i = 0; i < 20; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
}

/* From the header alone: a label, never the gate. Either SHA-1 matches, since
 * the data's alone is the same disk re-made with other metadata. */
static const game_mame_disk_t *mame_disk(const chd_t *c)
{
    const game_mame_disk_t *d;
    uint8_t b[20];
    char sha1[41], raw[41];

    chd_sha1(c, b);
    hex20(b, sha1);
    chd_raw_sha1(c, b);
    hex20(b, raw);
    for (d = g_mame_disks; d->set; d++)
        if (!strcmp(sha1, d->sha1) || !strcmp(raw, d->rawsha1))
            return d;
    return NULL;
}

/* " MAME's hydrthnd101b (hydro_101b.chd) and hydrthnd (hydro.chd) hold the
 * one that runs.", or nothing when MAME lists no disk of that build. */
static void mame_runs(const ingest_title_t *t, char *out, size_t outsz)
{
    const game_mame_disk_t *d;
    size_t at = 0;
    int n = 0, total = 0;

    for (d = g_mame_disks; d->set; d++)
        total += !strcmp(d->title, t->id) && !_stricmp(d->exe_sha256, t->sha256);
    out[0] = 0;
    for (d = g_mame_disks; d->set && at < outsz; d++) {
        if (strcmp(d->title, t->id) || _stricmp(d->exe_sha256, t->sha256))
            continue;
        at += snprintf(out + at, outsz - at, "%s%s (%s.chd)",
                       !n ? " MAME's " : n == total - 1 ? " and " : ", ",
                       d->set, d->chd);
        n++;
    }
    if (n && at < outsz)
        snprintf(out + at, outsz - at, " hold%s the one that runs.",
                 n == 1 ? "s" : "");
}

/* ---- extracting ---------------------------------------------------------------- */

typedef struct {
    const ingest_ui_t *ui;
    fat_t *vol;
    ntfs_t *nv;
    iso_t *disc;
    wchar_t root[WPATH];
    uint64_t total, done, files;
    int writing;                /* 0: the sizing pass */
    HANDLE out;
    int write_failed;
    DWORD write_error;
    char err[256];
} extract_t;

static int sink_write(void *ctx, const void *buf, size_t n)
{
    extract_t *x = ctx;
    DWORD put;
    if (cancelled(x->ui))
        return 2;
    if (!WriteFile(x->out, buf, (DWORD)n, &put, NULL) || put != n) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    x->done += n;
    if (x->ui && x->ui->progress)
        x->ui->progress(x->ui->ctx, x->done, x->total);
    return 0;
}

static int visit_extract(void *ctx, const wchar_t *path, uint32_t cl,
                         uint32_t size)
{
    extract_t *x = ctx;
    wchar_t dst[WPATH];
    int rc;

    if (!x->writing) {
        x->total += size;
        x->files++;
        return 0;
    }
    if (cancelled(x->ui))
        return 2;
    if (_snwprintf(dst, WPATH, L"%ls\\%ls", x->root, path) < 0 ||
        !make_parents(dst)) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    /* CREATE_ALWAYS: a later partition's file of the same path replaces an
     * earlier one's, as in the flat merge the oracle does. */
    x->out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (x->out == INVALID_HANDLE_VALUE) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    rc = fat_read_file(x->vol, cl, size, sink_write, x, x->err, sizeof x->err);
    CloseHandle(x->out);
    x->out = INVALID_HANDLE_VALUE;
    x->files++;
    return rc;
}

/* One pass over every partition. A partition that cannot be read is a
 * PROBLEM, which fails the import: the executable's hash says nothing about
 * the rest of the disk, and a directory missing every audio sample once
 * verified as the right build. */
static int each_partition(const fat_image_t *img, extract_t *x, char *why,
                          size_t whysz)
{
    fat_part_t parts[FAT_MAX_PARTS];
    int n = fat_partitions(img, parts, FAT_MAX_PARTS), i, problems = 0;

    if (x->writing)
        tell(x->ui, "partition table: %d entr%s", n, n == 1 ? "y" : "ies");
    for (i = 0; i < n; i++) {
        uint64_t before = x->files;
        int rc;
        if (parts[i].type && !fat_type_known(parts[i].type)) {
            if (x->writing)
                tell(x->ui, "  lba %10u  type 0x%02X: not a FAT type, not "
                     "read", parts[i].lba, parts[i].type);
            problems++;
            continue;
        }
        if (fat_open(img, parts[i].lba, &x->vol, x->err, sizeof x->err)) {
            if (x->writing)
                tell(x->ui, "  lba %10u  %s: not readable: %s", parts[i].lba,
                     fat_type_name(parts[i].type), x->err);
            problems++;
            continue;
        }
        rc = fat_walk(x->vol, visit_extract, x, x->err, sizeof x->err);
        if (x->writing && !rc)
            tell(x->ui, "  lba %10u  %s -> FAT%u%s%s  %llu files",
                 parts[i].lba, fat_type_name(parts[i].type),
                 fat_bits(x->vol), fat_label(x->vol)[0] ? ", label " : "",
                 fat_label(x->vol), (unsigned long long)(x->files - before));
        fat_close(x->vol);
        x->vol = NULL;
        if (rc == 2)
            return 2;
        if (rc) {
            if (x->write_failed)
                return 3;
            /* The reader's own words name the hunk and the check it failed;
             * fat.c only knows that a read did. */
            const char *cause = chd_error(img->ctx);
            why_set(why, whysz, "The CHD could not be read: %s. Nothing was "
                    "kept.", cause[0] ? cause : x->err);
            return 1;
        }
    }
    if (problems) {
        why_set(why, whysz, "%d partition%s of this disk could not be read, so "
                "the game would be incomplete. Nothing was kept.", problems,
                problems == 1 ? "" : "s");
        return 1;
    }
    return 0;
}

/* A 32-bit program without a manifest gets UAC file virtualization: a write
 * under Program Files "succeeds" into the user's VirtualStore, so the game
 * would be imported somewhere nobody will find it, half of it at that. The
 * import turns it off for this process while it runs, so such a folder is
 * refused honestly, and puts it back for the game that may follow in the
 * same process. Returns the previous setting, or -1 if it could not ask. */
static int virtualization(int on)
{
    HANDLE tok;
    DWORD was = 0, len = 0, want = (DWORD)on;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &tok))
        return -1;
    if (!GetTokenInformation(tok, TokenVirtualizationEnabled, &was,
                             sizeof was, &len)) {
        CloseHandle(tok);
        return -1;
    }
    if (was != want)
        SetTokenInformation(tok, TokenVirtualizationEnabled, &want,
                            sizeof want);
    CloseHandle(tok);
    return (int)was;
}

static void full_path(const char *in, wchar_t *out)
{
    wchar_t w[WPATH];
    MultiByteToWideChar(CP_ACP, 0, in, -1, w, WPATH);
    if (!GetFullPathNameW(w, WPATH, out, NULL))
        wcscpy(out, w);
}

/* A win32 title's folder on the NTFS volume, to x->root. */
static int visit_extract_ntfs(void *ctx, const wchar_t *path, uint64_t rec)
{
    extract_t *x = ctx;
    wchar_t dst[WPATH];
    int rc;

    if (!x->writing) {
        uint64_t size;
        if (ntfs_file_size(x->nv, rec, &size, x->err, sizeof x->err))
            return -1;
        x->total += size;
        x->files++;
        return 0;
    }
    if (cancelled(x->ui))
        return 2;
    if (_snwprintf(dst, WPATH, L"%ls\\%ls", x->root, path) < 0 ||
        !make_parents(dst)) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    /* An ordinary file: goose decrypts only what carries the cabinet's
     * hidden, read-only attributes, and the package is plaintext. */
    x->out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (x->out == INVALID_HANDLE_VALUE) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    rc = ntfs_read_file(x->nv, rec, sink_write, x, x->err, sizeof x->err);
    CloseHandle(x->out);
    x->out = INVALID_HANDLE_VALUE;
    x->files++;
    return rc;
}

/* A file of the CD, to x->root; over the base's copy when it has one. */
static int visit_extract_iso(void *ctx, const wchar_t *path,
                             const iso_node_t *n)
{
    extract_t *x = ctx;
    wchar_t dst[WPATH];
    int rc;

    if (!x->writing) {
        x->total += n->size;
        x->files++;
        return 0;
    }
    if (cancelled(x->ui))
        return 2;
    if (_snwprintf(dst, WPATH, L"%ls\\%ls", x->root, path) < 0 ||
        !make_parents(dst)) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    x->out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (x->out == INVALID_HANDLE_VALUE) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    rc = iso_read_file(x->disc, n, sink_write, x, x->err, sizeof x->err);
    CloseHandle(x->out);
    x->out = INVALID_HANDLE_VALUE;
    x->files++;
    return rc;
}

/* A file of a folder on the host, to x->root, through the same sink so the
 * bar moves and Cancel works. */
static int copy_host_file(extract_t *x, const wchar_t *src, const wchar_t *rel)
{
    static unsigned char buf[1 << 16];
    wchar_t dst[WPATH];
    HANDLE in;
    DWORD got;
    int rc = 0;

    if (_snwprintf(dst, WPATH, L"%ls\\%ls", x->root, rel) < 0 ||
        !make_parents(dst)) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        return 3;
    }
    in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) {
        narrow(src, x->err, sizeof x->err);
        return -1;
    }
    x->out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (x->out == INVALID_HANDLE_VALUE) {
        x->write_failed = 1;
        x->write_error = GetLastError();
        CloseHandle(in);
        return 3;
    }
    for (;;) {
        if (!ReadFile(in, buf, sizeof buf, &got, NULL)) {
            narrow(src, x->err, sizeof x->err);
            rc = -1;
            break;
        }
        if (!got || (rc = sink_write(x, buf, got)) != 0)
            break;
    }
    CloseHandle(x->out);
    x->out = INVALID_HANDLE_VALUE;
    CloseHandle(in);
    x->files++;
    return rc;
}

/* Every file under `dir` (rel is its path so far, "" at the top). */
static int folder_pass(extract_t *x, const wchar_t *dir, wchar_t *rel)
{
    wchar_t pat[WPATH], sub[WPATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    size_t at = wcslen(rel);
    int rc = 0;

    if (_snwprintf(pat, WPATH, L"%ls\\%ls*", dir, rel) < 0)
        return -1;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (at + wcslen(fd.cFileName) + 2 >= WPATH) {
            rc = -1;
            break;
        }
        wcscpy(rel + at, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wcscat(rel, L"\\");
            rc = folder_pass(x, dir, rel);
        } else if (!x->writing) {
            x->total += (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            x->files++;
        } else if (cancelled(x->ui)) {
            rc = 2;
        } else if (_snwprintf(sub, WPATH, L"%ls\\%ls", dir, rel) < 0) {
            rc = -1;
        } else {
            rc = copy_host_file(x, sub, rel);
        }
        rel[at] = 0;
    } while (!rc && FindNextFileW(f, &fd));
    FindClose(f);
    return rc;
}

/* Where a title's files come from: a CHD's FAT partitions or NTFS volume, a
 * folder already on the host (a win32 title's package, extracted), or an
 * update's CD laid over the base title's folders. */
typedef struct {
    const fat_image_t *img;         /* a CHD; NULL for a folder or a CD */
    uint64_t vol;                   /* its NTFS volume, for a win32 title */
    wchar_t folder[2][WPATH];       /* a folder, or the base: each dest's */
    iso_t *disc;                    /* an update's CD, over folder[] */
    chd_t *disc_chd;                /* the CD's CHD, when it is one */
} source_t;

/* An update's dest k: the base's folder, then the CD's over it, then (into
 * the game folder) the one more file the CD copies. */
static int disc_pass(const ingest_title_t *t, const source_t *src,
                     const dest_t *d, int k, extract_t *x, char *why,
                     size_t whysz)
{
    wchar_t rel[WPATH] = L"";
    iso_node_t n;
    int rc = folder_pass(x, src->folder[k], rel);

    if (rc == 2 || rc == 3)
        return rc;
    if (rc) {
        why_set(why, whysz, "%s could not be read. Nothing was kept.",
                x->err[0] ? x->err : "A file of the imported base");
        return 1;
    }
    x->disc = src->disc;
    rc = iso_lookup(src->disc, d->disk, &n, x->err, sizeof x->err);
    if (rc == 1 && n.dir)
        rc = iso_walk(src->disc, &n, visit_extract_iso, x, x->err,
                      sizeof x->err);
    else if (rc >= 0) {
        snprintf(x->err, sizeof x->err, "%s is not on the disc", d->disk);
        rc = -1;
    }
    if (!rc && k == 0 && t->update_file) {
        wchar_t to[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, t->update_to, -1, to, MAX_PATH);
        rc = iso_lookup(src->disc, t->update_file, &n, x->err, sizeof x->err);
        if (rc == 1 && !n.dir)
            rc = visit_extract_iso(x, to, &n);
        else if (rc >= 0) {
            snprintf(x->err, sizeof x->err, "%s is not on the disc",
                     t->update_file);
            rc = -1;
        }
    }
    if (x->writing && !rc)
        tell(x->ui, "  %s, over %s -> %s", d->disk, t->base, d->name);
    if (rc == 2 || rc == 3)
        return rc;
    if (rc) {
        const char *cause = src->disc_chd ? chd_error(src->disc_chd) : "";
        if (x->write_failed)
            return 3;
        why_set(why, whysz, "The disc could not be read: %s. Nothing was "
                "kept.", cause[0] ? cause : x->err);
        return 1;
    }
    return 0;
}

/* One pass, sizing or writing, of dest k. 0 done; 2 cancelled; 3 a write
 * failed; 1 a read failed (why says so). */
static int source_pass(const ingest_title_t *t, const source_t *src,
                       const dest_t *d, int k, extract_t *x, char *why,
                       size_t whysz)
{
    int rc;

    if (src->disc)
        return disc_pass(t, src, d, k, x, why, whysz);
    if (!src->img) {
        wchar_t rel[WPATH] = L"";
        rc = folder_pass(x, src->folder[k], rel);
        if (rc == 2 || rc == 3)
            return rc;
        if (rc) {
            why_set(why, whysz, "%s could not be read. Nothing was kept.",
                    x->err[0] ? x->err : "A file of the folder");
            return 1;
        }
        return 0;
    }
    if (!d->disk)
        return each_partition(src->img, x, why, whysz);
    {
        uint64_t rec;
        if (ntfs_open(src->img, src->vol, &x->nv, x->err, sizeof x->err)) {
            why_set(why, whysz, "The disk's NTFS volume could not be read: "
                    "%s. Nothing was kept.", x->err);
            return 1;
        }
        rc = ntfs_lookup(x->nv, d->disk, &rec, x->err, sizeof x->err);
        if (rc == 1)
            rc = ntfs_walk(x->nv, rec, visit_extract_ntfs, x, x->err,
                           sizeof x->err);
        else if (rc == 0) {
            snprintf(x->err, sizeof x->err, "%s is not on the disk", d->disk);
            rc = -1;
        }
        if (x->writing && !rc)
            tell(x->ui, "  %s -> %s", d->disk, d->name);
        ntfs_close(x->nv);
        x->nv = NULL;
    }
    if (rc == 2 || rc == 3)
        return rc;
    if (rc) {
        if (x->write_failed)
            return 3;
        {
            const char *cause = chd_error(src->img->ctx);
            why_set(why, whysz, "The CHD could not be read: %s. Nothing was "
                    "kept.", cause[0] ? cause : x->err);
        }
        return 1;
    }
    return 0;
}

/* Every file a title pins, in the folders as written: the executable, and a
 * win32 title's own DLLs and operator system. */
static int verify_written(const ingest_title_t *t, wchar_t part[2][WPATH],
                          char *why, size_t whysz)
{
    /* THIS TITLE'S pinned files, not the first win32 title's: `t` is whichever
     * one was imported, and the hashes have to be its own. 0 for an ETS title,
     * which takes the branch below instead. */
    const arctic_title_t *w = arctic_title_by_id(t->id);
    wchar_t exe[WPATH];
    char got[65];
    int i;

    if (!t->disk_dir) {
        if (!find_ci(part[0], t->exe, exe) || !hash_file(exe, got) ||
            _stricmp(got, t->sha256)) {
            why_set(why, whysz, "The written %s does not match the build it "
                    "was read as. Nothing was kept.", t->exe);
            return 0;
        }
        return 1;
    }
    if (!w) {
        why_set(why, whysz, "%s is not a win32 title this build knows. "
                "Nothing was kept.", t->id);
        return 0;
    }
    for (i = 0; i < ARCTIC_IMAGES + 2; i++) {
        const arctic_file_t *f = i < ARCTIC_IMAGES
                               ? &w->ids->file[i]
                               : &w->ids->diag[i - ARCTIC_IMAGES];
        int in = i == ARCTIC_IMAGES + ARCTIC_DIAG_EXE;
        if (!find_ci(part[in], f->name, exe) || !hash_file(exe, got) ||
            _stricmp(got, f->sha256)) {
            why_set(why, whysz, "The written %s is not the registered %s "
                    "build's. Nothing was kept.", f->name, t->name);
            return 0;
        }
    }
    return 1;
}

/* Copy a title from `src` into the data folder: refuse what is already there
 * unless replacing, size it, write <name>.part for each folder, verify every
 * pinned file, and only then swap them all into place. */
static ingest_result_t place(const ingest_title_t *t, const source_t *src,
                             const char *data_dir, int replace,
                             const ingest_ui_t *ui, extract_t *x, char *why,
                             size_t whysz, DWORD t0)
{
    ingest_result_t res = INGEST_FAILED;
    wchar_t data[WPATH], dest[2][WPATH], part[2][WPATH], old[2][WPATH];
    wchar_t probe[WPATH];
    char err[256];
    dest_t d[2];
    ULARGE_INTEGER avail;
    int nd = dests_of(t, d), k, rc, any = 0, moved = 0;

    full_path(data_dir, data);
    for (k = 0; k < nd; k++)
        if (_snwprintf(dest[k], WPATH, L"%ls\\%hs", data, d[k].name) < 0 ||
            _snwprintf(part[k], WPATH, L"%ls.part", dest[k]) < 0 ||
            _snwprintf(old[k], WPATH, L"%ls.old", dest[k]) < 0)
            return INGEST_FAILED;

    for (k = 0; k < nd; k++)
        any |= is_dir(dest[k]);
    if (any) {
        char ignore[8];
        int same = 1;
        for (k = 0; k < nd; k++)
            same &= is_dir(dest[k]);
        same = same && verify_written(t, dest, ignore, sizeof ignore);
        if (same && !replace) {
            why_set(why, whysz, "%s is already imported; nothing to do.",
                    t->name);
            return INGEST_ALREADY;
        }
        if (!replace) {
            narrow(dest[0], err, sizeof err);
            why_set(why, whysz, "%s already holds files that are not the "
                    "supported build of %s. Import again with Replace to "
                    "overwrite them.", err, t->name);
            return INGEST_EXISTS;
        }
    }

    /* The sizing pass reads directories only. */
    for (k = 0; k < nd; k++)
        if ((rc = source_pass(t, src, &d[k], k, x, why, whysz)) != 0)
            return rc == 2 ? INGEST_CANCELLED : INGEST_FAILED;
    if (_snwprintf(probe, WPATH, L"%ls\\", data) < 0 || !make_parents(probe)) {
        narrow(data, err, sizeof err);
        why_set(why, whysz, "VCThunder cannot create %s (Windows error %lu). "
                "Put the VCThunder folder somewhere you can write to, such as "
                "your Documents or Desktop, not under Program Files.", err,
                GetLastError());
        return INGEST_NOT_WRITABLE;
    }
    if (GetDiskFreeSpaceExW(data, &avail, NULL, NULL) &&
        avail.QuadPart < x->total + (64u << 20)) {
        why_set(why, whysz, "%s needs %.1f GB of free disk space and the drive "
                "has %.1f GB.", t->name, x->total / (double)(1u << 30),
                avail.QuadPart / (double)(1u << 30));
        return INGEST_NO_SPACE;
    }

    for (k = 0; k < nd; k++) {
        remove_tree(part[k]);
        if (!CreateDirectoryW(part[k], NULL)) {
            DWORD e = GetLastError();
            narrow(data, err, sizeof err);
            why_set(why, whysz, "VCThunder cannot write in %s (Windows error "
                    "%lu). Put the VCThunder folder somewhere you can write "
                    "to, such as your Documents or Desktop, not under Program "
                    "Files.", err, e);
            res = INGEST_NOT_WRITABLE;
            goto clean;
        }
    }
    tell(ui, "writing %llu files, %.1f GB", (unsigned long long)x->files,
         x->total / (double)(1u << 30));
    x->writing = 1;
    x->files = 0;
    x->ui = ui;
    for (k = 0; k < nd; k++) {
        wcscpy(x->root, part[k]);
        rc = source_pass(t, src, &d[k], k, x, why, whysz);
        if (rc == 2) {
            why_set(why, whysz, "Cancelled; nothing was kept.");
            res = INGEST_CANCELLED;
            goto clean;
        }
        if (rc == 3 || x->write_failed) {
            why_set(why, whysz, "Writing the game's files failed (Windows "
                    "error %lu)%s. Nothing was kept.", x->write_error,
                    x->write_error == ERROR_DISK_FULL ? ": the disk is full"
                                                      : "");
            res = x->write_error == ERROR_DISK_FULL ? INGEST_NO_SPACE
                                                    : INGEST_FAILED;
            goto clean;
        }
        if (rc)
            goto clean;
    }

    /* The backstop: what was written, against the registered hashes. */
    if (!verify_written(t, part, why, whysz))
        goto clean;
    for (k = 0; k < nd; k++)
        remove_tree(old[k]);
    for (moved = 0; moved < nd; moved++) {
        if (is_dir(dest[moved]) && !MoveFileExW(dest[moved], old[moved], 0)) {
            why_set(why, whysz, "The old game folder is in use (Windows error "
                    "%lu); close anything using it and import again.",
                    GetLastError());
            goto unwind;
        }
        if (!MoveFileExW(part[moved], dest[moved], 0)) {
            why_set(why, whysz, "The imported folder could not be put in "
                    "place (Windows error %lu).", GetLastError());
            if (is_dir(old[moved]))
                MoveFileExW(old[moved], dest[moved], 0);
            goto unwind;
        }
    }
    for (k = 0; k < nd; k++)
        remove_tree(old[k]);
    narrow(dest[0], err, sizeof err);
    why_set(why, whysz, "%s imported: %llu files, %.1f GB, in %.1f s.",
            t->name, (unsigned long long)x->files,
            x->total / (double)(1u << 30), (GetTickCount() - t0) / 1000.0);
    tell(ui, "OK: %s, the registered build (%s), is in %s", t->exe, t->build,
         err);
    return INGEST_OK;
unwind:
    /* The folders already swapped go back, so a half-placed title is never
     * left where the launcher looks. */
    while (moved-- > 0) {
        MoveFileExW(dest[moved], part[moved], 0);
        if (is_dir(old[moved]))
            MoveFileExW(old[moved], dest[moved], 0);
    }
clean:
    for (k = 0; k < nd; k++)
        remove_tree(part[k]);
    return res;
}

static ingest_result_t ingest_inner(const wchar_t *chd_path,
                                    const char *data_dir, int replace,
                                    unsigned jobs, const ingest_ui_t *ui,
                                    ingest_ident_t *ident, char *why,
                                    size_t whysz);
static ingest_result_t ingest_disc(iso_t *v, chd_t *c, const wchar_t *path,
                                   const char *data_dir, int replace,
                                   unsigned jobs, const ingest_ui_t *ui,
                                   ingest_ident_t *ident, char *why,
                                   size_t whysz, DWORD t0);

ingest_result_t ingest_chd(const wchar_t *chd_path, const char *data_dir,
                           int replace, unsigned jobs, const ingest_ui_t *ui,
                           ingest_ident_t *ident, char *why, size_t whysz)
{
    int was = virtualization(0);
    ingest_result_t r = ingest_inner(chd_path, data_dir, replace, jobs, ui,
                                     ident, why, whysz);
    if (was > 0)
        virtualization(1);
    return r;
}

static ingest_result_t ingest_inner(const wchar_t *chd_path,
                                    const char *data_dir, int replace,
                                    unsigned jobs, const ingest_ui_t *ui,
                                    ingest_ident_t *ident, char *why,
                                    size_t whysz)
{
    ingest_ident_t local;
    ingest_result_t res = INGEST_FAILED;
    char err[256], name[MAX_PATH];
    fat_image_t img;
    extract_t x;
    chd_t *c = NULL;
    DWORD t0 = GetTickCount();
    int rc;

    if (!ident)
        ident = &local;
    memset(ident, 0, sizeof *ident);
    memset(&x, 0, sizeof x);
    x.out = INVALID_HANDLE_VALUE;
    why[0] = 0;
    narrow(leaf_w(chd_path), name, sizeof name);

    rc = chd_open(chd_path, jobs, &c, err, sizeof err);
    if (rc != CHD_OK) {
        why_set(why, whysz, "%s cannot be read: %s.", name, err);
        return rc == CHD_UNSUPPORTED ? INGEST_NOT_A_GAME : INGEST_FAILED;
    }
    tell(ui, "reading %s directly (%.1f GB disk, no extraction) on %u "
         "core%s", name, chd_size(c) / (double)(1u << 30), chd_jobs(c),
         chd_jobs(c) == 1 ? "" : "s");
    img.read = img_read;
    img.plan = img_plan;
    img.ctx = c;

    {
        /* A CD CHD: an update's disc. Asked first, because a CD's sector 16
         * says so plainly and a hard disk's never does. */
        iso_t *v;
        rc = iso_open(&img, &v, err, sizeof err);
        if (rc < 0) {
            why_set(why, whysz, "%s is a CD image VCThunder cannot read: %s.",
                    name, err);
            goto out;
        }
        if (rc > 0) {
            ident->mame = mame_disk(c);
            if (ident->mame)
                tell(ui, "%s is the disc MAME lists as %s, %s", name,
                     ident->mame->set, ident->mame->name);
            res = ingest_disc(v, c, chd_path, data_dir, replace, jobs, ui,
                              ident, why, whysz, t0);
            iso_close(v);
            goto out;
        }
    }
    if (!identify(&img, ident, why, whysz))
        goto out;
    ident->mame = mame_disk(c);
    if (ident->mame)
        tell(ui, "%s is the disk MAME lists as %s, %s", name,
             ident->mame->set, ident->mame->name);
    else
        tell(ui, "%s is not a disk MAME lists; its executable decides",
             name);
    if (!ident->title && ident->mame && !ident->mame->exe_sha256[0]) {
        /* A disk MAME lists that holds nothing the import can use: say what
         * it is, and what the title is imported from instead. */
        const ingest_title_t *t, *b;
        const game_mame_disk_t *d;
        char from[320] = "";
        titles_init();
        t = title_by_id(ident->mame->title);
        b = t ? title_by_id(t->base) : NULL;
        for (d = g_mame_disks; t && b && d->set; d++)
            if (!strcmp(d->title, t->id) && !_stricmp(d->exe_sha256, t->sha256)) {
                snprintf(from, sizeof from, " %s is imported from its update "
                         "disc instead, MAME's %s (%s.chd), laid over %s.",
                         t->name, d->set, d->chd, b->name);
                break;
            }
        why_set(why, whysz, "%s is MAME's %s, %s: the cabinet's own disk, whose "
                "game files are encrypted to that cabinet's hard drive, so "
                "nothing on it can be imported.%s Nothing was written.",
                name, ident->mame->set, ident->mame->name, from);
        res = INGEST_NOT_A_GAME;
        goto out;
    }
    if (!ident->title) {
        why_set(why, whysz, "%s does not hold a game VCThunder runs: no "
                "Hydro Thunder, Offroad Thunder or Arctic Thunder executable "
                "is on it, nor Ultimate Arctic Thunder's update disc.", name);
        res = INGEST_NOT_A_GAME;
        goto out;
    }
    tell(ui, "%s holds %s; %s %s", name, ident->title->name,
         ident->title->exe, ident->sha256);
    if (!ident->supported) {
        char runs[256], what[160];
        mame_runs(ident->title, runs, sizeof runs);
        if (ident->mame)
            snprintf(what, sizeof what, "is MAME's %s, %s: a different build",
                     ident->mame->set, ident->mame->name);
        else
            snprintf(what, sizeof what, "holds a different build of %s",
                     ident->title->name);
        why_set(why, whysz, "%s %s from the one VCThunder supports (%s). "
                "Every address VCThunder patches is an offset into that exact "
                "build, so this one cannot run.%s Nothing was written.", name,
                what, ident->title->build, runs);
        res = INGEST_UNSUPPORTED;
        goto out;
    }

    {
        source_t src;
        uint64_t vols[NTFS_MAX_VOLUMES];

        memset(&src, 0, sizeof src);
        src.img = &img;
        if (ident->title->disk_dir &&
            ntfs_volumes(&img, vols, NTFS_MAX_VOLUMES) > 0)
            src.vol = vols[0];
        res = place(ident->title, &src, data_dir, replace, ui, &x, why, whysz,
                    t0);
    }
    if (res == INGEST_OK)
        tell(ui, "%llu hunks decoded (%.0f MB of the disk)",
             (unsigned long long)chd_hunks_decoded(c),
             chd_hunks_decoded(c) * chd_hunk_bytes(c) / (double)(1u << 20));
out:
    chd_close(c);
    return res;
}

/* ---- an update's CD ------------------------------------------------------------- */

/* The base title's folders under the data folder, as it imported them: 1
 * when every pinned file of its registered build is there. */
static int base_ready(const ingest_title_t *b, const char *data_dir,
                      wchar_t folder[2][WPATH])
{
    wchar_t data[WPATH];
    dest_t d[2];
    char ignore[8];
    int nd = dests_of(b, d), k;

    full_path(data_dir, data);
    for (k = 0; k < nd; k++)
        if (_snwprintf(folder[k], WPATH, L"%ls\\%hs", data, d[k].name) < 0 ||
            !is_dir(folder[k]))
            return 0;
    return verify_written(b, folder, ignore, sizeof ignore);
}

/* A CHD of the base title's registered build in `dir`, or 0. */
static int find_base_chd(const wchar_t *dir, const ingest_title_t *b,
                         wchar_t out[WPATH])
{
    wchar_t pat[WPATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int hit = 0;

    if (_snwprintf(pat, WPATH, L"%ls\\*.chd", dir) < 0)
        return 0;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        ingest_ident_t id;
        fat_image_t img;
        chd_t *c;
        char err[256];
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            _snwprintf(out, WPATH, L"%ls\\%ls", dir, fd.cFileName) < 0 ||
            chd_open(out, 1, &c, err, sizeof err) != CHD_OK)
            continue;
        img.read = img_read;
        img.plan = img_plan;
        img.ctx = c;
        hit = identify(&img, &id, err, sizeof err) && id.title == b &&
              id.supported;
        chd_close(c);
    } while (!hit && FindNextFileW(f, &fd));
    FindClose(f);
    return hit;
}

static void folder_of(const wchar_t *path, wchar_t out[WPATH])
{
    const wchar_t *leaf = leaf_w(path);
    if (leaf == path)
        _snwprintf(out, WPATH, L".");
    else
        _snwprintf(out, WPATH, L"%.*ls", (int)(leaf - path - 1), path);
    out[WPATH - 1] = 0;
}

static ingest_result_t ingest_disc(iso_t *v, chd_t *c, const wchar_t *path,
                                   const char *data_dir, int replace,
                                   unsigned jobs, const ingest_ui_t *ui,
                                   ingest_ident_t *ident, char *why,
                                   size_t whysz, DWORD t0)
{
    const game_mame_disk_t *mame = ident->mame;
    const ingest_title_t *t, *b;
    char name[MAX_PATH];
    source_t src;
    extract_t x;

    narrow(leaf_w(path), name, sizeof name);
    tell(ui, "%s is a CD image (%s)", name, iso_layout(v));
    if (!identify_disc(v, ident, why, whysz))
        return INGEST_FAILED;
    ident->mame = mame;
    if (!ident->title) {
        why_set(why, whysz, "%s is a CD, but not one VCThunder imports: "
                "Ultimate Arctic Thunder's update package is not on it.",
                name);
        return INGEST_NOT_A_GAME;
    }
    t = ident->title;
    tell(ui, "%s holds %s; %s %s", name, t->name, t->exe, ident->sha256);
    if (!ident->supported) {
        char runs[256];
        mame_runs(t, runs, sizeof runs);
        why_set(why, whysz, "%s holds a different build of %s from the one "
                "VCThunder supports (%s), so it cannot run.%s Nothing was "
                "written.", name, t->name, t->build, runs);
        return INGEST_UNSUPPORTED;
    }
    b = title_by_id(t->base);
    if (!b) {
        why_set(why, whysz, "%s is laid over a title this build does not "
                "know (%s). Nothing was written.", t->name, t->base);
        return INGEST_FAILED;
    }

    memset(&src, 0, sizeof src);
    if (!base_ready(b, data_dir, src.folder)) {
        /* The update needs what it updates. A CHD of it beside the disc, or
         * beside VCThunder.exe, is imported first, so the order the files
         * were chosen in never matters. */
        wchar_t dir[WPATH], base_chd[WPATH];
        char runs[256], bname[MAX_PATH];
        int found;

        folder_of(path, dir);
        found = find_base_chd(dir, b, base_chd);
        if (!found) {
            ingest_own_folder(dir, WPATH);
            found = find_base_chd(dir, b, base_chd);
        }
        if (!found) {
            mame_runs(b, runs, sizeof runs);
            why_set(why, whysz, "%s's disc holds only what the update changed, "
                    "and it is laid over %s (%s). Import %s first, then this "
                    "disc.%s Nothing was written.", t->name, b->name, b->build,
                    b->name, runs);
            return INGEST_NEEDS_BASE;
        }
        narrow(leaf_w(base_chd), bname, sizeof bname);
        tell(ui, "%s is laid over %s, which is not imported yet: importing it "
             "first, from %s", t->name, b->name, bname);
        {
            ingest_ident_t bid;
            ingest_result_t r = ingest_inner(base_chd, data_dir, replace, jobs,
                                             ui, &bid, why, whysz);
            if (r != INGEST_OK && r != INGEST_ALREADY)
                return r;
            tell(ui, "%s", why);
        }
        if (!base_ready(b, data_dir, src.folder)) {
            why_set(why, whysz, "%s did not verify after its import. Nothing "
                    "was written.", b->name);
            return INGEST_FAILED;
        }
    }
    tell(ui, "laid over %s, as imported", b->name);
    memset(&x, 0, sizeof x);
    x.out = INVALID_HANDLE_VALUE;
    src.disc = v;
    src.disc_chd = c;
    return place(t, &src, data_dir, replace, ui, &x, why, whysz, t0);
}

/* A CD image on the host: a cue or toc sheet, or the image itself. */
static ingest_result_t ingest_disc_file(const wchar_t *path,
                                        const char *data_dir, int replace,
                                        unsigned jobs, const ingest_ui_t *ui,
                                        ingest_ident_t *ident, char *why,
                                        size_t whysz)
{
    ingest_ident_t local;
    ingest_result_t res = INGEST_FAILED;
    fat_image_t img;
    char err[256], name[MAX_PATH];
    iso_t *v;
    int rc, was = virtualization(0);
    DWORD t0 = GetTickCount();

    if (!ident)
        ident = &local;
    memset(ident, 0, sizeof *ident);
    why[0] = 0;
    err[0] = 0;
    narrow(leaf_w(path), name, sizeof name);
    if (!iso_file_open(path, &img, err, sizeof err)) {
        why_set(why, whysz, "%s cannot be read: %s.", name,
                err[0] ? err : "not a CD image");
        goto out;
    }
    rc = iso_open(&img, &v, err, sizeof err);
    if (rc <= 0) {
        why_set(why, whysz, rc < 0 ? "%s is a CD image VCThunder cannot read: "
                "%s." : "%s is not a CD image VCThunder reads%s.", name,
                rc < 0 ? err : "");
        res = rc < 0 ? INGEST_FAILED : INGEST_NOT_A_GAME;
    } else {
        res = ingest_disc(v, NULL, path, data_dir, replace, jobs, ui, ident,
                          why, whysz, t0);
        iso_close(v);
    }
    iso_file_close(&img);
out:
    if (was > 0)
        virtualization(1);
    return res;
}

ingest_result_t ingest_file(const wchar_t *path, const char *data_dir,
                            int replace, unsigned jobs, const ingest_ui_t *ui,
                            ingest_ident_t *ident, char *why, size_t whysz)
{
    if (is_dir(path))
        return ingest_folder(path, data_dir, replace, ui, ident, why, whysz);
    if (iso_file_name(path))
        return ingest_disc_file(path, data_dir, replace, jobs, ui, ident, why,
                                whysz);
    return ingest_chd(path, data_dir, replace, jobs, ui, ident, why, whysz);
}

/* ---- a folder already on the host --------------------------------------------- */

/* `rel` ('/' between components) under `base`, with '\\' for Windows. */
static int join_rel(wchar_t *out, const wchar_t *base, const char *rel)
{
    wchar_t w[WPATH], *p;
    if (!MultiByteToWideChar(CP_UTF8, 0, rel, -1, w, WPATH))
        return 0;
    for (p = w; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    return _snwprintf(out, WPATH, L"%ls%ls%ls", base, *w ? L"\\" : L"", w) >= 0;
}

/* Where a win32 title's game folder is under `dir`: the drive root
 * (dir/<disk_dir> or dir/C/<disk_dir>), any folder on the way down to it, or
 * the game folder itself. Its operator system's folder is then where the
 * package keeps it relative to that. 1 found. */
static int find_package(const wchar_t *dir, const ingest_title_t *t,
                        wchar_t game[WPATH], wchar_t diag[WPATH])
{
    const char *p = t->disk_dir, *g, *d;
    char up[WPATH];
    wchar_t c[WPATH], exe[WPATH];
    size_t common = 0, at = 0;

    for (;;) {
        if (join_rel(c, dir, p) && find_ci(c, t->exe, exe))
            break;
        if (p == t->disk_dir && _snwprintf(c, WPATH, L"%ls\\C", dir) >= 0 &&
            join_rel(game, c, p) && find_ci(game, t->exe, exe)) {
            wcscpy(c, game);
            break;
        }
        p = strchr(p, '/');
        if (!p) {
            if (!find_ci(dir, t->exe, exe))
                return 0;
            wcscpy(c, dir);
            break;
        }
        p++;
    }
    wcscpy(game, c);
    /* diag_dir relative to disk_dir: up past what they do not share. */
    for (g = t->disk_dir, d = t->diag_dir; *g && *g == *d; g++, d++)
        if (*g == '/')
            common = (size_t)(g - t->disk_dir) + 1;
    for (g = t->disk_dir + common; *g; g++)
        if (*g == '/' || !g[1])
            at += (size_t)snprintf(up + at, sizeof up - at, "../");
    snprintf(up + at, sizeof up - at, "%s", t->diag_dir + common);
    if (!join_rel(diag, game, up))
        return 0;
    /* ".." components, resolved the way Windows opens them. */
    if (!GetFullPathNameW(diag, WPATH, c, NULL))
        return 0;
    wcscpy(diag, c);
    return 1;
}

ingest_result_t ingest_folder(const wchar_t *dir, const char *data_dir,
                              int replace, const ingest_ui_t *ui,
                              ingest_ident_t *ident, char *why, size_t whysz)
{
    ingest_ident_t local;
    ingest_result_t res = INGEST_NOT_A_GAME;
    extract_t x;
    char name[MAX_PATH];
    int was = virtualization(0), i;
    DWORD t0 = GetTickCount();

    if (!ident)
        ident = &local;
    memset(ident, 0, sizeof *ident);
    memset(&x, 0, sizeof x);
    x.out = INVALID_HANDLE_VALUE;
    why[0] = 0;
    titles_init();
    narrow(dir, name, sizeof name);
    why_set(why, whysz, "%s does not hold a game VCThunder imports from a "
            "folder: Arctic Thunder's install package is not in it.", name);
    for (i = 0; i < s_ntitles; i++) {
        const ingest_title_t *t = &s_titles[i];
        source_t src;
        wchar_t exe[WPATH];

        memset(&src, 0, sizeof src);
        if (!t->disk_dir || !find_package(dir, t, src.folder[0], src.folder[1]))
            continue;
        ident->title = t;
        if (!find_ci(src.folder[0], t->exe, exe) ||
            !hash_file(exe, ident->sha256)) {
            why_set(why, whysz, "%s could not be read.", t->exe);
            res = INGEST_FAILED;
            break;
        }
        /* Arctic Thunder and its update keep snow.exe at the same path: an
         * install of the update, already whole, is the update's. */
        ident->title = t = title_for(t, ident->sha256);
        ident->supported = !_stricmp(ident->sha256, t->sha256);
        narrow(src.folder[0], name, sizeof name);
        tell(ui, "%s holds %s; %s %s", name, t->name, t->exe, ident->sha256);
        if (!ident->supported) {
            char runs[256];
            mame_runs(t, runs, sizeof runs);
            why_set(why, whysz, "%s holds a different build of %s from the "
                    "one VCThunder supports (%s), so it cannot run.%s Nothing "
                    "was written.", name, t->name, t->build, runs);
            res = INGEST_UNSUPPORTED;
            break;
        }
        if (!is_dir(src.folder[1])) {
            narrow(src.folder[1], name, sizeof name);
            why_set(why, whysz, "%s is not there: the install package's "
                    "operator system is copied with the game. Nothing was "
                    "written.", name);
            res = INGEST_FAILED;
            break;
        }
        res = place(t, &src, data_dir, replace, ui, &x, why, whysz, t0);
        break;
    }
    if (was > 0)
        virtualization(1);
    return res;
}

/* ---- the CHDs beside the program ------------------------------------------- */

void ingest_own_folder(wchar_t *out, size_t outsz)
{
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)outsz);
    if (!n || n >= outsz) {
        _snwprintf(out, outsz, L".");
        return;
    }
    slash = wcsrchr(out, L'\\');
    if (slash)
        *slash = 0;
}

/* Which title and build a CHD or a CD image holds, read in place. 1 when it
 * could be asked at all. */
static int identify_path(const wchar_t *path, ingest_ident_t *id)
{
    fat_image_t img;
    char err[256];
    iso_t *v = NULL;
    chd_t *c = NULL;
    int ok = 0;

    memset(id, 0, sizeof *id);
    if (iso_file_name(path)) {
        if (!iso_file_open(path, &img, err, sizeof err))
            return 0;
    } else {
        if (chd_open(path, 1, &c, err, sizeof err) != CHD_OK)
            return 0;
        img.read = img_read;
        img.plan = img_plan;
        img.ctx = c;
    }
    if (iso_open(&img, &v, err, sizeof err) > 0) {
        ok = identify_disc(v, id, err, sizeof err);
        iso_close(v);
    } else if (c) {
        ok = identify(&img, id, err, sizeof err);
    }
    if (c)
        chd_close(c);
    else
        iso_file_close(&img);
    return ok;
}

int ingest_find(const wchar_t *dir, const char *data_dir,
                wchar_t (*out)[MAX_PATH], int max)
{
    /* Per title: PRESENT is never offered; OTHER_BUILD gives way to BUILD. */
    enum { PRESENT, MISSING, OTHER_BUILD, BUILD };
    static const wchar_t *const kinds[] = { L"*.chd", L"*.cue", L"*.toc",
                                            L"*.iso", L"*.bin" };
    struct { wchar_t path[MAX_PATH]; int state; } pick[MAX_TITLES];
    wchar_t pat[WPATH], path[WPATH], data[WPATH], dest[WPATH], exe[WPATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    unsigned kind;
    int i, n = 0, missing = 0;

    titles_init();
    memset(pick, 0, sizeof pick);
    full_path(data_dir, data);
    for (i = 0; i < s_ntitles; i++)
        if (_snwprintf(dest, WPATH, L"%ls\\%hs", data, s_titles[i].id) < 0 ||
            !find_ci(dest, s_titles[i].exe, exe)) {
            pick[i].state = MISSING;
            missing++;
        }
    if (!missing)
        return 0;
    for (kind = 0; kind < sizeof kinds / sizeof kinds[0]; kind++) {
        if (_snwprintf(pat, WPATH, L"%ls\\%ls", dir, kinds[kind]) < 0)
            continue;
        f = FindFirstFileW(pat, &fd);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        do {
            ingest_ident_t id;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                _snwprintf(path, WPATH, L"%ls\\%ls", dir, fd.cFileName) < 0 ||
                wcslen(path) >= MAX_PATH || !identify_path(path, &id) ||
                !id.title)
                continue;
            for (i = 0; i < s_ntitles; i++) {
                int state = id.supported ? BUILD : OTHER_BUILD;
                if (&s_titles[i] != id.title || pick[i].state == PRESENT ||
                    state <= pick[i].state)
                    continue;
                wcscpy(pick[i].path, path);
                pick[i].state = state;
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    for (i = 0; i < s_ntitles && n < max; i++)
        if (pick[i].state > MISSING)
            wcscpy(out[n++], pick[i].path);
    return n;
}

/* ---- the command line --------------------------------------------------------- */

static int ends_chd(const char *s)
{
    size_t n = strlen(s);
    return n >= 4 && !_stricmp(s + n - 4, ".chd");
}

int ingest_is_command(int argc, char **argv)
{
    return argc >= 2 && !_stricmp(argv[1], "import");
}

int ingest_dropped(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[i], -1, w, MAX_PATH);
        if (!ends_chd(argv[i]) && !iso_file_name(w))
            return 0;
    }
    return argc >= 2;
}

/* The percentage line is rewritten in place; a line of narration arriving
 * while it is showing starts below it. */
typedef struct {
    unsigned last;
    int open;
} cli_state_t;

static volatile LONG s_cli_cancel;

static void cli_say(void *ctx, const char *line)
{
    cli_state_t *st = ctx;
    if (st->open)
        printf("\n");
    st->open = 0;
    printf("%s\n", line);
    fflush(stdout);
}

static void cli_progress(void *ctx, uint64_t done, uint64_t total)
{
    cli_state_t *st = ctx;
    unsigned pct = total ? (unsigned)(done * 100 / total) : 100;
    if (pct != st->last) {
        st->last = pct;
        st->open = 1;
        printf("\r  %llu of %llu MB (%u%%)", (unsigned long long)(done >> 20),
               (unsigned long long)(total >> 20), pct);
        fflush(stdout);
    }
}

/* Ctrl+C asks the import to stop, so it can take its half-written folder with
 * it rather than leave one for the next run to find. */
static BOOL WINAPI cli_ctrl(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    InterlockedExchange(&s_cli_cancel, 1);
    return TRUE;
}

/* One file. Returns 0 imported or already there, 1 not, 2 cancelled. */
static int cli_import(const wchar_t *chd, const char *dir, int replace,
                      unsigned jobs, ingest_ui_t *ui)
{
    cli_state_t *st = ui->ctx;
    char why[1024];
    ingest_result_t r;

    st->last = 101;
    st->open = 0;
    r = ingest_file(chd, dir, replace, jobs, ui, NULL, why, sizeof why);
    cli_say(st, why);
    printf("\n");
    if (r == INGEST_CANCELLED)
        return 2;
    return r != INGEST_OK && r != INGEST_ALREADY;
}

int ingest_cli(const char *data_dir)
{
    int argc, i, rc, failures = 0, replace = 0, any = 0;
    unsigned jobs = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char dir[MAX_PATH];
    cli_state_t st;
    ingest_ui_t ui = { cli_say, cli_progress, &s_cli_cancel, &st };

    if (!argv)
        return 2;
    SetConsoleCtrlHandler(cli_ctrl, TRUE);
    snprintf(dir, sizeof dir, "%s", data_dir);
    for (i = 2; i < argc; i++) {
        if (!wcscmp(argv[i], L"--replace")) {
            replace = 1;
            continue;
        }
        if (!wcscmp(argv[i], L"--data") && i + 1 < argc) {
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, dir, sizeof dir,
                                NULL, NULL);
            continue;
        }
        if (!wcscmp(argv[i], L"--jobs") && i + 1 < argc) {
            jobs = (unsigned)_wtoi(argv[++i]);
            continue;
        }
        any = 1;
        rc = cli_import(argv[i], dir, replace, jobs, &ui);
        failures += rc != 0;
        if (rc == 2)
            break;
    }
    LocalFree(argv);
    if (!any) {
        wchar_t own[MAX_PATH], found[8][MAX_PATH];
        int n;
        ingest_own_folder(own, MAX_PATH);
        n = ingest_find(own, dir, found, 8);
        for (i = 0; i < n; i++) {
            rc = cli_import(found[i], dir, replace, jobs, &ui);
            failures += rc != 0;
            if (rc == 2)
                break;
        }
        if (n)
            return failures ? 1 : 0;
        printf("No game to import: every title is already imported, or no "
               "CHD of one is\nbeside VCThunder.exe.\n\n");
        printf("usage: VCThunder.exe import [--replace] [--data DIR] "
               "[FILE|FOLDER...]\n"
               "  Imports Hydro Thunder, Offroad Thunder or Arctic Thunder "
               "from your own\n  MAME CHD into the game-data folder, after "
               "checking it is the build\n  VCThunder supports. Ultimate "
               "Arctic Thunder is imported from its update\n  disc (MAME's "
               "040503_1309.chd, or your own .cue and .bin), laid over\n"
               "  Arctic Thunder. A FOLDER is Arctic Thunder's disk already "
               "extracted:\n  its drive, or any folder down to the game's "
               "own. With no file, the CHDs\n  and CD images beside "
               "VCThunder.exe, for each title not yet imported.\n");
        return 2;
    }
    return failures ? 1 : 0;
}
