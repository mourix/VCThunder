/* ini.c -- the value grammar of vcthunder.ini, in one place.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See src/ini.h for what this is and, more importantly, what it is not: the
 * three readers over this file stay three readers. Only the grammar is shared.
 */
#include "ini.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *ini_trim_left(char *s)
{
    while (*s && isspace((unsigned char)*s) && *s != '\r' && *s != '\n')
        s++;
    return s;
}

void ini_trim_right(char *s)
{
    size_t n = strlen(s);

    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

char *ini_trim(char *s)
{
    s = ini_trim_left(s);
    ini_trim_right(s);
    return s;
}

const char *ini_comment_start(const char *text)
{
    const char *p;

    for (p = text; *p; p++)
        if ((*p == ';' || *p == '#') &&
            (p == text || isspace((unsigned char)p[-1])))
            return p;
    return NULL;
}

void ini_strip_comment(char *text)
{
    /* The one cast: the caller handed us a mutable string, so the position
     * ini_comment_start() found in it is mutable too. */
    char *p = (char *)ini_comment_start(text);

    if (p)
        *p = '\0';
    ini_trim_right(text);
}

int ini_parse_bool(const char *v, int *out)
{
    if (!v)
        return 0;
    if (_stricmp(v, "true") == 0 || _stricmp(v, "yes") == 0 ||
        _stricmp(v, "on") == 0 || strcmp(v, "1") == 0)
        *out = 1;
    else if (_stricmp(v, "false") == 0 || _stricmp(v, "no") == 0 ||
             _stricmp(v, "off") == 0 || strcmp(v, "0") == 0)
        *out = 0;
    else
        return 0;
    return 1;
}

/* strtoul accepts a leading '-' and wraps it, so "-1" reads as 4294967295 and
 * lands inside almost any range. Refuse the sign before parsing rather than
 * hoping the range catches it: io_config.c's own copy did not, and a negative
 * deadzone came out as an enormous one. */
int ini_parse_uint(const char *v, unsigned lo, unsigned hi, unsigned *out)
{
    char *end;
    unsigned long n;

    if (!v || !*v || *v == '-')
        return 0;
    errno = 0;
    n = strtoul(v, &end, 0);
    if (errno == ERANGE || end == v || *end != '\0' ||
        n < (unsigned long)lo || n > (unsigned long)hi)
        return 0;
    *out = (unsigned)n;
    return 1;
}

int ini_parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long n;

    if (!v || !*v)
        return 0;
    errno = 0;
    n = strtol(v, &end, 0);
    if (errno == ERANGE || end == v || *end != '\0' ||
        n < (long)lo || n > (long)hi)
        return 0;
    *out = (int)n;
    return 1;
}

int ini_bool_or(const char *v, int fallback)
{
    int n;

    return ini_parse_bool(v, &n) ? n : fallback;
}

unsigned ini_uint_or(const char *v, unsigned lo, unsigned hi, unsigned fallback)
{
    unsigned n;

    return ini_parse_uint(v, lo, hi, &n) ? n : fallback;
}

void ini_read_scoped(const char *path, const char *section, const char *key,
                     const char *fallback, char *out, DWORD out_size)
{
    char scoped[64];

    /* 1. this game's own scoped section */
    if (G && G->id[0]) {
        snprintf(scoped, sizeof scoped, "%s.%s", G->id, section);
        GetPrivateProfileStringA(scoped, key, "", out, out_size, path);
        ini_strip_comment(out);
        if (out[0])
            return;
    }
    /* 2. the plain section both cabinets share */
    GetPrivateProfileStringA(section, key, fallback, out, out_size, path);
    ini_strip_comment(out);
}

void ini_resolve_path(const char *in, char *out, size_t out_size)
{
    DWORD n = GetFullPathNameA(in, (DWORD)out_size, out, NULL);

    if (!n || n >= out_size)
        snprintf(out, out_size, "%s", in);
}

/* ---- devices: the [devices] table and the device scope ------------------ */

/* Case-insensitive substring, HERE and not in dinput.c because
 * `[devices] tmx = dinput:Thrustmaster` and `[analog] dinput_name =
 * Thrustmaster` are one question asked twice. An empty needle matches nothing:
 * "any device at all" is what leaving the key out already says.
 *
 * Written out rather than reached for in shlwapi: a fifth system DLL for one
 * string search is not a trade. */
int ini_name_contains(const char *haystack, const char *needle)
{
    size_t n, i;

    if (!haystack || !needle)
        return 0;
    n = strlen(needle);
    if (!n)
        return 0;
    for (i = 0; haystack[i]; i++)
        if (!_strnicmp(haystack + i, needle, n))
            return 1;
    return 0;
}

/* A LABEL IS SHORT, BOUNDED AND TYPABLE, which is why the device scope is not
 * keyed on the product name: `[offroad.analog.dinput:Bluetooth LE XINPUT
 * compatible input device]` is 65 characters against two char[64] section
 * buffers, so a product-name scope works on the wheel it is designed against
 * and truncates on the device that motivated it. With the label bounded here
 * the longest section the walker can build is 38 characters. */
int ini_label_ok(const char *label)
{
    size_t i;

    if (!label || !*label)
        return 0;
    for (i = 0; label[i]; i++) {
        if (i >= INI_LABEL_CAP - 1)
            return 0;
        if (!isalnum((unsigned char)label[i]) &&
            label[i] != '_' && label[i] != '-')
            return 0;
    }
    return 1;
}

int ini_device_entry(const char *value, char *provider, size_t provider_size,
                     char *match, size_t match_size)
{
    const char *colon;
    size_t n;

    if (!value)
        return 0;
    while (*value == ' ' || *value == '\t')
        value++;
    colon = strchr(value, ':');
    if (!colon)
        return 0;
    n = (size_t)(colon - value);
    while (n && (value[n - 1] == ' ' || value[n - 1] == '\t'))
        n--;
    if (!n || n >= provider_size)
        return 0;
    memcpy(provider, value, n);
    provider[n] = '\0';

    value = colon + 1;
    while (*value == ' ' || *value == '\t')
        value++;
    n = strlen(value);
    while (n && isspace((unsigned char)value[n - 1]))
        n--;
    if (!n || n >= match_size)
        return 0;
    memcpy(match, value, n);
    match[n] = '\0';
    return 1;
}

/* MAME's make_id() and guid_to_string(), with the product name (ini.h). */
void ini_device_id(char *out, size_t out_size, const char *name,
                   const GUID *product, const GUID *instance)
{
    char trimmed[MAX_PATH], g[2][40];
    const GUID *guid[2];
    unsigned k;

    guid[0] = product;
    guid[1] = instance;
    for (k = 0; k < 2; k++)
        snprintf(g[k], sizeof g[k],
                 "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 (unsigned long)guid[k]->Data1, guid[k]->Data2, guid[k]->Data3,
                 guid[k]->Data4[0], guid[k]->Data4[1], guid[k]->Data4[2],
                 guid[k]->Data4[3], guid[k]->Data4[4], guid[k]->Data4[5],
                 guid[k]->Data4[6], guid[k]->Data4[7]);
    snprintf(trimmed, sizeof trimmed, "%s", name ? name : "");
    snprintf(out, out_size, "%s product_%s instance_%s", ini_trim(trimmed),
             g[0], g[1]);
}

int ini_device_is_pin(const char *value)
{
    return ini_name_contains(value, "instance_");
}

/* A match names a GUID only by its prefix; otherwise it is a substring of the
 * name the ID starts with, because a short hex match (`2`, `dd`) would hit
 * some GUID by chance. */
static int id_contains(const char *id, const char *m)
{
    const char *guids = strstr(id, " product_");
    char name[INI_MATCH_CAP];

    if (!guids || ini_name_contains(m, "product_") ||
        ini_name_contains(m, "instance_"))
        return ini_name_contains(id, m);
    snprintf(name, sizeof name, "%.*s", (int)(guids - id), id);
    return ini_name_contains(name, m);
}

/* On a provider with names the match is part of the device's ID
 * (id_contains()), as for dinput_name. XInput has no names: the match is the
 * pad's number, compared as a number ("0" must not match "10"). */
int ini_device_matches(const char *value, const char *provider,
                       const char *id, unsigned index)
{
    char p[16], m[INI_MATCH_CAP];
    unsigned n;

    if (!ini_device_entry(value, p, sizeof p, m, sizeof m))
        return 0;
    if (_stricmp(p, provider) != 0)
        return 0;
    if (id && *id)
        return id_contains(id, m);
    return ini_parse_uint(m, 0, UINT_MAX, &n) && n == index;
}

/* The first device `value` matches that `claimed` does not hold, or -1. */
static int resolve_one(const char *value, const char *provider,
                       const ini_device_id_t *dev, unsigned count,
                       const unsigned char *claimed)
{
    unsigned d;

    for (d = 0; d < count; d++)
        if ((!claimed || !claimed[d]) &&
            ini_device_matches(value, provider, dev[d].id, dev[d].index))
            return (int)d;
    return -1;
}

int ini_devices_resolve(const char *const *lines, unsigned n,
                        const char *extra, const char *provider,
                        const ini_device_id_t *dev, unsigned count, int *out)
{
    unsigned char claimed[INI_DEVICES_MAX];
    unsigned i;

    if (count > INI_DEVICES_MAX)
        count = INI_DEVICES_MAX;
    memset(claimed, 0, sizeof claimed);
    for (i = 0; i < n; i++) {
        out[i] = -1;
        if (ini_device_is_pin(lines[i]) &&
            (out[i] = resolve_one(lines[i], provider, dev, count, NULL)) >= 0)
            claimed[out[i]] = 1;
    }
    for (i = 0; i < n; i++)
        if (!ini_device_is_pin(lines[i]))
            out[i] = resolve_one(lines[i], provider, dev, count, claimed);
    if (!extra)
        return -1;
    return resolve_one(extra, provider, dev, count,
                       ini_device_is_pin(extra) ? NULL : claimed);
}

/* THE FIVE-STEP ORDER, WRITTEN DOWN RATHER THAN INHERITED.
 *
 * ini_read_scoped() cannot do this: it prepends G->id as a PREFIX, so handed
 * `analog.tmx` it looks in [hydro.analog.tmx], then [analog.tmx], and stops,
 * never reaching [analog], which every file written before profiles depends on.
 * A device scope on top of a title scope is a 2x2 and both axes must be walked.
 *
 * DEVICE BEATS TITLE: a wrong axis map makes a device undriveable, while a
 * title scope only makes one cabinet differ from the other.
 *
 * With no label this IS ini_read_scoped(), by construction: steps 1 and 2 are
 * skipped and 3, 4 and 5 are its own three. */
void ini_read_device_scoped(const char *path, const char *section,
                            const char *label, const char *key,
                            const char *fallback, char *out, DWORD out_size)
{
    char scoped[64];

    if (label && *label && ini_label_ok(label)) {
        if (G && G->id[0]) {
            snprintf(scoped, sizeof scoped, "%s.%s.%s", G->id, section, label);
            GetPrivateProfileStringA(scoped, key, "", out, out_size, path);
            ini_strip_comment(out);
            if (out[0])
                return;
        }
        snprintf(scoped, sizeof scoped, "%s.%s", section, label);
        GetPrivateProfileStringA(scoped, key, "", out, out_size, path);
        ini_strip_comment(out);
        if (out[0])
            return;
    }
    ini_read_scoped(path, section, key, fallback, out, out_size);
}

/* The whole [devices] section, however long: the profile API truncates
 * silently, so the buffer doubles until it fits. NULL when it never does. */
static char *devices_block(const char *path)
{
    DWORD size = 8192, n;
    char *block = NULL;

    for (; size <= (1u << 20); size *= 2) {
        char *grown = (char *)realloc(block, size);

        if (!grown)
            break;
        block = grown;
        n = GetPrivateProfileSectionA("devices", block, size, path);
        if (n < size - 2)
            return block;
    }
    free(block);
    return NULL;
}

unsigned ini_devices_read(const char *path, ini_device_line_t *out,
                          unsigned max)
{
    char *block = devices_block(path);
    const char *p;
    unsigned seen = 0;

    if (!block)
        return 0;
    for (p = block; *p; p += strlen(p) + 1) {
        char name[INI_LABEL_CAP];
        const char *eq = strchr(p, '=');
        size_t len;

        if (!eq)
            continue;
        len = (size_t)(eq - p);
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
            len--;
        if (!len || len >= sizeof name)
            continue;
        memcpy(name, p, len);
        name[len] = '\0';
        if (!ini_label_ok(name))
            continue;
        if (seen < max) {
            snprintf(out[seen].label, sizeof out[seen].label, "%s", name);
            snprintf(out[seen].value, sizeof out[seen].value, "%s", eq + 1);
            ini_strip_comment(out[seen].value);
        }
        seen++;
    }
    free(block);
    return seen;
}

/* The label whose [devices] line opens this device, or 0. The table is on the
 * heap: 64 lines are 25 KB, and this runs on the game's thread. */
int ini_device_label(const char *path, const char *provider,
                     const ini_device_id_t *dev, unsigned count,
                     unsigned which, char *out, size_t out_size)
{
    ini_device_line_t *line;
    const char *value[INI_DEVICES_MAX] = { 0 };
    int at[INI_DEVICES_MAX];
    unsigned n, i;
    int found = 0;

    out[0] = '\0';
    line = (ini_device_line_t *)malloc(INI_DEVICES_MAX * sizeof *line);
    if (!line)
        return 0;
    n = ini_devices_read(path, line, INI_DEVICES_MAX);
    if (n > INI_DEVICES_MAX)
        n = INI_DEVICES_MAX;
    for (i = 0; i < n; i++)
        value[i] = line[i].value;
    ini_devices_resolve(value, n, NULL, provider, dev, count, at);
    for (i = 0; i < n && !found; i++)
        if (at[i] == (int)which) {
            snprintf(out, out_size, "%s", line[i].label);
            found = 1;
        }
    free(line);
    return found;
}

/* Every section name in the file, as the profile API's own double-NUL block,
 * so a caller can find sections that should not be there. Returns the number
 * of names, 0 when the file has none or the buffer is too small to be sure it
 * holds them all. */
unsigned ini_section_names(const char *path, char *buf, size_t buf_size)
{
    DWORD n = GetPrivateProfileSectionNamesA(buf, (DWORD)buf_size, path);
    unsigned count = 0;
    const char *p;

    if (!n || n >= buf_size - 2) {
        if (buf_size)
            buf[0] = '\0';
        return 0;
    }
    for (p = buf; *p; p += strlen(p) + 1)
        count++;
    return count;
}

/* Is this section name a device profile, and for which label? The shape is
 * `[<something>.]<section>.<label>`, which is why the sections that can carry
 * a profile are a short list rather than "anything with a dot in it": a
 * [hydro.digital] must not be mistaken for a profile of [hydro] named
 * "digital". Returns 1 and fills `label` when it is one. */
int ini_device_profile_section(const char *name, const char *section,
                               char *label, size_t label_size)
{
    size_t n = strlen(section);
    const char *p = name;

    for (;;) {
        const char *dot;

        if (!_strnicmp(p, section, n) && p[n] == '.') {
            snprintf(label, label_size, "%s", p + n + 1);
            return ini_label_ok(label);
        }
        dot = strchr(p, '.');
        if (!dot)
            return 0;
        p = dot + 1;
    }
}

/* ---- a list, and one button reference ------------------------------------
 *
 * The binding grammar, here rather than in io_config.c
 * because the launcher has to accept what the host accepts or a hand-written
 * file is refused by the form meant to edit it. ini.h's opening paragraph is
 * the law: three mechanisms read this file and none may carry its own spelling
 * of what a value MEANS. */

int ini_list_item(const char *text, unsigned index, char *out, size_t out_size)
{
    const char *p, *start, *end;
    unsigned n = 0;
    size_t len;

    if (!text || !out || !out_size)
        return 0;
    p = text;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return 0;                    /* no items at all, not one empty one */
    for (;;) {
        start = p;
        while (*p && *p != ',')
            p++;
        end = p;
        if (n == index) {
            while (start < end && (*start == ' ' || *start == '\t'))
                start++;
            while (end > start && isspace((unsigned char)end[-1]))
                end--;
            len = (size_t)(end - start);
            if (len >= out_size)
                len = out_size - 1;
            memcpy(out, start, len);
            out[len] = '\0';
            return 1;
        }
        if (!*p)
            return 0;
        p++;                         /* past the comma, onto the next item */
        n++;
    }
}

/* The `<label>:` prefix both reference grammars share: returns what follows,
 * with `label` empty when there is no colon; NULL for a bad label. */
static const char *split_ref_label(const char *text, char *label,
                                   size_t label_size)
{
    const char *colon;
    size_t n;

    while (*text == ' ' || *text == '\t')
        text++;
    label[0] = '\0';
    colon = strchr(text, ':');
    if (colon) {
        n = (size_t)(colon - text);
        while (n && (text[n - 1] == ' ' || text[n - 1] == '\t'))
            n--;
        if (!n || n >= label_size)
            return NULL;
        memcpy(label, text, n);
        label[n] = '\0';
        if (!ini_label_ok(label)) {
            label[0] = '\0';
            return NULL;
        }
        text = colon + 1;
        while (*text == ' ' || *text == '\t')
            text++;
    }
    return text;
}

int ini_parse_axis_ref(const char *text, char *label, size_t label_size,
                       char *axis, size_t axis_size)
{
    size_t n;

    if (!text || !label || !label_size || !axis || !axis_size)
        return 0;
    axis[0] = '\0';
    if (!(text = split_ref_label(text, label, label_size)))
        return 0;
    n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t'))
        n--;
    if (!n || n >= axis_size) {
        label[0] = '\0';
        return 0;
    }
    memcpy(axis, text, n);
    axis[n] = '\0';
    return 1;
}

int ini_parse_pad_ref(const char *text, char *label, size_t label_size,
                      unsigned *ordinal)
{
    if (!text || !label || !label_size || !ordinal)
        return 0;
    if (!(text = split_ref_label(text, label, label_size)))
        return 0;
    /* BASE TEN, NOT ini_parse_uint()'s BASE 0. A button ordinal is what the
     * device's own control panel prints beside the button, and `08` is button
     * eight there and not an invalid octal constant. Nothing else in this file
     * has a value a user reads off a diagram. */
    {
        char *end;
        unsigned long v;

        if (!*text)
            return 0;
        v = strtoul(text, &end, 10);
        while (*end == ' ' || *end == '\t')
            end++;
        if (end == text || *end)
            return 0;
        /* strtoul saturates at ULONG_MAX rather than failing, so the ceiling
         * is the caller's: it owns IO_MAX_BUTTONS and it owns the log. */
        *ordinal = (unsigned)v;
    }
    return 1;
}
