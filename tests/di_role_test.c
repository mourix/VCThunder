/* di_role_test.c -- steering, gas and brake from different controllers.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * src/di_role.c driven with a fake slot table and reading, no controller
 * attached: which slot each role reads, where its facts come from, the one
 * refusal, and what a role reads when its controller does not answer.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/di_role.c"
#include "../src/ini.c"
#include "../src/xinput_hid.c"

static const game_profile_t k_hydro = { .id = "hydro" };
const game_profile_t *g_game = &k_hydro;

static int failures, checks;
static int warnings;
static char last_warning[1024];

void log_printf(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    if (level != LOG_WARN)
        return;
    warnings++;
    va_start(ap, fmt);
    vsnprintf(last_warning, sizeof last_warning, fmt, ap);
    va_end(ap);
}

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static int about(float a, float b)
{
    return a - b < 0.0005f && b - a < 0.0005f;
}

/* ---- the slots, as dinput.c would have opened them ---------------------- */

/* Slot 0 is the default device. A label names the slot it is open in; one
 * label may be a second name for a slot, as dinput.c's alias table allows. */
static const struct { const char *label; int slot; } k_open[] = {
    { "fanatec", 0 }, { "twcs", 1 }, { "dash", 2 }, { "wheel2", 0 },
    { "pad", 3 },
};
static const char *s_scope[4] = { "fanatec", "twcs", "dash", "pad" };

static int fake_slot_of(const char *label)
{
    unsigned i;

    for (i = 0; i < sizeof k_open / sizeof k_open[0]; i++)
        if (!_stricmp(k_open[i].label, label))
            return k_open[i].slot;
    return -1;
}

static const char *fake_scope_of(unsigned slot)
{
    return slot < 4 ? s_scope[slot] : "";
}

/* Slot 3 is an XInput pad held beside the wheel (`pad = xinput:0`). */
static int fake_is_xinput(unsigned slot)
{
    return slot == 3;
}

static char s_path[MAX_PATH];

static void write_ini(const char *text)
{
    FILE *f = fopen(s_path, "wb");

    if (!f) {
        fprintf(stderr, "cannot write %s\n", s_path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static void load(di_roles_t *r, const char *text)
{
    write_ini(text);
    warnings = 0;
    last_warning[0] = '\0';
    di_roles_load(r, s_path, fake_slot_of, fake_scope_of, fake_is_xinput);
}

/* One reading per slot, every axis centred, all answering. */
static void readings(di_slot_reading_t *slot, unsigned n)
{
    unsigned i, a;

    memset(slot, 0, sizeof *slot * n);
    for (i = 0; i < n; i++) {
        slot[i].answered = 1;
        for (a = 0; a < DI_AXIS_COUNT; a++)
            slot[i].unit[a] = 0.0f;
    }
}

/* ---- the grammar -------------------------------------------------------- */

static void test_axis_ref(void)
{
    char label[INI_LABEL_CAP], axis[16];

    check(ini_parse_axis_ref("rz", label, sizeof label, axis, sizeof axis) &&
          !label[0] && !strcmp(axis, "rz"), "a bare axis has no label");
    check(ini_parse_axis_ref(" twcs : z ", label, sizeof label, axis,
                             sizeof axis) &&
          !strcmp(label, "twcs") && !strcmp(axis, "z"),
          "label:axis splits, blanks trimmed");
    check(!ini_parse_axis_ref("twcs:", label, sizeof label, axis, sizeof axis),
          "a label with no axis is refused");
    check(!ini_parse_axis_ref("bad label:z", label, sizeof label, axis,
                              sizeof axis), "a label with a blank is refused");
    check(!ini_parse_axis_ref(":z", label, sizeof label, axis, sizeof axis),
          "an empty label is refused");
    check(!ini_parse_axis_ref("", label, sizeof label, axis, sizeof axis),
          "an empty value is refused");
}

/* ---- a file without labels reads as it always did ----------------------- */

static void test_unlabelled(void)
{
    di_roles_t r;
    di_slot_reading_t slot[1];
    float steer, drive;

    load(&r, "[analog]\r\n");
    check(r.role[DI_ROLE_STEER].axis == DI_AXIS_X &&
          r.role[DI_ROLE_THROTTLE].axis == DI_AXIS_Y &&
          r.role[DI_ROLE_BRAKE].axis == DI_AXIS_RZ,
          "the defaults are x, y, rz");
    check(r.role[0].slot == 0 && r.role[1].slot == 0 && r.role[2].slot == 0,
          "no label is the default device");
    check(!r.role[0].invert && r.role[1].invert && r.role[2].invert,
          "pedals default to inverted, steering does not");
    check(r.pedals_separate && di_roles_separate(&r), "separate by default");
    check(!warnings, "a default file warns about nothing");

    /* Pedals at rest (+1, inverted) and the wheel a quarter right. */
    readings(slot, 1);
    slot[0].unit[DI_AXIS_X] = 0.25f;
    slot[0].unit[DI_AXIS_Y] = 1.0f;
    slot[0].unit[DI_AXIS_RZ] = 1.0f;
    di_roles_sample(&r, slot, 1, &steer, &drive);
    check(about(steer, 0.25f) && about(drive, 0.0f), "resting pedals drive 0");
    slot[0].unit[DI_AXIS_Y] = -1.0f;            /* throttle floored */
    di_roles_sample(&r, slot, 1, &steer, &drive);
    check(about(drive, 1.0f), "a floored throttle is full drive");
    slot[0].unit[DI_AXIS_RZ] = -1.0f;           /* and the brake */
    di_roles_sample(&r, slot, 1, &steer, &drive);
    check(about(drive, 0.0f), "throttle minus brake");

    /* The steering range and deadzone, the old arithmetic unchanged. */
    load(&r, "[analog.fanatec]\r\ndinput_steering_range_percent = 50\r\n"
             "dinput_deadzone_percent = 10\r\n");
    readings(slot, 1);
    slot[0].unit[DI_AXIS_X] = 0.25f;
    slot[0].unit[DI_AXIS_Y] = 1.0f;
    slot[0].unit[DI_AXIS_RZ] = 1.0f;
    di_roles_sample(&r, slot, 1, &steer, &drive);
    check(about(steer, (0.5f - 0.1f) / 0.9f),
          "range expands before the deadzone");

    /* Combined: one axis, -1 full brake .. +1 full throttle once inverted. */
    load(&r, "[analog]\r\ndinput_pedal_mode = combined\r\n"
             "dinput_brake_axis = none\r\n");
    check(!di_roles_separate(&r), "combined");
    readings(slot, 1);
    slot[0].unit[DI_AXIS_Y] = -0.5f;
    di_roles_sample(&r, slot, 1, &steer, &drive);
    check(about(drive, 0.5f), "combined reads the throttle axis, inverted");

    /* Separate with no brake axis is one pedal axis, read combined. */
    load(&r, "[analog]\r\ndinput_brake_axis = none\r\n");
    check(r.pedals_separate && !di_roles_separate(&r),
          "a missing brake axis reads as combined");
}

/* ---- a role on another controller --------------------------------------- */

static void test_labelled(void)
{
    di_roles_t r;
    di_slot_reading_t slot[3];
    float steer, drive;

    load(&r,
         "[devices]\r\nfanatec = dinput:FANATEC\r\ntwcs = dinput:TWCS\r\n"
         "[analog.fanatec]\r\n"
         "dinput_steering_axis = x\r\n"
         "dinput_throttle_axis = twcs:z\r\n"
         "dinput_brake_axis = rz\r\n"
         "[analog.twcs]\r\n"
         "dinput_throttle_invert = false\r\n"
         /* A role key in another device's profile is not read: the default
          * device says which axis plays which role. */
         "dinput_throttle_axis = slider1\r\n");
    check(r.role[DI_ROLE_THROTTLE].slot == 1 &&
          r.role[DI_ROLE_THROTTLE].axis == DI_AXIS_Z &&
          !strcmp(r.role[DI_ROLE_THROTTLE].label, "twcs"),
          "twcs:z is axis z of slot 1");
    check(r.role[DI_ROLE_STEER].slot == 0 && r.role[DI_ROLE_BRAKE].slot == 0,
          "the unlabelled roles stay on the default device");
    check(!r.role[DI_ROLE_THROTTLE].invert,
          "the throttle's invert comes from [analog.twcs]");
    check(r.role[DI_ROLE_BRAKE].invert,
          "the brake's invert is still the default device's");
    check(!warnings, "a labelled file warns about nothing");

    /* The lever floored on the TWCS (not inverted: +1 is full), the wheel's
     * own z resting where it would read as throttle if it were used. */
    readings(slot, 3);
    slot[0].unit[DI_AXIS_X] = -0.5f;
    slot[0].unit[DI_AXIS_Z] = -1.0f;
    slot[0].unit[DI_AXIS_RZ] = 1.0f;
    slot[1].unit[DI_AXIS_Z] = 1.0f;
    di_roles_sample(&r, slot, 3, &steer, &drive);
    check(about(steer, -0.5f) && about(drive, 1.0f),
          "steering from slot 0, gas from slot 1");
    slot[1].unit[DI_AXIS_Z] = -1.0f;
    di_roles_sample(&r, slot, 3, &steer, &drive);
    check(about(drive, 0.0f), "the lever back is no gas, whatever slot 0's z");

    /* A second name for the default device is the default device. */
    load(&r, "[analog.fanatec]\r\ndinput_steering_axis = wheel2:x\r\n");
    check(r.role[DI_ROLE_STEER].slot == 0, "an alias of slot 0 is slot 0");
}

/* ---- per-role scope order ----------------------------------------------- */

static void test_scope_order(void)
{
    di_roles_t r;
    static const char *const k_base =
        "[analog.fanatec]\r\ndinput_throttle_axis = twcs:z\r\n";
    char text[1024];

    /* [hydro.analog.twcs] beats [analog.twcs] beats [hydro.analog] beats
     * [analog] beats the default; one rung removed at a time. */
    snprintf(text, sizeof text, "%s"
             "[analog]\r\ndinput_deadzone_percent = 1\r\n"
             "[hydro.analog]\r\ndinput_deadzone_percent = 2\r\n"
             "[analog.twcs]\r\ndinput_deadzone_percent = 3\r\n"
             "[hydro.analog.twcs]\r\ndinput_deadzone_percent = 4\r\n", k_base);
    load(&r, text);
    check(r.role[DI_ROLE_THROTTLE].deadzone_percent == 4,
          "[hydro.analog.twcs] first");
    check(r.role[DI_ROLE_STEER].deadzone_percent == 2,
          "the steering, on the default device, never sees twcs's");

    snprintf(text, sizeof text, "%s"
             "[analog]\r\ndinput_deadzone_percent = 1\r\n"
             "[hydro.analog]\r\ndinput_deadzone_percent = 2\r\n"
             "[analog.twcs]\r\ndinput_deadzone_percent = 3\r\n", k_base);
    load(&r, text);
    check(r.role[DI_ROLE_THROTTLE].deadzone_percent == 3,
          "[analog.twcs] before [hydro.analog]");

    snprintf(text, sizeof text, "%s"
             "[analog]\r\ndinput_deadzone_percent = 1\r\n"
             "[hydro.analog]\r\ndinput_deadzone_percent = 2\r\n", k_base);
    load(&r, text);
    check(r.role[DI_ROLE_THROTTLE].deadzone_percent == 2,
          "[hydro.analog] before [analog]");

    snprintf(text, sizeof text, "%s"
             "[analog]\r\ndinput_deadzone_percent = 1\r\n", k_base);
    load(&r, text);
    check(r.role[DI_ROLE_THROTTLE].deadzone_percent == 1, "[analog] last");

    load(&r, k_base);
    check(r.role[DI_ROLE_THROTTLE].deadzone_percent == 0, "then the default");

    /* The steering range is the steering role's device's fact. */
    load(&r, "[analog.fanatec]\r\ndinput_steering_axis = twcs:x\r\n"
             "dinput_steering_range_percent = 30\r\n"
             "[analog.twcs]\r\ndinput_steering_range_percent = 60\r\n");
    check(r.role[DI_ROLE_STEER].range_percent == 60,
          "the steering range follows the steering's controller");
}

/* ---- released on loss --------------------------------------------------- */

static void test_released_on_loss(void)
{
    di_roles_t r;
    di_slot_reading_t slot[3];
    float steer, drive;

    load(&r, "[analog.fanatec]\r\n"
             "dinput_steering_axis = twcs:x\r\n"
             "dinput_throttle_axis = twcs:z\r\n"
             "dinput_brake_axis = rz\r\n"
             "[analog.twcs]\r\ndinput_throttle_invert = true\r\n");
    /* A failed read's raw 0 on an inverted separate pedal would be half
     * throttle; released is 0. */
    readings(slot, 3);
    slot[0].unit[DI_AXIS_RZ] = 1.0f;          /* brake at rest */
    slot[1].answered = 0;
    slot[1].unit[DI_AXIS_X] = 0.7f;           /* stale, must not be read */
    di_roles_sample(&r, slot, 3, &steer, &drive);
    check(about(steer, 0.0f), "a lost steering controller reads centred");
    check(about(drive, 0.0f), "a lost throttle reads released, not half");

    slot[0].unit[DI_AXIS_RZ] = -1.0f;         /* the brake still works */
    di_roles_sample(&r, slot, 3, &steer, &drive);
    check(about(drive, -1.0f), "the brake on the live controller still brakes");

    /* A slot past the count is a slot that is not open. */
    slot[1].answered = 1;
    di_roles_sample(&r, slot, 1, &steer, &drive);
    check(about(drive, -1.0f) && about(steer, 0.0f),
          "a slot beyond the open count reads released");

    /* A label that opens nothing reads released, never as the default
     * device's axis of that name. */
    load(&r, "[analog.fanatec]\r\ndinput_throttle_axis = gone:y\r\n");
    check(r.role[DI_ROLE_THROTTLE].slot == -1, "an unopened label is slot -1");
    readings(slot, 1);
    slot[0].unit[DI_AXIS_Y] = -1.0f;          /* slot 0's y floored */
    slot[0].unit[DI_AXIS_RZ] = 1.0f;
    di_roles_sample(&r, slot, 1, &steer, &drive);
    check(about(drive, 0.0f), "an unopened throttle does not read slot 0's y");

    /* Combined, the lost axis is no drive at all. */
    load(&r, "[analog.fanatec]\r\ndinput_pedal_mode = combined\r\n"
             "dinput_throttle_axis = twcs:y\r\ndinput_brake_axis = none\r\n");
    readings(slot, 2);
    slot[1].answered = 0;
    slot[1].unit[DI_AXIS_Y] = -1.0f;
    di_roles_sample(&r, slot, 2, &steer, &drive);
    check(about(drive, 0.0f), "a lost combined pedal axis is no drive");
}

/* ---- the refusal -------------------------------------------------------- */

static void test_combined_refused(void)
{
    di_roles_t r;

    load(&r, "[analog.fanatec]\r\ndinput_pedal_mode = combined\r\n"
             "dinput_throttle_axis = y\r\ndinput_brake_axis = twcs:z\r\n");
    check(r.pedals_separate && di_roles_separate(&r),
          "combined pedals across two controllers read as separate");
    check(warnings == 1 && strstr(last_warning, "dinput_pedal_mode = combined"),
          "and say so");

    load(&r, "[analog.fanatec]\r\ndinput_pedal_mode = combined\r\n"
             "dinput_throttle_axis = twcs:y\r\ndinput_brake_axis = twcs:z\r\n");
    check(!r.pedals_separate && !warnings,
          "combined on one other controller is not refused");

    load(&r, "[analog.fanatec]\r\ndinput_pedal_mode = combined\r\n"
             "dinput_throttle_axis = y\r\ndinput_brake_axis = wheel2:rz\r\n");
    check(!r.pedals_separate && !warnings,
          "an alias of the default device is the same controller");

    load(&r, "[analog.fanatec]\r\ndinput_pedal_mode = combined\r\n"
             "dinput_throttle_axis = y\r\ndinput_brake_axis = none\r\n");
    check(!r.pedals_separate && !warnings, "combined with no brake stands");
}

/* ---- an XInput pad beside the wheel -------------------------------------- */

static void test_xinput_slot(void)
{
    di_roles_t r;
    di_slot_reading_t slot[4];
    float steer, drive;

    /* XInput's names on an XInput slot: `rt` is the right trigger, `rx` the
     * right stick. */
    load(&r, "[analog.fanatec]\r\n"
             "dinput_throttle_axis = pad:rt\r\n"
             "dinput_brake_axis = pad:lt\r\n"
             "[analog.pad]\r\n"
             "dinput_throttle_invert = false\r\n"
             "dinput_brake_invert = false\r\n");
    check(r.role[DI_ROLE_THROTTLE].slot == 3 &&
          r.role[DI_ROLE_THROTTLE].axis == DI_AXIS_RT &&
          r.role[DI_ROLE_BRAKE].axis == DI_AXIS_LT && !warnings,
          "pad:rt and pad:lt are the pad's triggers");
    load(&r, "[analog.fanatec]\r\ndinput_steering_axis = pad:rx\r\n");
    check(r.role[DI_ROLE_STEER].axis == DI_AXIS_XRX,
          "rx on an XInput pad is its right stick");
    load(&r, "[analog.fanatec]\r\ndinput_steering_axis = twcs:rx\r\n");
    check(r.role[DI_ROLE_STEER].axis == DI_AXIS_RX,
          "rx on a DirectInput controller is its rotation");
    load(&r, "[analog.fanatec]\r\ndinput_throttle_axis = pad:z\r\n");
    check(r.role[DI_ROLE_THROTTLE].axis == DI_AXIS_NONE && warnings == 1,
          "a DirectInput name on an XInput pad is not an axis, out loud");
    load(&r, "[analog.fanatec]\r\ndinput_throttle_axis = rt\r\n");
    check(r.role[DI_ROLE_THROTTLE].axis == DI_AXIS_NONE && warnings == 1,
          "an XInput name on the DirectInput wheel is not an axis");
    load(&r, "[analog.fanatec]\r\ndinput_throttle_axis = gone:rt\r\n");
    check(r.role[DI_ROLE_THROTTLE].axis == DI_AXIS_RT &&
          r.role[DI_ROLE_THROTTLE].slot == -1 && !warnings,
          "a label that opens nothing parses in either provider's names");

    /* A trigger rests at -1 and a press is +1; a pedal role reads
     * (v + 1) / 2. */
    load(&r, "[analog.fanatec]\r\n"
             "dinput_throttle_axis = pad:rt\r\n"
             "dinput_brake_axis = pad:lt\r\n"
             "[analog.pad]\r\n"
             "dinput_throttle_invert = false\r\n"
             "dinput_brake_invert = false\r\n");
    readings(slot, 4);
    slot[3].unit[DI_AXIS_RT] = -1.0f;
    slot[3].unit[DI_AXIS_LT] = -1.0f;
    di_roles_sample(&r, slot, 4, &steer, &drive);
    check(about(drive, 0.0f), "both triggers released, no drive");
    slot[3].unit[DI_AXIS_RT] = 1.0f;
    di_roles_sample(&r, slot, 4, &steer, &drive);
    check(about(drive, 1.0f), "the right trigger floored is full gas");
    slot[3].answered = 0;
    di_roles_sample(&r, slot, 4, &steer, &drive);
    check(about(drive, 0.0f), "a pad that stops answering reads released");

    /* A trigger with no invert key is not inverted, and a lone trigger is one
     * pedal: released is no drive either way. */
    load(&r, "[analog.fanatec]\r\n"
             "dinput_throttle_axis = pad:rt\r\n"
             "dinput_brake_axis = pad:lt\r\n");
    check(!r.role[DI_ROLE_THROTTLE].invert && !r.role[DI_ROLE_BRAKE].invert,
          "a trigger is not inverted by default");
    load(&r, "[analog.fanatec]\r\n"
             "dinput_throttle_axis = pad:rt\r\n"
             "dinput_brake_axis = none\r\n"
             "dinput_pedal_mode = combined\r\n");
    readings(slot, 4);
    slot[3].unit[DI_AXIS_RT] = -1.0f;
    di_roles_sample(&r, slot, 4, &steer, &drive);
    check(di_roles_separate(&r) && about(drive, 0.0f),
          "a lone trigger released is no drive, never full brake");
    slot[3].unit[DI_AXIS_RT] = 1.0f;
    di_roles_sample(&r, slot, 4, &steer, &drive);
    check(about(drive, 1.0f), "and floored it is full gas");
}

/* ---- which DirectInput entry is an XInput pad ---------------------------- */

static void test_xinput_hid(void)
{
    /* The Bluetooth pad: VID 045E, PID 0B13, product GUID 0b13045e-...,
     * RawInput path holding IG_00. */
    static const xinput_hid_t hid[] = {
        { 0x046D, 0xC548, 0 },          /* a receiver, not a pad          */
        { 0x045E, 0x0B13, 1 },          /* the pad's XInput collection    */
        { 0x044F, 0xB67F, 0 },          /* the TMX: no IG_                */
    };

    check(xinput_hid_is_xinput(hid, 3, 0x0B13045Eul),
          "the pad's product GUID matches its IG_ collection");
    check(!xinput_hid_is_xinput(hid, 3, 0xB67F044Ful),
          "the TMX has no IG_ collection");
    check(!xinput_hid_is_xinput(hid, 3, 0x045E0B13ul),
          "VID and PID the other way round is another device");
    check(xinput_hid_hides(hid, 3, 0x0B13045Eul, 0),
          "the pad's DirectInput view is set aside");
    check(!xinput_hid_hides(hid, 3, 0x0B13045Eul, 1),
          "an XInput device that publishes force feedback stays: its motor "
          "has no other path");
    check(holds_ig("\\\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_"
                   "VID&02045e_PID&0b13_REV&0520&IG_00") &&
          !holds_ig("\\\\?\\HID#VID_046D&PID_C548&MI_02&Col01"),
          "IG_ is found wherever it sits in the path");
}

/* ---- a value that is not an axis ---------------------------------------- */

static void test_bad_values(void)
{
    di_roles_t r;

    load(&r, "[analog]\r\ndinput_steering_axis = q\r\n");
    check(r.role[DI_ROLE_STEER].axis == DI_AXIS_NONE && warnings == 1 &&
          strstr(last_warning, "dinput_steering_axis = q"),
          "an unknown axis name is none, out loud");
    load(&r, "[analog]\r\ndinput_throttle_axis = bad label:z\r\n");
    check(r.role[DI_ROLE_THROTTLE].axis == DI_AXIS_NONE && warnings == 1,
          "a bad label is none, out loud");
    load(&r, "[analog]\r\ndinput_brake_axis = twcs:\r\n");
    check(r.role[DI_ROLE_BRAKE].axis == DI_AXIS_NONE && warnings == 1,
          "a label with no axis is none, out loud");
    load(&r, "[analog]\r\ndinput_brake_axis = slider\r\n");
    check(r.role[DI_ROLE_BRAKE].axis == DI_AXIS_SLIDER0 && !warnings,
          "`slider` is still slider0");
}

int main(void)
{
    char root[MAX_PATH], folder[MAX_PATH];

    GetTempPathA(sizeof root, root);
    snprintf(folder, sizeof folder, "%.*sVCThunder-roles-%lu",
             (int)(sizeof folder - sizeof "VCThunder-roles-" - 12), root,
             GetCurrentProcessId());
    CreateDirectoryA(folder, NULL);
    snprintf(s_path, sizeof s_path, "%s\\vcthunder.ini", folder);

    test_axis_ref();
    test_unlabelled();
    test_labelled();
    test_scope_order();
    test_released_on_loss();
    test_combined_refused();
    test_bad_values();
    test_xinput_slot();
    test_xinput_hid();

    DeleteFileA(s_path);
    RemoveDirectoryA(folder);
    if (failures) {
        fprintf(stderr, "di-role-test: %d of %d check(s) failed\n", failures,
                checks);
        return 1;
    }
    printf("di-role-test: %d checks, all passed\n", checks);
    return 0;
}
