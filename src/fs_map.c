/* fs_map.c -- map arcade drive paths into portable data and save directories.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * THE DUMP IS READ-ONLY, and this file is the only thing that makes that true.
 * The rule is the ACCESS MODE, never a list of names, because a list is a thing
 * that goes stale and this one has to hold for both titles: whatever the game
 * opens for writing goes to save/, whatever it opens for reading comes from
 * save/ if it has written one and from data/ otherwise. A file the game means
 * to update in place is SEEDED into save/ from the dump on first write, so the
 * user's accumulated settings survive. Between them the two titles write eight
 * such files.
 *
 * IT ALSO OWNS CASE, and by the same rule. The dump's names are FAT 8.3 upper
 * case (`HT.R2`, `AUDT1.NVR`); both games ask in lower or mixed case (`ht.r2`,
 * `d:\audt1.nvr`, `d:\HiSc1.nvr`). Those are one name on a case-insensitive
 * filesystem and two on a case-sensitive one, which a data directory reached
 * through `\\wsl.localhost\` is. So an open that misses is retried against what
 * the directory ACTUALLY HOLDS -- never against a table of the names the titles
 * ask for in the wrong case, which would be an invariant about the table, and
 * neither title's set of filenames is closed. The exact hit comes first, so
 * none of this runs on an ordinary drive letter.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

static char s_data[MAX_PATH];
static char s_save[MAX_PATH];

/* One line per distinct path that takes the write route, so a run says which
 * files it considers the game's rather than the dump's. Small and bounded:
 * both titles together have eight. */
#define FS_SEEN_MAX 32
static char s_seen[FS_SEEN_MAX][MAX_PATH];
static int s_seen_count;

static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* The one name in `dir` that equals `want` but for case. Returns 0 when the
 * directory holds no such entry, which is the answer whenever a path is simply
 * absent -- so a miss still reaches CreateFileA as the path the game asked for
 * and the log still names it. */
static int match_ci(const char *dir, const char *want, char *got, size_t got_sz)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int hits = 0;

    if (snprintf(pattern, sizeof pattern, "%s\\*", dir) >= (int)sizeof pattern)
        return 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (_stricmp(fd.cFileName, want) != 0)
            continue;
        if (++hits == 1) {
            snprintf(got, got_sz, "%s", fd.cFileName);
            continue;
        }
        /* Only a case-sensitive filesystem can hold two of these, and the
         * game's own spelling is not among them or the exact open would have
         * succeeded. A FAT dump cannot produce it, so the directory was
         * assembled from somewhere else and the user should hear about it. */
        LOGW("fs: '%s' and '%s' in '%s' differ only in case and the game asked "
             "for '%s'; using '%s'. A dump read off FAT cannot contain both, so "
             "this directory was merged from more than one source.",
             got, fd.cFileName, dir, want, got);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return hits > 0;
}

/* Resolve `rel` under `base` in the case the filesystem actually uses, one
 * component at a time so a subdirectory in the game's path is covered too.
 * `out` is written only on success, so a caller keeps the naive path -- and
 * therefore its own error message -- when this cannot help.
 *
 * Exported because game.c and launcher.c locate the executable before
 * fs_map_init() has run, and they have the same problem. */
int fs_resolve_ci(const char *base, const char *rel, char *out, size_t out_sz)
{
    char cur[MAX_PATH], next[MAX_PATH], comp[MAX_PATH], got[MAX_PATH];
    const char *p = rel;

    if (!base || !rel || snprintf(cur, sizeof cur, "%s", base) >= (int)sizeof cur)
        return 0;

    while (*p) {
        size_t n = 0;

        while (*p == '\\' || *p == '/')
            p++;
        while (p[n] && p[n] != '\\' && p[n] != '/')
            n++;
        if (!n)
            break;
        if (n >= sizeof comp)
            return 0;
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;

        /* Exact first, at every level: only the components that actually
         * disagree cost a directory listing. */
        if (snprintf(next, sizeof next, "%s\\%s", cur, comp) >= (int)sizeof next)
            return 0;
        if (!exists(next)) {
            if (!match_ci(cur, comp, got, sizeof got))
                return 0;
            if (snprintf(next, sizeof next, "%s\\%s", cur, got) >= (int)sizeof next)
                return 0;
        }
        snprintf(cur, sizeof cur, "%s", next);
    }
    if (p == rel)                          /* nothing to resolve */
        return 0;
    snprintf(out, out_sz, "%s", cur);
    return 1;
}

/* Ask the data directory whether it is case-sensitive, once, at startup, by
 * flipping the case of a name it actually holds.
 *
 * Ask rather than infer from the path: a `\\wsl.localhost\` share is ext4, a
 * drive letter usually is not, and an NTFS directory carrying the per-directory
 * case-sensitivity flag is. The run works either way -- this is here so that a
 * log from such a directory says so without anyone having to ask. */
static void probe_case(const char *dir)
{
    char pattern[MAX_PATH], probe[MAX_PATH], entry[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int flipped = 0;

    if (snprintf(pattern, sizeof pattern, "%s\\*", dir) >= (int)sizeof pattern)
        return;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LOGW("fs: data directory '%s' cannot be listed (%lu). Nothing has been "
             "read from it yet, so this is the first thing to check if the game "
             "then reports missing data.", dir, GetLastError());
        return;
    }
    do {
        char *q;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        snprintf(entry, sizeof entry, "%s", fd.cFileName);
        if (snprintf(probe, sizeof probe, "%s\\%s", dir, entry) >= (int)sizeof probe)
            continue;
        for (q = probe + strlen(dir) + 1; *q; q++) {
            if (islower((unsigned char)*q)) {
                *q = (char)toupper((unsigned char)*q);
                flipped = 1;
            } else if (isupper((unsigned char)*q)) {
                *q = (char)tolower((unsigned char)*q);
                flipped = 1;
            }
        }
    } while (!flipped && FindNextFileA(h, &fd));
    FindClose(h);

    if (!flipped || exists(probe))         /* no letters to flip, or the
                                            * ordinary case-insensitive host */
        return;
    LOGI("fs: '%s' is CASE-SENSITIVE ('%s' and its case-flipped spelling are "
         "different names here). The dump's names are FAT 8.3 upper case and "
         "both games ask in lower case, so every open is resolved against the "
         "directory's real spelling. This is what a data directory under "
         "\\\\wsl.localhost\\ does; one under a drive letter takes the exact "
         "hit instead.", dir, entry);
}

void fs_map_init(const char *data_dir, const char *save_dir)
{
    snprintf(s_data, sizeof s_data, "%s", data_dir);
    snprintf(s_save, sizeof s_save, "%s", save_dir);
    LOGI("fs: data='%s' (read-only) save='%s' (every write)", s_data, s_save);
    probe_case(s_data);
}

static int announced(const char *path)
{
    int i;

    for (i = 0; i < s_seen_count; i++)
        if (_stricmp(s_seen[i], path) == 0)
            return 1;
    if (s_seen_count < FS_SEEN_MAX)
        snprintf(s_seen[s_seen_count++], MAX_PATH, "%s", path);
    return 0;
}

/* The dump's copy of a file the game is about to update in place. Copying it
 * once is what keeps an existing pack's operator settings, audits and high
 * scores across this change: without it the first write would start from an
 * empty save directory and the game would rebuild its defaults over them.
 *
 * A size ceiling, because the rule is the access mode and not a name: nothing
 * the game writes is remotely this big, so a file that trips it is a question
 * rather than a copy. Both titles' largest write target is 10 KB. */
#define FS_SEED_MAX (4u * 1024u * 1024u)

static void seed_from_dump(const char *save_path, const char *data_path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;

    if (exists(save_path) || !exists(data_path))
        return;
    if (GetFileAttributesExA(data_path, GetFileExInfoStandard, &info) &&
        (info.nFileSizeHigh || info.nFileSizeLow > FS_SEED_MAX)) {
        LOGW("fs: '%s' is opened for WRITING and is %lu KB; not seeding a copy "
             "into the save directory. Nothing either title writes is that "
             "big, so find out what asked for write access before trusting "
             "this run.", data_path, (unsigned long)(info.nFileSizeLow / 1024u));
        return;
    }
    if (CopyFileA(data_path, save_path, TRUE))
        LOGI("fs: seeded '%s' from the dump's own copy; the dump is not "
             "written to again", save_path);
    else
        LOGW("fs: could not seed '%s' from '%s' (%lu); the game starts this "
             "file from scratch", save_path, data_path, GetLastError());
}

/* Returns `out` on success, or `in` unchanged when no mapping applies. */
const char *fs_map_path_mode(const char *in, char *out, size_t out_sz,
                             fs_access_t mode)
{
    char other[MAX_PATH];
    const char *leaf = in;

    if (!in || !*in)
        return in;

    /* Strip a leading drive specifier of any letter; the arcade only ever used
     * C: and D:, but a stray A: (the dev-only a:\aud_%i_%02i.txt) should land
     * somewhere harmless rather than at the host's floppy driver. */
    if (in[0] && in[1] == ':')
        leaf = in + 2;
    while (*leaf == '\\' || *leaf == '/')
        leaf++;

    if (snprintf(out, out_sz, "%s\\%s", s_save, leaf) >= (int)out_sz ||
        snprintf(other, sizeof other, "%s\\%s", s_data, leaf) >= (int)sizeof other) {
        LOGW("fs: path too long, passing through: '%s'", in);
        return in;
    }

    /* Normalise separators so the log is readable and Windows is happy. */
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
    for (char *p = other; *p; p++)
        if (*p == '/')
            *p = '\\';

    /* The dump's path in the spelling the dump uses, which on a case-sensitive
     * filesystem is not the spelling the game asked for. Both the read route
     * and the seed below want this one, and neither can find its file without
     * it: `d:\audt1.nvr` is `AUDT1.NVR` on the disk. */
    if (!exists(other))
        fs_resolve_ci(s_data, leaf, other, sizeof other);

    switch (mode) {
    case FS_WRITE:
        /* The WRITE target itself is never case-resolved: save/ is ours, the
         * game's own spelling is the right one to create, and the file usually
         * does not exist yet, so there would be nothing to resolve against. */
        seed_from_dump(out, other);
        if (!announced(out))
            LOGI("fs: '%s' is the game's, not the dump's: writes go to '%s'",
                 leaf, out);
        break;
    case FS_DELETE:
        /* Never delete out of the dump. A delete of something the game has not
         * written is a no-op against a path that does not exist, which is the
         * answer the game gets on a cabinet with nothing to delete. */
        break;
    case FS_READ:
    default:
        /* The game's own copy wins once it has written one; otherwise the
         * cabinet's. `htlowres.bin` has no copy in the dump at all, so a
         * first-run Hydro correctly reads a name in save/ that is not there
         * yet, exactly as it did before this file changed. save/ gets the same
         * case fallback as the dump: a pack carried from a case-insensitive
         * host to a case-sensitive one has the game's own files in whatever
         * spelling that host recorded. */
        if (exists(out) || fs_resolve_ci(s_save, leaf, out, out_sz))
            break;
        snprintf(out, out_sz, "%s", other);
        break;
    }

    LOGT("fs: '%s' -> '%s' (%s)", in, out,
         mode == FS_WRITE ? "write" : mode == FS_DELETE ? "delete" : "read");
    return out;
}

const char *fs_map_path(const char *in, char *out, size_t out_sz)
{
    return fs_map_path_mode(in, out, out_sz, FS_READ);
}
