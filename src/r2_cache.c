/* r2_cache.c -- cache successful Hydro R2 directory lookups.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Names are normalised from the archive's fixed-width representation. Cache
 * misses always fall through to the game's authoritative scan.
 */
#include "vcthunder.h"

#include <string.h>

#define R2_CACHE_SIZE 16384u
#define R2_NAME_SIZE  12u

typedef void *(__cdecl *find_name_fn)(const char *);
typedef int   (__cdecl *name_to_doid_fn)(const char *);

typedef struct {
    char name[R2_NAME_SIZE];
    void *object;
    int doid;
    unsigned have_object : 1;
    unsigned have_doid : 1;
} r2_cache_entry_t;

static find_name_fn s_real_find_name;
static name_to_doid_fn s_real_name_to_doid;
static r2_cache_entry_t s_cache[R2_CACHE_SIZE];
static volatile LONG s_hits, s_misses;
static int s_index_ready;
static int s_index_refused;      /* built and found unsound, or unbuildable */

/* Verification calls the game's own lookup, and the game's own lookup may call
 * the other hooked function internally, which would re-enter index building,
 * or answer the verification from the very index being verified. While this is
 * set, both hooks are pass-throughs and building is refused. */
static int s_in_index_work;

/* The last asset name the game asked for. Nothing else in the process knows
 * what a texture is CALLED (grTexDownloadMipMap carries an address and a
 * format and no identity at all), but the game resolves an asset by name
 * shortly before it uploads it, so the most recent lookup is a usable label.
 * It is a correlation, not a proof, and texstate.c prints it as such. */
static char s_last_name[R2_NAME_SIZE];

/* CONSUMED, not sticky. The first version left the name in place, and because
 * the game resolves a name once and then uploads a whole bank of textures, one
 * lookup ended up labelling sixty unrelated uploads: 'M_XARROWC12' appeared
 * against addresses spread across all of texture memory. A label that is wrong
 * most of the time is worse than no label. Now exactly one upload can claim
 * each lookup, and everything else is honestly blank. */
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
 * padding so lookup and directory spellings share one key. */
static unsigned normalized_name(const char *name, char out[R2_NAME_SIZE])
{
    unsigned hash = 2166136261u, i, len = 0;

    for (i = 0; i + 1u < R2_NAME_SIZE && name && name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        out[i] = (char)c;
        if (c != ' ')
            len = i + 1u;
    }
    out[len] = '\0';

    for (i = 0; i < len; i++)
        hash = (hash ^ (unsigned char)out[i]) * 16777619u;
    return hash;
}

static r2_cache_entry_t *find_slot(const char normalized[R2_NAME_SIZE],
                                   unsigned hash)
{
    unsigned probe;

    for (probe = 0; probe < R2_CACHE_SIZE; probe++) {
        r2_cache_entry_t *entry = &s_cache[(hash + probe) & (R2_CACHE_SIZE - 1u)];
        if (!entry->name[0])
            return entry;
        if (strcmp(entry->name, normalized) == 0)
            return entry;
    }
    return NULL;
}

static void unpack_name(const unsigned char *packed, char out[R2_NAME_SIZE])
{
    unsigned j;

    for (j = 0; j + 1u < R2_NAME_SIZE; j++)
        out[j] = (char)((packed[j] & 0x3fu) + 0x20u);
    out[R2_NAME_SIZE - 1u] = '\0';
}

/* The index rests on two inferences about the game's own functions that were
 * never checked: that _r2file_FindName returns a pointer to the 20-byte
 * directory entry, and that _r2file_NameToDoid returns that entry's index. If
 * either is wrong, every "hit" is a plausible-looking wrong answer: the worst
 * failure mode a cache has, because nothing downstream can detect it. Sixteen
 * linear scans at startup is nothing; serving 4,558 wrong answers is not. */
static int verify_index(const unsigned char *directory, unsigned count)
{
    unsigned step = count / 16u ? count / 16u : 1u, i;

    for (i = 0; i < count; i += step) {
        const unsigned char *packed = directory + i * 20u;
        char raw[R2_NAME_SIZE], name[R2_NAME_SIZE];
        void *object;
        int doid;

        unpack_name(packed, raw);
        normalized_name(raw, name);
        if (!name[0])
            continue;

        object = s_real_find_name(name);
        doid = s_real_name_to_doid(name);
        if (object == (const void *)packed && doid == (int)i)
            continue;

        LOGW("r2 cache: entry %u '%s'; the game answers object=%p doid=%d, "
             "the index would answer %p/%d. The index's layout assumption is "
             "wrong; serving from it is disabled and every lookup goes to the "
             "game's own scan", i, name, object, doid, (const void *)packed,
             (int)i);
        return 0;
    }
    return 1;
}

static int build_index_if_ready(void)
{
    const unsigned char *directory;
    unsigned count, i;

    if (s_index_ready || s_index_refused || s_in_index_work)
        return s_index_ready;
    directory = *(const unsigned char **)(uintptr_t)G->r2_directory;
    count = *(const unsigned *)(uintptr_t)G->r2_directory_count;
    if (!directory || !count)
        return 0;
    if (count >= R2_CACHE_SIZE / 2u) {
        LOGW("r2 cache: directory has %u entries; index capacity is insufficient",
             count);
        s_index_refused = 1;
        return 0;
    }

    for (i = 0; i < count; i++) {
        const unsigned char *packed = directory + i * 20u;
        char name[R2_NAME_SIZE];
        unsigned hash;
        r2_cache_entry_t *entry;

        unpack_name(packed, name);
        hash = normalized_name(name, name);
        entry = find_slot(name, hash);
        if (!entry) {
            s_index_refused = 1;
            return 0;
        }
        if (!entry->name[0])
            memcpy(entry->name, name, R2_NAME_SIZE);
        entry->object = (void *)packed;
        entry->doid = (int)i;
        entry->have_object = 1;
        entry->have_doid = 1;
    }

    s_in_index_work = 1;
    i = (unsigned)verify_index(directory, count);
    s_in_index_work = 0;
    if (!i) {
        memset(s_cache, 0, sizeof s_cache);
        s_index_refused = 1;
        return 0;
    }

    s_index_ready = 1;
    LOGI("r2 cache: indexed all %u archive directory entries, layout verified "
         "against the game's own lookup", count);
    return 1;
}

/* The cache may answer only hits; misses defer to the original lookup. */
static void *cached_find_name(const char *name)
{
    char normalized[R2_NAME_SIZE];
    unsigned hash = normalized_name(name, normalized);
    r2_cache_entry_t *entry;
    void *result;

    if (s_in_index_work)
        return s_real_find_name(name);
    memcpy(s_last_name, normalized, R2_NAME_SIZE);
    note_arrow_lookup(normalized);
    build_index_if_ready();
    entry = find_slot(normalized, hash);
    if (entry && entry->name[0] && entry->have_object) {
        InterlockedIncrement(&s_hits);
        return entry->object;
    }
    InterlockedIncrement(&s_misses);
    result = s_real_find_name(name);
    if (result && entry) {
        if (!entry->name[0])
            memcpy(entry->name, normalized, R2_NAME_SIZE);
        entry->object = result;
        entry->have_object = 1;
    }
    return result;
}

static int cached_name_to_doid(const char *name)
{
    char normalized[R2_NAME_SIZE];
    unsigned hash = normalized_name(name, normalized);
    r2_cache_entry_t *entry;
    int result;

    if (s_in_index_work)
        return s_real_name_to_doid(name);
    memcpy(s_last_name, normalized, R2_NAME_SIZE);
    note_arrow_lookup(normalized);
    build_index_if_ready();
    entry = find_slot(normalized, hash);
    if (entry && entry->name[0] && entry->have_doid) {
        InterlockedIncrement(&s_hits);
        return entry->doid;
    }
    InterlockedIncrement(&s_misses);
    result = s_real_name_to_doid(name);         /* never fabricate a negative */
    if (result >= 0 && entry) {
        if (!entry->name[0])
            memcpy(entry->name, normalized, R2_NAME_SIZE);
        entry->doid = result;
        entry->have_doid = 1;
    }
    return result;
}

int r2_cache_install(void)
{
    void *find_trampoline, *doid_trampoline;
    int ready = 1;

    if (!g_cfg.r2_cache) {
        LOGI("r2 cache: disabled by [diagnostics] r2_cache; the game does its "
             "own linear directory scan");
        return 1;
    }

    /* The index is built from the game's own directory pointer and count,
     * decoded out of _r2file_NameToDoid's first two absolute loads. Offroad's
     * copy of that function does not match the pattern, so those come out 0 and
     * there is nothing to index. The game's linear scan is the fallback, and it
     * is the authority anyway: this cache is an accelerator that must never
     * answer "no such asset". */
    if (!game_require("r2 cache", "a decodable R2 directory pointer",
                      G->r2_directory)) {
        LOGI("r2 cache: not installed; the game does its own linear scan");
        return 1;
    }

    /* Both begin with a five-byte mov eax,[absolute] in both titles, but the
     * lengths still come from the profile; see hook_call_through(). */
    ready &= game_require("r2 cache", "_r2file_FindName", G->r2_find_name);
    ready &= game_require("r2 cache", "_r2file_NameToDoid",
                          G->r2_name_to_doid);
    ready &= game_require("r2 cache", "relocatable _r2file_FindName prologue",
                          G->hook_r2_find_name);
    ready &= game_require("r2 cache", "relocatable _r2file_NameToDoid prologue",
                          G->hook_r2_name_to_doid);
    if (!ready) {
        LOGI("r2 cache: not installed; the game does its own linear scan");
        return 1;
    }
    find_trampoline = hook_prepare_call_through(G->r2_find_name,
                                                G->hook_r2_find_name);
    doid_trampoline = hook_prepare_call_through(G->r2_name_to_doid,
                                                G->hook_r2_name_to_doid);
    if (!find_trampoline || !doid_trampoline) {
        LOGE("r2 cache: failed to install directory lookup hooks");
        return 0;
    }
    s_real_find_name = (find_name_fn)find_trampoline;
    s_real_name_to_doid = (name_to_doid_fn)doid_trampoline;
    patch_jmp(G->r2_find_name, (void *)cached_find_name);
    patch_jmp(G->r2_name_to_doid, (void *)cached_name_to_doid);
    LOGI("r2 cache: archive directory will be hash-indexed after initialization");
    return 1;
}

void r2_cache_report(void)
{
    LONG hits = InterlockedExchange(&s_hits, 0);
    LONG misses = InterlockedExchange(&s_misses, 0);

    if (hits || misses)
        LOGI("r2 cache: %ld indexed hits, %ld misses/fallbacks (%.1f%% hit rate)",
             (long)hits, (long)misses,
             hits + misses ? 100.0 * (double)hits / (double)(hits + misses) : 0.0);
}
