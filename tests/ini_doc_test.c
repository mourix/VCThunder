/* ini_doc_test.c -- the launcher's INI document, round-tripped.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * It builds src/ini_doc.c and src/ini.c and NOTHING ELSE, and that is the
 * point: a test that drags a GUI in to reach a string function is a test that
 * cannot say what it covers. If this ever needs a stub for something that
 * draws, the document model has grown something it should not have.
 */
#include "../src/ini_doc.c"
#include "../src/ini.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ini_read_scoped() consults the running title's profile (G, i.e. g_game).
 * Nothing here calls it, but ini.c references it and this binary has no
 * loader to select a title. */
const game_profile_t *g_game;

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int write_bytes(const char *path, const char *data)
{
    FILE *fp = fopen(path, "wb");
    size_t size = strlen(data);
    int ok = fp && fwrite(data, 1, size, fp) == size;
    if (fp)
        fclose(fp);
    return ok;
}

static char *read_bytes(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long size;
    char *data;
    if (!fp || fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET)) {
        if (fp) fclose(fp);
        return NULL;
    }
    data = (char *)malloc((size_t)size + 1);
    if (!data || (size && fread(data, 1, (size_t)size, fp) != (size_t)size)) {
        free(data);
        fclose(fp);
        return NULL;
    }
    data[size] = '\0';
    fclose(fp);
    return data;
}

static void test_lf_round_trip(const char *folder)
{
    static const char original[] =
        "; heading\n"
        "[graphics]\n"
        "unknown = keep # untouched\n"
        "video_mode = auto   ; first duplicate\n"
        "video_mode = 1\t # active duplicate\n"
        "empty =\n"
        "[hydro.digital]\n"
        "boost=SPACE\n"
        "[custom]\n"
        "foo=bar";
    char path[MAX_PATH], backup[MAX_PATH], why[256], value[64];
    char *saved, *saved_backup;
    ini_doc_t doc;

    snprintf(path, sizeof path, "%.*s\\lf.ini",
             (int)(sizeof path - sizeof "\\lf.ini"), folder);
    snprintf(backup, sizeof backup, "%.*s.bak",
             (int)(sizeof backup - sizeof ".bak"), path);
    check(write_bytes(path, original), "write LF fixture");
    check(ini_doc_load(&doc, path), "load LF fixture");
    check(ini_doc_get(&doc, "graphics", "video_mode", value, sizeof value) &&
          !strcmp(value, "1"), "last duplicate wins");
    check(ini_doc_set(&doc, "graphics", "video_mode", "2"), "update duplicate");
    check(ini_doc_set(&doc, "graphics", "fullscreen", "true"), "insert managed key");
    check(ini_doc_remove(&doc, "hydro.digital", "boost"), "remove title override");
    check(ini_doc_save(&doc, why, sizeof why), why);
    ini_doc_free(&doc);

    saved = read_bytes(path);
    saved_backup = read_bytes(backup);
    check(saved && strstr(saved, "; heading\n"), "preserve leading comment");
    check(saved && strstr(saved, "unknown = keep # untouched\n"),
          "preserve unknown key and inline comment");
    check(saved && strstr(saved, "video_mode = auto   ; first duplicate\n"),
          "preserve earlier duplicate");
    check(saved && strstr(saved, "video_mode = 2\t # active duplicate\n"),
          "preserve inline-comment spacing on changed value");
    check(saved && strstr(saved, "empty =\n"), "preserve empty value");
    check(saved && strstr(saved, "; boost=SPACE\n"), "comment removed override");
    check(saved && strstr(saved, "[custom]\nfoo=bar"), "preserve custom section");
    check(saved && !strchr(saved, '\r'), "preserve LF endings");
    check(saved_backup && !strcmp(saved_backup, original),
          "backup is the exact previous generation");
    free(saved);
    free(saved_backup);
}

static void test_crlf_and_failed_replace(const char *folder)
{
    static const char original[] =
        "[audio]\r\nvolume = 100  ; cabinet\r\n[unknown]\r\nx = y\r\n";
    char path[MAX_PATH], why[256];
    char *saved;
    HANDLE lock;
    ini_doc_t doc;

    snprintf(path, sizeof path, "%.*s\\crlf.ini",
             (int)(sizeof path - sizeof "\\crlf.ini"), folder);
    check(write_bytes(path, original), "write CRLF fixture");
    check(ini_doc_load(&doc, path), "load CRLF fixture");
    check(ini_doc_set(&doc, "audio", "volume", "125"), "update CRLF value");
    check(ini_doc_save(&doc, why, sizeof why), why);
    ini_doc_free(&doc);
    saved = read_bytes(path);
    check(saved && strstr(saved, "volume = 125  ; cabinet\r\n"),
          "preserve CRLF and inline spacing");
    free(saved);

    check(ini_doc_load(&doc, path), "reload replacement fixture");
    check(ini_doc_set(&doc, "audio", "volume", "130"), "prepare locked update");
    lock = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    check(lock != INVALID_HANDLE_VALUE, "lock target without delete sharing");
    check(!ini_doc_save(&doc, why, sizeof why), "locked replacement must fail");
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);
    ini_doc_free(&doc);
    saved = read_bytes(path);
    check(saved && strstr(saved, "volume = 125  ; cabinet\r\n"),
          "failed replacement leaves original intact");
    free(saved);
}


/* FILLING IN A KEY THAT SHIPPED EMPTY, which is what adopting a device does to
 * `dinput_name` and what Browse does to `glide_capture` and `glide_frames`, the
 * three shipped keys with no value. There is no spacing to preserve, so the one
 * thing this can get wrong is inventing none: `dinput_name =` coming back as
 * `dinput_name =Thrustmaster TMX Racing Wheel`.
 *
 * The last two rows are the pair that keeps the rule honest. A compact
 * `compact=old` is a style a person chose and must survive being changed, and
 * an empty value followed immediately by a comment must not end up with the
 * comment glued to the new value: ini_comment_start() takes a `;` only at the
 * start of the text or after whitespace, so `bare=v; note` would read back
 * with "; note" INSIDE the value. */
static void test_empty_value_gets_a_gap(const char *folder)
{
    static const char original[] =
        "[analog]\r\n"
        "dinput_name              =\r\n"
        "padded                   = \r\n"
        "compact=old\r\n"
        "bare=; note\r\n";
    char path[MAX_PATH], why[256], value[64];
    char *saved;
    ini_doc_t doc;

    snprintf(path, sizeof path, "%.*s\\empty.ini",
             (int)(sizeof path - sizeof "\\empty.ini"), folder);
    check(write_bytes(path, original), "write empty-value fixture");
    check(ini_doc_load(&doc, path), "load empty-value fixture");
    check(ini_doc_get(&doc, "analog", "bare", value, sizeof value) &&
          !value[0], "a value that is only a comment reads as empty");
    check(ini_doc_set(&doc, "analog", "dinput_name", "Thrustmaster TMX Racing Wheel"),
          "fill in the empty key");
    check(ini_doc_set(&doc, "analog", "padded", "x"), "fill in the padded key");
    check(ini_doc_set(&doc, "analog", "compact", "new"), "change the compact key");
    check(ini_doc_set(&doc, "analog", "bare", "v"), "fill in the bare key");
    check(ini_doc_save(&doc, why, sizeof why), why);
    ini_doc_free(&doc);

    saved = read_bytes(path);
    check(saved && strstr(saved,
          "dinput_name              = Thrustmaster TMX Racing Wheel\r\n"),
          "an empty value gains one space and keeps the key's alignment");
    check(saved && strstr(saved, "padded                   = x\r\n"),
          "a padded empty value keeps the space it already had");
    check(saved && strstr(saved, "compact=new\r\n"),
          "a compact line the user wrote stays compact");
    check(saved && strstr(saved, "bare= v ; note\r\n"),
          "a comment with no gap in front of it gets one");
    free(saved);

    /* THE READ-BACK IS THE POINT, not the shape of the line: every one of
     * these has to come back as the value that was written. */
    check(ini_doc_load(&doc, path), "reload the filled-in fixture");
    check(ini_doc_get(&doc, "analog", "dinput_name", value, sizeof value) &&
          !strcmp(value, "Thrustmaster TMX Racing Wheel"),
          "the filled-in name reads back whole");
    check(ini_doc_get(&doc, "analog", "compact", value, sizeof value) &&
          !strcmp(value, "new"), "the compact value reads back");
    check(ini_doc_get(&doc, "analog", "bare", value, sizeof value) &&
          !strcmp(value, "v"),
          "the comment stayed a comment and is not part of the value");
    ini_doc_free(&doc);
}

/* A value with blanks around it cannot be held, so it is written without
 * them, and a padded old value's trailing blanks are not kept as spacing.
 * Blanks in front of a comment are spacing, and stay. */
static void test_padded_value_reads_back(const char *folder)
{
    static const char original[] =
        "[analog]\r\n"
        "dinput_name = old\r\n"
        "noted = old   ; a note\r\n";
    char path[MAX_PATH], why[256], value[64];
    char *saved;
    ini_doc_t doc;

    snprintf(path, sizeof path, "%.*s\\padded.ini",
             (int)(sizeof path - sizeof "\\padded.ini"), folder);
    check(write_bytes(path, original), "write padded-value fixture");
    check(ini_doc_load(&doc, path), "load padded-value fixture");

    check(ini_doc_set(&doc, "devices", "generic",
                      "dinput:Generic   USB  Joystick  ") &&
          ini_doc_get(&doc, "devices", "generic", value, sizeof value) &&
          !strcmp(value, "dinput:Generic   USB  Joystick"),
          "a padded value reads back as what was stored, interior kept");
    check(ini_doc_set(&doc, "analog", "dinput_name", "Generic   USB  Joystick  ") &&
          ini_doc_set(&doc, "analog", "dinput_name", "FANATEC Wheel") &&
          ini_doc_set(&doc, "analog", "dinput_name", "Generic   USB  Joystick  ") &&
          ini_doc_set(&doc, "analog", "dinput_name", "FANATEC Wheel"),
          "swap a padded controller and the wheel twice");
    check(ini_doc_set(&doc, "analog", "noted", "new  "),
          "rewrite a value that has a comment after it");
    check(ini_doc_save(&doc, why, sizeof why), why);
    ini_doc_free(&doc);

    saved = read_bytes(path);
    check(saved && strstr(saved, "dinput_name = FANATEC Wheel\r\n"),
          "no padding survives the swaps: the line ends at the value");
    check(saved && strstr(saved, "generic = dinput:Generic   USB  Joystick\r\n"),
          "and none was written in the first place");
    check(saved && strstr(saved, "noted = new   ; a note\r\n"),
          "the gap in front of a comment is spacing, and is kept");
    free(saved);
}

/* ---- appending to a section that ends the file --------------------------
 *
 * ini_doc_set()'s scan records the end of the target section when it meets a
 * FOLLOWING header or a following line, so a file whose LAST line is the
 * section's own header can leave `section_end` unset and make the append path
 * write a SECOND `[section]` header.
 *
 * THE TWO READERS THEN DISAGREE, WHICH IS WHY THIS IS WORTH A TEST RATHER THAN
 * A TIDY-UP. ini_doc_get() is last-wins, so the launcher shows the value it
 * just wrote. The host reads through GetPrivateProfileStringA, which stops at
 * the FIRST matching header and treats the duplicate as the end of that
 * section, so the key is invisible to the game and takes its compiled default,
 * silently, for as long as the file lives.
 *
 * The unterminated-final-line case beside it is the same append path: appending
 * at doc->count is the ORDINARY case of the target section being the file's
 * last, so a file with no final newline concatenates two keys into one line. */
static void test_append_to_trailing_section(const char *folder)
{
    char path[MAX_PATH], why[256], value[INI_VALUE_CAP];
    char *saved;
    ini_doc_t doc;
    const char *first;

    /* The section header IS the last line of the file. */
    snprintf(path, sizeof path, "%.*s\\tail.ini",
             (int)(sizeof path - sizeof "\\tail.ini"), folder);
    check(write_bytes(path, "[audio]\r\nvolume = 100\r\n[ffb]\r\n"),
          "write a fixture ending in a bare section header");
    check(ini_doc_load(&doc, path), "load the trailing-header fixture");
    check(ini_doc_set(&doc, "ffb", "gain_percent", "80"),
          "write a key into the trailing section");
    check(ini_doc_save(&doc, why, sizeof why), why);
    ini_doc_free(&doc);

    saved = read_bytes(path);
    check(saved != NULL, "read back the trailing-header fixture");
    if (saved) {
        first = strstr(saved, "[ffb]");
        check(first && !strstr(first + 1, "[ffb]"),
              "exactly ONE [ffb] header, not two");
        /* The key must land INSIDE the first [ffb], which is what
         * GetPrivateProfileStringA reads. */
        check(first && strstr(first, "gain_percent = 80"),
              "the key lands under the existing header");
        free(saved);
    }
    check(ini_doc_load(&doc, path), "reload the repaired fixture");
    check(ini_doc_get(&doc, "ffb", "gain_percent", value, sizeof value) &&
          !strcmp(value, "80"), "and it reads back");
    ini_doc_free(&doc);

    /* And the file whose last line has no newline at all. */
    snprintf(path, sizeof path, "%.*s\\noeol.ini",
             (int)(sizeof path - sizeof "\\noeol.ini"), folder);
    check(write_bytes(path, "[ffb]\r\ngain_percent = 80"),
          "write a fixture with no final newline");
    check(ini_doc_load(&doc, path), "load the unterminated fixture");
    check(ini_doc_set(&doc, "ffb", "strength_percent", "90"),
          "append after an unterminated final line");
    check(ini_doc_save(&doc, why, sizeof why), why);
    ini_doc_free(&doc);
    saved = read_bytes(path);
    check(saved && !strstr(saved, "80strength_percent"),
          "the appended key is not concatenated onto the last line");
    free(saved);
    check(ini_doc_load(&doc, path), "reload the unterminated fixture");
    check(ini_doc_get(&doc, "ffb", "gain_percent", value, sizeof value) &&
          !strcmp(value, "80"), "the original key survives");
    check(ini_doc_get(&doc, "ffb", "strength_percent", value, sizeof value) &&
          !strcmp(value, "90"), "and the appended one reads back");
    ini_doc_free(&doc);
}

/* ---- the value grammar (src/ini.c) --------------------------------------
 *
 * Four readers of vcthunder.ini share this and nothing tested it. Two of the
 * cases below are the drift that consolidating them exposed: "-5" through
 * strtoul is 4294967291, which passes every upper bound anyone had written,
 * and a value that will not parse must leave its out parameter ALONE so the
 * previous layer stays in force. */
static void test_value_grammar(void)
{
    int b = 7;
    unsigned u = 7;
    int n = 7;
    char text[64];

    check(ini_parse_bool("true", &b) && b, "true");
    check(ini_parse_bool("YES", &b) && b, "YES is case-insensitive");
    check(ini_parse_bool("on", &b) && b, "on");
    check(ini_parse_bool("1", &b) && b, "1");
    check(ini_parse_bool("false", &b) && !b, "false");
    check(ini_parse_bool("Off", &b) && !b, "Off");
    check(ini_parse_bool("0", &b) && !b, "0");
    b = 7;
    check(!ini_parse_bool("maybe", &b) && b == 7, "maybe leaves b alone");
    check(!ini_parse_bool("", &b) && b == 7, "empty leaves b alone");
    check(!ini_parse_bool(NULL, &b) && b == 7, "NULL leaves b alone");
    check(ini_bool_or("nonsense", 1) == 1, "the forgiving form falls back");

    check(ini_parse_uint("42", 0, 100, &u) && u == 42, "42");
    check(ini_parse_uint("0x1f", 0, 100, &u) && u == 31, "0x1f is base 0");
    u = 7;
    check(!ini_parse_uint("-5", 0, 100, &u) && u == 7,
          "-5 is refused, not wrapped to 4294967291");
    check(!ini_parse_uint("101", 0, 100, &u) && u == 7, "101 is above the range");
    check(!ini_parse_uint("12x", 0, 100, &u) && u == 7, "a trailing x is not a number");
    check(!ini_parse_uint("", 0, 100, &u) && u == 7, "empty is not a number");
    check(ini_uint_or("-5", 0, 100, 12) == 12, "the forgiving form falls back");

    check(ini_parse_int("-5", -10, 10, &n) && n == -5, "-5 is a signed value");
    n = 7;
    check(!ini_parse_int("-11", -10, 10, &n) && n == 7, "-11 is below the range");

    /* A ; or # is a comment only at the start or after whitespace, so a value
     * may contain either: a Windows path with a # in it stays whole. */
    snprintf(text, sizeof text, "%s", "raster   ; the cabinet's own shape");
    ini_strip_comment(text);
    check(!strcmp(text, "raster"), "an inline comment goes, with its spacing");
    snprintf(text, sizeof text, "%s", "C:\\games\\hydro#1");
    ini_strip_comment(text);
    check(!strcmp(text, "C:\\games\\hydro#1"), "a # inside a value stays");
    snprintf(text, sizeof text, "%s", "# the whole line");
    ini_strip_comment(text);
    check(!*text, "a line that is only a comment goes entirely");

    snprintf(text, sizeof text, "%s", "  \t spaced out  \t ");
    check(!strcmp(ini_trim(text), "spaced out"), "trim takes both ends");
}


/* ---- the device scope --------------------------------------------------- */

/* THE FIVE-STEP ORDER, ASSERTED AS AN ORDER AND NOT AS FIVE LOOKUPS.
 *
 * One file carries the same key at every level, so each assertion below is
 * only satisfied by the level above it being consulted FIRST. The two that
 * matter most are the ends: `device beats title`, which is the step that makes
 * this walk different from ini_read_scoped(), and `[analog] is still reached`,
 * which is the step ini_read_scoped() cannot do: it prepends the title as a
 * prefix, so handed `analog.tmx` it looks in [hydro.analog.tmx] and
 * [analog.tmx] and then stops, and every file written before profiles existed
 * reads as unconfigured. */
static void test_device_scope(const char *folder)
{
    static const char original[] =
        "[devices]\r\n"
        "tmx  = dinput:Thrustmaster TMX\r\n"
        "pad  = xinput:0\r\n"
        "junk = nonsense\r\n"
        "\r\n"
        "[analog]\r\n"
        "dinput_steering_axis = shared_only\r\n"
        "dinput_throttle_axis = shared\r\n"
        "dinput_brake_axis    = shared\r\n"
        "dinput_pedal_mode    = shared\r\n"
        "[offroad.analog]\r\n"
        "dinput_throttle_axis = title\r\n"
        "dinput_brake_axis    = title\r\n"
        "dinput_pedal_mode    = title\r\n"
        "[analog.tmx]\r\n"
        "dinput_brake_axis    = device\r\n"
        "dinput_pedal_mode    = device\r\n"
        "[offroad.analog.tmx]\r\n"
        "dinput_pedal_mode    = title_device\r\n";
    /* Only `id` is read here, and every other field is deliberately zero:
     * an absent anchor is 0 and 0 is a real answer. */
    static game_profile_t offroad;
    const game_profile_t *saved = g_game;
    char path[MAX_PATH], v[64];

    snprintf(path, sizeof path, "%.*s\\scope.ini",
             (int)(sizeof path - sizeof "\\scope.ini"), folder);
    if (!write_bytes(path, original)) {
        check(0, "device scope fixture written");
        return;
    }
    offroad.id = "offroad";
    g_game = &offroad;

    ini_read_device_scoped(path, "analog", "tmx", "dinput_pedal_mode",
                           "MISSING", v, sizeof v);
    check(!strcmp(v, "title_device"),
          "step 1: [offroad.analog.tmx] wins over everything");
    ini_read_device_scoped(path, "analog", "tmx", "dinput_brake_axis",
                           "MISSING", v, sizeof v);
    check(!strcmp(v, "device"),
          "step 2: [analog.tmx] beats [offroad.analog], device beats title");
    ini_read_device_scoped(path, "analog", "tmx", "dinput_throttle_axis",
                           "MISSING", v, sizeof v);
    check(!strcmp(v, "title"), "step 3: [offroad.analog] when the device's "
          "own sections are silent");
    ini_read_device_scoped(path, "analog", "tmx", "dinput_steering_axis",
                           "MISSING", v, sizeof v);
    check(!strcmp(v, "shared_only"),
          "step 4: [analog] IS STILL REACHED through a device scope");
    ini_read_device_scoped(path, "analog", "tmx", "dinput_deadzone_percent",
                           "0", v, sizeof v);
    check(!strcmp(v, "0"), "step 5: the caller's fallback");

    /* WITH NO LABEL THIS IS ini_read_scoped(), and that is what lets a file
     * that has never seen a profile keep reading exactly as it did. */
    ini_read_device_scoped(path, "analog", NULL, "dinput_brake_axis",
                           "MISSING", v, sizeof v);
    check(!strcmp(v, "title"), "no label: the device's section is not "
          "consulted at all");
    ini_read_device_scoped(path, "analog", "", "dinput_brake_axis",
                           "MISSING", v, sizeof v);
    check(!strcmp(v, "title"), "an EMPTY label is not a label");
    /* A label that could not have been written by the launcher is not quietly
     * used as a section name either: refused, never truncated. */
    ini_read_device_scoped(path, "analog", "a b", "dinput_brake_axis",
                           "MISSING", v, sizeof v);
    check(!strcmp(v, "title"), "an ILLEGAL label falls straight through");

    /* The [devices] table, through the profile API's own enumeration. */
    {
        static const ini_device_id_t tmx[] = {
            { "Thrustmaster TMX Racing Wheel product_b67f044f-0000-0000-0000-"
              "504944564944 instance_11111111-0000-0000-0000-000000000001",
              "Thrustmaster TMX Racing Wheel", 0 },
            { "Logitech G29 product_c24f046d-0000-0000-0000-504944564944 "
              "instance_11111111-0000-0000-0000-000000000002",
              "Logitech G29", 1 },
        };
        static const ini_device_id_t pad[] = { { "", "", 0 }, { "", "", 1 } };
        ini_device_line_t line[4];

        check(ini_device_label(path, "dinput", tmx, 2, 0, v, sizeof v) &&
              !strcmp(v, "tmx"),
              "the label of a DirectInput device, matched on a substring of "
              "its ID, which starts with its name");
        check(!ini_device_label(path, "dinput", tmx, 2, 1, v, sizeof v),
              "a device no entry names has no label");
        check(ini_device_label(path, "xinput", pad, 2, 0, v, sizeof v) &&
              !strcmp(v, "pad"), "an XInput pad is matched on its slot number");
        check(!ini_device_label(path, "xinput", pad, 2, 1, v, sizeof v),
              "and slot 1 is not slot 0");
        check(!ini_device_label(path, "winmm", pad, 2, 0, v, sizeof v),
              "the provider has to match too");

        /* The table in file order, which is the slot order. */
        check(ini_devices_read(path, line, 4) == 3 &&
              !strcmp(line[0].label, "tmx") &&
              !strcmp(line[0].value, "dinput:Thrustmaster TMX"),
              "the first [devices] entry, label and value, in file order");
        check(!strcmp(line[1].label, "pad"), "the second");
        /* `junk = nonsense` has a LEGAL LABEL and an illegal value, so it is an
         * entry here and is rejected by whoever parses the value. Skipping it
         * would renumber the entries below it on a typo. */
        check(!strcmp(line[2].label, "junk") &&
              !strcmp(line[2].value, "nonsense"),
              "an entry whose VALUE is malformed still occupies its place");
        check(ini_devices_read(path, line, 1) == 3,
              "the count is the table's, not what the caller had room for");
    }

    g_game = saved;
}

/* The grammar of a [devices] line, and of a section that claims to be one. */
static void test_device_grammar(void)
{
    char provider[16], match[INI_MATCH_CAP], label[INI_LABEL_CAP];

    check(ini_label_ok("tmx") && ini_label_ok("pad-2") && ini_label_ok("a_1"),
          "a label is letters, digits, underscore and dash");
    check(!ini_label_ok("") && !ini_label_ok("a b") && !ini_label_ok("a.b"),
          "a label is not empty and carries no space or dot");
    check(!ini_label_ok("0123456789abcdef"),
          "a label longer than 15 characters is REFUSED, not truncated: the "
          "section buffers that carry it are char[64]");

    check(ini_device_entry("dinput:Thrustmaster TMX", provider,
                           sizeof provider, match, sizeof match) &&
          !strcmp(provider, "dinput") && !strcmp(match, "Thrustmaster TMX"),
          "provider:match, with the spacing around both trimmed");
    check(ini_device_entry("  winmm : two words  ", provider, sizeof provider,
                           match, sizeof match) &&
          !strcmp(provider, "winmm") && !strcmp(match, "two words"),
          "and a match may contain spaces");
    check(!ini_device_entry("nonsense", provider, sizeof provider, match,
                            sizeof match), "a line with no colon is refused");
    check(!ini_device_entry("dinput:", provider, sizeof provider, match,
                            sizeof match), "and so is an empty match");

    check(ini_device_matches("dinput:tmx", "dinput",
                             "Thrustmaster TMX Racing Wheel", 0),
          "the name match is a case-insensitive substring, as dinput_name is");
    check(!ini_device_matches("dinput:tmx", "winmm",
                              "Thrustmaster TMX Racing Wheel", 0),
          "a match on the wrong provider is not a match");
    /* A substring test on an index would make "0" match "10". XInput has four
     * slots today, so this is a predicate that would be right by accident. */
    check(ini_device_matches("xinput:0", "xinput", "", 0) &&
          !ini_device_matches("xinput:1", "xinput", "", 10),
          "an index is compared as a NUMBER, not as a substring");

    check(ini_device_profile_section("analog.tmx", "analog", label,
                                     sizeof label) && !strcmp(label, "tmx"),
          "[analog.tmx] is a device profile");
    check(ini_device_profile_section("offroad.analog.tmx", "analog", label,
                                     sizeof label) && !strcmp(label, "tmx"),
          "and so is [offroad.analog.tmx]");
    check(!ini_device_profile_section("analog", "analog", label, sizeof label),
          "[analog] itself is not");
    check(!ini_device_profile_section("hydro.digital", "analog", label,
                                      sizeof label),
          "and [hydro.digital] is not a profile of [hydro] named \"digital\"");
    check(!ini_device_profile_section("analog.a.b", "analog", label,
                                      sizeof label),
          "a label that is not a legal label does not make a profile");
}

/* A comma list, and one `[<label>:]<ordinal>` of it, §6.2's whole grammar.
 *
 * IT IS TESTED HERE AND NOT BESIDE THE HOST because two readers depend on it
 * agreeing with itself: io_config.c resolves the binding and launcher.c decides
 * whether the form may be saved, and a form that refuses what the host accepts
 * cannot edit a file a user hand-wrote. */
static void test_binding_grammar(void)
{
    char item[64], label[INI_LABEL_CAP];
    unsigned ordinal;

    /* THE ADDITIVE CASE FIRST, because it is the one that must not move: every
     * shipped value and every file written before Phase 4 is this. */
    check(ini_list_item("9", 0, item, sizeof item) && !strcmp(item, "9"),
          "a value with no comma is one item");
    check(!ini_list_item("9", 1, item, sizeof item), "and only one");
    check(ini_parse_pad_ref("9", label, sizeof label, &ordinal) &&
          !label[0] && ordinal == 9,
          "a bare ordinal has no label and means the primary device");

    check(ini_list_item(" 9 , th8:1 ", 0, item, sizeof item) &&
          !strcmp(item, "9"), "items are trimmed");
    check(ini_list_item(" 9 , th8:1 ", 1, item, sizeof item) &&
          !strcmp(item, "th8:1"), "on both sides of the comma");
    check(!ini_list_item("", 0, item, sizeof item) &&
          !ini_list_item("   ", 0, item, sizeof item),
          "an empty value has no items at all, rather than one empty one");
    /* A TRAILING COMMA IS RETURNED AS AN EMPTY ITEM, not skipped: `9,` is a
     * typo, and a caller that never sees the second item cannot say so. */
    check(ini_list_item("9,", 1, item, sizeof item) && !item[0],
          "a trailing comma yields an EMPTY item, which the caller refuses");

    check(ini_parse_pad_ref("th8:1", label, sizeof label, &ordinal) &&
          !strcmp(label, "th8") && ordinal == 1,
          "a label, a colon and an ordinal");
    check(ini_parse_pad_ref(" th8 : 12 ", label, sizeof label, &ordinal) &&
          !strcmp(label, "th8") && ordinal == 12,
          "and the spacing around each part is trimmed");
    /* The plan's own acceptance writes `[devices] 2 = ...` and `shift1 = 2:1`.
     * A digit is a legal label character, so a numeric label is a label. */
    check(ini_parse_pad_ref("2:1", label, sizeof label, &ordinal) &&
          !strcmp(label, "2") && ordinal == 1,
          "a numeric label is a label, not a slot number");
    /* BASE TEN, NOT ini_parse_uint()'s BASE 0: an ordinal is what the device's
     * own control panel prints beside the button, where 08 is button eight. */
    check(ini_parse_pad_ref("08", label, sizeof label, &ordinal) &&
          ordinal == 8, "an ordinal is read in base ten");
    check(ini_parse_pad_ref("0", label, sizeof label, &ordinal) && !ordinal &&
          !label[0], "0 is how the file spells unbound, and still parses");

    check(!ini_parse_pad_ref("", label, sizeof label, &ordinal),
          "an empty element is not a button");
    check(!ini_parse_pad_ref("th8:", label, sizeof label, &ordinal),
          "a label with no ordinal is not a button");
    check(!ini_parse_pad_ref(":1", label, sizeof label, &ordinal),
          "and neither is an ordinal with an empty label");
    check(!ini_parse_pad_ref("th8:x", label, sizeof label, &ordinal),
          "an ordinal that is not a number is refused");
    check(!ini_parse_pad_ref("1 2", label, sizeof label, &ordinal),
          "so is a trailing anything");
    check(!ini_parse_pad_ref("a b:1", label, sizeof label, &ordinal),
          "a label that is not ini_label_ok() is refused, not truncated");
    check(!ini_parse_pad_ref("0123456789abcdef:1", label, sizeof label,
                             &ordinal),
          "including one past the 15 characters the section buffers allow");
}

/* ---- device IDs and the table resolver -----------------------------------
 *
 * Four DirectInput devices, in one enumeration order and then the other: two
 * collections of one Fanatec, and two identical button encoders whose product
 * name is padded with blanks. */
#define ID_FANATEC "FANATEC Wheel product_00060eb7-0000-0000-0000-504944564944"
#define ID_GENERIC "Generic   USB  Joystick product_00060079-0000-0000-0000-" \
                   "504944564944"
#define INST_A " instance_aaaa0000-0000-0000-0000-000000000000"
#define INST_B " instance_bbbb0000-0000-0000-0000-000000000002"

static void test_device_identity(const char *folder)
{
    static const ini_device_id_t dev[] = {
        { ID_FANATEC INST_A, "FANATEC Wheel", 0 },
        { ID_FANATEC INST_B, "FANATEC Wheel", 1 },
        { ID_GENERIC " instance_c0000000-0000-0000-0000-000000000000",
          "Generic   USB  Joystick", 2 },
        { ID_GENERIC " instance_d0000000-0000-0000-0000-000000000000",
          "Generic   USB  Joystick", 3 },
    };
    /* The two encoders, the other way round: what a replug can do. */
    static const ini_device_id_t flipped[] = {
        { ID_GENERIC " instance_d0000000-0000-0000-0000-000000000000",
          "Generic   USB  Joystick", 0 },
        { ID_GENERIC " instance_c0000000-0000-0000-0000-000000000000",
          "Generic   USB  Joystick", 1 },
    };
    static const GUID product = { 0x0b13045e, 0, 0,
        { 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 } };
    static const GUID instance = { 0xd60686b0, 0x8594, 0x11ee,
        { 0x80, 0x02, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    char id[INI_MATCH_CAP], path[MAX_PATH], *text;
    const char *line[40];
    int at[40], got;
    unsigned i;

    /* The string idprobe built for the Bluetooth pad. */
    ini_device_id(id, sizeof id, "  Bluetooth LE XINPUT compatible input "
                  "device  ", &product, &instance);
    check(!strcmp(id, "Bluetooth LE XINPUT compatible input device "
                  "product_0b13045e-0000-0000-0000-504944564944 "
                  "instance_d60686b0-8594-11ee-8002-444553540000"),
          "the ID is MAME's, to the character, with the name trimmed");

    line[0] = "dinput:FANATEC Wheel";
    line[1] = "dinput:generic";
    line[2] = "dinput:" ID_FANATEC INST_B;
    line[3] = "dinput:FANATEC Wheel product_00060eb7-0000-0000-0000-"
              "504944564944 instance_eeee0000-0000-0000-0000-000000000000";
    line[4] = "dinput:Generic   USB  Joystick  ";
    line[5] = "dinput:2";
    line[6] = "xinput:0";
    ini_devices_resolve(line, 7, NULL, "dinput", dev, 4, at);
    check(at[2] == 1, "a full ID opens exactly that device");
    check(at[0] == 0,
          "a name opens the first device carrying it that no pin claims");
    check(at[1] == 2 && at[4] == 2,
          "a hand-shortened name and a padded one open the same encoder");
    check(at[3] == -1, "a stale instance opens nothing, and no other device");
    check(at[5] == -1,
          "`dinput:2` opens nothing, however many 2s the GUIDs carry");
    check(at[6] == -1, "a line for another provider opens nothing here");

    /* Hex letters only a GUID carries open nothing unless the match names
     * the GUID by its prefix. */
    line[0] = "dinput:bbbb";
    line[1] = "dinput:fanatec wheel";
    ini_devices_resolve(line, 2, NULL, "dinput", dev, 4, at);
    check(at[0] == -1 && at[1] == 0,
          "hex letters only a GUID carries open nothing; a name still opens");
    line[0] = "dinput:instance_bbbb";
    ini_devices_resolve(line, 1, NULL, "dinput", dev, 4, at);
    check(at[0] == 1, "and the GUID named by its prefix opens its device");

    /* Encoder A has the name line, B a pin. After a replug B enumerates
     * first, and the pin's claim keeps the name line on A. */
    line[0] = "dinput:Generic   USB  Joystick";
    line[1] = "dinput:instance_d0000000";
    ini_devices_resolve(line, 2, NULL, "dinput", dev + 2, 2, at);
    check(at[0] == 0 && at[1] == 1, "before the replug: A by name, B pinned");
    ini_devices_resolve(line, 2, NULL, "dinput", flipped, 2, at);
    check(at[0] == 1 && at[1] == 0,
          "after it: the pin keeps B and the name line skips it to A");
    line[2] = "dinput:generic";
    ini_devices_resolve(line, 3, NULL, "dinput", flipped, 2, at);
    check(at[0] == 1 && at[2] == 1, "name lines share a device among themselves");

    /* dinput_name is resolved against the claims and claims nothing. */
    got = ini_devices_resolve(line, 2, "dinput:Generic", "dinput", flipped, 2,
                              at);
    check(got == 1, "dinput_name skips a device a line pins");
    got = ini_devices_resolve(line, 1, "dinput:instance_d0000000", "dinput",
                              flipped, 2, at);
    check(got == 0 && at[0] == 0,
          "and a pinned dinput_name takes no device from a name line");

    /* A table longer than 4096 bytes, which a fixed block would read as
     * empty. */
    {
        ini_device_line_t *read = malloc(INI_DEVICES_MAX * sizeof *read);
        size_t at_text = 0, cap = 64 * 1024;

        text = malloc(cap);
        at_text += (size_t)snprintf(text, cap, "[devices]\r\n");
        for (i = 0; i < 40; i++)
            at_text += (size_t)snprintf(text + at_text, cap - at_text,
                                        "d%u = dinput:" ID_GENERIC
                                        " instance_%08x-0000-0000-0000-"
                                        "000000000000\r\n", i, i);
        snprintf(path, sizeof path, "%s\\long.ini", folder);
        check(read && write_bytes(path, text), "the long ini was written");
        check(at_text > 4096 && read &&
              ini_devices_read(path, read, INI_DEVICES_MAX) == 40 &&
              !strcmp(read[39].label, "d39") &&
              strstr(read[39].value, "instance_00000027"),
              "a table of 40 IDs reads whole, the last line included");
        free(read);
        free(text);
    }

    /* A tester's [devices]: twenty-eight padded labels for one encoder, and
     * `fanatec2 = dinput:2`. */
    {
        static const ini_device_id_t his[] = {
            { ID_FANATEC INST_A, "FANATEC Wheel", 0 },
            { ID_FANATEC INST_B, "FANATEC Wheel", 1 },
            { ID_GENERIC INST_A, "Generic   USB  Joystick", 2 },
        };
        char store[30][24];
        unsigned n = 0;

        line[n++] = "dinput:TWCS Throttle";
        line[n++] = "dinput:FANATEC Wheel";
        line[n++] = "dinput:2";
        for (i = 0; i < 28; i++, n++) {
            snprintf(store[i], sizeof store[i], "%s", "dinput:Generic   USB  "
                     "Joystick  ");
            line[n] = store[i];
        }
        ini_devices_resolve(line, n, "dinput:FANATEC Wheel", "dinput", his, 3,
                            at);
        got = 1;
        for (i = 3; i < n; i++)
            got = got && at[i] == 2;
        check(got, "generic..generic28 all open the encoder");
        check(at[0] == -1 && at[1] == 0 && at[2] == -1,
              "the absent TWCS opens nothing, fanatec the first collection, "
              "and fanatec2 = dinput:2 nothing");
    }
}

/* [devices] keys are the DATA, so the document model has to enumerate them. */
static void test_doc_key_enumeration(const char *folder)
{
    static const char original[] =
        "[devices]\n"
        "tmx = dinput:Thrustmaster\n"
        "pad = xinput:0\n"
        "tmx = dinput:Thrustmaster TMX\n"
        "[analog]\n"
        "provider = dinput\n";
    char path[MAX_PATH], key[64], value[64];
    ini_doc_t doc;

    snprintf(path, sizeof path, "%.*s\\keys.ini",
             (int)(sizeof path - sizeof "\\keys.ini"), folder);
    if (!write_bytes(path, original) || !ini_doc_load(&doc, path)) {
        check(0, "key enumeration fixture loaded");
        return;
    }
    check(ini_doc_key_index(&doc, "devices", 0, key, sizeof key) &&
          !strcmp(key, "tmx"), "the first key of [devices]");
    check(ini_doc_key_index(&doc, "devices", 1, key, sizeof key) &&
          !strcmp(key, "pad"), "the second");
    check(!ini_doc_key_index(&doc, "devices", 2, key, sizeof key),
          "a key assigned twice is ONE key, or the table reads as three "
          "devices when the file names two");
    check(ini_doc_get(&doc, "devices", "tmx", value, sizeof value) &&
          !strcmp(value, "dinput:Thrustmaster TMX"),
          "and its value is still the last assignment, as everywhere else");
    check(!ini_doc_key_index(&doc, "gamepad", 0, key, sizeof key),
          "a section the file does not have enumerates nothing");
    ini_doc_free(&doc);
}

int main(void)
{
    char root[MAX_PATH], folder[MAX_PATH];
    GetTempPathA(sizeof root, root);
    snprintf(folder, sizeof folder, "%.*sVCThunder-launcher-%lu",
             (int)(sizeof folder - sizeof "VCThunder-launcher-" - 10),
             root, GetCurrentProcessId());
    CreateDirectoryA(folder, NULL);

    test_value_grammar();
    test_lf_round_trip(folder);
    test_crlf_and_failed_replace(folder);
    test_empty_value_gets_a_gap(folder);
    test_padded_value_reads_back(folder);
    test_append_to_trailing_section(folder);
    test_device_grammar();
    test_binding_grammar();
    test_device_scope(folder);
    test_device_identity(folder);
    test_doc_key_enumeration(folder);
    if (failures) {
        fprintf(stderr, "ini document tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("ini document tests: all passed");
    return 0;
}
