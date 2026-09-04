/* config_test.c -- what the host does with a vcthunder.ini a person wrote.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * src/config.c is 505 lines of pure string logic and the most user-facing code
 * in the host: an alias table, per-title section precedence, range and choice
 * validation, and the promise in docs/configuration.md §6 that "the parser
 * takes any key it knows, wherever it finds it". Its failure mode is not a
 * crash -- it is a setting that reads as applied and is not, which is the
 * single thing a user cannot debug from the outside.
 *
 * tests/settings_round_trip_test.c already asserts that every LEGAL value
 * survives launcher -> file -> host. This is the other half, and the harder
 * one: what happens to a value that is wrong, misplaced, misspelled, commented
 * out, or written twice.
 *
 * THE LOG LINE IS THE PRODUCT. When config.c rejects a value it keeps the
 * previous layer's and files a note, and that note is the only thing the user
 * ever sees. So the assertions here are on the message and not merely on the
 * count: a warning that does not name the key, the value it refused and the
 * value it kept is a warning that costs a support round.
 */
#include "../src/ini.c"
#include "../src/settings.c"
#include "../src/config.c"

#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* config_parse_argv() prints its usage text on a refusal, which is correct and
 * is thirty lines. Two cases below provoke it deliberately, and `make check`
 * should not carry sixty lines of expected output that a reader has to learn
 * to ignore -- that is how a real line stops being noticed. So stderr is
 * pointed at the null device for exactly those calls, by descriptor rather
 * than by freopen(), because a freopen'd stderr cannot be given back. */
static int s_saved_stderr = -1;

static void stderr_off(void)
{
    fflush(stderr);
    s_saved_stderr = _dup(_fileno(stderr));
    freopen("NUL", "w", stderr);
}

static void stderr_on(void)
{
    if (s_saved_stderr < 0)
        return;
    fflush(stderr);
    _dup2(s_saved_stderr, _fileno(stderr));
    _close(s_saved_stderr);
    s_saved_stderr = -1;
}

/* ---- the log, captured ------------------------------------------------- */
#define MAX_LOG 64

static char logged[MAX_LOG][512];
static int  log_count;

void log_printf(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    if (log_count < MAX_LOG) {
        va_start(ap, fmt);
        vsnprintf(logged[log_count], sizeof logged[0], fmt, ap);
        va_end(ap);
    }
    log_count++;
}

int aspect_is_4_3(const char *aspect)
{
    return _stricmp(aspect, "4:3") == 0;
}

static const game_profile_t k_hydro   = { .id = "hydro" };
static const game_profile_t k_offroad = { .id = "offroad" };
const game_profile_t *const g_profiles[] = { &k_hydro, &k_offroad, NULL };
const game_profile_t *g_game;

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* Every captured line, searched. The order notes come out in is config.c's
 * business, not this test's. */
static int logged_says(const char *needle)
{
    int i;

    for (i = 0; i < log_count && i < MAX_LOG; i++)
        if (strstr(logged[i], needle))
            return 1;
    return 0;
}

static void check_says(const char *needle, const char *message)
{
    if (!logged_says(needle)) {
        int i;
        fprintf(stderr, "FAIL: %s (no logged line contains \"%s\")\n",
                message, needle);
        for (i = 0; i < log_count && i < MAX_LOG; i++)
            fprintf(stderr, "        had: %s\n", logged[i]);
        failures++;
    }
}

/* ---- one ini, written and loaded --------------------------------------- */
static char s_path[MAX_PATH];

static void write_ini(const char *text)
{
    FILE *fp = fopen(s_path, "wb");

    if (!fp) {
        fprintf(stderr, "FAIL: cannot write %s\n", s_path);
        failures++;
        return;
    }
    fputs(text, fp);
    fclose(fp);
}

/* Defaults, then the file, then whatever it had to say. The three steps the
 * host itself takes, in the order it takes them. */
static int load_text(shim_config_t *c, const char *text)
{
    int applied;

    write_ini(text);
    log_count = 0;
    config_defaults(c);
    applied = config_load_shared(c, s_path);
    config_report_notes();
    return applied;
}

/* ---- a default that will not parse is a bug in settings.def ------------ */
static void test_defaults_are_all_legal(void)
{
    shim_config_t c;

    log_count = 0;
    config_defaults(&c);
    config_report_notes();
    check(log_count == 0,
          "every default in settings.def parses through the same code the "
          "file does");
    if (log_count)
        fprintf(stderr, "        first: %s\n", logged[0]);
}

static void test_absent_file(void)
{
    shim_config_t c;
    char missing[MAX_PATH];

    snprintf(missing, sizeof missing, "%s.nope", s_path);
    log_count = 0;
    config_defaults(&c);
    check(config_load_shared(&c, missing) < 0, "an absent ini reports -1");
    check(c.render_scale == 1, "and the defaults stand");
    check(log_count == 0, "and it is not an error: a pack ships without one");
}

/* ---- an invalid value keeps the previous one, and says which ----------- */
static void test_invalid_keeps_the_previous_value(void)
{
    shim_config_t c;

    load_text(&c, "[graphics]\nrender_scale = 4\n");
    check(c.render_scale == 4, "a value in range lands");

    load_text(&c, "[graphics]\nrender_scale = 99\n");
    check(c.render_scale == 1, "one out of range does not, and the default stands");
    check_says("render_scale", "the warning names the key");
    check_says("99", "and the value it refused");
    check_says("1..8", "and the range it wanted");
    check_says("keeping the previous value", "and what it kept");

    load_text(&c, "[graphics]\nrender_scale = fast\n");
    check(c.render_scale == 1, "and a word where a number goes is refused");

    /* strtoul accepts a leading '-' and wraps it, so "-1" used to arrive as
     * 4294967295 and land inside almost any range. src/ini.c refuses the sign
     * before parsing; this is that fix, asserted where a user would meet it. */
    load_text(&c, "[graphics]\nrender_scale = -5\n");
    check(c.render_scale == 1, "a negative unsigned is refused, not wrapped");

    /* Both ends of the range are IN it. */
    load_text(&c, "[graphics]\nrender_scale = 8\n");
    check(c.render_scale == 8, "the top of the range is inside it");
    load_text(&c, "[graphics]\nrender_scale = 1\n");
    check(c.render_scale == 1, "and so is the bottom");
    load_text(&c, "[graphics]\nrender_scale = 0\n");
    check(c.render_scale == 1, "and one below the bottom is not");
}

static void test_invalid_enum(void)
{
    shim_config_t c;

    load_text(&c, "[graphics]\naspect = 16:9\n");
    check(strcmp(c.aspect, "16:9") == 0, "a legal choice lands");

    load_text(&c, "[graphics]\naspect = 16-9\n");
    check(strcmp(c.aspect, "raster") == 0,
          "a mistyped choice is refused and the default stands");
    check_says("raster|4:3|16:9|stretch", "and the warning lists the choices");
}

static void test_invalid_bool(void)
{
    shim_config_t c;

    load_text(&c, "[graphics]\nfullscreen = yes\n");
    check(c.fullscreen == 1, "yes is true");
    load_text(&c, "[graphics]\nfullscreen = maybe\n");
    check(c.fullscreen == 0, "maybe is refused and false stands");
    check_says("true/false, yes/no, on/off, or 1/0",
               "and the warning spells out what it would have taken");
}

/* ---- the alias table --------------------------------------------------- */
static void test_alias(void)
{
    shim_config_t c;

    /* A run directory older than the rename still carries the old spelling,
     * and must keep working without a word. */
    load_text(&c, "[controls]\ndiego_ms = 7\n");
    check(c.io_ms == 7, "diego_ms still sets io_ms");
    check(log_count == 0, "and an alias is not a complaint");
}

/* ---- a key found outside its own section ------------------------------- */
static void test_misplaced_key_is_applied_and_reported(void)
{
    shim_config_t c;

    /* docs/configuration.md §6 promises this works. It has to keep working
     * AND it has to say so, or the user is left with two plausible places to
     * have put the line and no way to tell which one was read. */
    load_text(&c, "[audio]\nrender_scale = 3\n");
    check(c.render_scale == 3, "a known key is taken from the wrong section");
    check_says("belongs in [graphics]", "and the log says where it belongs");
    check_says("applied anyway", "and that it was applied");
}

static void test_stranded_key_is_reported_harder(void)
{
    shim_config_t c;

    /* [analog] and [ffb] are read by diego.c and dinput.c, which look ONLY in
     * their own section. A misplaced one of those does nothing at all, so it
     * gets the stronger warning rather than the reassuring one. */
    load_text(&c, "[graphics]\ndinput_deadzone_percent = 12\n");
    check_says("is NOT read there", "a stranded key is reported as ineffective");
    check_says("[analog]", "and names the section that would work");
    check(!logged_says("applied anyway"),
          "and is NOT reported as applied, because it was not");
}

static void test_unknown_key_is_silent(void)
{
    shim_config_t c;

    /* The binding maps in [digital] and [gamepad] are in this same file and
     * are read by somebody else. An unknown key is normal here. */
    load_text(&c, "[digital]\nbutton_boost = KEY_SPACE\n"
             "[nonsense]\nnot_a_key = 1\n");
    check(log_count == 0, "an unknown key is not a warning");
}

/* ---- the grammar, as it arrives through this parser -------------------- */
static void test_comments_and_whitespace(void)
{
    shim_config_t c;

    load_text(&c, "; a whole line\n# another\n[graphics]\n"
             "  render_scale   =   4   ; trailing\n");
    check(c.render_scale == 4, "comments and padding around a value");

    load_text(&c, "[graphics]\nrender_scale=4 # hash comment\n");
    check(c.render_scale == 4, "a # comment too, and no spaces around =");

    /* A '#' NOT preceded by whitespace is part of the value. That one
     * character is the whole difference between a path that works and a path
     * that is silently truncated. */
    load_text(&c, "[paths]\ndata_dir = C:\\games\\hydro#1\n");
    check(strcmp(c.data_dir, "C:\\games\\hydro#1") == 0,
          "a # inside a value is part of it");

    load_text(&c, "[paths]\ndata_dir = C:\\games\\hydro ; here\n");
    check(strcmp(c.data_dir, "C:\\games\\hydro") == 0,
          "and one after whitespace is not");

    load_text(&c, "[GRAPHICS]\nRENDER_SCALE = 4\n");
    check(c.render_scale == 4, "sections and keys are case-insensitive");

    load_text(&c, "[graphics]\nrender_scale = 2\nrender_scale = 5\n");
    check(c.render_scale == 5,
          "the last assignment wins, as Windows' own profile API does");

    load_text(&c, "[graphics]\nno_equals_sign\nrender_scale = 4\n");
    check(c.render_scale == 4, "a line with no = is skipped, not fatal");
}

/* ---- per-title sections ------------------------------------------------- */
static void test_per_title_sections(void)
{
    shim_config_t c;

    write_ini("[graphics]\nrender_scale = 2\n"
              "[hydro.graphics]\nrender_scale = 5\n"
              "[offroad.graphics]\nrender_scale = 7\n");

    log_count = 0;
    config_defaults(&c);
    config_load_shared(&c, s_path);
    check(c.render_scale == 2, "the shared pass skips every per-title section");
    config_load_title(&c, s_path, "hydro");
    config_report_notes();
    check(c.render_scale == 5, "and the title pass applies its own");
    check(log_count == 0, "and neither pass has anything to complain about");

    log_count = 0;
    config_defaults(&c);
    config_load_shared(&c, s_path);
    config_load_title(&c, s_path, "offroad");
    config_report_notes();
    check(c.render_scale == 7, "for the other title too");

    /* A FLAT [hydro] is not a form this file has. docs/configuration.md §1
     * says the scopes cannot collapse into one, because [digital] and
     * [gamepad] use the same key names for different things. So a key written
     * there is in no section at all: the "any key wherever it is found"
     * fallback still applies it -- a user who wrote it meant it -- and it is
     * reported, because the next key they write that way might be a `coin1`
     * that could have meant either of two things. */
    write_ini("[hydro]\nrender_scale = 6\n");
    log_count = 0;
    config_defaults(&c);
    config_load_shared(&c, s_path);
    check(c.render_scale == 1, "a bare [hydro] is skipped by the shared pass");
    config_load_title(&c, s_path, "hydro");
    config_report_notes();
    check(c.render_scale == 6, "and the title pass still applies it");
    check_says("belongs in [graphics]",
               "and says the section it should have been written in");

    /* log_file names a file that is already open by the time the title is
     * known, so honouring it per title would be a setting that reads as
     * applied and is not. */
    write_ini("[diagnostics]\nlog_file = shared.log\n"
              "[hydro.diagnostics]\nlog_file = hydro.log\n");
    log_count = 0;
    config_defaults(&c);
    config_load_shared(&c, s_path);
    check(strcmp(c.log_file, "shared.log") == 0, "log_file is read once");
    config_load_title(&c, s_path, "hydro");
    config_report_notes();
    check(strcmp(c.log_file, "shared.log") == 0,
          "and a per-title log_file is deliberately ignored");
}

/* ---- the notes themselves ---------------------------------------------- */
static void test_notes_are_consumed(void)
{
    shim_config_t c;

    load_text(&c, "[graphics]\nrender_scale = 99\n");
    check(log_count == 1, "one bad value, one warning");
    log_count = 0;
    config_report_notes();
    check(log_count == 0,
          "and reporting again says nothing: shared and per-title passes must "
          "not repeat one another");
}

static void test_note_overflow_is_announced(void)
{
    shim_config_t c;
    char text[8192];
    size_t n;
    unsigned i;

    /* More bad values than the bounded note list holds. Losing the tail is
     * fine; losing it silently is not. */
    n = (size_t)snprintf(text, sizeof text, "[graphics]\n");
    for (i = 0; i < INVALID_NOTE_MAX + 4u; i++)
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "render_scale = bad%u\n", i);
    load_text(&c, text);
    check_says("further warnings suppressed",
               "overflowing the note list is announced, not silent");
}

/* ---- the one value config.c computes rather than reads ----------------- */
static void test_resolve_graphics(void)
{
    shim_config_t c;

    load_text(&c, "[graphics]\nwidescreen = true\naspect = 4:3\n");
    log_count = 0;
    config_resolve_graphics(&c);
    check(c.widescreen == 0, "an explicit 4:3 turns the widescreen hack off");
    check_says("aspect = 4:3", "and says why, because the user asked for both");

    load_text(&c, "[graphics]\nwidescreen = true\naspect = 16:9\n");
    log_count = 0;
    config_resolve_graphics(&c);
    check(c.widescreen == 1, "16:9 leaves it on");
    check(log_count == 0, "with nothing to report");
}

/* ---- the command line -------------------------------------------------- */
static void test_argv(void)
{
    shim_config_t c;
    char *ok[]      = { "VCThunder.exe", "hydro", "--dry-run" };
    char *upper[]   = { "VCThunder.exe", "HYDRO" };
    char *titled[]  = { "VCThunder.exe", "--title", "offroad" };
    char *probe[]   = { "VCThunder.exe", "hydro", "--probe-image" };
    char *typo[]    = { "VCThunder.exe", "hydra" };
    char *dangling[] = { "VCThunder.exe", "--data" };

    config_defaults(&c);
    check(config_parse_argv(&c, 3, ok), "a bare title and a flag are accepted");
    check(strcmp(c.title, "hydro") == 0 && c.dry_run == 1, "and both land");

    config_defaults(&c);
    config_parse_argv(&c, 2, upper);
    check(strcmp(c.title, "HYDRO") == 0, "the title word is case-insensitive");

    config_defaults(&c);
    config_parse_argv(&c, 3, titled);
    check(strcmp(c.title, "offroad") == 0, "--title works too");

    config_defaults(&c);
    config_parse_argv(&c, 3, probe);
    check(c.probe_image == 1 && c.dry_run == 1,
          "--probe-image implies --dry-run: it must never precede the game");

    /* A bare word that is not a title is a typo. Taking it for one would turn
     * a typo into a confusing failure much later.
     *
     * An option whose argument is missing falls through to the same refusal
     * rather than reading past the end of argv. */
    config_defaults(&c);
    stderr_off();
    {
        int typo_refused     = !config_parse_argv(&c, 2, typo);
        int dangling_refused = !config_parse_argv(&c, 2, dangling);
        stderr_on();
        check(typo_refused, "an unknown bare word is refused");
        check(dangling_refused,
              "an option with no argument is refused, not read past");
    }
}

int main(void)
{
    char root[MAX_PATH], folder[MAX_PATH];

    GetTempPathA(sizeof root, root);
    snprintf(folder, sizeof folder, "%.*sVCThunder-config-%lu",
             (int)(sizeof folder - sizeof "VCThunder-config-" - 12),
             root, GetCurrentProcessId());
    CreateDirectoryA(folder, NULL);
    snprintf(s_path, sizeof s_path, "%s\\vcthunder.ini", folder);

    test_defaults_are_all_legal();
    test_absent_file();
    test_invalid_keeps_the_previous_value();
    test_invalid_enum();
    test_invalid_bool();
    test_alias();
    test_misplaced_key_is_applied_and_reported();
    test_stranded_key_is_reported_harder();
    test_unknown_key_is_silent();
    test_comments_and_whitespace();
    test_per_title_sections();
    test_notes_are_consumed();
    test_note_overflow_is_announced();
    test_resolve_graphics();
    test_argv();

    DeleteFileA(s_path);
    RemoveDirectoryA(folder);

    if (failures) {
        fprintf(stderr, "config tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("config tests: all passed");
    return 0;
}
