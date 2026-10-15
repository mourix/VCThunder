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
 *      left where the launcher looks. */
#include "ingest.h"
#include "chd.h"
#include "fat.h"
#include "hash.h"

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

/* ---- identifying a disk ----------------------------------------------------- */

typedef struct {
    fat_t *vol;
    const game_profile_t *hit;
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
    for (i = 0; g_profiles[i]; i++) {
        if (_stricmp(leaf, g_profiles[i]->exe))
            continue;
        if (!sha256_begin(&s) ||
            fat_read_file(w->vol, cl, size, sink_hash, &s, err, sizeof err)) {
            sha256_end(&s, w->sha);
            w->failed = 1;
            return 1;
        }
        sha256_end(&s, w->sha);
        w->hit = g_profiles[i];
        return 1;                       /* the first executable found decides */
    }
    return 0;
}

/* The title and the build, from the executable read where it lies on the
 * disk: a second, nothing written. */
static int identify(const fat_image_t *img, ingest_ident_t *id, char *why,
                    size_t whysz)
{
    fat_part_t parts[FAT_MAX_PARTS];
    int n = fat_partitions(img, parts, FAT_MAX_PARTS), i;

    memset(id, 0, sizeof *id);
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
static void mame_runs(const game_profile_t *t, char *out, size_t outsz)
{
    const game_mame_disk_t *d;
    size_t at = 0;
    int n = 0, total = 0;

    for (d = g_mame_disks; d->set; d++)
        total += d->title == t && !_stricmp(d->exe_sha256, t->sha256);
    out[0] = 0;
    for (d = g_mame_disks; d->set && at < outsz; d++) {
        if (d->title != t || _stricmp(d->exe_sha256, t->sha256))
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

static ingest_result_t ingest_inner(const wchar_t *chd_path,
                                    const char *data_dir, int replace,
                                    unsigned jobs, const ingest_ui_t *ui,
                                    ingest_ident_t *ident, char *why,
                                    size_t whysz);

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
    wchar_t data[WPATH], dest[WPATH], part[WPATH], old[WPATH], exe[WPATH];
    char err[256], got[65], name[MAX_PATH];
    fat_image_t img;
    extract_t x;
    chd_t *c = NULL;
    ULARGE_INTEGER avail;
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

    if (!identify(&img, ident, why, whysz))
        goto out;
    ident->mame = mame_disk(c);
    if (ident->mame)
        tell(ui, "%s is the disk MAME lists as %s, %s", name,
             ident->mame->set, ident->mame->name);
    else
        tell(ui, "%s is not a disk MAME lists; its executable decides",
             name);
    if (!ident->title) {
        why_set(why, whysz, "%s does not hold a game VCThunder runs: neither "
                "Hydro Thunder's nor Offroad Thunder's executable is on it.",
                name);
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

    full_path(data_dir, data);
    if (_snwprintf(dest, WPATH, L"%ls\\%hs", data, ident->title->id) < 0 ||
        _snwprintf(part, WPATH, L"%ls.part", dest) < 0 ||
        _snwprintf(old, WPATH, L"%ls.old", dest) < 0)
        goto out;

    if (is_dir(dest)) {
        int same = find_ci(dest, ident->title->exe, exe) &&
                   hash_file(exe, got) && !_stricmp(got, ident->title->sha256);
        if (same && !replace) {
            why_set(why, whysz, "%s is already imported; nothing to do.",
                    ident->title->name);
            res = INGEST_ALREADY;
            goto out;
        }
        if (!replace) {
            narrow(dest, err, sizeof err);
            why_set(why, whysz, "%s already holds files that are not the "
                    "supported build of %s. Import again with Replace to "
                    "overwrite them.", err, ident->title->name);
            res = INGEST_EXISTS;
            goto out;
        }
    }

    /* The sizing pass reads directories only. */
    rc = each_partition(&img, &x, why, whysz);
    if (rc)
        goto out;
    if (_snwprintf(exe, WPATH, L"%ls\\", data) < 0 || !make_parents(exe)) {
        narrow(data, err, sizeof err);
        why_set(why, whysz, "VCThunder cannot create %s (Windows error %lu). "
                "Put the VCThunder folder somewhere you can write to, such as "
                "your Documents or Desktop, not under Program Files.", err,
                GetLastError());
        res = INGEST_NOT_WRITABLE;
        goto out;
    }
    if (GetDiskFreeSpaceExW(data, &avail, NULL, NULL) &&
        avail.QuadPart < x.total + (64u << 20)) {
        why_set(why, whysz, "%s needs %.1f GB of free disk space and the drive "
                "has %.1f GB.", ident->title->name,
                x.total / (double)(1u << 30),
                avail.QuadPart / (double)(1u << 30));
        res = INGEST_NO_SPACE;
        goto out;
    }

    remove_tree(part);
    if (!CreateDirectoryW(part, NULL)) {
        DWORD e = GetLastError();
        narrow(data, err, sizeof err);
        why_set(why, whysz, "VCThunder cannot write in %s (Windows error %lu). "
                "Put the VCThunder folder somewhere you can write to, such as "
                "your Documents or Desktop, not under Program Files.", err, e);
        res = INGEST_NOT_WRITABLE;
        goto out;
    }
    tell(ui, "writing %llu files, %.1f GB", (unsigned long long)x.files,
         x.total / (double)(1u << 30));
    wcscpy(x.root, part);
    x.writing = 1;
    x.files = 0;
    x.ui = ui;
    rc = each_partition(&img, &x, why, whysz);
    if (rc == 2) {
        why_set(why, whysz, "Cancelled; nothing was kept.");
        res = INGEST_CANCELLED;
        goto clean;
    }
    if (rc == 3 || x.write_failed) {
        why_set(why, whysz, "Writing the game's files failed (Windows error "
                "%lu)%s. Nothing was kept.", x.write_error,
                x.write_error == ERROR_DISK_FULL ? ": the disk is full" : "");
        res = x.write_error == ERROR_DISK_FULL ? INGEST_NO_SPACE
                                                : INGEST_FAILED;
        goto clean;
    }
    if (rc)
        goto clean;

    /* The backstop: the executable as written, against the registered hash. */
    if (!find_ci(part, ident->title->exe, exe) || !hash_file(exe, got) ||
        _stricmp(got, ident->title->sha256)) {
        why_set(why, whysz, "The written %s does not match the build it was "
                "read as. Nothing was kept.", ident->title->exe);
        goto clean;
    }
    remove_tree(old);
    if (is_dir(dest) && !MoveFileExW(dest, old, 0)) {
        why_set(why, whysz, "The old game folder is in use (Windows error "
                "%lu); close anything using it and import again.",
                GetLastError());
        goto clean;
    }
    if (!MoveFileExW(part, dest, 0)) {
        why_set(why, whysz, "The imported folder could not be put in place "
                "(Windows error %lu).", GetLastError());
        if (is_dir(old))
            MoveFileExW(old, dest, 0);
        goto clean;
    }
    remove_tree(old);
    narrow(dest, err, sizeof err);
    why_set(why, whysz, "%s imported: %llu files, %.1f GB, in %.1f s.",
            ident->title->name, (unsigned long long)x.files,
            x.total / (double)(1u << 30), (GetTickCount() - t0) / 1000.0);
    tell(ui, "OK: %s, the registered build (%s), is in %s",
         ident->title->exe, ident->title->build, err);
    tell(ui, "%llu hunks decoded (%.0f MB of the disk)",
         (unsigned long long)chd_hunks_decoded(c),
         chd_hunks_decoded(c) * chd_hunk_bytes(c) / (double)(1u << 20));
    res = INGEST_OK;
    goto out;
clean:
    remove_tree(part);
out:
    chd_close(c);
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

int ingest_find(const wchar_t *dir, const char *data_dir,
                wchar_t (*out)[MAX_PATH], int max)
{
    /* Per title: PRESENT is never offered; OTHER_BUILD gives way to BUILD. */
    enum { PRESENT, MISSING, OTHER_BUILD, BUILD };
    struct { wchar_t path[MAX_PATH]; int state; } pick[8];
    wchar_t pat[WPATH], path[WPATH], data[WPATH], dest[WPATH], exe[WPATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int i, n = 0, missing = 0;

    memset(pick, 0, sizeof pick);
    full_path(data_dir, data);
    for (i = 0; g_profiles[i] && i < 8; i++)
        if (_snwprintf(dest, WPATH, L"%ls\\%hs", data, g_profiles[i]->id) < 0 ||
            !find_ci(dest, g_profiles[i]->exe, exe)) {
            pick[i].state = MISSING;
            missing++;
        }
    if (!missing || _snwprintf(pat, WPATH, L"%ls\\*.chd", dir) < 0)
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
            _snwprintf(path, WPATH, L"%ls\\%ls", dir, fd.cFileName) < 0 ||
            wcslen(path) >= MAX_PATH ||
            chd_open(path, 1, &c, err, sizeof err) != CHD_OK)
            continue;
        img.read = img_read;
        img.plan = img_plan;
        img.ctx = c;
        if (identify(&img, &id, err, sizeof err) && id.title) {
            for (i = 0; g_profiles[i] && i < 8; i++) {
                int state = id.supported ? BUILD : OTHER_BUILD;
                if (g_profiles[i] != id.title || pick[i].state == PRESENT ||
                    state <= pick[i].state)
                    continue;
                wcscpy(pick[i].path, path);
                pick[i].state = state;
            }
        }
        chd_close(c);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    for (i = 0; g_profiles[i] && i < 8 && n < max; i++)
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
    for (i = 1; i < argc; i++)
        if (!ends_chd(argv[i]))
            return 0;
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
    r = ingest_chd(chd, dir, replace, jobs, ui, NULL, why, sizeof why);
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
               "[FILE.chd...]\n"
               "  Imports Hydro Thunder or Offroad Thunder from your own MAME "
               "CHD into\n  the game-data folder, after checking it is the "
               "build VCThunder supports.\n  With no file, the CHDs beside "
               "VCThunder.exe, for each title not yet imported.\n");
        return 2;
    }
    return failures ? 1 : 0;
}
