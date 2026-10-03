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

/* ---- the known-good copy -------------------------------------------------- */

static void test_known_good_round_trip(void)
{
    char path[MAX_PATH], good[MAX_PATH], why[512], *text;
    int changed = -1;

    check(open_model("good.ini", "[graphics]\r\nrender_scale = 2\r\n",
                     path, sizeof path), "the scratch ini opened");
    known_good_path(good, sizeof good);
    check(strstr(good, ".good") != NULL, "the copy is named beside the ini");
    DeleteFileA(good);
    check(!model_known_good_exists(), "there is no copy yet");

    seed_known_good();
    check(model_known_good_exists(),
          "the file as this window found it is seeded once");

    snprintf(model_slot((unsigned)setting_find("graphics", "render_scale")),
             INI_VALUE_CAP, "%s", "8");
    check(model_save(why, sizeof why, &changed) == MODEL_SAVE_OK &&
          changed == 1, "the settings are changed and saved");

    check(model_known_good_restore(why, sizeof why),
          "reverting puts the copy back");
    check(model_reopen(), "and the restored file reads back");
    model_load();
    check(!strcmp(model_value("graphics", "render_scale"), "2"),
          "the value from the known-good copy is what the model now holds");

    snprintf(good, sizeof good, "%s.bak", path);
    text = read_bytes(good);
    check(text && strstr(text, "= 8") != NULL,
          "AND THE FILE AS IT WAS IS KEPT AS .bak: a revert a user did not "
          "mean has to be recoverable too");
    free(text);
    model_close();
}

/* THE VALIDATORS ARE THE POINT. They are the gate a binding a user TYPES
 * passes through, and a form that refuses what the host accepts cannot edit a
 * file somebody hand-wrote: a Pro DD's button 40 is visible in the live preview
 * and must not be rejected by the form beside it. */
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
    test_duplicate_keys();
    test_device_scope();
    test_device_labels();
    test_configured_device();
    test_save_writes_only_what_changed();
    test_binding_save();
    test_known_good_round_trip();

    if (failures) {
        fprintf(stderr, "launcher model tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("launcher model tests: all passed");
    return 0;
}
