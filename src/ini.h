/* ini.h -- the value grammar of vcthunder.ini, in one place.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Four files read that one file through three mechanisms, and those stay three
 * things. What is NOT three things is what a value MEANS: a boolean, a comment
 * delimiter, an integer and the `[<title>.<section>]` precedence rule are one
 * answer each, and this file is where that answer is. NOTHING ELSE MAY CARRY A
 * SECOND SPELLING OF ANY OF THEM. Nothing here logs, because config.c's parse
 * runs before the log file is open.
 */
#ifndef VCT_INI_H
#define VCT_INI_H

#include <stddef.h>

#include "vcthunder.h"

/* ---- whitespace --------------------------------------------------------- */

/* trim_left does NOT eat \r or \n: the launcher's document model keeps each
 * line's own terminator and needs to find it again when it writes the file
 * back. Everywhere else the terminator is gone before these are called. */
char *ini_trim_left(char *s);
void  ini_trim_right(char *s);
char *ini_trim(char *s);            /* both; returns the new start */

/* ---- comments ----------------------------------------------------------- */

/* An inline comment starts at a ; or # that is at the start of the text or
 * preceded by whitespace, so a value may contain either character: a Windows
 * path with a # in it, or a key name spelled "#". Returns NULL when there
 * is none. */
const char *ini_comment_start(const char *text);

/* Truncate at the inline comment, then right-trim. */
void  ini_strip_comment(char *text);

/* ---- values ------------------------------------------------------------- */

/* Each returns 1 when the text is a valid value and 0 when it is not; on 0 the
 * out parameter is untouched, so a caller may seed it with the previous layer
 * and let a bad value leave that in force. */
int ini_parse_bool(const char *v, int *out);
int ini_parse_uint(const char *v, unsigned lo, unsigned hi, unsigned *out);
int ini_parse_int(const char *v, int lo, int hi, int *out);

/* The forgiving forms, for the callers that have a default and no channel to
 * complain on. An out-of-range or unspellable value is the fallback. */
int      ini_bool_or(const char *v, int fallback);
unsigned ini_uint_or(const char *v, unsigned lo, unsigned hi, unsigned fallback);

/* ---- reading one key ---------------------------------------------------- */

/* Read [<G->id>.<section>] <key>, falling back to [<section>] <key> and then to
 * `fallback`. The precedence rule for the sections config.c does not own.
 *
 * `path` must be ABSOLUTE. The profile API resolves a bare filename against the
 * WINDOWS directory, so a relative path reads a file that does not exist and
 * every key silently takes its default. ini_resolve_path() is the door.
 *
 * Returns 1 when this game's own section answered, so a value a cabinet chose
 * for itself can say so in the log. */
int  ini_read_scoped(const char *path, const char *section, const char *key,
                     const char *fallback, char *out, DWORD out_size);

/* The `<title>` of the precedence rule: the runtime profile's id, or, for a
 * title that has no profile, the one named here. "" when neither exists. */
void ini_set_title(const char *id);
const char *ini_title(void);

void ini_resolve_path(const char *in, char *out, size_t out_size);

/* ---- devices: a name for one controller, and that controller's own keys, */

/* A label is 15 characters of [A-Za-z0-9_-] and a terminator, short because
 * the section buffers that carry it are char[64] (ini_label_ok()). A match may
 * be a whole device ID: a MAX_PATH name plus two prefixed GUIDs, 91 more. */
#define INI_LABEL_CAP 16
#define INI_MATCH_CAP (MAX_PATH + 96)

/* Case-insensitive substring, the predicate `[analog] dinput_name` has always
 * used. An empty needle matches nothing. */
int ini_name_contains(const char *haystack, const char *needle);

/* A device ID in MAME's format (input_dinput.cpp, make_id()):
 *
 *     <name> product_<guidProduct> instance_<guidInstance>
 *
 * GUIDs lowercase without braces, the name trimmed. Two controllers of one
 * model differ only in the instance GUID. `name` is the product name; MAME
 * takes the instance name, measured equal on a TMX and an Xbox pad. */
void ini_device_id(char *out, size_t out_size, const char *name,
                   const GUID *product, const GUID *instance);

/* The [devices] table's grammar:
 *
 *     [devices]
 *     tmx = dinput:Thrustmaster TMX
 *
 * `tmx` is the label, keying [analog.tmx]. The value is `<provider>:<match>`:
 * on DirectInput a case-insensitive substring of the device's name, or of its
 * whole ID when the match holds `product_` or `instance_`; elsewhere the
 * device's number. */
int ini_label_ok(const char *label);
int ini_device_entry(const char *value, char *provider, size_t provider_size,
                     char *match, size_t match_size);
/* A value naming one instance (it holds `instance_`); see
 * ini_devices_resolve(). */
int ini_device_is_pin(const char *value);

/* One attached device as the resolver sees it: `id` (ini_device_id(), "" on a
 * provider without names), `name` (what the ID starts with), and `index`, its
 * number on its provider. */
typedef struct {
    const char *id;
    const char *name;
    unsigned    index;
} ini_device_id_t;

int ini_device_matches(const char *value, const char *provider,
                       const char *id, unsigned index);

/* Which device each line of a [devices] table opens, all at once:
 *
 *   1. A line holding `instance_` is a pin. It resolves first, and the device
 *      it matches belongs to the lines that pin it.
 *   2. Every other line resolves to the first device it matches that no pin
 *      has claimed; such lines may share a device.
 *
 * Step 1 is our one departure from MAME: it stops a named box and a pinned
 * twin swapping when enumeration order flips. `out[i]` is the position in
 * `dev` that `lines[i]` opens, or -1. `extra` (dinput_name) is resolved by the
 * same rule without claiming anything, and its position is returned. `dev` is
 * one provider's devices in enumeration order; other providers' lines get -1.
 */
int ini_devices_resolve(const char *const *lines, unsigned n,
                        const char *extra, const char *provider,
                        const ini_device_id_t *dev, unsigned count, int *out);

/* Read [<title>.<section>.<label>], then [<section>.<label>], then
 * [<title>.<section>], then [<section>], then `fallback`. Device beats title.
 * With no label this IS ini_read_scoped(), and calls it. `path` ABSOLUTE. */
void ini_read_device_scoped(const char *path, const char *section,
                            const char *label, const char *key,
                            const char *fallback, char *out, DWORD out_size);

/* The [devices] table in file order, `value` comment-stripped. Lines whose
 * label fails ini_label_ok() are skipped. Returns how many lines the table has,
 * which may exceed `max`; only the first `max` are stored. File order is the
 * slot order. */
#define INI_DEVICES_MAX 64
typedef struct {
    char label[INI_LABEL_CAP];
    char value[INI_MATCH_CAP + 32];
} ini_device_line_t;
unsigned ini_devices_read(const char *path, ini_device_line_t *out,
                          unsigned max);

/* The label of the first line that opens `dev[which]`, if any. */
int ini_device_label(const char *path, const char *provider,
                     const ini_device_id_t *dev, unsigned count,
                     unsigned which, char *out, size_t out_size);

/* ---- a list, and one button reference ----------------------------------- */

/* The `index`'th comma-separated item of `text`, trimmed, or 0 when there is no
 * such item. One item and no comma yields that item at 0, so every
 * single-valued key reads through this unchanged.
 *
 * AN EMPTY ITEM IS RETURNED RATHER THAN SKIPPED: `boost = 9,` is a typo, and a
 * caller that never sees the second item cannot say so. */
int ini_list_item(const char *text, unsigned index, char *out,
                  size_t out_size);

/* One element of a [gamepad] binding: `[<label>:]<ordinal>`.
 *
 *     9           the primary device's button 9; `label` comes back empty
 *     th8:1       button 1 of the device labelled th8 in [devices]
 *
 * 0 for anything else. The caller has the log and the ceiling (IO_MAX_BUTTONS),
 * so neither is checked here. */
int ini_parse_pad_ref(const char *text, char *label, size_t label_size,
                      unsigned *ordinal);

/* One axis role's value: `[<label>:]<axis>`, the prefix a pad ref takes.
 *
 *     rz          the default device's axis rz; `label` comes back empty
 *     twcs:z      axis z of the device labelled twcs in [devices]
 *
 * The axis comes back as trimmed text, unchecked. 0 for a bad label or an
 * empty axis. */
int ini_parse_axis_ref(const char *text, char *label, size_t label_size,
                       char *axis, size_t axis_size);

/* The file's section names, as the profile API's own double-NUL block, and
 * whether one of them is a device profile of `section`, so a profile whose
 * label nothing binds can be REPORTED rather than silently doing nothing. */
unsigned ini_section_names(const char *path, char *buf, size_t buf_size);
int ini_device_profile_section(const char *name, const char *section,
                               char *label, size_t label_size);

#endif
