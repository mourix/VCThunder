/* settings_round_trip_test.c -- what the launcher writes is what the host reads.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * One ini, three readers (src/ini.h says why they stay three). Two other
 * checks hold pieces of that arrangement still: tools/check-launcher-settings.py
 * holds settings.def against the shipped file, and tests/ini_doc_test.c holds
 * the document model to its own round trip. Neither crosses the gap between
 * them -- that a value the LAUNCHER writes is a value the HOST reads back to
 * the same thing -- and that gap is where three parsers drift. It is widest at
 * the keys config.c does not own: [analog], [ffb] and the binding sections go
 * through the third reader, which no other check touches.
 *
 * So this drives the whole of settings.def through the real path:
 *
 *   ini_doc_set + ini_doc_save   the launcher's Apply, byte for byte
 *   config_load_shared           the host's own pass, for the keys it stores
 *   ini_read_scoped              the [analog]/[ffb]/bindings reader, for the rest
 *
 * and asserts the value that comes back. Every entry, every choice in its
 * choice set, both ends of every numeric range, and every spelling of a
 * boolean -- because settings.def is the table, not a sample of it, and this
 * project's standing rule is that finding one instance of a class means
 * auditing the class.
 *
 * NOT covered, deliberately: whether a key belongs in the section it is in
 * (check-launcher-settings.py), and what the host does with a value it cannot
 * parse (tests/config_test.c). This one asks a single question and a value
 * that fails to parse would answer it by accident.
 */
#include "../src/ini_doc.c"
#include "../src/ini.c"
#include "../src/settings.c"
#include "../src/config.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what including config.c needs, and nothing more --------------------
 *
 * config.c reports a value it rejected through the log, and resolves widescreen
 * against the aspect. Both are somebody else's translation unit; here they are
 * the smallest thing that lets the parser run. Nothing in this file provokes a
 * note -- every value it writes is one settings.def says is legal -- so a line
 * arriving here at all is a finding, and it is printed. */
static int notes_logged;

void log_printf(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    notes_logged++;
    fputs("  unexpected log line: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int aspect_is_4_3(const char *aspect)
{
    return _stricmp(aspect, "4:3") == 0;
}

/* config.c classifies title-scoped sections from the profile table, and
 * ini_read_scoped prefers [<id>.<section>]. Two ids are the minimum that can
 * tell "the running title's section" from "some other title's section". */
static const game_profile_t k_hydro   = { .id = "hydro" };
static const game_profile_t k_offroad = { .id = "offroad" };
const game_profile_t *const g_profiles[] = { &k_hydro, &k_offroad, NULL };
const game_profile_t *g_game;

static int failures;

static void fail(const setting_desc_t *d, const char *value, const char *what)
{
    fprintf(stderr, "FAIL: [%s] %s = %s: %s\n", d->section, d->key, value, what);
    failures++;
}

/* ---- the value each entry should be tried with --------------------------
 *
 * The table's own columns, not a list written here: the default, every member
 * of the choice set, both ends of the range, and for a boolean every spelling
 * the grammar accepts. A candidate list written by hand would be a fourth copy
 * of settings.def, which is the thing settings.def exists to prevent. */
#define MAX_CANDIDATES 24

static void add(const char *v, char list[][INI_VALUE_CAP], unsigned *n)
{
    unsigned i;

    if (!v || !*v || *n >= MAX_CANDIDATES)
        return;
    for (i = 0; i < *n; i++)         /* the default is often also a choice */
        if (strcmp(list[i], v) == 0)
            return;
    snprintf(list[(*n)++], INI_VALUE_CAP, "%s", v);
}

static unsigned candidates(const setting_desc_t *d,
                           char list[][INI_VALUE_CAP])
{
    static const char *k_bools[] = { "true", "false", "yes", "no",
                                     "on", "off", "1", "0" };
    unsigned n = 0;
    char one[INI_VALUE_CAP];

    add(d->fallback, list, &n);

    if (d->choices) {
        const char *p = d->choices;
        while (*p) {
            const char *end = strchr(p, '|');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            snprintf(one, sizeof one, "%.*s", (int)len, p);
            add(one, list, &n);
            if (!end)
                break;
            p = end + 1;
        }
    }

    switch (d->store) {
    case VCT_STORE_BOOL: {
        unsigned i;
        for (i = 0; i < sizeof k_bools / sizeof k_bools[0]; i++)
            add(k_bools[i], list, &n);
        break;
    }
    case VCT_STORE_UINT:
    case VCT_STORE_UINT_AUTO:
        snprintf(one, sizeof one, "%lu", d->minimum);
        add(one, list, &n);
        snprintf(one, sizeof one, "%lu", d->maximum);
        add(one, list, &n);
        if (d->store == VCT_STORE_UINT_AUTO)
            add("auto", list, &n);
        break;
    default:
        break;
    }

    /* A free-text field has no choice set and often no default either --
     * `dinput_name` is a product-name substring and ships empty -- so the table
     * on its own leaves nothing to try. One awkward string covers all of them,
     * and it is awkward on purpose: a space, a backslash, and a '#' that is NOT
     * preceded by whitespace and so is part of the value rather than the start
     * of a comment. That last distinction is the grammar's, it is one character
     * wide, and all three readers have to agree about it. */
    if (d->type == ST_TEXT)
        add("Thrustmaster TMX C:\\x#1", list, &n);
    return n;
}

/* ---- did it come back? --------------------------------------------------
 *
 * Read through the same column that stored it. A per-kind comparison written
 * against the field's TYPE rather than against a list of key names is what
 * makes this test cover a key added to settings.def tomorrow. */
static int stored_matches(const shim_config_t *c, const setting_desc_t *d,
                          const char *v)
{
    const void *field = (const char *)c + d->field_off;

    switch (d->store) {
    case VCT_STORE_STR:
    case VCT_STORE_ENUM:
        return strcmp((const char *)field, v) == 0;
    case VCT_STORE_BOOL: {
        int want;
        return ini_parse_bool(v, &want) && *(const int *)field == want;
    }
    case VCT_STORE_UINT_AUTO:
        if (_stricmp(v, "auto") == 0)
            return *(const unsigned *)field == 0u;
        /* fall through */
    case VCT_STORE_UINT: {
        unsigned want;
        return ini_parse_uint(v, (unsigned)d->minimum, (unsigned)d->maximum,
                              &want) && *(const unsigned *)field == want;
    }
    case VCT_STORE_LEVEL: {
        log_level_t want;
        return parse_level(v, &want) && *(const log_level_t *)field == want;
    }
    case VCT_STORE_MODE: {
        int want;
        if (!*v || _stricmp(v, "auto") == 0 || _stricmp(v, "cabinet") == 0)
            return *(const int *)field == -1;
        return ini_parse_int(v, (int)d->minimum, (int)d->maximum, &want) &&
               *(const int *)field == want;
    }
    case VCT_STORE_NONE:
        break;
    }
    return 0;
}

/* ---- the launcher's half ------------------------------------------------ */
static int launcher_writes(const char *path, const char *section,
                           const char *key, const char *value)
{
    ini_doc_t doc;
    char why[256];
    int ok;

    if (!ini_doc_load(&doc, path))
        return 0;
    ok = ini_doc_set(&doc, section, key, value) &&
         ini_doc_save(&doc, why, sizeof why);
    if (!ok)
        fprintf(stderr, "  the launcher could not write it: %s\n", why);
    ini_doc_free(&doc);
    return ok;
}

/* Every entry the launcher shows, which is exactly the set it writes back on
 * Apply -- vct_ui_setting, not vct_setting: a developer switch is never in the
 * file the launcher saves, so round-tripping one would test a path the product
 * does not have. */
static void test_every_ui_setting(const char *path)
{
    char list[MAX_CANDIDATES][INI_VALUE_CAP];
    unsigned i, j;

    for (i = 0; i < VCT_UI_SETTING_COUNT; i++) {
        const setting_desc_t *d = &vct_ui_setting[i];
        unsigned n = candidates(d, list);

        if (!n)
            fail(d, "", "settings.def gives it no default and no choices, so "
                        "nothing here can be tried against it");

        for (j = 0; j < n; j++) {
            const char *v = list[j];

            if (!launcher_writes(path, d->section, d->key, v)) {
                fail(d, v, "the launcher refused to write it");
                continue;
            }

            if (d->store == VCT_STORE_NONE) {
                /* diego.c, dinput.c and input_probe.c read these themselves,
                 * through their own section-precedence rule. The host's own
                 * pass must leave them alone and the scoped reader must find
                 * exactly the text that was written. */
                char got[INI_VALUE_CAP];
                ini_read_scoped(path, d->section, d->key, "", got, sizeof got);
                if (strcmp(got, v) != 0)
                    fail(d, v, "ini_read_scoped read back something else");
            } else {
                shim_config_t c;
                config_defaults(&c);
                if (config_load_shared(&c, path) < 0)
                    fail(d, v, "config_load_shared could not open the file");
                else if (!stored_matches(&c, d, v))
                    fail(d, v, "config.c stored something else");
            }
        }
    }
}

/* ---- the scoped reader's precedence ------------------------------------
 *
 * The third reader is the one with no test, and its whole behaviour is a
 * precedence: [<title>.<section>] before [<section>]. A launcher that writes a
 * per-title override the game then ignores is a silent setting, which is the
 * failure mode docs/configuration.md §6 exists to describe. */
static void test_title_scope(const char *path)
{
    char got[INI_VALUE_CAP];

    if (!launcher_writes(path, "analog", "wheel_deadzone", "11") ||
        !launcher_writes(path, "hydro.analog", "wheel_deadzone", "22") ||
        !launcher_writes(path, "offroad.analog", "wheel_deadzone", "33")) {
        fprintf(stderr, "FAIL: could not stage the scoped sections\n");
        failures++;
        return;
    }

    g_game = &k_hydro;
    ini_read_scoped(path, "analog", "wheel_deadzone", "", got, sizeof got);
    if (strcmp(got, "22") != 0) {
        fprintf(stderr, "FAIL: [hydro.analog] should win for hydro, got %s\n", got);
        failures++;
    }

    g_game = &k_offroad;
    ini_read_scoped(path, "analog", "wheel_deadzone", "", got, sizeof got);
    if (strcmp(got, "33") != 0) {
        fprintf(stderr, "FAIL: [offroad.analog] should win for offroad, got %s\n", got);
        failures++;
    }

    /* No title yet: the shared section, and no crash reaching for G->id. */
    g_game = NULL;
    ini_read_scoped(path, "analog", "wheel_deadzone", "", got, sizeof got);
    if (strcmp(got, "11") != 0) {
        fprintf(stderr, "FAIL: with no title the plain section should win, "
                        "got %s\n", got);
        failures++;
    }

    /* And config.c must not have taken any of it: the key is not its own. */
    {
        shim_config_t c;
        config_defaults(&c);
        if (config_load_shared(&c, path) < 0) {
            fprintf(stderr, "FAIL: config_load_shared could not open the file\n");
            failures++;
        }
    }
    g_game = NULL;
}

/* ---- a per-title section the HOST reads --------------------------------
 *
 * The other half of the same promise, through config.c rather than through the
 * scoped reader: [hydro.graphics] must override [graphics] for hydro and do
 * nothing for offroad. The shared pass must skip both. */
static void test_per_title_pass(const char *path)
{
    shim_config_t c;

    if (!launcher_writes(path, "graphics", "render_scale", "2") ||
        !launcher_writes(path, "hydro.graphics", "render_scale", "5")) {
        fprintf(stderr, "FAIL: could not stage the per-title section\n");
        failures++;
        return;
    }

    config_defaults(&c);
    config_load_shared(&c, path);
    if (c.render_scale != 2) {
        fprintf(stderr, "FAIL: the shared pass took %u from a per-title "
                        "section\n", c.render_scale);
        failures++;
    }
    config_load_title(&c, path, "hydro");
    if (c.render_scale != 5) {
        fprintf(stderr, "FAIL: [hydro.graphics] did not override, got %u\n",
                c.render_scale);
        failures++;
    }

    config_defaults(&c);
    config_load_shared(&c, path);
    config_load_title(&c, path, "offroad");
    if (c.render_scale != 2) {
        fprintf(stderr, "FAIL: [hydro.graphics] leaked into offroad, got %u\n",
                c.render_scale);
        failures++;
    }
}

int main(void)
{
    char root[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];

    GetTempPathA(sizeof root, root);
    snprintf(folder, sizeof folder, "%.*sVCThunder-settings-%lu",
             (int)(sizeof folder - sizeof "VCThunder-settings-" - 12),
             root, GetCurrentProcessId());
    CreateDirectoryA(folder, NULL);
    snprintf(path, sizeof path, "%s\\vcthunder.ini", folder);

    test_every_ui_setting(path);
    test_title_scope(path);
    test_per_title_pass(path);

    DeleteFileA(path);

    if (notes_logged) {
        fprintf(stderr, "settings round trip: %d value(s) settings.def calls "
                        "legal were rejected by the parser\n", notes_logged);
        failures += notes_logged;
    }
    if (failures) {
        fprintf(stderr, "settings round trip: %d failure(s)\n", failures);
        return 1;
    }
    printf("settings round trip: %u settings, launcher -> file -> host, "
           "all passed\n", (unsigned)VCT_UI_SETTING_COUNT);
    return 0;
}
