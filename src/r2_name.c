/* r2_name.c -- observe the asset names the game resolves out of its R2 archive.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Both of the game's directory lookups are detoured, and neither detour
 * answers one: it records the name and forwards to the game's own scan.
 */
#include "vcthunder.h"

#include <string.h>

#define R2_NAME_SIZE  12u

typedef void *(__cdecl *find_name_fn)(const char *);
typedef int   (__cdecl *name_to_doid_fn)(const char *);

static find_name_fn s_real_find_name;
static name_to_doid_fn s_real_name_to_doid;

/* The last asset name the game asked for. Nothing else in the process knows
 * what a texture is CALLED (grTexDownloadMipMap carries an address and a
 * format and no identity at all), but the game resolves an asset by name
 * shortly before it uploads it, so the most recent lookup is a usable label.
 * It is a correlation, not a proof, and texstate.c prints it as such. */
static char s_last_name[R2_NAME_SIZE];

/* CONSUMED, not sticky. The game resolves a name once and then uploads a whole
 * bank of textures, so a name left in place labels sixty unrelated uploads:
 * 'M_XARROWC12' against addresses spread across all of texture memory. A label
 * that is wrong most of the time is worse than no label. Exactly one upload can
 * claim each lookup, and everything else is honestly blank. */
const char *r2_last_lookup(void)
{
    static char out[R2_NAME_SIZE];

    memcpy(out, s_last_name, R2_NAME_SIZE);
    s_last_name[0] = '\0';
    return out;
}

/* Targeted probe for the white arrow-ramp bases: does the game ever resolve the
 * base texture at all? The arrows (M_XARROWC12/13) are visibly rendering; the
 * base (M_XARROWC11) is not, and "never asked for" and "asked for but drawn
 * wrong" need completely different fixes. Each distinct name is logged once. */
static void note_arrow_lookup(const char *normalized)
{
    static char seen[8][R2_NAME_SIZE];
    static unsigned n;
    unsigned i;

    if (_strnicmp(normalized, "M_XARROW", 8) != 0)
        return;
    for (i = 0; i < n; i++)
        if (strcmp(seen[i], normalized) == 0)
            return;
    if (n < 8u) {
        memcpy(seen[n++], normalized, R2_NAME_SIZE);
        LOGI("r2: game resolved arrow-ramp asset '%s'", normalized);
    }
}

/* Uppercase and truncate names to eleven characters, then remove archive
 * padding, so a recorded label is spelled the way the directory spells it:
 * entries are 6-bit packed and unpack to exactly eleven characters, so
 * anything shorter arrives space-padded. */
static void normalized_name(const char *name, char out[R2_NAME_SIZE])
{
    unsigned i, len = 0;

    memset(out, 0, R2_NAME_SIZE);   /* the whole buffer is copied by value */
    for (i = 0; i + 1u < R2_NAME_SIZE && name && name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        out[i] = (char)c;
        if (c != ' ')
            len = i + 1u;
    }
    out[len] = '\0';
}

static void note_lookup(const char *name)
{
    char normalized[R2_NAME_SIZE];

    normalized_name(name, normalized);
    memcpy(s_last_name, normalized, R2_NAME_SIZE);
    note_arrow_lookup(normalized);
}

/* Record, then forward. These detours deliberately hold no answers of their
 * own. The game's scan reads all eleven name bytes of every entry it returns,
 * so it can only ever return 0 or a pointer it has just proved is there; a
 * remembered answer is a pointer nothing revalidates, and one of those faults
 * in _r2file_ExpandDirEntry. */
static void *observed_find_name(const char *name)
{
    note_lookup(name);
    return s_real_find_name(name);
}

static int observed_name_to_doid(const char *name)
{
    note_lookup(name);
    return s_real_name_to_doid(name);
}

int r2_name_install(void)
{
    void *find_trampoline, *doid_trampoline;
    int ready = 1;

    /* The prologue lengths come from the profile and are not assumed here: the
     * two functions do not have the same shape in the two titles. Hydro's are
     * both `mov eax,[absolute]`; Offroad's _r2file_NameToDoid is a wrapper
     * that opens `mov eax,[esp+4]; push eax` and then CALLS
     * _r2file_FindObjectNameInDir, so on that title one NameToDoid enters both
     * detours and records the same name twice. That is harmless while these
     * detours only observe; anything that later gives them state has to
     * survive the re-entry. See hook_call_through(). */
    ready &= game_require("r2 names", "_r2file_FindObjectNameInDir",
                          G->r2_find_name);
    ready &= game_require("r2 names", "_r2file_NameToDoid",
                          G->r2_name_to_doid);
    ready &= game_require("r2 names",
                          "relocatable _r2file_FindObjectNameInDir prologue",
                          G->hook_r2_find_name);
    ready &= game_require("r2 names", "relocatable _r2file_NameToDoid prologue",
                          G->hook_r2_name_to_doid);
    if (!ready) {
        LOGI("r2 names: not installed; the game resolves assets exactly as it "
             "always did and texture uploads carry no name");
        return 1;
    }
    find_trampoline = hook_prepare_call_through(G->r2_find_name,
                                                G->hook_r2_find_name);
    doid_trampoline = hook_prepare_call_through(G->r2_name_to_doid,
                                                G->hook_r2_name_to_doid);
    if (!find_trampoline || !doid_trampoline) {
        LOGE("r2 names: failed to install directory lookup detours");
        return 0;
    }
    s_real_find_name = (find_name_fn)find_trampoline;
    s_real_name_to_doid = (name_to_doid_fn)doid_trampoline;
    patch_jmp(G->r2_find_name, (void *)observed_find_name);
    patch_jmp(G->r2_name_to_doid, (void *)observed_name_to_doid);
    LOGI("r2 names: both archive lookups observed; the game's own directory "
         "scan answers every one of them");
    return 1;
}
