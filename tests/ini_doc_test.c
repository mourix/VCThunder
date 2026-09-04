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
    if (failures) {
        fprintf(stderr, "ini document tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("ini document tests: all passed");
    return 0;
}
