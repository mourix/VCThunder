/* launcher_model_test.c -- the launcher's model, with no launcher.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The four binding validators, the section-precedence rule, the R4 "never write
 * a key the user did not touch" loop and the ini problem scan, all reachable
 * without linking a GUI. It builds the model, the document, the value grammar
 * and the settings table and NOTHING ELSE: if this ever needs a stub for
 * something that draws, the model has grown something it should not have.
 */
#include "../src/launcher_model.c"
#include "../src/ini_doc.c"
#include "../src/ini.c"
#include "../src/settings.c"

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

static char s_folder[MAX_PATH];

static void ini_path_for(const char *name, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s\\%s", s_folder, name);
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
        if (fp)
            fclose(fp);
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

/* Write `text` to a scratch ini and load the model from it. */
static int open_model(const char *name, const char *text, char *path,
                      size_t path_size)
{
    ini_path_for(name, path, path_size);
    if (!write_bytes(path, text))
        return 0;
    if (!model_open(path))
        return 0;
    model_load();
    return 1;
}

/* ---- the validators ------------------------------------------------------ */

static void test_key_names(void)
{
    check(key_name_valid("LCTRL"), "LCTRL is a key");
    check(key_name_valid("lctrl"), "key names are case-insensitive");
    check(key_name_valid("KEYCODE_LCTRL"), "the MAME prefix is accepted");
    check(key_name_valid("F1") && key_name_valid("F24"), "F1..F24 are keys");
    check(!key_name_valid("F25"), "F25 is past the end of a keyboard");
    check(key_name_valid("A") && key_name_valid("5"), "one alphanumeric is a key");
    check(key_name_valid("-") && key_name_valid("="),
          "the two punctuation keys the game binds are keys");
    check(key_name_valid("NONE"), "NONE is how the file spells unbound");
    check(key_name_valid("0x41") && key_name_valid("255"),
          "a raw virtual-key number is a key");
    check(!key_name_valid("256"), "a byte is as far as a virtual key goes");
    check(!key_name_valid("SUPER"), "an invented name is not a key");
    check(!key_name_valid(""), "nothing is not a key");

    check(keyboard_binding_valid("LCTRL"), "one key is a list of one");
    check(keyboard_binding_valid("LCTRL,SPACE, F1"),
          "the keyboard half is a list and it is trimmed");
    check(keyboard_binding_valid("A,B,C,D"),
          "IO_MAX_KEY_REFS keys fit");
    check(!keyboard_binding_valid("A,B,C,D,E"),
          "one more than IO_MAX_KEY_REFS does not");
    check(!keyboard_binding_valid("LCTRL,SUPER"),
          "a list is only as good as its worst item");
}

static void test_pad_bindings(void)
{
    check(pad_binding_valid("0"), "a bare 0 is how the file spells unbound");
    check(pad_binding_valid("1"), "an ordinal is a binding");
    /* IO_MAX_BUTTONS is 128 and the form must not hold a
     * lower ceiling than the preview beside it. */
    check(pad_binding_valid("40"), "a Pro DD's button 40 is a binding");
    check(pad_binding_valid("128"), "IO_MAX_BUTTONS itself is a binding");
    check(!pad_binding_valid("129"), "one past IO_MAX_BUTTONS is not");
    check(pad_binding_valid("th8:7"),
          "a device-qualified reference is a binding");
    check(!pad_binding_valid("th8:0"),
          "button zero of a NAMED device is not: buttons count from one");
    check(pad_binding_valid("1,2,3,4"), "IO_MAX_PAD_REFS references fit");
    check(!pad_binding_valid("1,2,3,4,5"),
          "one more than IO_MAX_PAD_REFS does not");
    check(!pad_binding_valid("th8:"), "a colon with no ordinal is not");
    check(!pad_binding_valid("nine"), "a word is not an ordinal");
}

/* ---- validate_settings, and the page it names ---------------------------- */

static void test_validation_names_a_page(void)
{
    char path[MAX_PATH], why[512];
    int page = -1;

    check(open_model("validate.ini",
                     "[graphics]\r\n"
                     "render_scale = 99\r\n", path, sizeof path),
          "the scratch ini opened");
    check(!validate_settings(why, sizeof why, &page),
          "99 is past render_scale's maximum");
    check(page == PAGE_GRAPHICS, "and the page it is on is named");
    check(strstr(why, "1..8") != NULL,
          "the reason carries the range, not just the word invalid");

    snprintf(model_slot((unsigned)setting_find("graphics", "render_scale")),
             INI_VALUE_CAP, "%s", "4");
    check(validate_settings(why, sizeof why, &page),
          "a value inside the range passes");

    snprintf(model_slot((unsigned)setting_find("graphics", "aspect")),
             INI_VALUE_CAP, "%s", "sideways");
    check(!validate_settings(why, sizeof why, &page),
          "a choice the table does not carry fails");
    check(page == PAGE_GRAPHICS, "on its own page");
    snprintf(model_slot((unsigned)setting_find("graphics", "aspect")),
             INI_VALUE_CAP, "%s", "16:9");

    /* An axis on another controller is legal for the three DirectInput roles
     * only: XInput reads one device. */
    {
        unsigned di = (unsigned)setting_find("analog", "dinput_throttle_axis");
        unsigned xi = (unsigned)setting_find("analog", "xinput_throttle_axis");

        snprintf(model_slot(di), INI_VALUE_CAP, "%s", "twcs:z");
        check(validate_settings(why, sizeof why, &page),
              "dinput_throttle_axis = twcs:z passes");
        snprintf(model_slot(di), INI_VALUE_CAP, "%s", "twcs:q");
        check(!validate_settings(why, sizeof why, &page),
              "a label before an axis that is not one fails");
        snprintf(model_slot(di), INI_VALUE_CAP, "%s", "pad:rt");
        check(validate_settings(why, sizeof why, &page),
              "an XInput pad's trigger after a label passes");
        snprintf(model_slot(di), INI_VALUE_CAP, "%s", "rt");
        check(!validate_settings(why, sizeof why, &page),
              "an XInput name with no label is not the wheel's axis");
        snprintf(model_slot(di), INI_VALUE_CAP, "%s", "y");
        snprintf(model_slot(xi), INI_VALUE_CAP, "%s", "pad:rt");
        check(!validate_settings(why, sizeof why, &page),
              "an XInput axis cannot name another controller");
        snprintf(model_slot(xi), INI_VALUE_CAP, "%s", "rt");
        check(validate_settings(why, sizeof why, &page),
              "and the plain XInput axis still passes");
    }

    /* A BINDING HAS NO PAGE OF ITS OWN and every one of them is on Controls. */
    snprintf(model_pad_binding(0, VCT_ACTION_boost), MODEL_BINDING_CAP, "%s",
             "999");
    check(!validate_settings(why, sizeof why, &page),
          "an impossible button number fails");
    check(page == PAGE_CONTROLS, "and a binding's page is Controls");
    snprintf(model_pad_binding(0, VCT_ACTION_boost), MODEL_BINDING_CAP, "%s",
             "1");

    snprintf(model_key_binding(1, VCT_ACTION_boost), MODEL_BINDING_CAP, "%s",
             "SUPER");
    check(!validate_settings(why, sizeof why, &page),
          "an invented key name fails");
    check(page == PAGE_CONTROLS, "and it is on Controls too");
    check(strstr(why, "keyboard") != NULL,
          "the reason says which half of the row is wrong");
    model_close();
}

static void test_unsafe_video_mode(void)
{
    char path[MAX_PATH];
    int i;

    check(open_model("video.ini", "[graphics]\r\nvideo_mode = 1\r\n",
                     path, sizeof path), "the scratch ini opened");
    check(!model_unsafe_video_mode(), "mode 1 is the cabinet's own");
    i = setting_find("graphics", "video_mode");
    check(i >= 0, "video_mode is in the table");
    snprintf(model_slot((unsigned)i), INI_VALUE_CAP, "%s", "2");
    check(model_unsafe_video_mode(),
          "mode 2 is 640x480 and Apply has to ask first");
    model_close();
}

/* ---- what is wrong with the file ----------------------------------------- */

static int problem_mentions(const char *needle)
{
    unsigned i;

    for (i = 0; i < model_problem_count(); i++)
        if (strstr(model_problem(i), needle))
            return 1;
    return 0;
}

static void test_ini_problems(void)
{
    char path[MAX_PATH];

    check(open_model("problems.ini",
            "stray = 1\r\n"                     /* above the first section  */
            "[graphics]\r\n"
            "render_scale = 2\r\n"
            "rendr_scale = 2\r\n"               /* a typo                   */
            "[devices]\r\n"
            "th8 = dinput:Thrustmaster\r\n"     /* the user's own words     */
            "[hydro.graphics]\r\n"
            "render_scale = 3\r\n"              /* a row can say this       */
            "[hydro.analog]\r\n"
            "dinput_steering_range_percent = 50\r\n"  /* nothing can        */
            "[offroad.graphics]\r\n"
            "bezel = bezels/offroad.png\r\n"  /* per cabinet by design    */
            "[gamepad]\r\n"
            "boost = 1\r\n"
            "[graphics]\r\n"                    /* the duplicate section    */
            "widescreen = true\r\n",
            path, sizeof path), "the scratch ini opened");
    scan_ini_problems();

    check(problem_mentions("appears twice"),
          "a second [section] of a name the file already has is reported");
    check(problem_mentions("rendr_scale"), "a misspelled key is reported");
    check(problem_mentions("above the first"),
          "a key before the first [section] is reported");
    check(!problem_mentions("th8"),
          "[devices] keys are the user's own words and are not typos");
    check(!problem_mentions("[hydro.graphics]"),
          "a per-title override of a key this window SHOWS is said by its row");
    check(problem_mentions("dinput_steering_range_percent"),
          "a per-title override of a key it does not show has nowhere else "
          "to be said");
    check(!problem_mentions("boost"),
          "a binding is not a setting and is not unknown");
    check(!problem_mentions("bezel"),
          "bezel art set per cabinet is how it is meant to be set");
    check(model_problem_total() >= model_problem_count(),
          "the total counts what the list could not hold");
    model_close();
}

/* A key twice in one block, and a section twice, each said the way the host
 * reads it: first-wins through the profile API, last-wins through config.c. */
static void test_duplicate_keys(void)
{
    char path[MAX_PATH];

    check(open_model("duplicates.ini",
            "[gamepad]\r\n"
            "boost = 2\r\n"
            "boost = 5\r\n"                     /* first-wins: disagrees   */
            "[graphics]\r\n"
            "render_scale = 1\r\n"
            "render_scale = 3\r\n"              /* last-wins: agrees        */
            "[diagnostics]\r\n"
            "ffb_trace = true\r\n",             /* read, just not by config.c */
            path, sizeof path), "the scratch ini opened");
    scan_ini_problems();
    check(problem_mentions("[gamepad] boost is set twice. The game uses the FIRST"),
          "a first-wins key set twice says the game and this window disagree");
    check(problem_mentions("[graphics] render_scale is set twice. The later"),
          "a config.c key set twice says the later line wins");
    check(!problem_mentions("ffb_trace"),
          "a switch io_config.c reads is not a line nothing reads");
    check(model_problem_total() == 2, "exactly the two duplicates are reported");
    model_close();

    check(open_model("dup-sections.ini",
            "[graphics]\r\nrender_scale = 1\r\n"
            "[ffb]\r\nrumble = true\r\n"
            "[graphics]\r\nrender_scale = 3\r\n"
            "[ffb]\r\nrumble = false\r\n",
            path, sizeof path), "the scratch ini opened");
    scan_ini_problems();
    check(problem_mentions("[graphics] appears twice. The game reads both"),
          "a second block of a config.c section is read, and says so");
    check(problem_mentions("[ffb] appears twice. The game reads the first one"),
          "a second block of a profile-API section is ignored, and says so");
    check(!problem_mentions("is set twice"),
          "a key in a second block is the section report's, not a key report");
    model_close();
}

/* ---- the section a key is read from and written to ------------------------ */

static void test_device_scope(void)
{
    char path[MAX_PATH], section[96];
    int i;

    check(open_model("scoped.ini",
            "[analog]\r\n"
            "provider = dinput\r\n"
            "dinput_name = Thrustmaster\r\n"
            "deadzone_percent = 12\r\n"
            "dinput_steering_range_percent = 100\r\n"
            "[devices]\r\n"
            "th8 = dinput:Thrustmaster\r\n"
            "[analog.th8]\r\n"
            "dinput_steering_range_percent = 50\r\n",
            path, sizeof path), "the scratch ini opened");

    check(!strcmp(model_profile(), "th8"),
          "the [devices] label of the configured device is resolved");
    check(!strcmp(model_value("analog", "dinput_steering_range_percent"), "50"),
          "a key belonging to that device is read from its own section");
    check(!strcmp(model_value("analog", "deadzone_percent"), "12"),
          "a key that is not about the device still comes from [analog]");

    i = setting_find("analog", "dinput_steering_range_percent");
    snprintf(section, sizeof section, "%s", setting_section((unsigned)i));
    check(!strcmp(section, "analog.th8"),
          "and that is the section it is written back to");

    i = setting_find("analog", "dinput_name");
    check(!strcmp(setting_section((unsigned)i), "analog"),
          "a key that SELECTS a device cannot live in that device's section");
    model_close();
}

/* ---- a [devices] label per controller, not per button --------------------
 *
 * Whether a controller already has a line is the host resolver's question:
 * the padded line, a line shortened by hand, a fresh device asked twice, the
 * second of two same-named collections, and a label already taken. */
static unsigned device_line_count(void)
{
    char label[INI_LABEL_CAP];
    unsigned n = 0;

    while (ini_doc_key_index(model_doc(), "devices", n, label, sizeof label))
        n++;
    return n;
}

#define TM_FANATEC "FANATEC Wheel product_00060eb7-0000-0000-0000-504944564944"
#define TM_GENERIC "Generic   USB  Joystick product_00060079-0000-0000-0000-" \
                   "504944564944"
#define TM_INST(c) " instance_" c "0000000-0000-0000-0000-000000000000"

/* Every [devices] line in the model opens the device the test says it does. */
static int line_opens(const char *label, const ini_device_id_t *dev,
                      unsigned count, int want)
{
    return model_device_for_label(label, "dinput", dev, count) == want;
}

static void test_device_labels(void)
{
    static const ini_device_id_t dev[] = {
        { TM_FANATEC TM_INST("a"), "FANATEC Wheel", 0 },
        { TM_FANATEC TM_INST("b"), "FANATEC Wheel", 1 },
        { TM_GENERIC TM_INST("c"), "Generic   USB  Joystick", 2 },
        { "TH8A RS Shifter product_00000000-0000-0000-0000-504944564944"
          TM_INST("e"), "TH8A RS Shifter", 3 },
    };
    /* Two identical encoders, in both orders. */
    static const ini_device_id_t two[] = {
        { TM_GENERIC TM_INST("c"), "Generic   USB  Joystick", 0 },
        { TM_GENERIC TM_INST("d"), "Generic   USB  Joystick", 1 },
    };
    static const ini_device_id_t two_flipped[] = {
        { TM_GENERIC TM_INST("d"), "Generic   USB  Joystick", 0 },
        { TM_GENERIC TM_INST("c"), "Generic   USB  Joystick", 1 },
    };
    static const ini_device_id_t pads[] = { { "", "", 0 }, { "", "", 3 } };
    char path[MAX_PATH], label[INI_LABEL_CAP], value[INI_VALUE_CAP];
    char match[INI_MATCH_CAP];
    unsigned i;

    check(open_model("labels.ini",
            "[devices]\r\n"
            "fanatec = dinput:FANATEC Wheel\r\n"
            "fanatec2 = dinput:2\r\n"
            "generic = dinput:Generic   USB  Joystick  \r\n"
            "th8 = dinput:TH8A\r\n",
            path, sizeof path), "the scratch ini opened");

    for (i = 0; i < 3; i++)
        check(model_device_label_ensure("dinput", dev, 4, 2, "generic", label,
                                        sizeof label) &&
              !strcmp(label, "generic"),
              "the padded line is the encoder's, however often a button on it "
              "is bound");
    check(model_device_label_ensure("dinput", dev, 4, 3, "th8a", label,
                                    sizeof label) && !strcmp(label, "th8"),
          "a line the user shortened by hand is reused, not duplicated");
    check(model_device_label_find("dinput", dev, 4, 0, label, sizeof label) &&
          !strcmp(label, "fanatec"),
          "the wheel's line is the first collection's");
    check(!model_device_label_find("dinput", dev, 4, 1, label, sizeof label),
          "and neither `fanatec` nor `fanatec2 = dinput:2` opens the second");
    check(line_opens("fanatec2", dev, 4, -1), "`dinput:2` opens nothing");
    /* The second collection gets its ID. */
    check(model_device_label_ensure("dinput", dev, 4, 1, "fanatec", label,
                                    sizeof label) &&
          !strcmp(label, "fanatec3") &&
          ini_doc_get(model_doc(), "devices", label, value, sizeof value) &&
          !strcmp(value, "dinput:" TM_FANATEC TM_INST("b")),
          "the second collection is given its full ID, its name opening the "
          "first");
    check(line_opens("fanatec3", dev, 4, 1) && line_opens("fanatec", dev, 4, 0)
          && line_opens("generic", dev, 4, 2) && line_opens("th8", dev, 4, 3),
          "and every line still opens what it opened");
    model_close();

    check(open_model("fresh.ini", "[analog]\r\nprovider = auto\r\n",
                     path, sizeof path), "the fresh ini opened");
    check(model_device_label_ensure("dinput", dev, 4, 2, "generic", label,
                                    sizeof label) &&
          !strcmp(label, "generic") &&
          ini_doc_get(model_doc(), "devices", "generic", value, sizeof value) &&
          !strcmp(value, "dinput:Generic   USB  Joystick"),
          "a new controller with a name of its own gets its name, not its ID");
    check(model_device_label_ensure("dinput", dev, 4, 2, "generic", label,
                                    sizeof label) &&
          !strcmp(label, "generic") && device_line_count() == 1,
          "and binding a second button on it finds that line");
    check(model_device_label_ensure("dinput", dev, 4, 3, "generic", label,
                                    sizeof label) &&
          !strcmp(label, "generic2"),
          "a label already taken by another controller gets a number");
    check(model_device_label_ensure("xinput", pads, 2, 1, "xinput", label,
                                    sizeof label) &&
          ini_doc_get(model_doc(), "devices", label, value, sizeof value) &&
          !strcmp(value, "xinput:3"),
          "a numbered provider writes the device's number, not its position");
    model_close();

    /* Two identical encoders: the first gets the name, the second its ID, and
     * each line still opens its own after a replug flips their order. */
    check(open_model("twins.ini", "[analog]\r\nprovider = auto\r\n",
                     path, sizeof path), "the twins ini opened");
    check(model_device_label_ensure("dinput", two, 2, 0, "generic", label,
                                    sizeof label) && !strcmp(label, "generic")
          && ini_doc_get(model_doc(), "devices", "generic", value,
                         sizeof value) &&
          !strcmp(value, "dinput:Generic   USB  Joystick"),
          "the first of two identical devices gets the name");
    check(model_device_label_ensure("dinput", two, 2, 1, "generic", label,
                                    sizeof label) &&
          !strcmp(label, "generic2") &&
          ini_doc_get(model_doc(), "devices", "generic2", value,
                      sizeof value) &&
          !strcmp(value, "dinput:" TM_GENERIC TM_INST("d")),
          "the second gets its ID");
    check(line_opens("generic", two, 2, 0) && line_opens("generic2", two, 2, 1),
          "each line opens its own encoder");
    check(line_opens("generic", two_flipped, 2, 1) &&
          line_opens("generic2", two_flipped, 2, 0),
          "and still does after the order flips");
    model_close();

    /* The first of two, the second already pinned: the name is enough,
     * because the pin keeps the name line off the second. */
    check(open_model("pinned.ini",
            "[devices]\r\n"
            "box2 = dinput:" TM_GENERIC TM_INST("d") "\r\n",
            path, sizeof path), "the pinned ini opened");
    check(model_device_label_ensure("dinput", two_flipped, 2, 1, "generic",
                                    label, sizeof label) &&
          ini_doc_get(model_doc(), "devices", label, value, sizeof value) &&
          !strcmp(value, "dinput:Generic   USB  Joystick"),
          "the first of two, with the second pinned, gets the name even where "
          "the second enumerates first");
    check(line_opens(label, two, 2, 0) && line_opens(label, two_flipped, 2, 1),
          "and that line re-resolves to it in both orders");

    /* And dinput_name by the same rule, as adopt writes it. */
    check(model_device_match("dinput", two, 2, 0, 0, match, sizeof match) &&
          !strcmp(match, "Generic   USB  Joystick"),
          "adopting the unpinned encoder writes its name to dinput_name");
    check(model_device_match("dinput", two, 2, 1, 0, match, sizeof match) &&
          !strcmp(match, TM_GENERIC TM_INST("d")),
          "adopting the pinned one writes its ID: the name opens the other");
    snprintf(model_slot((unsigned)setting_find("analog", "dinput_name")),
             INI_VALUE_CAP, "%s", "Generic   USB  Joystick");
    check(model_configured_device(two_flipped, NULL, 2) == 1,
          "and the name, once written, opens the unpinned encoder after a "
          "replug");
    model_close();
}

/* ---- the controller the host drives with ----------------------------------
 *
 * pick_device(): dinput_name, then (auto) the first that publishes effects.
 * The launcher must agree, or the page edits a section the game does not
 * read. */
static const ini_device_id_t k_pad_wheel[2] = {
    { "Bluetooth LE XINPUT compatible input device product_0b13045e-0000-0000-"
      "0000-504944564944 instance_d60686b0-8594-11ee-8002-444553540000",
      "Bluetooth LE XINPUT compatible input device", 0 },
    { "Thrustmaster TMX Racing Wheel product_b67f044f-0000-0000-0000-"
      "504944564944 instance_5be31ef0-95b0-11f1-8001-444553540000",
      "Thrustmaster TMX Racing Wheel", 1 },
};
static const int k_pad_wheel_ffb[2] = { 0, 1 };

static unsigned fake_attached(const char *provider, ini_device_id_t *ids,
                              int *ffb, unsigned max)
{
    unsigned k;

    if (_stricmp(provider, "dinput") || max < 2)
        return 0;
    for (k = 0; k < 2; k++) {
        ids[k] = k_pad_wheel[k];
        ffb[k] = k_pad_wheel_ffb[k];
    }
    return 2;
}

static void test_configured_device(void)
{
    char path[MAX_PATH];
    static const char *const k_table =
        "[devices]\r\n"
        "pad = dinput:Bluetooth LE XINPUT\r\n"
        "tmx = dinput:Thrustmaster TMX\r\n"
        "[analog.tmx]\r\n"
        "dinput_throttle_axis = pad:z\r\n";
    char text[512];

    model_set_attached(fake_attached);

    snprintf(text, sizeof text, "[analog]\r\nprovider = dinput\r\n%s",
             k_table);
    check(open_model("auto.ini", text, path, sizeof path), "auto.ini opened");
    check(model_configured_device(k_pad_wheel, k_pad_wheel_ffb, 2) == 1,
          "auto is the first controller with force feedback, not the first");
    check(model_configured_device(k_pad_wheel, NULL, 2) == -1,
          "and with nothing known about effects, auto opens nothing");
    check(!strcmp(model_profile(), "tmx"),
          "so the profile is the wheel's, the section the host reads");
    check(!strcmp(model_value("analog", "dinput_throttle_axis"), "pad:z"),
          "and the page shows the wheel's role keys, a label and all");
    model_close();

    snprintf(text, sizeof text, "[analog]\r\nprovider = auto\r\n%s",
             k_table);
    check(open_model("auto2.ini", text, path, sizeof path), "auto2.ini opened");
    check(!strcmp(model_profile(), "tmx"),
          "provider = auto opens the same wheel by the same rule");
    model_close();

    snprintf(text, sizeof text,
             "[analog]\r\nprovider = dinput\r\n"
             "dinput_name = Bluetooth\r\n%s", k_table);
    check(open_model("name.ini", text, path, sizeof path), "name.ini opened");
    check(model_configured_device(k_pad_wheel, k_pad_wheel_ffb, 2) == 0 &&
          !strcmp(model_profile(), "pad"), "a name beats auto");
    model_close();

    /* A role's facts follow its controller, as the host reads them. */
    {
        char why[256], have[64];
        int changed = -1;
        unsigned axis = (unsigned)setting_find("analog", "dinput_throttle_axis");
        unsigned inv = (unsigned)setting_find("analog", "dinput_throttle_invert");

        check(open_model("facts.ini",
                "[analog]\r\nprovider = dinput\r\n"
                "dinput_deadzone_percent = 2\r\n"
                "[devices]\r\n"
                "pad = dinput:Bluetooth LE XINPUT\r\n"
                "pad2 = dinput:Bluetooth\r\n"
                "tmx = dinput:Thrustmaster TMX\r\n"
                "[analog.tmx]\r\n"
                "dinput_throttle_axis = pad:z\r\n"
                "dinput_brake_invert = false\r\n"
                "[analog.pad]\r\n"
                "dinput_throttle_invert = false\r\n"
                "dinput_deadzone_percent = 5\r\n",
                path, sizeof path), "facts.ini opened");
        /* What the preview reads for a role on another controller: that
         * controller's section, under its first label, then [analog]. */
        check(!strcmp(model_device_value("pad", "dinput_deadzone_percent"), "5"),
              "the pad's deadzone is [analog.pad]'s");
        check(!strcmp(model_device_value("pad2", "dinput_deadzone_percent"),
                      "5"), "and a second name for the pad reads the same");
        check(!strcmp(model_device_value("tmx", "dinput_deadzone_percent"),
                      model_value("analog", "dinput_deadzone_percent")) &&
              !strcmp(model_device_value("", "dinput_deadzone_percent"),
                      model_value("analog", "dinput_deadzone_percent")),
              "the configured wheel's is the model's own value");
        check(!strcmp(model_device_value("gone", "dinput_deadzone_percent"),
                      "2"), "a label with no section falls back to [analog]");
        check(!strcmp(setting_section(inv), "analog.pad") &&
              !strcmp(model_value("analog", "dinput_throttle_invert"), "false"),
              "the throttle's invert is the pad's, read from [analog.pad]");
        check(!strcmp(setting_section((unsigned)setting_find("analog",
                          "dinput_brake_invert")), "analog.tmx") &&
              !strcmp(model_value("analog", "dinput_brake_invert"), "false"),
              "the brake's, on the wheel, is still the wheel's");

        snprintf(model_slot(axis), INI_VALUE_CAP, "%s", "pad2:z");
        model_sync_role_facts(NULL);
        check(!strcmp(setting_section(inv), "analog.pad"),
              "a second name for the pad reads the pad's first label's section");

        /* Moved back to the wheel: re-read there, not copied across. */
        snprintf(model_slot(axis), INI_VALUE_CAP, "%s", "rz");
        check(model_sync_role_facts(NULL) == 1 &&
              !strcmp(setting_section(inv), "analog.tmx") &&
              !strcmp(model_value("analog", "dinput_throttle_invert"), "true"),
              "a role moved back re-reads its facts from the wheel");
        check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
              changed == 1, "and saving writes the axis alone");
        check(!ini_doc_get(model_doc(), "analog.tmx", "dinput_throttle_invert",
                           have, sizeof have),
              "the pad's invert was not copied into [analog.tmx]");
        model_close();
    }

    snprintf(text, sizeof text, "[analog]\r\nprovider = xinput\r\n%s",
             k_table);
    check(open_model("xi.ini", text, path, sizeof path), "xi.ini opened");
    check(!model_profile()[0],
          "provider = xinput is not the DirectInput wheel's profile");
    model_close();

    model_set_attached(NULL);
}

/* ---- R4: never write a key the user did not touch ------------------------ */

static void test_save_writes_only_what_changed(void)
{
    char path[MAX_PATH], why[512], *before, *after;
    int changed = -1;

    check(open_model("r4.ini",
            "; a comment the user wrote\r\n"
            "[graphics]\r\n"
            "render_scale   = 2   ; and a trailing one\r\n"
            "[hydro.digital]\r\n"
            "boost = LCTRL\r\n",             /* == what it would inherit   */
            path, sizeof path), "the scratch ini opened");
    before = read_bytes(path);
    check(before != NULL, "the file read back");

    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "saving an untouched model succeeds");
    check(changed == 0, "...and writes nothing at all");
    after = read_bytes(path);
    check(after && before && !strcmp(before, after),
          "AN APPLY THAT CHANGED NOTHING LEAVES THE FILE BYTE-IDENTICAL: it "
          "must not comment out a line that spells out an inherited value, "
          "and must not replace vcthunder.ini.bak with a copy of this");
    free(after);

    snprintf(model_slot((unsigned)setting_find("graphics", "render_scale")),
             INI_VALUE_CAP, "%s", "3");
    changed = -1;
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "saving one changed key succeeds");
    check(changed == 1, "...and writes exactly one key");
    after = read_bytes(path);
    check(after && strstr(after, "render_scale") &&
          strstr(after, "= 3") != NULL, "the new value is in the file");
    check(after && strstr(after, "a comment the user wrote") != NULL,
          "the user's comments survive a save");
    check(after && strstr(after, "trailing one") != NULL,
          "a trailing comment survives too");
    check(after && strstr(after, "boost = LCTRL") != NULL,
          "AND THE SPELLED-OUT INHERITED BINDING IS STILL THERE. It is only "
          "removed when the value CHANGED to the inherited one, so a save "
          "that changes nothing else leaves it alone");
    check(after && !strstr(after, "window_scale"),
          "no key the file did not carry was injected");
    free(after);
    free(before);
    model_close();
}

/* ---- one controller per cabinet ------------------------------------------
 *
 * The host reads [<title>.analog] and [<title>.analog.<label>] first, so each
 * cabinet can choose its own controller and axes; facts about a controller
 * stay shared. The model shows one cabinet at a time and Apply writes both. */
static int doc_says(const char *section, const char *key, const char *want)
{
    char have[INI_VALUE_CAP];

    return ini_doc_get(model_doc(), section, key, have, sizeof have) &&
           !strcmp(have, want);
}

static int doc_has(const char *section, const char *key)
{
    char have[INI_VALUE_CAP];

    return ini_doc_get(model_doc(), section, key, have, sizeof have);
}

static void slot_set(const char *key, const char *value)
{
    snprintf(model_slot((unsigned)setting_find("analog", key)), INI_VALUE_CAP,
             "%s", value);
}

static const char *section_of(const char *key)
{
    return setting_section((unsigned)setting_find("analog", key));
}

static void test_per_cabinet_controls(void)
{
    char path[MAX_PATH], why[512], *before, *after;
    int changed = -1;
    unsigned k;

    check(open_model("cabinets.ini",
            "[analog]\r\n"
            "provider = dinput\r\n"
            "dinput_name = TMX\r\n"
            "[devices]\r\n"
            "tmx = dinput:TMX\r\n"
            "pad = xinput:0\r\n"
            "[analog.tmx]\r\n"
            "dinput_throttle_axis = rz\r\n"
            "dinput_throttle_invert = true\r\n"
            "[offroad.analog]\r\n"
            "provider = xinput\r\n",
            path, sizeof path), "the scratch ini opened");

    check(model_controls_split(), "a choice in [offroad.analog] splits the "
          "file");
    check(model_controls_title() == 0, "Hydro is shown first");
    check(!strcmp(model_value("analog", "provider"), "dinput") &&
          !strcmp(model_profile(), "tmx"),
          "Hydro inherits the shared wheel");
    check(!strcmp(model_value("analog", "dinput_throttle_axis"), "rz"),
          "and its axis map");
    check(!strcmp(section_of("provider"), "hydro.analog"),
          "a cabinet's choice of controller is written under its title");
    check(!strcmp(section_of("dinput_throttle_axis"), "hydro.analog.tmx"),
          "AN AXIS GOES TO [hydro.analog.tmx], NOT [hydro.analog]: the "
          "device's own section beats the title's, so [analog.tmx] would win");
    check(!strcmp(section_of("dinput_throttle_invert"), "analog.tmx"),
          "a fact about the controller stays shared");
    check(!strcmp(section_of("deadzone_percent"), "analog"),
          "and so does a key that chooses nothing");

    check(model_controls_show(1), "Offroad can be shown");
    check(!strcmp(model_value("analog", "provider"), "xinput") &&
          !strcmp(model_profile(), "pad"),
          "Offroad reads its own provider, and the pad's profile with it");
    slot_set("xinput_steering_axis", "rx");
    check(model_controls_show(0), "back to Hydro");
    check(doc_says("offroad.analog.pad", "xinput_steering_axis", "rx"),
          "Offroad's edit went into the document on the way past");
    check(!strcmp(model_value("analog", "provider"), "dinput"),
          "and Hydro is shown again as it was");
    slot_set("dinput_throttle_axis", "z");

    before = read_bytes(path);
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "Apply succeeds");
    check(changed == 2, "both cabinets' edits are counted");
    check(doc_says("hydro.analog.tmx", "dinput_throttle_axis", "z") &&
          doc_says("analog.tmx", "dinput_throttle_axis", "rz"),
          "Hydro's axis is its own; the shared map is untouched");
    check(!doc_has("hydro.analog", "provider"),
          "a choice Hydro still inherits is not written down");

    check(model_reopen(), "the file reads back");
    model_load();
    check(model_controls_split() && model_controls_title() == 0,
          "it is still split, and the cabinet shown is kept");
    check(!strcmp(model_value("analog", "dinput_throttle_axis"), "z"),
          "Hydro's axis comes back from [hydro.analog.tmx]");
    model_controls_show(1);
    check(!strcmp(model_value("analog", "xinput_steering_axis"), "rx"),
          "and Offroad's from [offroad.analog.pad]");

    free(before);
    before = read_bytes(path);
    changed = -1;
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
          changed == 0, "R4: an Apply after switching and changing nothing "
          "writes nothing");
    after = read_bytes(path);
    check(before && after && !strcmp(before, after),
          "...and the file is byte-identical");
    free(after);
    free(before);

    scan_ini_problems();
    for (k = 0; k < model_problem_count(); k++)
        if (strstr(model_problem(k), "offroad.analog"))
            break;
    check(k == model_problem_count(),
          "a cabinet's own choices are not reported as lines nothing shows");

    /* Join from Offroad: its pad becomes everybody's. */
    check(model_controls_join(), "join succeeds");
    check(!model_controls_split(), "and the file is no longer split");
    changed = -1;
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "Apply after join succeeds");
    check(doc_says("analog", "provider", "xinput"),
          "the shown cabinet's controller is now the shared one");
    check(!doc_has("offroad.analog", "provider") &&
          !doc_has("hydro.analog.tmx", "dinput_throttle_axis") &&
          !doc_has("offroad.analog.pad", "xinput_steering_axis"),
          "every per-cabinet choice is commented out");
    check(doc_says("analog.pad", "xinput_steering_axis", "rx"),
          "the shown cabinet's axis is now the pad's shared one");
    check(model_reopen(), "the joined file reads back");
    model_load();
    check(!model_controls_split(), "and reads as shared");
    model_close();
}

/* ---- force feedback per cabinet ----------------------------------------
 *
 * Every [ffb] key is read [<title>.ffb] then [ffb], split or not, so the page
 * always shows one cabinet's and an edit goes under its title. */
static const char *ffb_value(const char *key)
{
    return model_value("ffb", key);
}

static void test_per_cabinet_ffb(void)
{
    char path[MAX_PATH], why[512], *before, *after;
    int changed = -1, i = setting_find("ffb", "strength_percent");
    unsigned k;

    check(open_model("ffb.ini",
            "[ffb]\r\n"
            "strength_percent = 100\r\n"
            "rumble_texture_percent = 50\r\n"
            "[hydro.ffb]\r\n"
            "rumble =\r\n"
            "[offroad.ffb]\r\n"
            "rumble_texture_percent = 8\r\n",
            path, sizeof path), "the scratch ini opened");
    check(model_controls_show(MODEL_TITLE_HYDRO), "Hydro can be shown");
    check(!model_controls_split(), "a cabinet's tuning splits no controller");
    check(i >= 0 && model_setting_per_cabinet((unsigned)i),
          "a force-feedback row is per cabinet");
    check(!strcmp(setting_section((unsigned)i), "hydro.ffb"),
          "and is written under the shown cabinet's title");
    check(!strcmp(ffb_value("rumble_texture_percent"), "50"),
          "Hydro falls through to [ffb]");
    check(!strcmp(ffb_value("rumble"), "true"),
          "an empty cabinet line is absent: the default, not an empty box");

    check(model_controls_show(MODEL_TITLE_OFFROAD), "Offroad can be shown");
    check(!strcmp(ffb_value("rumble_texture_percent"), "8"),
          "Offroad reads its own [offroad.ffb]");
    snprintf(model_slot((unsigned)i), INI_VALUE_CAP, "%s", "80");
    check(model_controls_show(MODEL_TITLE_HYDRO), "back to Hydro");
    check(doc_says("offroad.ffb", "strength_percent", "80"),
          "Offroad's edit went into the document on the way past");
    check(!strcmp(ffb_value("strength_percent"), "100"),
          "and Hydro's strength is still the shared one");

    changed = -1;
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
          changed == 1, "Apply writes the one edit");
    check(doc_says("ffb", "strength_percent", "100") &&
          !doc_has("hydro.ffb", "strength_percent") &&
          !doc_has("arctic.ffb", "strength_percent"),
          "R4: the shared value and the other cabinets are not written");
    check(doc_says("hydro.ffb", "rumble", ""),
          "and the empty line nobody touched is left as it was");

    check(model_reopen(), "the file reads back");
    model_load();
    check(model_controls_title() == MODEL_TITLE_HYDRO &&
          !strcmp(ffb_value("strength_percent"), "100"),
          "Hydro is still shown, at the shared strength");
    model_controls_show(MODEL_TITLE_OFFROAD);
    check(!strcmp(ffb_value("strength_percent"), "80"),
          "and Offroad's comes back from [offroad.ffb]");
    model_controls_show(MODEL_TITLE_ARCTIC);
    check(!strcmp(ffb_value("strength_percent"), "100") &&
          !strcmp(ffb_value("rumble_texture_percent"), "50"),
          "Arctic, with no section of its own, reads [ffb]");

    before = read_bytes(path);
    changed = -1;
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
          changed == 0, "R4: switching through all three writes nothing");
    after = read_bytes(path);
    check(before && after && !strcmp(before, after),
          "...and the file is byte-identical");
    free(after);
    free(before);

    scan_ini_problems();
    for (k = 0; k < model_problem_count(); k++)
        if (strstr(model_problem(k), ".ffb"))
            break;
    check(k == model_problem_count(),
          "a cabinet's own tuning is not reported as a line nothing shows");
    model_controls_show(MODEL_TITLE_HYDRO);
    model_close();
}

/* Edits made and undone across game-tab switches cancel out: Apply writes
 * nothing, and so leaves vcthunder.ini.bak alone. */
static void test_per_cabinet_ffb_revert(void)
{
    static const char *const text[2] = {
        "[ffb]\r\nstrength_percent = 100\r\n",
        "[ffb]\r\nstrength_percent = 100\r\n"
        "[hydro.ffb]\r\nstrength_percent = 90\r\n",
    };
    char path[MAX_PATH], bak[MAX_PATH], why[512], *before, *after;
    int changed, i = setting_find("ffb", "strength_percent"), page;
    unsigned t;

    for (t = 0; t < 2; t++) {
        const char *was = t ? "90" : "100";

        check(open_model("ffb-revert.ini", text[t], path, sizeof path),
              "the scratch ini opened");
        snprintf(bak, sizeof bak, "%s.bak", path);
        DeleteFileA(bak);
        before = read_bytes(path);
        model_controls_show(MODEL_TITLE_HYDRO);
        snprintf(model_slot((unsigned)i), INI_VALUE_CAP, "%s", "80");
        check(model_controls_show(MODEL_TITLE_OFFROAD), "to Offroad");
        check(doc_says("hydro.ffb", "strength_percent", "80"),
              "Hydro's edit went into the document");
        check(model_controls_show(MODEL_TITLE_HYDRO), "back to Hydro");
        snprintf(model_slot((unsigned)i), INI_VALUE_CAP, "%s", was);
        check(model_controls_show(MODEL_TITLE_OFFROAD), "and away again");
        changed = -1;
        check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
              changed == 0, t ? "an edit undone on a line the file has "
                                "writes nothing"
                              : "an edit undone on a line a switch added "
                                "writes nothing");
        after = read_bytes(path);
        check(before && after && !strcmp(before, after),
              "...and the file is byte-identical");
        check(GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES,
              "...and no backup was made of it");
        free(after);
        free(before);
        model_controls_show(MODEL_TITLE_HYDRO);
        model_close();
    }

    /* The game a switch left behind is validated as the shown one is. */
    check(open_model("ffb-revert.ini", text[0], path, sizeof path),
          "the scratch ini opened");
    model_controls_show(MODEL_TITLE_OFFROAD);
    snprintf(model_slot((unsigned)i), INI_VALUE_CAP, "%s", "lots");
    check(model_controls_show(MODEL_TITLE_HYDRO), "away from Offroad");
    page = -1;
    check(!validate_settings(why, sizeof why, &page) &&
          !strncmp(why, "Offroad Thunder: ", 17) &&
          page == k_settings[i].page,
          "a bad value on a game not shown is refused, naming the game");
    model_close();
}

static void test_per_cabinet_split_on(void)
{
    char path[MAX_PATH], why[512];
    int changed = -1, i;

    check(open_model("split-on.ini",
            "[analog]\r\n"
            "provider = auto\r\n",
            path, sizeof path), "the scratch ini opened");
    check(!model_controls_split(), "a file with no cabinet section is shared");
    i = setting_find("analog", "provider");
    snprintf(model_slot((unsigned)i), INI_VALUE_CAP, "%s", "dinput");
    check(model_controls_split_on(), "split succeeds");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "Apply succeeds");
    check(doc_says("hydro.analog", "provider", "dinput") &&
          doc_says("offroad.analog", "provider", "dinput") &&
          doc_says("arctic.analog", "provider", "dinput"),
          "what the page held when it split is both cabinets' starting point, "
          "and the provider in both is what makes the file read as split");
    check(!doc_has("analog", "provider"),
          "THE SELECTOR MOVES OUT OF [analog]: a copy left there is what a "
          "cabinet that clears its own would fall through to");
    check(model_reopen(), "the file reads back");
    model_load();
    check(model_controls_split(), "and it does");
    model_close();

    /* The cabinet shown survives a reload, as the launcher's tab does. */
    model_controls_show(0);
    check(open_model("hydro-own.ini",
            "[analog]\r\n"
            "provider = dinput\r\n"
            "[hydro.analog]\r\n"
            "provider = xinput\r\n"
            "xinput_device = 1\r\n",
            path, sizeof path), "the scratch ini opened");
    check(!strcmp(model_value("analog", "provider"), "xinput") &&
          !strcmp(model_value("analog", "xinput_device"), "1"),
          "the cabinet shown at load reads its own choice, not the shared one");
    model_controls_show(1);
    check(!strcmp(model_value("analog", "provider"), "dinput") &&
          !strcmp(model_value("analog", "xinput_device"), "0"),
          "and the other cabinet falls through to [analog] and the default");
    model_close();

    check(open_model("fact-scoped.ini",
            "[devices]\r\n"
            "tmx = dinput:TMX\r\n"
            "[hydro.analog.tmx]\r\n"
            "dinput_throttle_invert = true\r\n",
            path, sizeof path), "the scratch ini opened");
    check(!model_controls_split(), "a fact is not a choice and splits nothing");
    scan_ini_problems();
    check(model_problem_count() == 0,
          "a fact set for one cabinet is said by its own row "
          "(setting_title_override), not twice");
    model_close();

    check(open_model("selector-under-label.ini",
            "[devices]\r\n"
            "tmx = dinput:TMX\r\n"
            "[hydro.analog.tmx]\r\n"
            "provider = xinput\r\n"
            "[hydro.analog]\r\n"
            "dinput_name =\r\n",
            path, sizeof path), "the scratch ini opened");
    check(!model_controls_split(),
          "A SELECTOR UNDER A CONTROLLER'S LABEL IS READ BY NOTHING, and an "
          "empty one is read as absent: neither splits the file");
    scan_ini_problems();
    check(model_problem_count() == 1 &&
          strstr(model_problem(0), "chooses a controller") != NULL,
          "the dead selector is listed on Help");
    model_close();
}

/* THE THIRD TITLE: Arctic's own column of bindings.def, the controls it does
 * not have, its own sections, and a controller choice of its own. */
static void test_arctic_title(void)
{
    char path[MAX_PATH], why[512];
    binding_item_t row[8], one;
    int changed = -1;
    unsigned n;

    check(!strcmp(model_title_id(MODEL_TITLE_ARCTIC), "arctic") &&
          !strcmp(model_title_name(MODEL_TITLE_ARCTIC), "Arctic Thunder"),
          "Arctic's section prefix and name");
    check(k_actions[VCT_ACTION_boost].label[MODEL_TITLE_ARCTIC] &&
          !strcmp(k_actions[VCT_ACTION_boost].label[MODEL_TITLE_ARCTIC],
                  "Attack"), "Hydro's Boost row is Arctic's Attack");
    check(!k_actions[VCT_ACTION_brake].label[MODEL_TITLE_ARCTIC] &&
          !k_actions[VCT_ACTION_view_high].label[MODEL_TITLE_ARCTIC] &&
          !k_actions[VCT_ACTION_shift1].label[MODEL_TITLE_ARCTIC],
          "no brake, views or shifter on Arctic's page");

    check(open_model("arctic.ini", "[analog]\r\nprovider = auto\r\n",
                     path, sizeof path), "the scratch ini opened");
    n = model_binding_items(MODEL_TITLE_ARCTIC, VCT_ACTION_boost, row, 8);
    check(n == 2 && !row[0].pad && !strcmp(row[0].value, "LCTRL") &&
          row[1].pad && !strcmp(row[1].value, "1"),
          "Attack defaults to LCTRL and pad button 1");
    one.pad = 0;
    snprintf(one.value, sizeof one.value, "%s", "SPACE");
    check(model_binding_set_items(MODEL_TITLE_ARCTIC, VCT_ACTION_boost, &one,
                                  1), "Attack can be rebound");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "Apply succeeds");
    check(doc_says("arctic.digital", "boost", "SPACE") &&
          !doc_has("digital", "boost") && !doc_has("hydro.digital", "boost"),
          "an Arctic binding goes under [arctic.digital] and nowhere else");
    model_close();

    check(open_model("arctic-own.ini",
            "[analog]\r\n"
            "provider = dinput\r\n"
            "[arctic.analog]\r\n"
            "provider = xinput\r\n",
            path, sizeof path), "the scratch ini opened");
    check(model_controls_split(), "a choice in [arctic.analog] splits the file");
    model_controls_show(MODEL_TITLE_ARCTIC);
    check(!strcmp(model_value("analog", "provider"), "xinput"),
          "Arctic's tab reads its own choice");
    model_controls_show(MODEL_TITLE_OFFROAD);
    check(!strcmp(model_value("analog", "provider"), "dinput"),
          "and Offroad falls through to [analog]");
    model_controls_show(MODEL_TITLE_HYDRO);
    model_close();
}

/* The four review findings on the per-cabinet controls, one case each. */
static void test_per_cabinet_review(void)
{
    char path[MAX_PATH], why[512], long_key[128], text[512];
    int changed = -1, page = -1;

    /* An emptied choice falls through to the default, in the game and here. */
    model_controls_show(0);
    check(open_model("empty-choice.ini",
            "[analog]\r\n"
            "provider = dinput\r\n"
            "dinput_name = TMX\r\n",
            path, sizeof path), "the scratch ini opened");
    check(model_controls_split_on(), "split succeeds");
    check(!doc_has("analog", "dinput_name") &&
          doc_says("hydro.analog", "dinput_name", "TMX") &&
          doc_says("offroad.analog", "dinput_name", "TMX"),
          "the shared wheel name moved into both cabinets");
    slot_set("dinput_name", "");
    check(validate_settings(why, sizeof why, &page),
          "an empty name with nothing shared under it is legal");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "Apply succeeds");
    check(!doc_has("hydro.analog", "dinput_name"),
          "AN EMPTIED CHOICE IS TAKEN OUT, NOT WRITTEN EMPTY: the host reads "
          "`dinput_name =` as absent and falls through");
    check(doc_says("offroad.analog", "dinput_name", "TMX"),
          "and the other cabinet keeps its own");
    check(model_reopen(), "the file reads back");
    model_load();
    check(!strcmp(model_value("analog", "dinput_name"), ""),
          "Hydro shows the default the game will use");
    model_close();

    /* A hand-split file still sharing a name cannot express "none". */
    model_controls_show(0);
    check(open_model("empty-over-shared.ini",
            "[analog]\r\n"
            "dinput_name = TMX\r\n"
            "[hydro.analog]\r\n"
            "provider = dinput\r\n",
            path, sizeof path), "the scratch ini opened");
    check(!strcmp(model_value("analog", "dinput_name"), "TMX"),
          "Hydro inherits the shared name");
    slot_set("dinput_name", "");
    check(!validate_settings(why, sizeof why, &page) &&
          page == PAGE_CONTROLS && strstr(why, "Hydro Thunder") != NULL,
          "CLEARING IT WOULD CHANGE NOTHING THE GAME READS, so Apply refuses "
          "and says which game");
    model_close();

    /* The cabinet not shown is validated from the document. */
    model_controls_show(0);
    check(open_model("hidden-invalid.ini",
            "[hydro.analog]\r\n"
            "provider = xinput\r\n"
            "[offroad.analog]\r\n"
            "provider = xinput\r\n",
            path, sizeof path), "the scratch ini opened");
    slot_set("xinput_device", "9");
    check(model_controls_show(1), "switch to Offroad");
    check(!validate_settings(why, sizeof why, &page) &&
          strstr(why, "Hydro Thunder") != NULL,
          "A VALUE LEFT BEHIND ON THE OTHER TAB IS STILL VALIDATED, and the "
          "message names that game");
    model_close();

    /* Join ends on a key it cannot classify the way it removes it. */
    memset(long_key, 'a', 84);
    snprintf(long_key + 84, sizeof long_key - 84, "_brake_axiszz");
    snprintf(text, sizeof text,
             "[hydro.analog]\r\nprovider = xinput\r\n%s = x\r\n",
             long_key);
    model_controls_show(0);
    check(open_model("join-long-key.ini", text, path, sizeof path),
          "the scratch ini opened");
    check(model_controls_join(), "JOIN ENDS, whatever a key's length");
    check(!doc_has("hydro.analog", "provider"), "and did its job");
    model_close();
}

static void test_binding_save(void)
{
    char path[MAX_PATH], why[512], *after;
    int changed = -1;

    check(open_model("bind.ini", "[graphics]\r\nrender_scale = 1\r\n",
                     path, sizeof path), "the scratch ini opened");
    check(!strcmp(model_key_binding(0, VCT_ACTION_boost), "LCTRL"),
          "an absent binding comes from the table's per-title default");
    check(!strcmp(model_key_binding(1, VCT_ACTION_boost), "SPACE"),
          "and the two titles have different ones");

    snprintf(model_key_binding(1, VCT_ACTION_boost), MODEL_BINDING_CAP, "%s",
             "SPACE,B");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "saving a changed binding succeeds");
    check(changed == 1, "one binding, one write");
    after = read_bytes(path);
    check(after && strstr(after, "[offroad.digital]") != NULL,
          "the per-title section is created");
    check(after && strstr(after, "SPACE,B") != NULL,
          "the keyboard half is a list and is written as one");
    check(after && !strstr(after, "[hydro.digital]"),
          "the title that did not change gets no section");
    free(after);
    model_close();
}

/* A BINDING ROW AS TWO SLOTS: its inputs in order, keys first; a row rebuilt
 * from them; and the restore behind Reset to defaults, which puts back what
 * the row inherits and lets the save take the title's line out again. */
static void test_binding_slots(void)
{
    char path[MAX_PATH], why[512], *after;
    binding_item_t row[8];
    int changed = -1;

    check(open_model("slots.ini",
                     "[digital]\r\nview_high = B\r\n"
                     "[hydro.digital]\r\nboost = SPACE\r\n",
                     path, sizeof path), "the scratch ini opened");

    check(model_binding_items(0, VCT_ACTION_view_high, row, 8) == 2,
          "a key and a button are two inputs");
    check(!row[0].pad && !strcmp(row[0].value, "B") && row[1].pad &&
          !strcmp(row[1].value, "2"), "keys first, then buttons");
    check(model_binding_is_default(0, VCT_ACTION_view_high),
          "a row the title section does not mention is on its default");
    model_binding_default(0, VCT_ACTION_view_high, 0, why, sizeof why);
    check(!strcmp(why, "B"), "the shared section beats the table");
    check(model_binding_items(0, VCT_ACTION_coin2, row, 8) == 1,
          "a button 0 is not an input");

    check(model_binding_default_items(0, VCT_ACTION_boost, row, 8) == 2 &&
          !row[0].pad && !strcmp(row[0].value, "LCTRL") && row[1].pad &&
          !strcmp(row[1].value, "1"),
          "Backspace's default for a row its title section changed is the "
          "table's, slot by slot");

    check(!model_binding_is_default(0, VCT_ACTION_boost),
          "a title line that differs is not the default");
    row[0].pad = 0;
    snprintf(row[0].value, sizeof row[0].value, "LCTRL");
    row[1] = row[0];
    snprintf(row[1].value, sizeof row[1].value, "SPACE");
    check(model_binding_set_items(0, VCT_ACTION_boost, row, 2),
          "two keys fit in two slots");
    check(!strcmp(model_key_binding(0, VCT_ACTION_boost), "LCTRL, SPACE"),
          "and are written as one keyboard list");
    check(!strcmp(model_pad_binding(0, VCT_ACTION_boost), "0"),
          "and the controller half with nothing in it is 0");

    row[0].pad = 1;
    snprintf(row[0].value, sizeof row[0].value, "th8:3");
    check(!model_binding_set_items(0, VCT_ACTION_steer_left, row, 1),
          "a button on a keys-only control is refused");
    check(!strcmp(model_key_binding(0, VCT_ACTION_steer_left), "LEFT"),
          "and the refusal changed nothing");

    model_binding_restore(0, VCT_ACTION_boost);
    check(!strcmp(model_key_binding(0, VCT_ACTION_boost), "LCTRL") &&
          !strcmp(model_pad_binding(0, VCT_ACTION_boost), "1"),
          "Default puts back the table's two halves");
    check(model_binding_is_default(0, VCT_ACTION_boost),
          "and the row then reads as default");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK,
          "saving the restored row succeeds");
    after = read_bytes(path);
    check(after && !strstr(after, "\nboost = SPACE"),
          "the title's line is out: the file says nothing about the row");
    check(after && strstr(after, "view_high = B") != NULL,
          "and the shared line is left alone");
    free(after);
    model_close();
}

/* ONE ANSWER TO "IS THIS THE DEFAULT": a shared line spelled without spaces
 * and the same list rebuilt by the slots with them are one value, to the
 * Reset button and to Apply alike. */
static void test_binding_default_spacing(void)
{
    char path[MAX_PATH], why[512], *after;
    int changed = -1;

    check(open_model("spacing.ini", "[digital]\r\nboost = LCTRL,SPACE\r\n",
                     path, sizeof path), "the scratch ini opened");
    snprintf(model_key_binding(0, VCT_ACTION_boost), MODEL_BINDING_CAP, "%s",
             "LCTRL, SPACE");
    check(model_binding_is_default(0, VCT_ACTION_boost),
          "the respaced list is the inherited one");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
          changed == 0, "and Apply has nothing to write for it");
    after = read_bytes(path);
    check(after && !strstr(after, "[hydro.digital]"),
          "no title line pins what the shared line already says");
    free(after);
    model_close();
}

/* ---- restoring the defaults ---------------------------------------------- */

/* RESTORE DEFAULTS WRITES THE SHIPPED INI WHOLE, and keeps the file it
 * replaces under a name Apply never writes: it is the only way back. */
static void test_defaults_restore(void)
{
    static const char shipped[] = "[graphics]\r\nwindow_scale = auto\r\n";
    char path[MAX_PATH], other[MAX_PATH], why[512], *text;

    check(open_model("defaults.ini", "[graphics]\r\nrender_scale = 3\r\n",
                     path, sizeof path), "the scratch ini opened");

    check(model_defaults_restore(shipped, sizeof shipped - 1, why, sizeof why),
          "restoring the defaults succeeds");
    text = read_bytes(path);
    check(text && !strcmp(text, shipped),
          "the ini is exactly the shipped bytes");
    free(text);
    check(model_reopen(), "and the restored file reads back");
    model_load();
    check(!strcmp(model_value("graphics", "render_scale"), "1"),
          "a key the shipped ini lacks is its default again");

    snprintf(other, sizeof other, "%s" MODEL_RESTORE_SUFFIX, path);
    text = read_bytes(other);
    check(text && strstr(text, "render_scale = 3") != NULL,
          "the file as it was is kept beside it");
    free(text);
    /* AND AN APPLY AFTERWARDS DOES NOT TOUCH IT: Apply's own backup is .bak,
     * and it would otherwise hold the defaults after the first one. */
    snprintf(model_slot((unsigned)setting_find("graphics", "render_scale")),
             INI_VALUE_CAP, "%s", "2");
    {
        int changed = 0;
        check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
              changed == 1, "an Apply after the restore writes");
    }
    text = read_bytes(other);
    check(text && strstr(text, "render_scale = 3") != NULL,
          "and the user's own file is still there after it");
    free(text);
    snprintf(other, sizeof other, "%s.new", path);
    check(GetFileAttributesA(other) == INVALID_FILE_ATTRIBUTES,
          "no temporary file is left behind");
    model_close();
}

/* THE VALIDATORS ARE THE POINT. They are the gate a binding a user TYPES
 * passes through, and a form that refuses what the host accepts cannot edit a
 * file somebody hand-wrote: a Pro DD's button 40 is visible in the live preview
 * and must not be rejected by the form beside it. */
/* ---- the Bezel box -------------------------------------------------------- */

static int count_of(const char *text, const char *needle)
{
    int n = 0;
    const char *at = text;
    while (at && (at = strstr(at, needle)) != NULL) {
        n++;
        at += strlen(needle);
    }
    return n;
}

static void test_bezel_rows(void)
{
    char path[MAX_PATH], why[256], art[MAX_PATH], dest[MAX_PATH], dir[MAX_PATH];
    char value[INI_VALUE_CAP], shown[MAX_PATH], *text;
    int changed = 0;

    check(open_model("bezel.ini",
            "[graphics]\r\n"
            "widescreen = true\r\n"
            "[hydro.graphics]\r\n"
            "bezel = bezels/hydro.png\r\n",
            path, sizeof path), "the bezel ini opened");
    check(model_bezel_get(0, shown, sizeof shown) &&
          !strcmp(shown, "bezels/hydro.png"),
          "bezel: a cabinet's line is its row, on, naming its file");
    check(!model_bezel_get(1, shown, sizeof shown) && !shown[0],
          "bezel: a cabinet with no line and no file is off with no art");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK && !changed,
          "bezel: an untouched box writes nothing");

    snprintf(art, sizeof art, "%s\\art.png", s_folder);
    check(write_bytes(art, "the chosen art"), "bezel: scratch art written");
    model_bezel_choose(1, art);
    check(model_bezel_get(1, shown, sizeof shown) && !strcmp(shown, art),
          "bezel: a choice is shown, and on, before Apply");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK && changed == 1 &&
          model_bezel_copied() == 1, "bezel: a choice is one key changed and one copy");
    model_bezel_destination(1, dest, sizeof dest);
    text = read_bytes(dest);
    check(text && !strcmp(text, "the chosen art"),
          "bezel: Apply copied the choice to bezels\\offroad.png");
    free(text);
    check(ini_doc_get(model_doc(), "offroad.graphics", "bezel", value,
                      sizeof value) && !strcmp(value, "bezels/offroad.png"),
          "bezel: the line names the copy, relative to the pack");

    /* A new file under the line that already names its copy: no key, one copy. */
    check(write_bytes(art, "newer art"), "bezel: scratch art rewritten");
    model_bezel_choose(1, art);
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK && !changed &&
          model_bezel_copied() == 1,
          "bezel: replacing the art under an unchanged line is a copy, not a key");
    text = read_bytes(dest);
    check(text && !strcmp(text, "newer art"), "bezel: and the copy is the new art");
    free(text);

    model_bezel_set_enabled(0, 0);
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK && changed == 1 &&
          ini_doc_get(model_doc(), "hydro.graphics", "bezel", value,
                      sizeof value) && !_stricmp(value, "off"),
          "bezel: turning a cabinet off writes off in its own section");
    text = read_bytes(path);
    check(text && count_of(text, "[hydro.graphics]") == 1 &&
          count_of(text, "[offroad.graphics]") == 1,
          "bezel: each cabinet's line goes in its one section");
    free(text);

    check(model_reopen(), "bezel: the saved file reopened");
    model_load();
    check(model_bezel_get(1, shown, sizeof shown) &&
          !strcmp(shown, "bezels/offroad.png"), "bezel: the choice survives a reload");
    model_bezel_set_enabled(1, 0);
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK && changed == 1,
          "bezel: turning the other off saves");
    check(model_reopen(), "bezel: reopened again");
    model_load();
    check(!model_bezel_get(1, shown, sizeof shown) &&
          !strcmp(shown, "bezels/offroad.png"),
          "bezel: art left off is still named, so it can be turned back on");
    model_close();

    DeleteFileA(dest);
    snprintf(dir, sizeof dir, "%s", dest);
    *strrchr(dir, '\\') = '\0';
    RemoveDirectoryA(dir);
    DeleteFileA(art);
}

int main(void)
{
    char root[MAX_PATH];

    GetTempPathA(sizeof root, root);
    snprintf(s_folder, sizeof s_folder, "%.*sVCThunder-model-%lu",
             (int)(sizeof s_folder - sizeof "VCThunder-model-" - 10),
             root, GetCurrentProcessId());
    CreateDirectoryA(s_folder, NULL);

    test_key_names();
    test_pad_bindings();
    test_validation_names_a_page();
    test_unsafe_video_mode();
    test_ini_problems();
    test_bezel_rows();
    test_duplicate_keys();
    test_device_scope();
    test_device_labels();
    test_configured_device();
    test_save_writes_only_what_changed();
    test_per_cabinet_controls();
    test_per_cabinet_ffb();
    test_per_cabinet_ffb_revert();
    test_per_cabinet_split_on();
    test_per_cabinet_review();
    test_binding_save();
    test_binding_slots();
    test_binding_default_spacing();
    test_defaults_restore();
    test_arctic_title();

    if (failures) {
        fprintf(stderr, "launcher model tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("launcher model tests: all passed");
    return 0;
}
