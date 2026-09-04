/* game.c -- choose the title, then hold it still.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * One decision, made once, before anything is mapped. Everything downstream
 * reads G and never asks again: a profile that could change mid-run would mean
 * every patch site had to be re-derived, and the game's code is already resident
 * at addresses only one profile describes.
 */
#include "vcthunder.h"
#include "../generated/span.h"

#include <stdio.h>
#include <string.h>

const game_profile_t *g_game;

const game_profile_t *game_find(const char *id)
{
    unsigned i;

    if (!id || !*id)
        return NULL;
    for (i = 0; g_profiles[i]; i++)
        if (_stricmp(g_profiles[i]->id, id) == 0)
            return g_profiles[i];
    return NULL;
}

/* Locate `<dir>[\<sub>]\<name>`, writing the path that actually exists.
 *
 * Case-resolved like every other read (fs_map.c): `p->exe` is whatever spelling
 * the dump carried when the profile was generated -- "HYDRO.EXE" from an 8.3
 * entry, "offroad3.exe" from a long-filename one -- so the two titles agreeing
 * with their own dumps today is luck, not a property to rely on. */
static int locate(const char *dir, const char *sub, const char *name,
                  char *out, size_t out_sz)
{
    char rel[MAX_PATH];

    if (sub)
        snprintf(rel, sizeof rel, "%s\\%s", sub, name);
    else
        snprintf(rel, sizeof rel, "%s", name);
    if (snprintf(out, out_sz, "%s\\%s", dir, rel) >= (int)out_sz)
        return 0;
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
        return 1;
    return fs_resolve_ci(dir, rel, out, out_sz);
}

/* There is no game_detect().
 *
 * One pack carries both dumps, so detection would have exactly one answer
 * ("both are here, choose one") on every run of a correctly populated pack.
 * The title is a command-line word; see main.c's select_title(). */

int game_resolve_paths(const game_profile_t *p, const char *data_root,
                       char *data_dir, size_t data_sz, char *exe, size_t exe_sz)
{
    /* The data directory is the executable's OWN parent, not a second guess at
     * how it was spelled: locate() may have resolved the case of the title
     * subdirectory as well as of the file, and everything the game later opens
     * is composed from this string. */
    if (locate(data_root, p->id, p->exe, exe, exe_sz) ||
        locate(data_root, NULL, p->exe, exe, exe_sz)) {
        const char *slash = strrchr(exe, '\\');
        size_t n = slash ? (size_t)(slash - exe) : strlen(exe);

        if (n >= data_sz)
            n = data_sz - 1;
        memcpy(data_dir, exe, n);
        data_dir[n] = 0;
        return 1;
    }
    LOGE("game: %s not found in %s\\%s\\ or %s\\", p->exe, data_root, p->id, data_root);
    return 0;
}

/* Table sizes against this build's ceilings.
 *
 * The generator already refuses to emit a profile that overruns one, so this can
 * only fire when a generated file and this binary were built from different
 * revisions of game.h, which is exactly the case where the failure would
 * otherwise be a silent overwrite of whatever follows the array. */
static int profile_check(const game_profile_t *p)
{
    const struct { const char *what; unsigned n, cap; } lim[] = {
        { "glide", p->glide_count, VCT_MAX_GLIDE },
        { "__imp__", p->imp_count, VCT_MAX_IMP },
        { "direct win32", p->w32_count, VCT_MAX_W32 },
        { "mesh loop", p->loop_count, VCT_MAX_MESH_LOOPS },
    };
    unsigned i;
    int ok = 1;

    for (i = 0; i < sizeof lim / sizeof lim[0]; i++)
        if (lim[i].n > lim[i].cap) {
            LOGE("game: %s's %s table has %u entries, this build's ceiling is %u"
                 ": rebuild (generated/ is stale)",
                 p->id, lim[i].what, lim[i].n, lim[i].cap);
            ok = 0;
        }

    /* The stub reserved VCT_SPAN_END at link time. A title needing more than
     * that would run with part of its .bss unreserved, and the symptom would
     * be a fault at an address nothing in the log ever mentions. */
    if (p->span_end > VCT_SPAN_END) {
        LOGE("game: %s spans to 0x%08X but this build reserved only 0x%08X"
             ": rebuild VCThunder.exe (generated/span.h is stale)",
             p->id, p->span_end, (unsigned)VCT_SPAN_END);
        ok = 0;
    }
    if (p->image_base != VCT_IMAGE_BASE) {
        LOGE("game: %s is based at 0x%08X, the stub reserved 0x%08X",
             p->id, p->image_base, (unsigned)VCT_IMAGE_BASE);
        ok = 0;
    }
    return ok;
}

int game_select(const game_profile_t *p)
{
    if (!p)
        return 0;
    if (!profile_check(p))
        return 0;

    g_game = p;
    LOGI("game: %s (%s), image 0x%08X..0x%08X, %u KB",
         p->name, p->id, p->image_base, p->span_end,
         (p->span_end - p->image_base) / 1024);
    LOGI("      exe %s   io board %s   sha256 %.16s...",
         p->exe, p->io_board, p->sha256);
    LOGI("      glide %u  __imp__ %u  direct win32 %u  hardware %u",
         p->glide_count, p->imp_count, p->w32_count, p->hw_count);
    return 1;
}

/* Say what a subsystem cannot do and why, once, in the terms the profile uses.
 *
 * Every installer that patches a title-specific address needs this, and they all
 * need to say the same thing: the anchor is absent, so nothing was patched, and
 * the game keeps its own code. The alternative (each subsystem inventing its
 * own wording, or worse, patching address 0) is how a port ends up reporting
 * success while running a function that was never there. */
int game_require(const char *subsystem, const char *what, uint32_t va)
{
    if (va)
        return 1;
    LOGW("%s: not installed; %s has no %s; the game keeps its own code",
         subsystem, G->id, what);
    return 0;
}
