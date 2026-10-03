/* launcher_model.c -- the launcher's settings model, with no window in it.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See src/launcher_model.h for what this half is and where the line between it
 * and src/launcher.c runs. It carries none of the catalogue (src/settings.def,
 * src/bindings.def), none of the value grammar (src/ini.h) and not the file
 * itself (src/ini_doc.h): each of those is another file's and says so.
 */
#include "launcher_model.h"

#include "io.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const setting_desc_t *const k_settings = vct_ui_setting;

const action_desc_t k_actions[ACTION_COUNT] = {
#define BINDING(key, hydro, offroad, pad_hydro, pad_offroad, button, hl, ol)  \
    { #key, hl, ol, hydro, offroad, 1, pad_hydro, pad_offroad },
#define BINDING_AXIS(key, hydro, offroad, hl, ol)                             \
    { #key, hl, ol, hydro, offroad, 0, 0, 0 },
#define BINDING_PAD(key, pad_hydro, pad_offroad, hl, ol)                      \
    { #key, hl, ol, NULL, NULL, 1, pad_hydro, pad_offroad },
#include "bindings.def"
#undef BINDING
#undef BINDING_AXIS
#undef BINDING_PAD
};

static ini_doc_t s_doc;
/* The [devices] table out of s_doc, loaded per question; see table_load(). */
static struct {
    unsigned    n;
    char        label[INI_DEVICES_MAX][INI_LABEL_CAP];
    char        value[INI_DEVICES_MAX][INI_VALUE_CAP];
    const char *v[INI_DEVICES_MAX];
    int         at[INI_DEVICES_MAX];
} s_table;
static void table_load(void);
static char s_value[SETTING_COUNT][INI_VALUE_CAP];
static char s_key_binding[2][ACTION_COUNT][MODEL_BINDING_CAP];
static char s_pad_binding[2][ACTION_COUNT][MODEL_BINDING_CAP];

ini_doc_t *model_doc(void)
{
    return &s_doc;
}

char *model_slot(unsigned i)
{
    return s_value[i];
}

char *model_key_binding(int title, unsigned action)
{
    return s_key_binding[title][action];
}

char *model_pad_binding(int title, unsigned action)
{
    return s_pad_binding[title][action];
}

int model_ini_exists(void)
{
    return GetFileAttributesA(s_doc.path) != INVALID_FILE_ATTRIBUTES;
}


int setting_find(const char *section, const char *key)
{
    unsigned i;
    for (i = 0; i < SETTING_COUNT; i++)
        if (!_stricmp(k_settings[i].section, section) &&
            !_stricmp(k_settings[i].key, key))
            return (int)i;
    return -1;
}

const char *model_value(const char *section, const char *key)
{
    int i = setting_find(section, key);
    return i >= 0 ? s_value[i] : "";
}

/* The model never holds an unparseable boolean, load_settings() replaces one
 * with the table's default, so the 0 here is unreachable and is only what
 * ini_bool_or() needs to be handed. */
int model_bool(const char *section, const char *key)
{
    return ini_bool_or(model_value(section, key), 0);
}

unsigned model_uint(const char *section, const char *key, unsigned dflt)
{
    return ini_uint_or(model_value(section, key), 0, UINT_MAX, dflt);
}

/* ---- the device's own section ------------------------------------------ */

/* The [devices] label of the device THE FILE CONFIGURES, not the one the
 * combo is testing. Empty when the file pins no device, which is every file
 * that has never been near the Use this device button. */
static char s_profile[INI_LABEL_CAP];
/* A [devices] line this session added and Apply has not written yet. Counted
 * into the Apply message: "nothing to save" with a new line in the document
 * would be a lie about the one thing the user just did. */
static int  s_device_entry_pending;

/* The provider whose keys belong to the configured device (s_profile), or NULL
 * when the file configures none. Set by resolve_profile(). */
static const char *s_profile_provider;

/* IS THIS KEY A FACT ABOUT A DEVICE, OR A CHOICE OF DEVICE?
 *
 * Derived rather than listed, so a key added to settings.def later is
 * classified by the same rule: a key in [analog] beginning with a provider's
 * prefix is a fact about that provider's device, EXCEPT the four that SELECT a
 * device, which cannot be, because a device's own section cannot say which
 * device it is. dinput.c's di_load_device_keys() is written under the same
 * sentence, and has to be: this half decides where a value is WRITTEN and that
 * half where it is READ.
 *
 * `provider` in particular MUST stay shared. Re-resolving it from the file is
 * what makes adopting a second controller overwrite the first one's axis map:
 * the model has just been told the new provider, and a reload puts the old one
 * back, classifying every later key against the device being replaced. */
static int setting_is_device_key(unsigned i)
{
    static const char *const k_prefix[] = { "dinput_", "xinput_", "winmm_" };
    const char *key = k_settings[i].key;
    unsigned p;

    if (_stricmp(k_settings[i].section, "analog"))
        return 0;
    for (p = 0; p < sizeof k_prefix / sizeof k_prefix[0]; p++)
        if (!_strnicmp(key, k_prefix[p], strlen(k_prefix[p])))
            break;
    if (p == sizeof k_prefix / sizeof k_prefix[0])
        return 0;
    return _stricmp(key, "dinput_name") && _stricmp(key, "xinput_device") &&
           _stricmp(key, "winmm_device");
}

/* ...and is it a fact about THE DEVICE THIS FILE CONFIGURES? Only then does it
 * live in that device's own section. */
static int setting_is_device_scoped(unsigned i)
{
    const char *provider = s_profile_provider;
    size_t n;

    if (!provider || !s_profile[0] || !setting_is_device_key(i))
        return 0;
    n = strlen(provider);
    return _strnicmp(k_settings[i].key, provider, n) == 0 &&
           k_settings[i].key[n] == '_';
}

static int setting_resolve(unsigned i, char *out, size_t out_size);
static model_attached_fn s_attached;
/* XInput's user indices, 0..3: an `xinput:` line names one by number. */
#define XUSER_PADS 4u

/* A role's facts live with the controller the role is read from: with
 * `dinput_throttle_axis = pad:z` the host reads the throttle invert from
 * [analog.pad], so that row shows and writes it there. The one deadzone row is
 * the default device's. */
static const struct { const char *fact, *role; } k_role_fact[] = {
    { "dinput_steering_invert",        "dinput_steering_axis" },
    { "dinput_steering_range_percent", "dinput_steering_axis" },
    { "dinput_throttle_invert",        "dinput_throttle_axis" },
    { "dinput_brake_invert",           "dinput_brake_axis" },
};
#define ROLE_FACT_COUNT (sizeof k_role_fact / sizeof k_role_fact[0])
/* The section each fact's value in s_value[] was read from. */
static char s_fact_section[ROLE_FACT_COUNT][64];

static int role_fact_of(unsigned i)
{
    unsigned k;

    if (_stricmp(k_settings[i].section, "analog"))
        return -1;
    for (k = 0; k < ROLE_FACT_COUNT; k++)
        if (!_stricmp(k_settings[i].key, k_role_fact[k].fact))
            return (int)k;
    return -1;
}

static int table_resolve(const char *line, const char *extra,
                         const char *provider, const ini_device_id_t *dev,
                         unsigned count);

/* The first label of the controller `label` opens, the one dinput.c opens its
 * slot under, resolved by the line's own provider (an XInput pad's number is
 * its identity, attached or not). `label` itself when it opens nothing. */
static void first_label(const char *label, char *out, size_t out_size)
{
    ini_device_id_t ids[MODEL_ATTACHED_MAX];
    int ffb[MODEL_ATTACHED_MAX], which;
    char provider[16], match[INI_MATCH_CAP];
    unsigned n = 0, i;

    snprintf(out, out_size, "%s", label);
    table_load();
    for (i = 0; i < s_table.n && _stricmp(s_table.label[i], label); i++)
        ;
    if (i == s_table.n ||
        !ini_device_entry(s_table.value[i], provider, sizeof provider,
                          match, sizeof match))
        return;
    if (!_stricmp(provider, "xinput")) {
        for (n = 0; n < XUSER_PADS; n++) {
            ids[n].id = ids[n].name = "";
            ids[n].index = n;
        }
    } else if (!_stricmp(provider, "dinput") && s_attached) {
        n = s_attached(provider, ids, ffb, MODEL_ATTACHED_MAX);
    }
    table_resolve(NULL, NULL, provider, ids, n);
    if ((which = s_table.at[i]) < 0)
        return;
    for (i = 0; s_table.at[i] != which; i++)
        ;
    snprintf(out, out_size, "%s", s_table.label[i]);
}

/* The profile a role's facts are read under when its value names a
 * controller. */
static int role_fact_label(unsigned k, char *out, size_t out_size)
{
    int r = setting_find("analog", k_role_fact[k].role);
    char label[INI_LABEL_CAP], axis[32];

    if (r < 0 || !ini_parse_axis_ref(s_value[r], label, sizeof label, axis,
                                     sizeof axis) || !label[0])
        return 0;
    first_label(label, out, out_size);
    return 1;
}

/* The section this key is READ FROM FIRST and WRITTEN TO. */
const char *setting_section(unsigned i)
{
    static char scoped[64];
    char label[INI_LABEL_CAP];
    int k = role_fact_of(i);

    if (k >= 0 && role_fact_label((unsigned)k, label, sizeof label)) {
        snprintf(scoped, sizeof scoped, "%s.%s", k_settings[i].section, label);
        return scoped;
    }
    if (!setting_is_device_scoped(i))
        return k_settings[i].section;
    snprintf(scoped, sizeof scoped, "%s.%s", k_settings[i].section, s_profile);
    return scoped;
}

/* An [analog] key as the host reads it for the controller `label` names: its
 * section, then [analog], then the default. The configured device's values
 * are the model's, edits included. */
const char *model_device_value(const char *label, const char *key)
{
    static char out[INI_VALUE_CAP];
    char first[INI_LABEL_CAP], scoped[64];
    int i;

    if (!label || !label[0])
        return model_value("analog", key);
    first_label(label, first, sizeof first);
    if (!_stricmp(first, s_profile))
        return model_value("analog", key);
    snprintf(scoped, sizeof scoped, "analog.%s", first);
    if (ini_doc_get(&s_doc, scoped, key, out, sizeof out) ||
        ini_doc_get(&s_doc, "analog", key, out, sizeof out))
        return out;
    i = setting_find("analog", key);
    return i >= 0 ? k_settings[i].fallback : "";
}

/* When a role moves to another controller its facts are re-read there, never
 * carried across, or a key nobody touched would be written. `moved` is called
 * for each fact whose section changed. */
int model_sync_role_facts(void (*moved)(const char *section, const char *key))
{
    unsigned k;
    int n = 0;

    for (k = 0; k < ROLE_FACT_COUNT; k++) {
        int i = setting_find("analog", k_role_fact[k].fact);
        const char *section;

        if (i < 0)
            continue;
        section = setting_section((unsigned)i);
        if (!strcmp(section, s_fact_section[k]))
            continue;
        snprintf(s_fact_section[k], sizeof s_fact_section[k], "%s", section);
        if (!setting_resolve((unsigned)i, s_value[i], sizeof s_value[i]))
            snprintf(s_value[i], sizeof s_value[i], "%s",
                     k_settings[i].fallback);
        if (moved)
            moved("analog", k_role_fact[k].fact);
        n++;
    }
    return n;
}

/* What the FILE says this key is, walking the device's section then the shared
 * one, the launcher's half of ini_read_device_scoped(), over the document
 * model. 0 when neither has it; the caller supplies the table's default.
 *
 * No title scope here: the launcher edits the shared sections and shows the
 * per-title binding pages separately, so this is the device axis only. */
static int setting_resolve(unsigned i, char *out, size_t out_size)
{
    const char *section = setting_section(i);

    if (ini_doc_get(&s_doc, section, k_settings[i].key, out, out_size))
        return 1;
    if (_stricmp(section, k_settings[i].section) &&
        ini_doc_get(&s_doc, k_settings[i].section, k_settings[i].key,
                    out, out_size))
        return 1;
    return 0;
}

/* The attached controllers, from whoever can enumerate them (the launcher's
 * probe). None is a valid state: then only the file's text is used. */
void model_set_attached(model_attached_fn fn)
{
    s_attached = fn;
}

/* The label [devices] gives the configured device: the first line that opens
 * the device the host would pick, by the host's resolver. When that device is
 * not attached, the line is compared with `dinput_name` as text. Under
 * `provider = auto` the configured device is DirectInput only when
 * pick_device() would open one. */
void resolve_profile(void)
{
    const char *p = model_value("analog", "provider");
    ini_device_id_t ids[MODEL_ATTACHED_MAX];
    int ffb[MODEL_ATTACHED_MAX];
    unsigned n = 0, i;
    int which;

    s_profile[0] = '\0';
    s_profile_provider = NULL;
    if (!_stricmp(p, "xinput") || !_stricmp(p, "winmm")) {
        ini_device_id_t one;

        s_profile_provider = !_stricmp(p, "xinput") ? "xinput" : "winmm";
        one.id = one.name = "";
        one.index = model_uint("analog", !_stricmp(p, "xinput") ?
                               "xinput_device" : "winmm_device", UINT_MAX);
        model_device_label_find(s_profile_provider, &one, 1, 0, s_profile,
                                sizeof s_profile);
        return;
    }
    if (_stricmp(p, "dinput") && _stricmp(p, "auto"))
        return;
    if (s_attached)
        n = s_attached("dinput", ids, ffb, MODEL_ATTACHED_MAX);
    which = model_configured_device(ids, ffb, n);
    if (which >= 0) {
        s_profile_provider = "dinput";
        model_device_label_find("dinput", ids, n, (unsigned)which, s_profile,
                                sizeof s_profile);
        return;
    }
    if (!_stricmp(p, "dinput"))
        s_profile_provider = "dinput";
    if (!*model_value("analog", "dinput_name"))
        return;
    s_profile_provider = "dinput";
    table_load();
    for (i = 0; i < s_table.n; i++) {
        char kind[16], m[INI_MATCH_CAP], want[INI_MATCH_CAP];

        snprintf(want, sizeof want, "%s", model_value("analog", "dinput_name"));
        if (ini_device_entry(s_table.value[i], kind, sizeof kind, m, sizeof m) &&
            !_stricmp(kind, "dinput") && !_stricmp(m, ini_trim(want))) {
            snprintf(s_profile, sizeof s_profile, "%s", s_table.label[i]);
            return;
        }
    }
}

/* An action has up to two halves (a key in [digital], an ordinal in [gamepad]),
 * and the loader and the saver want the same three facts about each: its
 * section, its per-title fallback, and where this file keeps its value. The
 * PRECEDENCE (title section, shared section, table) lives here only. */
typedef struct {
    const char *section;                /* "digital" | "gamepad"            */
    const char *fallback;               /* the table's default, as text     */
    char       *value;                  /* what the launcher currently holds*/
    size_t      value_sz;
} binding_half_t;

static unsigned action_halves(unsigned title, unsigned action,
                              binding_half_t *out, char *pad_text,
                              size_t pad_text_sz)
{
    const action_desc_t *a = &k_actions[action];
    const char *key_default = title ? a->offroad_key : a->hydro_key;
    unsigned n = 0;

    if (key_default) {
        out[n].section  = "digital";
        out[n].fallback = key_default;
        out[n].value    = s_key_binding[title][action];
        out[n].value_sz = sizeof s_key_binding[0][0];
        n++;
    }
    if (a->has_pad) {
        snprintf(pad_text, pad_text_sz, "%u",
                 title ? a->offroad_pad : a->hydro_pad);
        out[n].section  = "gamepad";
        out[n].fallback = pad_text;
        out[n].value    = s_pad_binding[title][action];
        out[n].value_sz = sizeof s_pad_binding[0][0];
        n++;
    }
    return n;
}

/* Re-resolve the keys belonging to the configured device, now that it is known
 * which device that is. THE TWO-READ SHAPE IS dinput.c's, for the same reason:
 * the device's own section cannot be read until the keys choosing the device
 * have been. Both halves go through setting_resolve(). */
void reload_device_settings(void)
{
    unsigned i;

    for (i = 0; i < SETTING_COUNT; i++) {
        if (!setting_is_device_key(i))
            continue;
        if (!setting_resolve(i, s_value[i], sizeof s_value[i]))
            snprintf(s_value[i], sizeof s_value[i], "%s",
                     k_settings[i].fallback);
    }
    model_sync_role_facts(NULL);
}

/* WHAT THE DOCUMENT SAYS, INTO THE MODEL. The caller flushes anything it
 * measured from the old document first, see launcher.c's load_settings(),
 * whose only other line is the caption-height cache. */
void model_load(void)
{
    unsigned i, title, action;

    s_profile[0] = '\0';
    s_device_entry_pending = 0;
    memset(s_fact_section, 0, sizeof s_fact_section);
    for (i = 0; i < SETTING_COUNT; i++) {
        int unused;

        if (!ini_doc_get(&s_doc, k_settings[i].section, k_settings[i].key,
                         s_value[i], sizeof s_value[i]))
            snprintf(s_value[i], sizeof s_value[i], "%s",
                     k_settings[i].fallback);
        /* A checkbox cannot show "neither", so a misspelled boolean arrives
         * unticked and would be written back as `false` on Apply, turning a
         * typo into a setting. Every other type keeps its text and is caught
         * by validate_settings(), which can name it. */
        if (k_settings[i].type == ST_BOOL &&
            !ini_parse_bool(s_value[i], &unused))
            snprintf(s_value[i], sizeof s_value[i], "%s",
                     k_settings[i].fallback);
    }
    for (title = 0; title < 2; title++) {
        const char *id = title ? "offroad" : "hydro";
        char section[64], shared[64], pad_text[16];
        for (action = 0; action < ACTION_COUNT; action++) {
            const char *key = k_actions[action].key;
            binding_half_t half[2];
            unsigned h, n = action_halves(title, action, half,
                                          pad_text, sizeof pad_text);

            for (h = 0; h < n; h++) {
                if (!ini_doc_get(&s_doc, half[h].section, key,
                             shared, sizeof shared))
                    snprintf(shared, sizeof shared, "%s", half[h].fallback);
                snprintf(section, sizeof section, "%s.%s", id,
                         half[h].section);
                if (!ini_doc_get(&s_doc, section, key,
                             half[h].value, half[h].value_sz))
                    snprintf(half[h].value, half[h].value_sz, "%.*s",
                             (int)(half[h].value_sz - 1), shared);
            }
        }
    }
    /* NOW the device is known; provider and dinput_name are in the model,
     * so the keys that belong to it are read again from its own section. */
    resolve_profile();
    if (s_profile[0])
        reload_device_settings();
    model_sync_role_facts(NULL);
}

static int is_title_section(const char *section, const char *title)
{
    size_t n = strlen(title);
    return !_strnicmp(section, title, n) &&
           (section[n] == '\0' || section[n] == '.');
}

/* ---- what is in the file that this build will not use --------------------
 *
 * EVERY READER OF THIS FILE IGNORES AN UNKNOWN KEY IN SILENCE, AND HAS TO:
 * config.c walks the same lines the binding maps are on, so `boost = LCTRL` is
 * not one of its keys and is not a mistake either. The consequence is that a
 * key this build removed and a key with a typo in it read exactly alike. This
 * window is the one reader that knows BOTH tables, so it is the only one that
 * can say so.
 *
 * THE SECOND HALF IS A DUPLICATE SECTION, WORSE THAN A DEAD KEY: both halves
 * are live and they disagree. GetPrivateProfileString stops at the FIRST
 * [gamepad] and the game reads that one, while this window's document model is
 * last-wins and reads and WRITES the other.
 *
 * A REPORT, NOT A REPAIR: nothing here edits a line the user wrote. */
#define INI_PROBLEM_MAX 6
static char s_ini_problem[INI_PROBLEM_MAX][192];
static unsigned s_ini_problem_count;    /* how many are listed above        */
static unsigned s_ini_problem_total;    /* how many there are               */

static void add_ini_problem(const char *text)
{
    s_ini_problem_total++;
    if (s_ini_problem_count < INI_PROBLEM_MAX)
        snprintf(s_ini_problem[s_ini_problem_count++],
                 sizeof s_ini_problem[0], "%s", text);
}

/* Is this a key some reader will act on? Both tables, plus `diego_ms`, an old
 * spelling still read. */
static int ini_key_is_known(const char *key)
{
    unsigned i;

    for (i = 0; i < VCT_SETTING_COUNT; i++)
        if (!_stricmp(vct_setting[i].key, key))
            return 1;
    for (i = 0; i < ACTION_COUNT; i++)
        if (!_stricmp(k_actions[i].key, key))
            return 1;
    return !_stricmp(key, "diego_ms");
}

/* Is this key shown on some page of this window? A per-title section carrying
 * one is explained by that row itself (setting_title_override); one carrying
 * anything else has nowhere to be said, so it is reported here. */
static int ini_key_is_on_a_page(const char *key)
{
    unsigned i;

    for (i = 0; i < SETTING_COUNT; i++)
        if (!_stricmp(k_settings[i].key, key) && k_settings[i].page >= 0)
            return 1;
    return 0;
}

/* The section name with any `hydro.`/`offroad.` prefix taken off, and whether
 * there was one. A bare [hydro] is a title scope with no section. */
static const char *plain_title_section(const char *section, int *scoped)
{
    *scoped = 0;
    if (is_title_section(section, "hydro")) {
        *scoped = 1;
        return section[5] == '.' ? section + 6 : "";
    }
    if (is_title_section(section, "offroad")) {
        *scoped = 1;
        return section[7] == '.' ? section + 8 : "";
    }
    return section;
}

/* WHICH OF TWO LINES WINS DEPENDS ON WHO READS THE KEY, NOT ON THE SECTION.
 * config.c walks every line of the file, so for its keys the LAST line wins,
 * in a second block of the same name too. Everything read through the profile
 * API (the binding maps, [devices], a device profile and every
 * VCT_STORE_NONE row) gets the FIRST line of the FIRST block, and nothing
 * after it. This window is last-wins for both, so only the second kind
 * disagrees with it. Measured, not read: a second [diagnostics] carrying
 * `log_level = trace` put 346 trace lines in a dry-run's log against none
 * without it, and `boost = 2` then `boost = 5` in one [gamepad] resolved 2.
 *
 * 1 first-wins, 0 last-wins, -1 a key neither table knows (reported as such
 * elsewhere). `plain` has any title prefix already removed. */
static int key_reads_first(const char *plain, const char *key)
{
    unsigned i;

    if (!_stricmp(plain, "devices") || !_stricmp(plain, "digital") ||
        !_stricmp(plain, "gamepad") || !_strnicmp(plain, "analog.", 7))
        return 1;
    for (i = 0; i < VCT_SETTING_COUNT; i++)
        if (!_stricmp(vct_setting[i].section, plain) &&
            !_stricmp(vct_setting[i].key, key))
            return vct_setting[i].store == VCT_STORE_NONE;
    return -1;
}

/* The same question asked of a whole section: 1 every key it can hold is
 * first-wins, 0 every one is config.c's, 2 it holds both kinds, -1 neither
 * table knows the section. */
static int section_reads_first(const char *plain)
{
    unsigned i;
    int first = 0, last = 0;

    if (key_reads_first(plain, "") == 1)
        return 1;
    for (i = 0; i < VCT_SETTING_COUNT; i++)
        if (!_stricmp(vct_setting[i].section, plain)) {
            if (vct_setting[i].store == VCT_STORE_NONE)
                first = 1;
            else
                last = 1;
        }
    if (first && last)
        return 2;
    return first ? 1 : last ? 0 : -1;
}

void scan_ini_problems(void)
{
    char current[96] = "";
    size_t i, j, block = 0;

    s_ini_problem_count = 0;
    s_ini_problem_total = 0;
    for (i = 0; i < s_doc.count; i++) {
        char name[96], key[96], text[192];
        const char *plain;
        int scoped;

        if (ini_doc_section(s_doc.line[i], name, sizeof name)) {
            char earlier[96] = "";
            int seen = 0;

            for (j = 0; j < i && !seen; j++)
                if (ini_doc_section(s_doc.line[j], earlier, sizeof earlier) &&
                    !_stricmp(earlier, name))
                    seen = 1;
            if (seen) {
                /* Said the way THIS section is read; see key_reads_first(). */
                switch (section_reads_first(plain_title_section(name,
                                                                &scoped))) {
                case 0:
                    snprintf(text, sizeof text,
                             "[%s] appears twice. The game reads both, and "
                             "where they set one key the later line wins, as "
                             "in this window. Merge them into one.", name);
                    break;
                case 2:
                    snprintf(text, sizeof text,
                             "[%s] appears twice. The game reads some of its "
                             "keys from the first one only; this window reads "
                             "and writes the second. Merge them into one.",
                             name);
                    break;
                default:
                    snprintf(text, sizeof text,
                             "[%s] appears twice. The game reads the first one "
                             "and ignores everything in the second; this "
                             "window reads and writes the second. Merge them "
                             "into one.", name);
                    break;
                }
                add_ini_problem(text);
            }
            snprintf(current, sizeof current, "%s", name);
            block = i;
            continue;
        }
        if (!ini_doc_line_key(s_doc.line[i], key, sizeof key))
            continue;
        plain = plain_title_section(current, &scoped);
        /* THE SAME KEY TWICE IN ONE BLOCK, the half of the duplicate-section
         * report nobody had written: pasting a line under a section that
         * already carries it is the easier mistake of the two. Reported once,
         * at the second line; a copy in a second block of the same name is the
         * section report's. */
        {
            char earlier[96];
            unsigned copies = 0;

            for (j = block + 1; j < i; j++)
                if (ini_doc_line_key(s_doc.line[j], earlier, sizeof earlier) &&
                    !_stricmp(earlier, key))
                    copies++;
            if (copies == 1) {
                int first = key_reads_first(plain, key);

                if (first == 1)
                    snprintf(text, sizeof text,
                             "[%s] %s is set twice. The game uses the FIRST "
                             "line; this window shows and changes the last. "
                             "Delete one of them.", current, key);
                else if (first == 0)
                    snprintf(text, sizeof text,
                             "[%s] %s is set twice. The later line wins and "
                             "the earlier one does nothing. Delete one of "
                             "them.", current, key);
                if (first >= 0)
                    add_ini_problem(text);
            }
        }
        /* [devices] is the one section whose KEYS are the user's own words. */
        if (!_stricmp(plain, "devices"))
            continue;
        if (!ini_key_is_known(key)) {
            /* A key before the file's first [section] has no section, and
             * "[] r2_cache" reads as a defect in this message rather than in
             * the file. Dead either way: the profile API cannot address it. */
            if (current[0])
                snprintf(text, sizeof text,
                         "[%s] %s is not a setting or a control this build "
                         "has. Nothing reads that line.", current, key);
            else
                snprintf(text, sizeof text,
                         "%s is above the first [section], so nothing can "
                         "read it wherever it belongs.", key);
            add_ini_problem(text);
            continue;
        }
        /* A per-title override of something this window shows is said by the
         * row; one of anything else has no other place to appear. The binding
         * sections are per-title BY DESIGN and are not overrides at all. */
        if (scoped && _stricmp(plain, "digital") && _stricmp(plain, "gamepad") &&
            !ini_key_is_on_a_page(key)) {
            snprintf(text, sizeof text,
                     "[%s] %s is set for one cabinet only. This window edits "
                     "the settings both cabinets share, so it cannot show you "
                     "that line.", current, key);
            add_ini_problem(text);
        }
    }
}

static int parse_unsigned_setting(const setting_desc_t *d, const char *value)
{
    unsigned n;

    if (d->type == ST_AUTO_UINT && !_stricmp(value, "auto"))
        return 1;
    return ini_parse_uint(value, (unsigned)d->minimum, (unsigned)d->maximum,
                          &n);
}

static int key_name_valid(const char *value)
{
    static const char *const names[] = {
        "NONE", "LEFT", "RIGHT", "UP", "DOWN",
        "LCTRL", "LCONTROL", "RCTRL", "RCONTROL",
        "LALT", "LMENU", "RALT", "RMENU", "LSHIFT", "RSHIFT",
        "SPACE", "ENTER", "ESC", "ESCAPE", "TAB", "BACKSPACE",
        "MINUS", "EQUALS", "COMMA", "PERIOD", "HOME", "END",
        "PGUP", "PGDN", "INSERT", "DELETE"
    };
    const char *s = value;
    char *end;
    unsigned long n;
    unsigned i;

    if (!_strnicmp(s, "KEYCODE_", 8))
        s += 8;
    if (s[0] && !s[1] && (isalnum((unsigned char)s[0]) ||
                           s[0] == '-' || s[0] == '='))
        return 1;
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        n = strtoul(s + 1, &end, 10);
        if (!*end && n >= 1 && n <= 24)
            return 1;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_stricmp(s, names[i]))
            return 1;
    n = strtoul(s, &end, 0);
    return end != s && !*end && n <= 0xFF;
}

/* A [gamepad] VALUE, BY THE GRAMMAR THE HOST READS IT WITH: the elements come
 * from ini.c, because a form that refuses what the host accepts cannot edit a
 * file a user hand-wrote (R1). The launcher's own part is the two CEILINGS,
 * which live in io.h, IO_MAX_BUTTONS, not 32, or a Pro DD's button 40 is
 * visible in the live preview and rejected by the form.
 *
 * THE LABEL IS NOT CHECKED AGAINST [devices] HERE: a user may write the binding
 * before the entry, or unplug the device while editing. The host says so at
 * startup, per binding, with all three reasons. */
/* THE WHOLE VALUE, WHICH IS A LIST. One element is every file written before
 * the keyboard half became a list and parses exactly as it did. */
static int keyboard_binding_valid(const char *value)
{
    char item[64];
    unsigned i;

    for (i = 0; ini_list_item(value, i, item, sizeof item); i++) {
        if (i >= IO_MAX_KEY_REFS)
            return 0;
        if (!key_name_valid(item))
            return 0;
    }
    return 1;
}

static int pad_binding_valid(const char *value)
{
    char item[64], label[INI_LABEL_CAP];
    unsigned i, ordinal;

    for (i = 0; ini_list_item(value, i, item, sizeof item); i++) {
        if (i >= IO_MAX_PAD_REFS)
            return 0;
        if (!ini_parse_pad_ref(item, label, sizeof label, &ordinal))
            return 0;
        if (ordinal > IO_MAX_BUTTONS)
            return 0;
        /* `<label>:0` is button zero of a named device, and buttons count from
         * one. A bare `0` is how the file spells unbound and stays legal. */
        if (!ordinal && label[0])
            return 0;
    }
    return 1;
}

/* `page` gets the page the offending row is on, so a caller can say WHERE to
 * go and not only what is wrong. The Launch page is the reason it exists: an
 * invalid value greys both Play buttons from a page that shows neither the
 * value nor the page it is on, and a user who never typed anything, the
 * value can have been in the file before this window opened; is left with a
 * dead button and nothing to read. PAGE_HOME means "no page of its own", which
 * is what a binding is: those live on Controls. */
int validate_settings(char *why, size_t why_size, int *page)
{
    unsigned i, title, action;

    if (page)
        *page = PAGE_HOME;

    for (i = 0; i < SETTING_COUNT; i++) {
        const setting_desc_t *d = &k_settings[i];
        const char *v = s_value[i];
        if ((d->flags & SF_REQUIRED) && !*v) {
            snprintf(why, why_size, "%s cannot be empty.", d->label);
            if (page)
                *page = d->page;
            return 0;
        }
        if (d->type == ST_ENUM &&
            !((d->flags & SF_AXIS_REF) ? vct_axis_choice_ok(d->choices, v)
                                   : vct_choice_contains(d->choices, v))) {
            snprintf(why, why_size, "%s is not a supported value.", d->label);
            if (page)
                *page = d->page;
            return 0;
        }
        if ((d->type == ST_UINT || d->type == ST_AUTO_UINT ||
             d->type == ST_MHZ_UINT) &&
            !parse_unsigned_setting(d, v)) {
            if (d->type == ST_MHZ_UINT)
                snprintf(why, why_size, "%s must be 1..4000.", d->label);
            else
                snprintf(why, why_size, "%s must be %s%lu..%lu.", d->label,
                         d->type == ST_AUTO_UINT ? "automatic or " : "",
                         d->minimum, d->maximum);
            if (page)
                *page = d->page;
            return 0;
        }
    }
    for (title = 0; title < 2; title++)
        for (action = 0; action < ACTION_COUNT; action++) {
            if ((title ? k_actions[action].offroad_key :
                         k_actions[action].hydro_key) &&
                !keyboard_binding_valid(s_key_binding[title][action])) {
                snprintf(why, why_size,
                         "%s keyboard binding is not supported.",
                         title ? k_actions[action].offroad_label :
                                 k_actions[action].hydro_label);
                if (page)
                    *page = PAGE_CONTROLS;
                return 0;
            }
            if (k_actions[action].has_pad &&
                !pad_binding_valid(s_pad_binding[title][action])) {
                snprintf(why, why_size,
                         "%s controller binding must be a button number "
                         "0..%d, optionally with a controller from [devices] "
                         "and a colon in front of it, and up to %d of them "
                         "separated by commas.",
                         title ? k_actions[action].offroad_label :
                                 k_actions[action].hydro_label,
                         IO_MAX_BUTTONS, IO_MAX_PAD_REFS);
                if (page)
                    *page = PAGE_CONTROLS;
                return 0;
            }
        }
    return 1;
}

/* A binding equal to what the title would inherit is REMOVED rather than
 * written, so the file keeps only what the user actually changed.
 *
 * ONLY WHEN IT CHANGED TO IT. Removing on every Apply, for every binding whose
 * value happens to equal the inherited one, comments out lines from under a
 * file that spells the inheritance out anyway, which is what `make pack-ini`
 * ships and what a user who likes reading their own configuration writes: an
 * Apply that changed nothing else would still move six lines.
 *
 * `*changed` counts what this actually wrote, so Apply can tell a save from a
 * file that already said all of it. */
static int save_bindings_to_doc(int *changed)
{
    unsigned title, action;
    char section[64], shared[64], pad_text[16];

    for (title = 0; title < 2; title++) {
        const char *id = title ? "offroad" : "hydro";
        for (action = 0; action < ACTION_COUNT; action++) {
            const char *key = k_actions[action].key;
            binding_half_t half[2];
            unsigned h, n = action_halves(title, action, half,
                                          pad_text, sizeof pad_text);

            for (h = 0; h < n; h++) {
                char have[64];
                int present;

                if (!ini_doc_get(&s_doc, half[h].section, key,
                             shared, sizeof shared))
                    snprintf(shared, sizeof shared, "%s", half[h].fallback);
                snprintf(section, sizeof section, "%s.%s", id,
                         half[h].section);
                present = ini_doc_get(&s_doc, section, key, have, sizeof have);
                /* THE FILE ALREADY SAYS THIS. Checked before the inheritance
                 * test and not after it, because a line that spells out what
                 * the title would have inherited anyway is still the user's
                 * line and still unchanged. */
                if (present && !strcmp(have, half[h].value))
                    continue;
                if (!_stricmp(shared, half[h].value)) {
                    if (!present)
                        continue;
                    if (!ini_doc_remove(&s_doc, section, key))
                        return 0;
                } else if (!ini_doc_set(&s_doc, section, key, half[h].value)) {
                    return 0;
                }
                (*changed)++;
            }
        }
    }
    return 1;
}

/* ---- the known-good copy ------------------------------------------------
 *
 * A FULL INI WITH THE USER'S WHEEL MAP INTACT, AND A ONE-CLICK RESTORE. This
 * window writes a configuration a user hand-tunes, so there has to be something
 * to go back to on the day it writes the wrong one.
 *
 * IT IS NEVER WRITTEN AUTOMATICALLY, except once when there is none at all.
 * That seed is the file as this launcher first opened it, the last state this
 * window cannot have damaged. After that only the button writes it, and that is
 * the point: a copy Apply refreshed would be a copy of the mistake, which is a
 * recovery file that recovers the thing it was made to undo.
 */
void known_good_path(char *out, size_t out_size)
{
    /* %.*s, for ini_doc_save()'s reason: doc.path is itself MAX_PATH, and a
     * truncation that dropped the suffix would name the REAL file, so
     * "saving a known-good copy" would overwrite the settings with
     * themselves and "reverting" would be a no-op that reported success. */
    snprintf(out, out_size, "%.*s.good",
             (int)(out_size - sizeof ".good"), s_doc.path);
}

/* One sentence for the page: whether there is a copy, and when it was made. */
void known_good_state(char *out, size_t out_size)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    FILETIME local;
    SYSTEMTIME when;
    char path[MAX_PATH];

    known_good_path(path, sizeof path);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) {
        snprintf(out, out_size, "There is no known-good copy yet.");
        return;
    }
    if (FileTimeToLocalFileTime(&info.ftLastWriteTime, &local) &&
        FileTimeToSystemTime(&local, &when))
        snprintf(out, out_size,
                 "The known-good copy was saved %04u-%02u-%02u %02u:%02u.",
                 when.wYear, when.wMonth, when.wDay, when.wHour, when.wMinute);
    else
        snprintf(out, out_size, "A known-good copy exists.");
}

void seed_known_good(void)
{
    char path[MAX_PATH];

    known_good_path(path, sizeof path);
    /* A first run has no ini yet and there is nothing good about an absent
     * file. FALSE for fail-if-exists is inverted here ON PURPOSE: TRUE means
     * "do not overwrite", which is the entire contract of this function. */
    if (GetFileAttributesA(s_doc.path) != INVALID_FILE_ATTRIBUTES)
        CopyFileA(s_doc.path, path, TRUE);
}
const char *model_profile(void)
{
    return s_profile;
}

void model_device_entry_added(void)
{
    s_device_entry_pending++;
}

/* ---- which controller each [devices] line opens --------------------------
 *
 * The host's resolver over the document, so unsaved lines count. The whole
 * table is resolved each time, because a pin changes what a name line opens. */

static void table_load(void)
{
    char label[INI_LABEL_CAP];
    unsigned i;

    s_table.n = 0;
    for (i = 0; s_table.n < INI_DEVICES_MAX &&
                ini_doc_key_index(&s_doc, "devices", i, label, sizeof label);
         i++) {
        if (!ini_label_ok(label) ||
            !ini_doc_get(&s_doc, "devices", label, s_table.value[s_table.n],
                         sizeof s_table.value[0]))
            continue;
        snprintf(s_table.label[s_table.n], sizeof s_table.label[0], "%s",
                 label);
        s_table.v[s_table.n] = s_table.value[s_table.n];
        s_table.n++;
    }
}

/* Resolve the loaded table, with `line` appended when not NULL (its answer is
 * s_table.at[s_table.n]); returns where `extra` resolves. */
static int table_resolve(const char *line, const char *extra,
                         const char *provider, const ini_device_id_t *dev,
                         unsigned count)
{
    unsigned n = s_table.n;

    if (line && n < INI_DEVICES_MAX)
        s_table.v[n++] = line;
    return ini_devices_resolve(s_table.v, n, extra, provider, dev, count,
                               s_table.at);
}

int model_device_label_find(const char *provider, const ini_device_id_t *dev,
                            unsigned count, unsigned which,
                            char *out, size_t out_size)
{
    unsigned i;

    if (which >= count)
        return 0;
    table_load();
    table_resolve(NULL, NULL, provider, dev, count);
    for (i = 0; i < s_table.n; i++)
        if (s_table.at[i] == (int)which) {
            if (out)
                snprintf(out, out_size, "%s", s_table.label[i]);
            return 1;
        }
    return 0;
}

int model_device_for_label(const char *label, const char *provider,
                           const ini_device_id_t *dev, unsigned count)
{
    unsigned i;

    table_load();
    table_resolve(NULL, NULL, provider, dev, count);
    for (i = 0; i < s_table.n; i++)
        if (!_stricmp(s_table.label[i], label))
            return s_table.at[i];
    return -1;
}

int model_configured_device(const ini_device_id_t *dev, const int *ffb,
                            unsigned count)
{
    const char *name = model_value("analog", "dinput_name");
    unsigned k;

    if (*name) {
        char value[INI_MATCH_CAP + 16];

        snprintf(value, sizeof value, "dinput:%s", name);
        table_load();
        return table_resolve(NULL, value, "dinput", dev, count);
    }
    /* The auto rule: the first controller publishing effects. */
    for (k = 0; ffb && k < count; k++)
        if (ffb[k])
            return (int)k;
    return -1;
}

/* What to write so that it opens `dev[which]`, proved before it is returned:
 * the device's name when that opens it, else its full ID; a numbered provider
 * writes the number. As a line it must also leave every existing line opening
 * what it opened; as `dinput_name` only its own answer matters. */
int model_device_match(const char *provider, const ini_device_id_t *dev,
                       unsigned count, unsigned which, int as_line,
                       char *out, size_t out_size)
{
    char match[2][INI_MATCH_CAP], value[INI_MATCH_CAP + 16];
    char named[INI_MATCH_CAP + 16];
    const char *extra = NULL;
    int before[INI_DEVICES_MAX], before_extra;
    unsigned k, nmatch = 0, i;

    if (which >= count)
        return 0;
    if (dev[which].id && dev[which].id[0]) {
        if (dev[which].name && dev[which].name[0])
            snprintf(match[nmatch++], sizeof match[0], "%s", dev[which].name);
        snprintf(match[nmatch++], sizeof match[0], "%s", dev[which].id);
    } else {
        snprintf(match[nmatch++], sizeof match[0], "%u", dev[which].index);
    }
    /* A new line must not move the device dinput_name opens either. */
    if (as_line && !_stricmp(provider, "dinput") &&
        *model_value("analog", "dinput_name")) {
        snprintf(named, sizeof named, "dinput:%s",
                 model_value("analog", "dinput_name"));
        extra = named;
    }
    table_load();
    before_extra = table_resolve(NULL, extra, provider, dev, count);
    memcpy(before, s_table.at, s_table.n * sizeof before[0]);
    for (k = 0; k < nmatch; k++) {
        int got;

        snprintf(value, sizeof value, "%s:%s", provider, match[k]);
        if (as_line) {
            if (s_table.n >= INI_DEVICES_MAX)
                return 0;
            got = table_resolve(value, extra, provider, dev, count) ==
                  before_extra ? s_table.at[s_table.n] : -1;
            for (i = 0; i < s_table.n && got == (int)which; i++)
                if (s_table.at[i] != before[i])
                    got = -1;
        } else {
            got = table_resolve(NULL, value, provider, dev, count);
        }
        if (got == (int)which) {
            snprintf(out, out_size, "%s", match[k]);
            return 1;
        }
    }
    return 0;
}

/* The label of a line that opens `dev[which]`, or a new line that provably
 * does (model_device_match()). */
int model_device_label_ensure(const char *provider, const ini_device_id_t *dev,
                              unsigned count, unsigned which, const char *base,
                              char *out, size_t out_size)
{
    char match[INI_MATCH_CAP], value[INI_MATCH_CAP + 16];
    char label[INI_LABEL_CAP], existing[INI_VALUE_CAP];
    unsigned suffix;

    if (which >= count || !base || !ini_label_ok(base))
        return 0;
    if (model_device_label_find(provider, dev, count, which, out, out_size))
        return 1;
    if (!model_device_match(provider, dev, count, which, 1, match,
                            sizeof match))
        return 0;
    snprintf(value, sizeof value, "%s:%s", provider, match);

    snprintf(label, sizeof label, "%s", base);
    for (suffix = 2;
         suffix < 100 &&
         ini_doc_get(&s_doc, "devices", label, existing, sizeof existing);
         suffix++)
        snprintf(label, sizeof label, "%.*s%u",
                 (int)(sizeof label - 4), base, suffix);
    if (ini_doc_get(&s_doc, "devices", label, existing, sizeof existing) ||
        !ini_doc_set(&s_doc, "devices", label, value))
        return 0;
    model_device_entry_added();
    if (out)
        snprintf(out, out_size, "%s", label);
    return 1;
}

unsigned model_problem_count(void)
{
    return s_ini_problem_count;
}

unsigned model_problem_total(void)
{
    return s_ini_problem_total;
}

const char *model_problem(unsigned i)
{
    return i < s_ini_problem_count ? s_ini_problem[i] : "";
}

/* ---- the document's life ------------------------------------------------- */

int model_open(const char *ini_path)
{
    return ini_doc_load(&s_doc, ini_path);
}

int model_reopen(void)
{
    char path[MAX_PATH];

    /* ini_doc_free() zeroes the document, path included, so take it first. */
    snprintf(path, sizeof path, "%s", s_doc.path);
    ini_doc_free(&s_doc);
    return ini_doc_load(&s_doc, path);
}

void model_close(void)
{
    ini_doc_free(&s_doc);
}

/* ---- saving --------------------------------------------------------------
 *
 * THE QUESTION THIS DOES NOT ASK. Video mode 2 is 640x480, which is for a
 * bench LCD and may damage an original arcade monitor, so Apply asks before
 * writing it. Asking is a window's job and the answer changes nothing here,
 * so it is a separate predicate rather than a callback into the caller. */
int model_unsafe_video_mode(void)
{
    /* An absent key gives -1, and the comparison below is unsigned, so it
     * wraps to a value no index can reach. Said out loud rather than left as
     * a thing the reader has to notice. */
    unsigned i = (unsigned)setting_find("graphics", "video_mode");

    return i < SETTING_COUNT && k_settings[i].warning == WARNING_DANGER &&
           !strcmp(s_value[i], "2");
}

int model_save(char *why, size_t why_size, int *changed)
{
    unsigned i;

    *changed = 0;
    model_sync_role_facts(NULL);
    /* R4: NEVER WRITE A KEY THE USER DID NOT TOUCH.
     *
     * Setting all SETTING_COUNT rows on every Apply INJECTS the launcher's own
     * fallback for every key the file does not carry, a hundred lines of
     * "configuration" nobody chose, in the one file a user hand-edits. A key
     * the file already spells this way is left exactly as it is: its spacing,
     * its ordering and its trailing comment included.
     *
     * AN ABSENT KEY IS COMPARED AGAINST THE FALLBACK, because that is what
     * absent means here: model_load() seeded s_value[] from it, so a row still
     * holding its fallback is a row nobody has touched. */
    for (i = 0; i < SETTING_COUNT; i++) {
        char have[INI_VALUE_CAP];
        const char *section = setting_section(i);

        if (ini_doc_get(&s_doc, section, k_settings[i].key,
                        have, sizeof have)) {
            if (!strcmp(have, s_value[i]))
                continue;
        } else {
            /* Absent from the section this would WRITE. For an ordinary key
             * that means absent from the file, and the fallback is what
             * model_load() put in the model. For a key that belongs to the
             * configured device it also means "[analog] still answers it", and
             * a value the shared section already supplies must not be copied
             * down into the device's section: that would write a key the user
             * did not touch, in a new place, which is R4 with a profile on. */
            char resolved[INI_VALUE_CAP];

            if (!setting_resolve(i, resolved, sizeof resolved))
                snprintf(resolved, sizeof resolved, "%s",
                         k_settings[i].fallback);
            if (!strcmp(s_value[i], resolved))
                continue;
        }
        if (!ini_doc_set(&s_doc, section, k_settings[i].key, s_value[i])) {
            snprintf(why, why_size,
                     "Not enough memory to update vcthunder.ini.");
            return MODEL_SAVE_NO_MEMORY;
        }
        (*changed)++;
    }
    *changed += s_device_entry_pending;
    if (!save_bindings_to_doc(changed)) {
        snprintf(why, why_size,
                 "Not enough memory to update title bindings.");
        return MODEL_SAVE_NO_MEMORY;
    }
    /* NOTHING TO WRITE IS NOT A FAILURE, AND IT MUST NOT WRITE. Saving an
     * identical file would still replace vcthunder.ini.bak with a copy of the
     * current one, which is the recovery a user reaches for after an Apply
     * they did not mean. */
    if (!*changed)
        return MODEL_SAVE_OK;
    if (!ini_doc_save(&s_doc, why, why_size))
        return MODEL_SAVE_FAILED;
    s_device_entry_pending = 0;
    return MODEL_SAVE_OK;
}

/* ---- the known-good copy, the half that touches files -------------------- */

int model_known_good_exists(void)
{
    char path[MAX_PATH];

    known_good_path(path, sizeof path);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int model_known_good_write(char *why, size_t why_size)
{
    char path[MAX_PATH];

    known_good_path(path, sizeof path);
    if (CopyFileA(s_doc.path, path, FALSE))
        return 1;
    snprintf(why, why_size, "Could not write %s (Windows error %lu).",
             path, GetLastError());
    return 0;
}

int model_known_good_restore(char *why, size_t why_size)
{
    char path[MAX_PATH], bak[MAX_PATH], ini[MAX_PATH];

    known_good_path(path, sizeof path);
    snprintf(ini, sizeof ini, "%s", s_doc.path);
    snprintf(bak, sizeof bak, "%.*s.bak",
             (int)(sizeof bak - sizeof ".bak"), ini);
    if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES &&
        !CopyFileA(ini, bak, FALSE)) {
        snprintf(why, why_size, "Could not back up %s (Windows error %lu). "
                 "Nothing was changed.", ini, GetLastError());
        return 0;
    }
    if (!CopyFileA(path, ini, FALSE)) {
        snprintf(why, why_size, "Could not replace %s (Windows error %lu). "
                 "Nothing was changed.", ini, GetLastError());
        return 0;
    }
    return 1;
}
