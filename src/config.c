/* config.c -- vcthunder.ini plus a few command-line overrides.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Deliberately tiny. It knows no key names: every one, with its section, type,
 * default, range and choice set, is in src/settings.def, and BOTH the defaults
 * and the file go through apply_value(), so a default is by construction
 * something that could have been written in the ini. Value SPELLINGS are
 * src/ini.c's. What is local is only what to DO with a value that will not
 * parse: keep the previous layer's and file a note. The other readers cannot
 * share that, because this pass runs before the log opens. */
#include "vcthunder.h"
#include "settings.h"
#include "ini.h"
#include "resource.h"

#include <stdio.h>
#include <string.h>

shim_config_t g_cfg;

/* Invalid values must leave the value inherited from the previous layer in
 * force. The shared pass runs before the log exists, so retain a small bounded
 * list and report it once logging is available. */
#define INVALID_NOTE_MAX 32

typedef enum {
    NOTE_REJECTED,      /* the value did not parse; the previous one stands  */
    NOTE_MISPLACED,     /* right key, wrong section: applied anyway        */
    NOTE_STRANDED       /* right key, wrong section, and NOT ours to apply   */
} note_kind_t;

typedef struct {
    note_kind_t kind;
    char section[64];
    char key[64];
    char value[96];
    char why[96];       /* NOTE_REJECTED: the reason. Otherwise: the section
                         * the key belongs in. */
} invalid_note_t;

static invalid_note_t s_invalid[INVALID_NOTE_MAX];
static unsigned s_invalid_count;
static int s_invalid_overflow;

/* THE FILE ITSELF WAS NOT THERE: one fact about the whole pass, not a per-key
 * note. Held as the RESOLVED path, so the line names the file rather than the
 * word "vcthunder.ini".
 *
 * IT DOES NOT BLAME THE WORKING DIRECTORY. The recorded "a relative ini
 * path reads a file in C:\Windows" cannot reach THIS file;
 * chdir_to_own_folder() runs before every load, and offering cwd would be a
 * diagnosis that cannot apply. What IS newly said is that the file is missing
 * at all: a deleted file produced a silent all-defaults run. */
static char s_no_file[MAX_PATH];

/* ...and the file this run CREATED, which is the other half of the same fact
 * and must not read as the one above: an all-defaults run whose defaults are
 * now on disk is a first run, not a lost configuration. */
static char s_seeded[MAX_PATH];
/* A pack that could not be written to at all. It is reported as the failure it
 * is, because every other thing this program saves (the ini, NVRAM, the log)
 * goes to the same folder, so it is never the only symptom. */
static char s_seed_failed[MAX_PATH];
static DWORD s_seed_error;
/* A file this parser read but the profile API cannot. Controls and force
 * feedback are read through the profile API, so they silently take their
 * defaults while everything parsed here applies. A WSL path (\\wsl.localhost)
 * does this: it refuses the byte-range lock the profile API takes. */
static char s_unreadable[MAX_PATH];
static DWORD s_unreadable_error;

/* THE SHIPPED INI IS A RESOURCE IN THIS BINARY, AND THAT IS WHAT LETS A RELEASE
 * CARRY NO SETTINGS FILE. Shipping vcthunder.ini beside the binaries makes
 * unzipping a new build over an existing folder replace the user's whole
 * configuration: [devices], the device's axis section and every binding live in
 * that one file. A zip without the file cannot do that.
 *
 * CREATE_NEW rather than a GetFileAttributes test then a write: the test is the
 * same question with a window between the answer and the act. It also makes
 * this safe on every start, which is what makes "delete vcthunder.ini and run
 * it again" the whole recovery procedure.
 *
 * It says nothing HERE, because the log is not open yet; both outcomes are
 * notes for config_report_notes(). */
int config_seed_ini(const char *path)
{
    HMODULE self = NULL;
    HRSRC found;
    HGLOBAL loaded;
    const void *data;
    HANDLE file;
    DWORD size, wrote;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)&config_seed_ini, &self))
        return 0;
    found = FindResourceA(self, MAKEINTRESOURCEA(IDR_DEFAULT_INI), RT_RCDATA);
    if (!found)
        return 0;
    size = SizeofResource(self, found);
    loaded = LoadResource(self, found);
    if (!size || !loaded || !(data = LockResource(loaded)))
        return 0;
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_EXISTS) {
            s_seed_error = GetLastError();
            ini_resolve_path(path, s_seed_failed, sizeof s_seed_failed);
        }
        return 0;
    }
    if (!WriteFile(file, data, size, &wrote, NULL) || wrote != size) {
        s_seed_error = GetLastError();
        ini_resolve_path(path, s_seed_failed, sizeof s_seed_failed);
        CloseHandle(file);
        /* A half-written settings file is worse than none: every key past the
         * truncation takes its default, which is exactly the silent
         * half-configured state this whole mechanism exists to prevent. */
        DeleteFileA(path);
        return 0;
    }
    CloseHandle(file);
    ini_resolve_path(path, s_seeded, sizeof s_seeded);
    return 1;
}

static void note(note_kind_t kind, const char *section, const char *key,
                 const char *value, const char *why)
{
    invalid_note_t *n;

    if (s_invalid_count >= INVALID_NOTE_MAX) {
        s_invalid_overflow = 1;
        return;
    }
    n = &s_invalid[s_invalid_count++];
    n->kind = kind;
    snprintf(n->section, sizeof n->section, "%s", *section ? section : "global");
    snprintf(n->key, sizeof n->key, "%s", key);
    snprintf(n->value, sizeof n->value, "%s", value);
    snprintf(n->why, sizeof n->why, "%s", why);
}

static void note_invalid(const char *section, const char *key,
                         const char *value, const char *why)
{
    note(NOTE_REJECTED, section, key, value, why);
}

static int set_uint(const char *section, const char *key, const char *v,
                    unsigned lo, unsigned hi, unsigned *dst)
{
    char why[96];
    unsigned n;

    if (ini_parse_uint(v, lo, hi, &n)) {
        *dst = n;
        return 1;
    }
    snprintf(why, sizeof why, "expected an integer in %u..%u", lo, hi);
    note_invalid(section, key, v, why);
    return 0;
}

static int set_int(const char *section, const char *key, const char *v,
                   int lo, int hi, int *dst)
{
    char why[96];
    int n;

    if (ini_parse_int(v, lo, hi, &n)) {
        *dst = n;
        return 1;
    }
    snprintf(why, sizeof why, "expected an integer in %d..%d", lo, hi);
    note_invalid(section, key, v, why);
    return 0;
}

static int parse_level(const char *v, log_level_t *out)
{
    if (_stricmp(v, "error") == 0) *out = LOG_ERR;
    else if (_stricmp(v, "warn")  == 0) *out = LOG_WARN;
    else if (_stricmp(v, "info")  == 0) *out = LOG_INFO;
    else if (_stricmp(v, "trace") == 0) *out = LOG_TRACE;
    else return 0;
    return 1;
}

static int set_bool(const char *section, const char *key, const char *v,
                    int *dst)
{
    int n;

    if (ini_parse_bool(v, &n)) {
        *dst = n;
        return 1;
    }
    note_invalid(section, key, v,
                 "expected true/false, yes/no, on/off, or 1/0");
    return 0;
}

/* Classify title-scoped sections from the runtime profile table. The shared
 * configuration pass skips all such sections. */
static int section_owner_is_title(const char *name, const char *id)
{
    size_t n = strlen(id);

    return _strnicmp(name, id, n) == 0 && (name[n] == '\0' || name[n] == '.');
}

static int section_is_per_title(const char *name)
{
    unsigned i;

    for (i = 0; g_profiles[i]; i++)
        if (section_owner_is_title(name, g_profiles[i]->id))
            return 1;
    return 0;
}

/* THE SECTIONS THAT CAN CARRY A PER-DEVICE PROFILE, and one entry is the whole
 * list: the axis map is the only thing keyed to a controller. */
static const char *const k_device_scoped[] = { "analog" };

/* NEITHER PASS OWNS A DEVICE PROFILE, AND SAYING SO IS THE POINT. Without this,
 * [analog.tmx] reaches setting_lookup(), which resolves a key by NAME from any
 * section, finds dinput_throttle_axis in [analog], and tells the user
 * confidently to move the line; destroying the thing the profile exists to
 * protect. These keys are STORE_ELSEWHERE, so nothing was ever wrongly applied;
 * the defect was entirely in what the host said about them. */
static int section_is_device_profile(const char *name)
{
    char label[INI_LABEL_CAP];
    unsigned i;

    for (i = 0; i < sizeof k_device_scoped / sizeof k_device_scoped[0]; i++)
        if (ini_device_profile_section(name, k_device_scoped[i],
                                       label, sizeof label))
            return 1;
    return 0;
}

typedef enum {
    LOAD_SHARED,        /* every plain section; skip every per-title one */
    LOAD_TITLE          /* only [<title>] and [<title>.*] */
} load_mode_t;

/* Store one value through the table's own description of it.
 *
 * The one place a configuration value is turned into a field, used by the ini
 * pass, the per-title pass and config_defaults() alike. `section` is only ever
 * for the message an invalid value produces. Returns 1 when the value landed. */
static int apply_value(shim_config_t *c, const setting_desc_t *d,
                       const char *section, const char *v)
{
    void *field = (char *)c + d->field_off;
    char why[128];

    switch (d->store) {
    case VCT_STORE_NONE:
        return 0;                       /* recognised, but not ours to store */

    case VCT_STORE_STR:
        snprintf((char *)field, d->field_size, "%s", v);
        return 1;

    case VCT_STORE_ENUM:
        /* Validated here, not only in the launcher: a mistyped `aspect` or
         * `output` must not silently behave as whatever the consumer's
         * fallback happens to be. */
        if (!vct_choice_contains(d->choices, v)) {
            snprintf(why, sizeof why, "expected one of %s", d->choices);
            note_invalid(section, d->key, v, why);
            return 0;
        }
        snprintf((char *)field, d->field_size, "%s", v);
        return 1;

    case VCT_STORE_BOOL:
        return set_bool(section, d->key, v, (int *)field);

    case VCT_STORE_UINT_AUTO:
        if (_stricmp(v, "auto") == 0) {
            *(unsigned *)field = 0u;
            return 1;
        }
        /* fall through */
    case VCT_STORE_UINT:
        return set_uint(section, d->key, v, (unsigned)d->minimum,
                        (unsigned)d->maximum, (unsigned *)field);

    case VCT_STORE_LEVEL: {
        log_level_t level;

        if (!parse_level(v, &level)) {
            snprintf(why, sizeof why, "expected one of %s", d->choices);
            note_invalid(section, d->key, v, why);
            return 0;
        }
        *(log_level_t *)field = level;
        return 1;
    }

    case VCT_STORE_MODE:
        /* -1 is "whatever the game asked for", which is the cabinet's own
         * mode. It is not in the numeric range, so it needs its own spelling. */
        if (!*v || _stricmp(v, "auto") == 0 || _stricmp(v, "cabinet") == 0) {
            *(int *)field = -1;
            return 1;
        }
        return set_int(section, d->key, v, (int)d->minimum, (int)d->maximum,
                       (int *)field);
    }
    return 0;
}

/* Every default, applied by the same code that applies the file: the table
 * spells each one as ini text, so "the shipped default" and "what the parser
 * makes of it" cannot be two values. Zero is a deliberate starting point for
 * the handful of fields with no key at all, `title` and `dry_run` come from
 * the command line only. */
void config_defaults(shim_config_t *c)
{
    unsigned i;

    memset(c, 0, sizeof *c);
    for (i = 0; i < VCT_SETTING_COUNT; i++) {
        const setting_desc_t *d = &vct_setting[i];

        if (d->store == VCT_STORE_NONE)
            continue;                   /* diego.c and dinput.c own these */
        /* A default that will not parse is a bug in settings.def, not a user
         * error, but it is reported through the same channel so it cannot be
         * silent, and it names "built-in" rather than a section. */
        apply_value(c, d, "built-in", d->fallback);
    }
}

/* Old spellings that a run directory older than a rename still carries. */
static const struct { const char *from, *to; } k_aliases[] = {
    { "diego_ms", "io_ms" },
};

static const char *unalias(const char *key)
{
    unsigned i;

    for (i = 0; i < sizeof k_aliases / sizeof k_aliases[0]; i++)
        if (_stricmp(key, k_aliases[i].from) == 0)
            return k_aliases[i].to;
    return key;
}

/* The section a key is really in, with any `<title>.` prefix removed: the
 * per-title pass sees [hydro.graphics] and the table only knows [graphics]. */
static const char *plain_section(const char *section, const char *title)
{
    size_t n;

    if (!title || !*title)
        return section;
    n = strlen(title);
    if (_strnicmp(section, title, n) != 0)
        return section;
    if (section[n] == '.')
        return section + n + 1;
    if (section[n] == '\0')
        return section + n;             /* a bare [hydro]: no section at all */
    return section;
}

/* Find a key, preferring its own section.
 *
 * docs/configuration.md §6 promises "the parser takes any key it knows,
 * wherever it finds it", so a key found outside its section is still applied,
 * and now says so once, rather than leaving the user with two plausible places
 * to have put it. That fallback needs key names unique across sections;
 * tools/check-launcher-settings.py refuses a table where they are not. */
static const setting_desc_t *setting_lookup(const char *section,
                                            const char *key, int *misplaced)
{
    const setting_desc_t *elsewhere = NULL;
    unsigned i;

    *misplaced = 0;
    for (i = 0; i < VCT_SETTING_COUNT; i++) {
        if (_stricmp(vct_setting[i].key, key) != 0)
            continue;
        if (_stricmp(vct_setting[i].section, section) == 0)
            return &vct_setting[i];
        elsewhere = &vct_setting[i];
    }
    *misplaced = elsewhere != NULL;
    return elsewhere;
}

/* One worker, two entry points, one file.
 *
 * `log_file` is deliberately not honoured in a per-title section: it names a
 * file that is already open by the time the title is known, so accepting it
 * would be a setting that reads as applied and is not. */
static int load(shim_config_t *c, const char *path, load_mode_t mode,
                const char *title)
{
    char line[512];
    char section[64] = "";
    int applied = 0;
    unsigned sections = 0;
    FILE *fp = fopen(path, "r");

    if (!fp) {
        /* Absent is a legitimate state; every default applies and both
         * titles boot, so this is a note and not a failure. Recorded on the
         * SHARED pass only: the title pass opens the same file a moment later
         * and would say it twice about one missing file. */
        if (mode == LOAD_SHARED)
            ini_resolve_path(path, s_no_file, sizeof s_no_file);
        return -1;
    }

    while (fgets(line, sizeof line, fp)) {
        char *k, *v, *eq;

        k = ini_trim(line);
        if (*k == '[') {
            char *end = strchr(k, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof section, "%s", k + 1);
                sections++;
            }
            continue;
        }
        if (!*k || *k == ';' || *k == '#')
            continue;

        /* Before the title test, not after it: [hydro.analog.tmx] IS owned
         * by hydro, and the title pass would take it. */
        if (section_is_device_profile(section))
            continue;
        if (mode == LOAD_TITLE) {
            if (!section_owner_is_title(section, title))
                continue;
        } else if (section_is_per_title(section)) {
            continue;
        }

        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = '\0';
        k = ini_trim(k);
        v = ini_trim(eq + 1);
        ini_strip_comment(v);

        if (mode == LOAD_TITLE && _stricmp(k, "log_file") == 0)
            continue;

        {
            const char *plain = plain_section(section,
                                    mode == LOAD_TITLE ? title : NULL);
            const char *canonical = unalias(k);
            int misplaced = 0;
            const setting_desc_t *d = setting_lookup(plain, canonical,
                                                     &misplaced);

            /* Not a key this file owns. The binding maps in [digital] and
             * [gamepad] land here and are read by diego.c from the same file,
             * so an unknown key is normal and must not count as applied. */
            if (!d)
                continue;
            if (misplaced)
                note(d->store == VCT_STORE_NONE ? NOTE_STRANDED
                                                : NOTE_MISPLACED,
                     plain, d->key, v, d->section);
            if (apply_value(c, d, *plain ? plain : "global", v))
                applied++;
        }
    }
    fclose(fp);

    if (mode == LOAD_SHARED && sections) {
        char full[MAX_PATH], names[64];

        ini_resolve_path(path, full, sizeof full);
        SetLastError(0);
        if (!GetPrivateProfileSectionNamesA(names, sizeof names, full)) {
            s_unreadable_error = GetLastError();
            snprintf(s_unreadable, sizeof s_unreadable, "%s", full);
        }
    }
    return applied;
}

/* Resolve raster widescreen and presentation aspect before either value is
 * consumed. An explicit 4:3 aspect disables widescreen raster expansion. */
void config_resolve_graphics(shim_config_t *c)
{
    if (c->widescreen && aspect_is_4_3(c->aspect)) {
        c->widescreen = 0;
        LOGW("graphics: aspect = 4:3 asks for the cabinet's shape, so "
             "widescreen is off for this run. Set aspect = 16:9 (or raster, "
             "or stretch) to use it.");
    }
}

int config_load_shared(shim_config_t *c, const char *path)
{
    return load(c, path, LOAD_SHARED, NULL);
}

int config_load_title(shim_config_t *c, const char *path, const char *title)
{
    return load(c, path, LOAD_TITLE, title);
}

/* Whatever a load pass could not say yet. Call after each pass whose log is
 * open; reported notes are consumed so shared and per-title loads do not repeat
 * one another. */
void config_report_notes(void)
{
    unsigned i;
    int reported_seed_failure = 0;

    for (i = 0; i < s_invalid_count; i++)
        switch (s_invalid[i].kind) {
        case NOTE_REJECTED:
            LOGW("config: [%s] %s = %s ignored; %s; keeping the previous "
                 "value", s_invalid[i].section, s_invalid[i].key,
                 s_invalid[i].value, s_invalid[i].why);
            break;
        case NOTE_MISPLACED:
            LOGW("config: [%s] %s belongs in [%s]. It was applied anyway, but "
                 "move it: a key is only guaranteed to be found in its own "
                 "section.", s_invalid[i].section, s_invalid[i].key,
                 s_invalid[i].why);
            break;
        case NOTE_STRANDED:
            LOGW("config: [%s] %s = %s is NOT read there; it belongs in "
                 "[%s], and the subsystem that owns it looks only in that "
                 "section. Move the line or it does nothing.",
                 s_invalid[i].section, s_invalid[i].key, s_invalid[i].value,
                 s_invalid[i].why);
            break;
        }
    if (s_invalid_overflow)
        LOGW("config: more than %u invalid values; further warnings suppressed",
             INVALID_NOTE_MAX);
    if (s_seeded[0]) {
        LOGI("config: there was no settings file, so the shipped one was "
             "written to %s. It is yours from now on: nothing this program "
             "does overwrites it, and no update can, because a release "
             "carries no copy of it. Delete it and start this again to get "
             "the shipped defaults back.", s_seeded);
        s_seeded[0] = '\0';
    }
    if (s_seed_failed[0]) {
        reported_seed_failure = 1;
        LOGW("config: %s does not exist and could not be created "
             "(Windows error %lu). EVERY setting is its built-in default. "
             "The same folder has to take NVRAM and the log, so if this is a "
             "read-only or protected location, Program Files, or a folder "
             "still inside a zip viewer, move the pack somewhere you own.",
             s_seed_failed, s_seed_error);
        s_seed_failed[0] = '\0';
    }
    /* ...and only when the line above has not already said it better. Since
     * config_seed_ini() the ordinary reason for a missing file is that it
     * could not be WRITTEN, which that line names along with the error. */
    if (s_no_file[0] && !reported_seed_failure) {
        LOGW("config: %s does not exist, so EVERY setting is its built-in "
             "default; graphics, audio, controls and paths alike. That is a "
             "working configuration and both titles run on it, but nothing "
             "you have tuned is in force. This host always looks for the file "
             "beside its own exe and takes no path for it, so it is genuinely "
             "not there, and it writes one itself when it can, so something "
             "is stopping it.", s_no_file);
    }
    s_no_file[0] = '\0';
    if (s_unreadable[0]) {
        LOGW("config: %s was read, but Windows' own ini reader cannot read "
             "it (Windows error %lu), so every CONTROLS and FORCE FEEDBACK "
             "setting is its built-in default; graphics and audio are not "
             "affected. This happens when the pack is on a WSL path "
             "(\\\\wsl.localhost\\...) or a share that refuses file locks: "
             "move the pack to a local drive.", s_unreadable,
             s_unreadable_error);
        s_unreadable[0] = '\0';
    }
    s_invalid_count = 0;
    s_invalid_overflow = 0;
}

/* Command line, parsed AFTER the shared pass so anything here overrides it.
 *
 * `VCThunder.exe hydro` and `VCThunder.exe offroad` are accepted as bare words
 * so one pack can carry a desktop shortcut per game. Only those two words: a
 * bare argument is otherwise a typo, and taking it for a title would turn one
 * into a confusing failure much later. */
int config_parse_argv(shim_config_t *c, int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            c->dry_run = 1;
        /* Implies --dry-run: the probe deliberately raises exceptions, loads
         * a module and makes a page of our own image executable, and none of
         * that belongs in a process that then hands control to the game. */
        else if (strcmp(argv[i], "--probe-image") == 0)
            c->probe_image = c->dry_run = 1;
        /* Separate, and separate on purpose: it ends the process by design.
         * See image_probe_refusal(). */
        else if (strcmp(argv[i], "--probe-image-refusal") == 0)
            c->probe_refusal = c->dry_run = 1;
        else if (strcmp(argv[i], "--trace") == 0)
            c->log_level = LOG_TRACE;
        else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc)
            snprintf(c->title, sizeof c->title, "%s", argv[++i]);
        else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc)
            snprintf(c->exe, sizeof c->exe, "%s", argv[++i]);
        else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc)
            snprintf(c->data_dir, sizeof c->data_dir, "%s", argv[++i]);
        /* The three files a SECOND instance on this machine cannot share with
         * the first. Its unit id and its link setting live in the game's own
         * NVRAM adjustments, which is what makes --save load-bearing rather
         * than tidy: one save directory is one cabinet's settings. The two
         * logs are opened "w", so without their own names the second instance
         * truncates the first one's evidence. */
        else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc)
            snprintf(c->save_dir, sizeof c->save_dir, "%s", argv[++i]);
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            snprintf(c->log_file, sizeof c->log_file, "%s", argv[++i]);
        else if (strcmp(argv[i], "--glide-log") == 0 && i + 1 < argc)
            snprintf(c->vcglide_log, sizeof c->vcglide_log, "%s", argv[++i]);
        else if (strcmp(argv[i], "--link") == 0 && i + 1 < argc)
            snprintf(c->link, sizeof c->link, "%s", argv[++i]);
        else if (_stricmp(argv[i], "hydro") == 0 ||
                 _stricmp(argv[i], "offroad") == 0)
            snprintf(c->title, sizeof c->title, "%s", argv[i]);
        else {
            fprintf(stderr,
                "usage: %s <hydro|offroad> [--dry-run] [--probe-image] [--trace]\n"
                "       [--exe PATH]\n"
                "       [--data DIR] [--save DIR] [--log FILE] [--glide-log FILE]\n"
                "       [--link off|lan|local]\n"
                "  hydro | offroad  the title to run. REQUIRED: one pack holds\n"
                "             both, and the ini does not name one. Also\n"
                "             accepted as --title ID.\n"
                "  --dry-run  map, unpack and patch, then stop without running\n"
                "  --probe-image  as --dry-run, and first ask what of this\n"
                "             process survives the game landing on our image\n"
                "  --trace    log every stub hit and every binding\n"
                "  --save/--log/--glide-log  what a SECOND instance on this\n"
                "             machine needs of its own, for link play\n", argv[0]);
            return 0;
        }
    }
    return 1;
}
