/* launcher_capture.c -- the launcher's capture machine: slots, Detect, Set up.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See src/launcher_capture.h for why this is its own translation unit. THE
 * THREE FLOWS ARE ONE MACHINE: `s_capture_kind` is 3 for the axis guard and 4
 * for the binding capture, and Set up drives the guard rather than running
 * beside it, so exactly one is live at any moment and every way out of one is a
 * way out of all three.
 */
#include "launcher_capture.h"
#include "launcher_model.h"

#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* SET UP SPEAKS IN ONE PLACE, THE TWO LINES UNDER THE BARS. The hold keeps a
 * result on the first line against update_live_input()'s 30 Hz rewrite, and
 * across a rebuild. */
static char s_note_hold[256];
static unsigned s_note_hold_ms;
#define NOTE_HOLD_MS 12000u
#define NOTE_TICK_MS 33u
/* While a step runs: line 1 is its question, line 2 the guard's state. */
static char s_note_say[256];
static capture_say_t s_note_state;
static int s_note_count;            /* the brake step's countdown, last drawn */

static void hold_note(const char *text)
{
    snprintf(s_note_hold, sizeof s_note_hold, "%s", text ? text : "");
    s_note_hold_ms = NOTE_HOLD_MS;
    s_note_say[0] = '\0';
    s_note_state = SAY_NONE;
    launcher_note_lines(s_note_hold, "");
}

static int s_capture_kind, s_capture_action, s_capture_setting, s_capture_armed;
/* WHICH OF THE ROW'S SLOTS THE CAPTURE FILLS. One press, one slot: the other
 * slot is never touched, so a wrong press is undone by pressing that slot
 * again, and two inputs on one controller are two slots. */
static int s_capture_slot;

/* The capture's own clock. A mode with no way out is worse than a mode, and
 * a slot is the control a user presses by accident. */
#define CAPTURE_MS 12000u
static DWORD s_capture_deadline;
static int s_capture_shown;         /* the countdown second last drawn      */

/* Every axis the guard watches, and its controller. A role row listens on
 * every DirectInput controller and XInput pad, so an axis on another device
 * can be picked; an XInput or WinMM row listens on the ticked one alone. Fixed
 * when the capture starts; a device that stops answering, or comes back with
 * another axis there, holds its last value. */
#define AXIS_WATCH_MAX (INPUT_PROBE_MAX_DEVICES * INPUT_PROBE_MAX_AXES)
typedef struct {
    unsigned device;
    unsigned axis;
    char     name[16];
} axis_src_t;
static axis_src_t s_axis_src[AXIS_WATCH_MAX];
static float s_axis_now[AXIS_WATCH_MAX];
static float s_axis_baseline[AXIS_WATCH_MAX];
static unsigned s_axis_watch_count;

/* ---- Detect's guard (R3) -------------------------------------------------
 *
 * Four guards, each one something an axis census needs in order not to give a
 * wrong answer about real hardware:
 *
 *   1. SETTLE. A device still arriving gives a baseline that is not its rest.
 *   2. LARGEST, NOT FIRST PAST THE POST. Combined pedals move two axes at
 *      once and so does a nudged wheel.
 *   3. REQUIRE THE RETURN. An axis that steps once and stays is a device
 *      settling, indistinguishable from a deliberate move until it comes back.
 *   4. ASK BEFORE COMMITTING, and state what was measured.
 */
#define AXIS_MOVED           0.40f   /* leaves rest by this much: a move     */
#define AXIS_RETURNED        0.15f   /* comes back within this: it returned  */
#define AXIS_SETTLE_TOL      0.02f   /* a sample-to-sample step that is noise*/
#define AXIS_SETTLE_QUIET_MS  400u   /* how long "not changing" has to last  */
#define AXIS_SETTLE_MS       3000u   /* ...and how long to wait for that     */
#define AXIS_FOLLOW_QUIET_MS  700u   /* a slow press is still one press      */
#define AXIS_RETURN_MS       8000u
#define AXIS_WAIT_MS        40000u   /* nothing moved: say so, do not hang   */
/* A lever has no return to wait for: on a pedal row, an axis that travelled
 * past AXIS_MOVED over at least two samples and then held still this long is
 * taken as a lever. A device arriving jumps in one sample, so that still has
 * to come back. */
#define AXIS_LEVER_MS        1300u
#define AXIS_LEVER_SAMPLES      2u

typedef enum {
    AXIS_DETECT_OFF = 0,
    AXIS_DETECT_SETTLE,
    AXIS_DETECT_WAIT,
    AXIS_DETECT_FOLLOW,
    AXIS_DETECT_RETURN
} axis_detect_state_t;

static axis_detect_state_t s_axis_state;
static DWORD s_axis_deadline;       /* this phase's limit                   */
static DWORD s_axis_quiet;          /* when something last changed          */
static float s_axis_prev[AXIS_WATCH_MAX];
static int   s_axis_have_prev;
static int   s_axis_excluded[AXIS_WATCH_MAX];
static int   s_axis_pick;           /* index into s_axis_src                */
static float s_axis_rest, s_axis_extreme;
static int   s_axis_attempt;        /* one rejected arrival is forgiven     */
static int   s_axis_settled_late;   /* it was still moving when we gave up  */
static int   s_axis_pedal;          /* a pedal row: a lever is an answer    */
static unsigned s_axis_moves;       /* samples the picked axis travelled in */
static float s_axis_last;           /* the picked axis, last sample         */
static DWORD s_axis_held;           /* since when it has sat at its extreme */
/* Snapshotted at the moment the measurement completes: the confirmation runs
 * a message later, and the preview keeps polling in between. */
static char  s_axis_pick_name[24];
static unsigned s_axis_pick_device; /* the probe's device index             */
static int   s_axis_lever;          /* it stayed where it was put           */
/* How long AXIS_DETECT_WAIT waits for movement. Shorter for the wizard, whose
 * third step has "nothing moves" as a LEGITIMATE answer, a wheel with
 * combined pedals has no second pedal axis, and 40 seconds of staring at a
 * screen is not a way to say so. */
static DWORD s_axis_wait_ms = AXIS_WAIT_MS;

/* ---- Set up: the three steps ---------------------------------------------
 *
 * A WRAPPER AROUND DETECT, NOT A SECOND INSTRUMENT: every guard above is the
 * one it runs. What it adds is the ORDER and the QUESTION, "press the
 * accelerator" rather than "detect the throttle axis", and one summary instead
 * of three confirmations.
 *
 * It also answers two things three separate Detects cannot: whether the pedals
 * are combined (step 3 finds step 2's axis, or finds nothing) and how much
 * wheel rotation to use, asked in degrees because that is the number on the
 * wheel's own driver page. */
enum { WIZ_OFF = 0, WIZ_STEER, WIZ_THROTTLE, WIZ_BRAKE };
static int   s_wizard;              /* WIZ_*, the step running now          */
static char  s_wiz_axis[3][24];     /* what each step measured, by role     */
static unsigned s_wiz_device[3];    /* ...and on which controller           */
static int   s_wiz_invert[3];
static float s_wiz_rest[3], s_wiz_extreme[3];
static int   s_wiz_found[3];
static int   s_wiz_skipped[3];      /* Skip step: the row stays as it was   */
static int   s_wiz_lever[3];        /* stayed where it was put              */
static int   s_wiz_late[3];         /* rest was taken while it still moved  */

static const char *wizard_ask(int step);
static void wizard_took(int step);
static void wizard_failed(const char *why);
static void wizard_stop(const char *why);
static void finish_capture(const char *said);

/* THE GUARD HAS TWO CALLERS, AND EACH GETS ITS OWN WORDS. Detect narrates in
 * axes and numbers on the status bar; Set up's line 2 names only the state,
 * one word first, because line 1 already says what to move. NULL: silent. */
static void guard_say(const char *detect, const char *setup,
                      capture_say_t state)
{
    if (s_wizard) {
        if (!setup)
            return;
        snprintf(s_note_say, sizeof s_note_say, "%s", setup);
        s_note_state = state;
        launcher_note_lines(wizard_ask(s_wizard), s_note_say);
    } else if (detect) {
        set_status(detect);
    }
}

/* "Go", and on the brake step the one number worth showing: how long to do
 * nothing if this wheel has no brake pedal of its own. */
static void guard_say_go(DWORD now)
{
    char text[96];
    int left;

    if (s_wizard != WIZ_BRAKE) {
        guard_say(NULL, "Go.  (Esc cancels)", SAY_GO);
        return;
    }
    left = (LONG)(s_axis_deadline - now) / 1000 + 1;
    if (left < 1)
        left = 1;
    s_note_count = left;
    snprintf(text, sizeof text, "Go.  No separate brake pedal? Just wait: %d",
             left);
    guard_say(NULL, text, SAY_GO);
}

void begin_axis_capture(unsigned index)
{
    const input_probe_device_t *device;
    const input_probe_state_t *st;
    unsigned i, d;

    if (index >= SETTING_COUNT)
        return;
    /* Say why nothing happens, as begin_pad_capture() does. */
    if (launcher_probe_selected() < 0) {
        guard_say("Attach and select a test controller before detecting an axis.",
                  "Select the controller that drives the car first.", SAY_NONE);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    device = input_probe_device((unsigned)launcher_probe_selected());
    if (!probe_backend_matches_setting(device, k_settings[index].key)) {
        guard_say("Select a test device from the same provider before detecting this axis.",
                  "Set up measures a wheel's axes, and this controller is not "
                  "one.", SAY_NONE);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (!launcher_probe_poll()) {
        guard_say("The selected controller is not currently responding.",
                  "The controller is not answering, so Set up stopped. "
                  "Nothing was changed.", SAY_NONE);
        return;
    }
    s_axis_watch_count = 0;
    for (d = 0; d < input_probe_count(); d++) {
        const input_probe_device_t *dev = input_probe_device(d);
        int every = (k_settings[index].flags & SF_AXIS_REF) && dev &&
                    dev->backend != INPUT_PROBE_WINMM;

        if (!every && (int)d != launcher_probe_selected())
            continue;
        if (!(st = launcher_device_state(d)))
            continue;
        for (i = 0; i < st->axis_count &&
                    s_axis_watch_count < AXIS_WATCH_MAX; i++) {
            axis_src_t *src = &s_axis_src[s_axis_watch_count];

            src->device = d;
            src->axis = i;
            snprintf(src->name, sizeof src->name, "%s",
                     st->axis_name[i] ? st->axis_name[i] : "");
            s_axis_now[s_axis_watch_count] = st->axis[i];
            s_axis_baseline[s_axis_watch_count] = st->axis[i];
            s_axis_watch_count++;
        }
    }
    s_axis_pedal = strstr(k_settings[index].key, "_throttle_") != NULL ||
                   strstr(k_settings[index].key, "_brake_") != NULL;
    /* SETTLE BEFORE BASELINING. The values just copied are only a starting
     * point for the quiet test; the baseline Detect measures against is taken
     * when the device has stopped changing, which is a different sample. */
    s_axis_state = AXIS_DETECT_SETTLE;
    s_axis_deadline = GetTickCount() + AXIS_SETTLE_MS;
    s_axis_quiet = GetTickCount();
    s_axis_have_prev = 0;
    s_axis_attempt = 0;
    s_axis_pick = -1;
    s_axis_settled_late = 0;
    memset(s_axis_excluded, 0, sizeof s_axis_excluded);
    s_capture_kind = 3;
    s_capture_setting = (int)index;
    /* Same as begin_pad_capture(): Escape is handled at the top level and the
     * Detect button had the focus. Backspace is deliberately NOT offered;
     * an axis has no "unbound" the way a button ordinal has 0. */
    SetFocus(launcher_window());
    guard_say("Hands off for a moment while the controller settles...",
              "Wait - hands off the wheel and pedals.", SAY_WAIT);
}

static float axis_abs(float v) { return v < 0.0f ? -v : v; }

static void axis_detect_fail(const char *detect_why, const char *setup_why)
{
    s_capture_kind = 0;
    s_axis_state = AXIS_DETECT_OFF;
    /* A STEP THAT MEASURED NOTHING IS NOT ALWAYS A FAILURE: on the brake it is
     * how a wheel with combined pedals answers. The guard cannot know that and
     * must not: it reports what it saw and the wizard decides what it means,
     * and says it on the first line, where a result goes. */
    if (s_wizard) {
        if (s_wizard != WIZ_BRAKE)
            MessageBeep(MB_ICONWARNING);
        wizard_failed(setup_why);
        return;
    }
    set_status(detect_why);
    MessageBeep(MB_ICONWARNING);
}

/* This tick's value of every watched axis. */
static void axis_sample(void)
{
    unsigned i;

    for (i = 0; i < s_axis_watch_count; i++) {
        const axis_src_t *src = &s_axis_src[i];
        const input_probe_state_t *st = launcher_device_state(src->device);

        if (st && src->axis < st->axis_count && st->axis_name[src->axis] &&
            !strcmp(st->axis_name[src->axis], src->name))
            s_axis_now[i] = st->axis[src->axis];
    }
}

/* The picked axis as a person reads it: its name, and its controller when
 * that is not the one ticked. */
static void pick_text(int pick, char *out, size_t out_size)
{
    const input_probe_device_t *d =
        input_probe_device(s_axis_src[pick].device);

    if ((int)s_axis_src[pick].device == launcher_probe_selected() || !d)
        snprintf(out, out_size, "%s", s_axis_src[pick].name);
    else
        snprintf(out, out_size, "%s on %.60s", s_axis_src[pick].name,
                 probe_product_name(d));
}

static void axis_detect_tick(void)
{
    const float *v = s_axis_now;
    DWORD now = GetTickCount();
    unsigned n = s_axis_watch_count, i;
    char why[256], what[96];

    axis_sample();

    switch (s_axis_state) {
    case AXIS_DETECT_SETTLE: {
        int moved = 0;

        if (s_axis_have_prev) {
            for (i = 0; i < n; i++)
                if (axis_abs(v[i] - s_axis_prev[i]) >
                    AXIS_SETTLE_TOL)
                    moved = 1;
        }
        for (i = 0; i < n; i++)
            s_axis_prev[i] = v[i];
        if (!s_axis_have_prev || moved) {
            s_axis_have_prev = 1;
            s_axis_quiet = now;
        }
        /* Quiet for long enough, or out of patience, and the second is NOT a
         * failure: a wheel whose motor is live jitters forever, the
         * measurement is still worth taking, and the user is told the rest may
         * be a moving one. */
        if ((LONG)(now - s_axis_quiet) >= (LONG)AXIS_SETTLE_QUIET_MS ||
            (LONG)(now - s_axis_deadline) >= 0) {
            s_axis_settled_late = (LONG)(now - s_axis_deadline) >= 0;
            for (i = 0; i < n; i++)
                s_axis_baseline[i] = v[i];
            s_axis_state = AXIS_DETECT_WAIT;
            s_axis_deadline = now + s_axis_wait_ms;
            guard_say(s_axis_settled_late ?
                "This controller never stops moving, so rest was taken while "
                "it was still going. Now move the axis you want, through at "
                "least 40% of its travel, and let it go. Escape cancels." :
                "Now move the axis you want, through at least 40% of its "
                "travel, and let it go. Escape cancels.", NULL, SAY_NONE);
            guard_say_go(now);  /* Set up's "never settled" is in the summary */
        }
        break;
    }

    case AXIS_DETECT_WAIT: {
        /* THE LARGEST DEVIATION, NOT THE FIRST OVER THE LINE. Combined pedals
         * move two axes at once and so does a nudged wheel. */
        int best = -1;

        for (i = 0; i < n; i++) {
            float d = axis_abs(v[i] - s_axis_baseline[i]);
            if (s_axis_excluded[i] || d <= AXIS_MOVED)
                continue;
            if (best < 0 ||
                d > axis_abs(v[best] - s_axis_baseline[best]))
                best = (int)i;
        }
        if (best >= 0) {
            s_axis_pick = best;
            s_axis_rest = s_axis_baseline[best];
            s_axis_extreme = v[best];
            s_axis_last = v[best];
            s_axis_moves = 1;
            s_axis_state = AXIS_DETECT_FOLLOW;
            s_axis_quiet = now;
            /* Without it a slow press gets no answer until it is over. */
            guard_say(NULL, "Seen - keep going to the end.", SAY_SEEN);
            break;
        }
        if ((LONG)(now - s_axis_deadline) >= 0) {
            axis_detect_fail("Nothing moved far enough to be sure, so nothing "
                             "was assigned. Pick the axis from the list "
                             "instead; the table above shows which one moves.",
                             "Nothing moved, so Set up stopped. Nothing was "
                             "changed.");
            break;
        }
        /* The countdown ticks only over "Go", never over an "Again". */
        if (s_wizard == WIZ_BRAKE && s_note_state == SAY_GO &&
            (LONG)(s_axis_deadline - now) / 1000 + 1 != s_note_count)
            guard_say_go(now);
        break;
    }

    case AXIS_DETECT_FOLLOW:
        /* A hand moves a lever over several samples; a device arriving
         * jumps in one. */
        if (axis_abs(v[s_axis_pick] - s_axis_last) > AXIS_SETTLE_TOL)
            s_axis_moves++;
        s_axis_last = v[s_axis_pick];
        /* Follow it while it is still going: a fixed window reports a slow
         * press as a short one, and the rest-to-extreme direction below is
         * read off this number. */
        if (axis_abs(v[s_axis_pick] - s_axis_rest) >
            axis_abs(s_axis_extreme - s_axis_rest) + 0.01f) {
            s_axis_extreme = v[s_axis_pick];
            s_axis_quiet = now;
        }
        if ((LONG)(now - s_axis_quiet) >= (LONG)AXIS_FOLLOW_QUIET_MS) {
            s_axis_state = AXIS_DETECT_RETURN;
            s_axis_deadline = now + AXIS_RETURN_MS;
            s_axis_held = now;
            pick_text(s_axis_pick, what, sizeof what);
            snprintf(why, sizeof why, "%s moved %+.2f to %+.2f. Let it go%s.",
                     what, (double)s_axis_rest, (double)s_axis_extreme,
                     s_axis_pedal ? "; a lever can stay where it is" :
                                    " and return it to where it was");
            guard_say(why, s_wizard == WIZ_STEER ?
                      "Got it.  Now back to centre." :
                      "Got it.  Now let go; a lever can stay put.", SAY_DONE);
        }
        break;

    case AXIS_DETECT_RETURN:
        /* AN AXIS THAT NEVER COMES BACK WAS NOT BEING MOVED. That is what a
         * device settling looks like, and it is the one failure that is
         * indistinguishable from success until you wait for it. */
        /* On a pedal row a lever may stay where it was put. */
        if (axis_abs(v[s_axis_pick] - s_axis_extreme) >= AXIS_RETURNED)
            s_axis_held = now;
        s_axis_lever = s_axis_pedal && s_axis_moves >= AXIS_LEVER_SAMPLES &&
                       (LONG)(now - s_axis_held) >= (LONG)AXIS_LEVER_MS;
        if (axis_abs(v[s_axis_pick] - s_axis_rest) < AXIS_RETURNED ||
            s_axis_lever) {
            snprintf(s_axis_pick_name, sizeof s_axis_pick_name, "%s",
                     s_axis_src[s_axis_pick].name);
            s_axis_pick_device = s_axis_src[s_axis_pick].device;
            s_capture_kind = 0;
            s_axis_state = AXIS_DETECT_OFF;
            /* Outside this tick: the confirmation is modal, and a modal
             * dialog opened from inside the preview's timer re-enters it. The
             * wizard's steps are not modal until the last, so they advance
             * here and post nothing. */
            if (s_wizard)
                wizard_took(s_wizard);
            else
                PostMessage(launcher_window(), WM_APP + 3, 0, 0);
            break;
        }
        if ((LONG)(now - s_axis_deadline) < 0)
            break;
        if (s_axis_attempt == 0) {
            /* THROWN AWAY AND RE-RUN, ONCE, with that axis excluded and the
             * new value in the baseline, the probe's own remedy. */
            s_axis_attempt = 1;
            s_axis_excluded[s_axis_pick] = 1;
            s_axis_state = AXIS_DETECT_WAIT;
            s_axis_deadline = now + s_axis_wait_ms;
            for (i = 0; i < n; i++)
                s_axis_baseline[i] = v[i];
            pick_text(s_axis_pick, what, sizeof what);
            snprintf(why, sizeof why, "%s went to %+.2f and stayed there, "
                     "which is an axis ARRIVING rather than a control being "
                     "moved. Ignoring it and listening again, move the axis "
                     "you want.", what, (double)s_axis_extreme);
            guard_say(why, "Again - that was the controller waking up, not "
                      "you.", SAY_AGAIN);
            break;
        }
        axis_detect_fail("That axis did not come back either, so nothing was "
                         "assigned: this reads as a controller settling rather "
                         "than a control being moved. Pick the axis from the "
                         "list instead.",
                         "It did not come back, so Set up stopped. Nothing was "
                         "changed.");
        break;

    case AXIS_DETECT_OFF:
        break;
    }
}

/* WHAT IT CHOSE, AND WHAT IT MEASURED, BEFORE IT IS WRITTEN ANYWHERE.
 *
 * R3's "confirms before it commits". The numbers are in the question because
 * they are the evidence: an axis that went +1.00 to -1.00 is a pedal being
 * floored, one that went -0.02 to +0.41 is a thumbstick being nudged, and only
 * one of those is what the user meant.
 *
 * AND THE MEASUREMENT DECIDES THE INVERT: a pedal resting at an END is a
 * measurement, not something a user should reason about with a checkbox. Rest
 * above extreme means pressing it counts DOWN, which is what `invert = true`
 * is for. Steering is left alone; its rest is the centre. */
/* What a row is set to for an axis on `device`: the bare name on the default
 * device, `<label>:<name>` on any other, making the label if needed. Called
 * only after the user said yes. 0, with the reason said, when the controller
 * cannot be given a line. */
static int axis_value(unsigned device, const char *name, int ref,
                      char *out, size_t out_size)
{
    char label[INI_LABEL_CAP];

    if (!ref || (int)device == launcher_default_row()) {
        snprintf(out, out_size, "%s", name);
        return 1;
    }
    if (!ensure_device_label(input_probe_device(device), label,
                             sizeof label)) {
        hold_note("That controller could not be given a line in [devices], so "
                  "nothing was changed. vcthunder.ini has too many [devices] "
                  "lines; remove the ones for controllers you no longer use.");
        MessageBeep(MB_ICONWARNING);
        return 0;
    }
    snprintf(out, out_size, "%s:%s", label, name);
    return 1;
}

/* The controller an axis is on, as its own line in a message box, or "" for
 * the default device. */
static void device_line(unsigned device, const char *before, char *out,
                        size_t out_size)
{
    const input_probe_device_t *d = input_probe_device(device);

    out[0] = '\0';
    if ((int)device != launcher_default_row() && d)
        snprintf(out, out_size, "%son %.48s", before, probe_product_name(d));
}

void confirm_axis_detect(void)
{
    const char *key, *role;
    char invert_key[64], question[512], said[256], value[INI_VALUE_CAP];
    char where[80];
    size_t n;
    int pedal, invert, index = s_capture_setting, ref;

    if (index < 0 || index >= (int)SETTING_COUNT)
        return;
    key = k_settings[index].key;
    ref = (k_settings[index].flags & SF_AXIS_REF) != 0;
    role = strstr(key, "_throttle_") ? "throttle" :
           strstr(key, "_brake_")    ? "brake"    : NULL;
    pedal = role != NULL;
    invert = s_axis_extreme < s_axis_rest;
    device_line(s_axis_pick_device, "\n", where, sizeof where);

    snprintf(question, sizeof question,
             "%s%s moved from %+.2f to %+.2f and %s.\n\n"
             "Use it for %s?%s",
             s_axis_pick_name, ref ? where : "",
             (double)s_axis_rest, (double)s_axis_extreme,
             s_axis_lever ? "stayed there (a lever)" : "came back",
             k_settings[index].label ? k_settings[index].label : key,
             /* HARD-WRAPPED UNDER 67 COLUMNS, like every box in this flow:
              * MessageBox stops widening at about that and wraps the rest
              * where it likes. */
             !pedal ? "" : invert ?
             "\n\nIt counts down as you press.\n"
             "It will be marked inverted." :
             "\n\nIt counts up as you press.\n"
             "The inverted box will be cleared.");
    if (MessageBoxA(launcher_window(), question, "Detected axis",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        set_status("Nothing was assigned.");
        return;
    }
    if (!axis_value(s_axis_pick_device, s_axis_pick_name, ref, value,
                    sizeof value))
        return;

    /* model_set(), so the role's facts move with it and the invert below
     * lands in that controller's section. */
    model_set(k_settings[index].section, k_settings[index].key, value);
    snprintf(said, sizeof said, "%s set to %s", role ? role : "the axis",
             value);

    if (pedal) {
        /* "<provider>_<role>_axis" -> "<provider>_<role>_invert", built from
         * the key itself so a provider added later needs nothing here. */
        n = strlen(key);
        if (n > 5 && !strcmp(key + n - 5, "_axis")) {
            snprintf(invert_key, sizeof invert_key, "%.*s_invert",
                     (int)(n - 5), key);
            if (setting_find(k_settings[index].section, invert_key) >= 0) {
                model_set(k_settings[index].section, invert_key,
                          invert ? "true" : "false");
                snprintf(said + strlen(said), sizeof said - strlen(said),
                         ", inverted %s (measured, not assumed)",
                         invert ? "on" : "off");
            }
        }
    }
    mark_dirty();
    snprintf(said + strlen(said), sizeof said - strlen(said),
             ". Press Apply to write it to vcthunder.ini.");
    set_status(said);
}

/* THE REFERENCE A CONTROLLER BUTTON IS WRITTEN AS. 0 when it was refused,
 * with the status line already saying why. */
static int pad_ref_for(unsigned device, unsigned ordinal, char *ref,
                       size_t ref_size)
{
    const input_probe_device_t *d = input_probe_device(device);
    char label[INI_LABEL_CAP];

    if (!d)
        return 0;
    /* Every binding written here names its controller, the wheel included,
     * so ticking another controller cannot re-point it. The exception is an
     * XInput pad that drives the car: io_host.c reads it and it holds no
     * slot, so a bare ordinal is the only form that works there. */
    if ((int)device == launcher_probe_selected() &&
        d->backend != INPUT_PROBE_DINPUT) {
        snprintf(ref, ref_size, "%u", ordinal);
    } else if (ensure_device_label(d, label, sizeof label)) {
        snprintf(ref, ref_size, "%s:%u", label, ordinal);
    } else if (d->backend != INPUT_PROBE_WINMM) {
        /* A DirectInput controller always gets a line (name or ID); this is
         * a full table or no free label. */
        set_status("This controller could not be given a line in [devices], "
                   "so a binding cannot name it. vcthunder.ini has too many "
                   "[devices] lines; remove the ones for controllers you no "
                   "longer use.");
        MessageBeep(MB_ICONWARNING);
        return 0;
    } else {
        /* A WinMM view is never a slot: it is a device DirectInput or XInput
         * already lists. */
        set_status("A controller's WinMM view cannot be used alongside the one "
                   "with the wheel; bind the button on its DirectInput or "
                   "XInput row instead.");
        MessageBeep(MB_ICONWARNING);
        return 0;
    }
    return 1;
}

/* ONE INPUT AS THE HOST READS IT: two names for one key are one key, and a
 * bare ordinal is the same button as that controller's label and ordinal. */
static int item_same(const binding_item_t *a, const binding_item_t *b)
{
    int x;

    if (a->pad != b->pad)
        return 0;
    if (a->pad)
        return pad_refs_same(a->value, b->value);
    x = io_config_key_code(a->value);
    return x ? x == io_config_key_code(b->value) : !_stricmp(a->value, b->value);
}

static const char *item_text(const binding_item_t *item, char *out,
                             size_t out_size)
{
    if (item->pad)
        pad_binding_text(item->value, out, out_size);
    else
        snprintf(out, out_size, "%s", key_display_name(item->value));
    return out;
}

static const char *action_name(unsigned action)
{
    const action_desc_t *a = &k_actions[action];
    const char *label = a->label[launcher_binding_title()];

    return label ? label : a->key;
}

/* The capture ends without writing anything, and says why. */
static void leave_slot(const char *said)
{
    s_capture_kind = 0;
    update_binding_display((unsigned)s_capture_action);
    set_status(said);
}

/* PUT ONE INPUT IN THE SLOT, OR EMPTY IT (`item` NULL), and nothing else: the
 * other slot is never touched. A slot past the row's last input appends, so
 * an empty slot always means "one more".
 *
 * A row a hand-edited file gave more inputs than there are slots keeps the
 * ones on screen. Keeping the rest would leave a binding the page cannot show
 * or remove; the status line says they went. */
static void commit_slot(const binding_item_t *item, int restoring)
{
    binding_item_t row[IO_MAX_KEY_REFS + IO_MAX_PAD_REFS];
    unsigned held, n, at, i;
    int title = launcher_binding_title();
    char said[320], what[160];
    size_t len;

    held = model_binding_items(title, (unsigned)s_capture_action, row,
                               sizeof row / sizeof row[0]);
    n = held < BINDING_SLOTS ? held : BINDING_SLOTS;
    at = (unsigned)s_capture_slot < n ? (unsigned)s_capture_slot : n;
    if (item) {
        for (i = 0; i < n; i++)
            if (i != at && item_same(&row[i], item)) {
                snprintf(said, sizeof said,
                         "%s already has %s in its other slot.",
                         action_name((unsigned)s_capture_action),
                         item_text(item, what, sizeof what));
                leave_slot(said);
                return;
            }
        row[at] = *item;
        if (at == n)
            n++;
    } else {
        if (at == n) {
            leave_slot("That slot was already empty.");
            return;
        }
        for (i = at; i + 1 < n; i++)
            row[i] = row[i + 1];
        n--;
    }
    if (!model_binding_set_items(title, (unsigned)s_capture_action, row, n)) {
        MessageBeep(MB_ICONWARNING);
        leave_slot("That does not fit this control's line in vcthunder.ini, "
                   "so nothing was changed.");
        return;
    }
    if (item && restoring && at != (unsigned)s_capture_slot)
        /* A row has no gaps, so a default for a slot past the row's end
         * lands at the end, and the sentence has to say where. */
        snprintf(said, sizeof said, "%s: %s is back as slot %d's default, "
                 "in slot %u because a row has no gaps.",
                 action_name((unsigned)s_capture_action),
                 item_text(item, what, sizeof what), s_capture_slot + 1,
                 at + 1);
    else if (item)
        snprintf(said, sizeof said, restoring ?
                 "%s: slot back to its default, %s." : "%s: %s.",
                 action_name((unsigned)s_capture_action),
                 item_text(item, what, sizeof what));
    else
        snprintf(said, sizeof said, restoring ?
                 "%s: slot back to its default, empty." :
                 "%s: slot emptied.",
                 action_name((unsigned)s_capture_action));
    len = strlen(said);
    if (held > BINDING_SLOTS)
        snprintf(said + len, sizeof said - len,
                 " The %u more vcthunder.ini gave it %s dropped: a control "
                 "holds two.", held - BINDING_SLOTS,
                 held - BINDING_SLOTS == 1 ? "was" : "were");
    len = strlen(said);
    snprintf(said + len, sizeof said - len,
             " Press Apply to write it to vcthunder.ini.");
    finish_capture(said);
}

/* ONE CAPTURE, EVERY CONTROLLER. The keyboard half arrives through WM_KEYDOWN
 * and the controller half through here, and whichever is pressed first fills
 * the slot; the page never asks which kind of thing is coming.
 *
 * ARMED ONLY AFTER EVERYTHING IS RELEASED, across every device: a wheel with a
 * paddle held, or the button the user is still holding from the last capture,
 * would otherwise bind itself instantly. Re-armed after a refused press for
 * the same reason. */
static void capture_tick(void)
{
    unsigned i, b, n = input_probe_count();
    int any = 0;

    if (n > INPUT_PROBE_MAX_DEVICES)
        n = INPUT_PROBE_MAX_DEVICES;
    for (i = 0; i < n; i++) {
        const input_probe_state_t *d = launcher_device_state(i);
        if (d && io_buttons_any(d->buttons))
            any = 1;
    }
    if (!s_capture_armed) {
        if (!any)
            s_capture_armed = 1;
        return;
    }
    /* In list order, one pass. */
    for (i = 0; i < n; i++) {
        const input_probe_state_t *d = launcher_device_state(i);

        if (!d)
            continue;
        for (b = 0; b < IO_MAX_BUTTONS; b++)
            if (io_button_down(d->buttons, b + 1)) {
                binding_item_t item;

                s_capture_armed = 0;
                if (!k_actions[s_capture_action].has_pad) {
                    /* Steering and the pedals: a controller drives the axis
                     * itself, so only the keyboard needs a binding here. */
                    MessageBeep(MB_ICONWARNING);
                    set_status("This control takes keys only: a controller "
                               "drives it through its axis. Press a key, or "
                               "Esc.");
                    return;
                }
                item.pad = 1;
                if (pad_ref_for(i, b + 1, item.value, sizeof item.value))
                    commit_slot(&item, 0);
                return;
            }
    }
    if ((LONG)(GetTickCount() - s_capture_deadline) >= 0) {
        leave_slot("Nothing was pressed, so the slot was left as it was.");
    } else {
        int left = (LONG)(s_capture_deadline - GetTickCount()) / 1000;
        if (left != s_capture_shown) {
            s_capture_shown = left;
            update_binding_display((unsigned)s_capture_action);
        }
    }
}

static const char *key_name_from_message(WPARAM key, LPARAM detail,
                                         char *out, size_t out_size)
{
    unsigned vk = (unsigned)key;
    unsigned scan = (unsigned)((detail >> 16) & 0xFF);
    int extended = (detail & (1L << 24)) != 0;

    if (vk >= 'A' && vk <= 'Z') {
        out[0] = (char)vk; out[1] = '\0'; return out;
    }
    if (vk >= '0' && vk <= '9') {
        out[0] = (char)vk; out[1] = '\0'; return out;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(out, out_size, "F%u", vk - VK_F1 + 1); return out;
    }
    if (vk == VK_SHIFT) {
        vk = MapVirtualKeyA(scan, MAPVK_VSC_TO_VK_EX);
        return vk == VK_RSHIFT ? "RSHIFT" : "LSHIFT";
    }
    if (vk == VK_LSHIFT) return "LSHIFT";
    if (vk == VK_RSHIFT) return "RSHIFT";
    if (vk == VK_CONTROL) return extended ? "RCTRL" : "LCTRL";
    if (vk == VK_LCONTROL) return "LCTRL";
    if (vk == VK_RCONTROL) return "RCTRL";
    if (vk == VK_MENU) return extended ? "RALT" : "LALT";
    if (vk == VK_LMENU) return "LALT";
    if (vk == VK_RMENU) return "RALT";
    switch (vk) {
    case VK_LEFT: return "LEFT";
    case VK_RIGHT: return "RIGHT";
    case VK_UP: return "UP";
    case VK_DOWN: return "DOWN";
    case VK_SPACE: return "SPACE";
    case VK_RETURN: return "ENTER";
    case VK_TAB: return "TAB";
    case VK_OEM_MINUS: return "MINUS";
    case VK_OEM_PLUS: return "EQUALS";
    case VK_OEM_COMMA: return "COMMA";
    case VK_OEM_PERIOD: return "PERIOD";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_PRIOR: return "PGUP";
    case VK_NEXT: return "PGDN";
    case VK_INSERT: return "INSERT";
    case VK_DELETE: return "DELETE";
    default: return NULL;
    }
}

/* DO THESE TWO BINDINGS SHARE AN INPUT? Element by element, because both
 * halves are comma lists and a whole-value compare only finds two rows given
 * the SAME list. `boost = LCTRL, SPACE` against `view_low = SPACE` is the
 * realistic collision.
 *
 * "NONE" and "0" are not inputs, or two emptied rows would be reported as two
 * controls bound to the same nothing. */
static int binding_lists_share(const char *a, const char *b, int pad,
                               char *shared, size_t shared_size)
{
    char x[64], y[64];
    unsigned i, j;

    for (i = 0; ini_list_item(a, i, x, sizeof x); i++) {
        if (!x[0] || !_stricmp(x, pad ? "0" : "NONE"))
            continue;
        for (j = 0; ini_list_item(b, j, y, sizeof y); j++)
            if (!_stricmp(x, y)) {
                snprintf(shared, shared_size, "%s", x);
                return 1;
            }
    }
    return 0;
}

/* Nonzero when a collision was found AND REPORTED, so the caller does not
 * overwrite the message with a success line.
 *
 * It names both actions and the input they share: a warning that does not name
 * the other end sends the user hunting through twenty rows. */
static int report_duplicate_binding(int pad)
{
    unsigned i, j;
    for (i = 0; i < ACTION_COUNT; i++) {
        int have_i = pad ? k_actions[i].has_pad :
            k_actions[i].key_default[launcher_binding_title()] != NULL;
        if (!action_visible(i) || !have_i)
            continue;
        for (j = i + 1; j < ACTION_COUNT; j++) {
            char one[64], rendered[224], why[256];
            const char *li, *lj, *what;
            int have_j = pad ? k_actions[j].has_pad :
                k_actions[j].key_default[launcher_binding_title()] != NULL;

            if (!action_visible(j) || !have_j)
                continue;
            if (!binding_lists_share(
                    pad ? model_pad_binding(launcher_binding_title(), i) :
                          model_key_binding(launcher_binding_title(), i),
                    pad ? model_pad_binding(launcher_binding_title(), j) :
                          model_key_binding(launcher_binding_title(), j),
                    pad, one, sizeof one))
                continue;
            li = k_actions[i].label[launcher_binding_title()];
            lj = k_actions[j].label[launcher_binding_title()];
            /* THE RENDERED BINDING, not the raw value with "Button " glued in
             * front: a device-qualified value came out as "Button 9, th8:1",
             * which reads as one button with a comma in its name. */
            if (pad) {
                pad_binding_text(one, rendered, sizeof rendered);
                what = rendered;
            } else {
                what = one;
            }
            snprintf(why, sizeof why,
                     "%s is bound to both %s and %s, "
                     "pressing it will do both.",
                     what, li ? li : k_actions[i].key,
                     lj ? lj : k_actions[j].key);
            set_status(why);
            MessageBeep(MB_ICONWARNING);
            return 1;
        }
    }
    return 0;
}

/* ---- a slot: press whatever you want in it ------------------------------
 *
 * ONE CAPTURE FOR BOTH HALVES. A slot takes a key or a button on any
 * controller, so nobody has to say what KIND of thing they are about to press;
 * whatever is pressed first fills it, and the half it is written to is the
 * half it is. */
static void finish_capture(const char *said)
{
    s_capture_kind = 0;
    mark_dirty();
    update_binding_display((unsigned)s_capture_action);
    /* BOTH HALVES, and the order is the order the user is most likely to have
     * just created one in. Either returning nonzero means the warning is on
     * the status line and the success sentence must not overwrite it. */
    if (!report_duplicate_binding(0) && !report_duplicate_binding(1))
        set_status(said);
}

static void handle_key_capture(WPARAM key, LPARAM detail)
{
    binding_item_t item;
    const char *mapped;
    const action_desc_t *a = &k_actions[s_capture_action];

    if (key == VK_ESCAPE) {
        /* NOTHING WAS WRITTEN YET, so there is nothing to undo: the slot is
         * committed when the press arrives and not before. */
        leave_slot("Left as it was.");
        return;
    }
    /* DELETE EMPTIES, BACKSPACE PUTS BACK: the slot's own default, the
     * input at the same place in the row's default, or empty if the
     * default has nothing there. One slot, as every other press. */
    if (key == VK_DELETE) {
        commit_slot(NULL, 0);
        return;
    }
    if (key == VK_BACK) {
        binding_item_t def[IO_MAX_KEY_REFS + IO_MAX_PAD_REFS];
        unsigned n = model_binding_default_items(launcher_binding_title(),
                         (unsigned)s_capture_action, def,
                         sizeof def / sizeof def[0]);

        commit_slot((unsigned)s_capture_slot < n ? &def[s_capture_slot] :
                                                   NULL, 1);
        return;
    }
    if (!a->key_default[launcher_binding_title()]) {
        /* Offroad's two shifter paddles have no keyboard half at all: they are
         * a controller gate and the three gears are the keyboard's answer. */
        MessageBeep(MB_ICONWARNING);
        set_status("This control has no keyboard key, press a button on a "
                   "controller instead.");
        return;
    }
    mapped = key_name_from_message(key, detail, item.value, sizeof item.value);
    if (!mapped) {
        MessageBeep(MB_ICONWARNING);
        set_status("That key is not one of the MAME key names supported by "
                   "VCThunder.");
        return;
    }
    item.pad = 0;
    if (mapped != item.value)
        snprintf(item.value, sizeof item.value, "%s", mapped);
    commit_slot(&item, 0);
}

/* A SLOT IS THE WAY IN, so a press replaces exactly one input and the row's
 * other input stays where it is. A slot with nothing in it adds one. */
void begin_capture(unsigned action, unsigned slot)
{
    s_capture_kind = 4;
    s_capture_action = (int)action;
    s_capture_slot = (int)slot;
    s_capture_armed = 0;
    s_capture_deadline = GetTickCount() + CAPTURE_MS;
    s_capture_shown = -1;
    /* THE KEYS THIS PROMISES ARRIVE AT THE MAIN WINDOW, AND THE BUTTON JUST
     * CLICKED HAS THE FOCUS. Without taking it back, Escape is dispatched to
     * that push button and the line below promises a key that does nothing. */
    SetFocus(launcher_window());
    update_binding_display(action);
    set_status("Press the key or button for this slot, on any controller. "
               "Delete empties it, Backspace puts back its default, Esc "
               "leaves it as it was.");
}

/* EVERY ROW OF THE GAME SHOWN back to its default, both halves. Nothing to
 * confirm: nothing is written before Apply, and Close asks about unsaved
 * changes. */
void restore_all_bindings(void)
{
    unsigned i;
    char said[160];

    s_capture_kind = 0;
    for (i = 0; i < ACTION_COUNT; i++) {
        model_binding_restore(launcher_binding_title(), i);
        update_binding_display(i);
    }
    mark_dirty();
    snprintf(said, sizeof said, "Every %s button is back to its default. "
             "Press Apply to write it to vcthunder.ini.",
             model_title_name(launcher_binding_title()));
    /* A shared [digital] line can still put one input on two rows. */
    if (!report_duplicate_binding(0) && !report_duplicate_binding(1))
        set_status(said);
}

/* ---- Set up: three steps, and they MEASURE -------------------------------
 *
 * Every guard is Detect's, unchanged. What this adds is the order, the question
 * in the user's own words, and one summary instead of three confirmations;
 * plus the two answers three separate Detects cannot give: whether the pedals
 * are combined, and how much wheel rotation to use. */
static const char *wizard_axis_key(int step)
{
    return step == WIZ_STEER    ? "dinput_steering_axis" :
           step == WIZ_THROTTLE ? "dinput_throttle_axis" :
                                  "dinput_brake_axis";
}

static const char *wizard_ask(int step)
{
    /* One line each at every window size that shows the page. */
    return step == WIZ_STEER ?
        "Step 1 of 3 - Steering: turn fully left and right, then centre." :
        step == WIZ_THROTTLE ?
        "Step 2 of 3 - Accelerator: press it fully, pedal or lever." :
        "Step 3 of 3 - Brake: press it fully, pedal or lever.";
}

static void wizard_note(const char *text)
{
    hold_note(text);
}

static void wizard_start_step(int step);

static void wizard_stop(const char *why)
{
    s_wizard = WIZ_OFF;
    s_capture_kind = 0;
    s_axis_state = AXIS_DETECT_OFF;
    s_axis_wait_ms = AXIS_WAIT_MS;
    wizard_note(why);
}

/* TAKING A CAPTURE AWAY FROM OUTSIDE HAS TO TAKE THE FLOW THAT STARTED IT.
 * `s_capture_kind` gates every tick and every key press, so zeroing it ALONE
 * leaves Set up running with nothing driving it: the guard is never ticked
 * again, its two lines still ask for a step nobody is measuring, and the
 * top-level cancel branch is gated on the flag just cleared.
 *
 * SEVEN CALLERS. Set up's OWN transitions are deliberately not here;
 * wizard_took(), wizard_failed() and wizard_start_step() hand the capture on
 * and manage both fields. One door out. */
void abandon_capture(void)
{
    int slot_row = s_capture_kind == 4;

    s_capture_kind = 0;
    if (slot_row)
        update_binding_display((unsigned)s_capture_action);
    if (s_wizard)
        wizard_stop("Set up was interrupted; nothing was changed.");
}

/* What the three steps measured, stated before any of it is written: a wrong
 * answer must not first reach the user as a wrong answer already in the
 * model. */
static void wizard_finish(void)
{
    static const char *const role[3] = { "Steering", "Accelerator", "Brake" };
    char text[1200], line[220], where[80], value[3][INI_VALUE_CAP];
    int combined, late, i, any, refused;
    size_t at;

    s_wizard = WIZ_OFF;
    s_capture_kind = 0;
    s_axis_wait_ms = AXIS_WAIT_MS;
    /* Combined pedals are measured, not chosen: a wheel that merges its
     * pedals has one axis, so the brake step finds nothing, or the
     * accelerator's axis on the same controller. */
    combined = !s_wiz_skipped[2] &&
               (!s_wiz_found[2] ||
                (s_wiz_found[1] && s_wiz_device[2] == s_wiz_device[1] &&
                 !strcmp(s_wiz_axis[2], s_wiz_axis[1])));
    any = s_wiz_found[0] || s_wiz_found[1] || s_wiz_found[2] || combined;
    if (!any) {
        wizard_note("Every step was skipped, so nothing was changed.");
        return;
    }

    at = (size_t)snprintf(text, sizeof text,
        "This is what the three steps measured:\n\n");
    for (i = 0; i < 3; i++) {
        /* At most 62 columns: MessageBox wraps at about 67. The controller,
         * when it is not the wheel, gets a line of its own. */
        where[0] = '\0';
        if (s_wiz_skipped[i]) {
            snprintf(line, sizeof line, "  %-12s skipped, left as it was\n",
                     role[i]);
        } else if (i == 2 && combined) {
            snprintf(line, sizeof line,
                     "  Brake        same axis as the accelerator "
                     "(combined)\n");
        } else if (!s_wiz_found[i]) {
            snprintf(line, sizeof line, "  %-12s nothing moved\n", role[i]);
        } else {
            snprintf(line, sizeof line,
                     "  %-12s %-7s %+.2f to %+.2f%s\n", role[i],
                     s_wiz_axis[i], (double)s_wiz_rest[i],
                     (double)s_wiz_extreme[i],
                     i == 0 ? "" :
                     s_wiz_lever[i] ?
                         (s_wiz_invert[i] ? ", a lever, counts down" :
                                            ", a lever, counts up") :
                     s_wiz_invert[i] ? ", rests high, counts down" :
                                       ", rests low, counts up");
            device_line(s_wiz_device[i], "               ", where,
                        sizeof where);
        }
        at += (size_t)snprintf(text + at, sizeof text - at, "%s", line);
        if (where[0] && at < sizeof text)
            at += (size_t)snprintf(text + at, sizeof text - at, "%s\n", where);
    }
    /* Worth knowing when judging the numbers, nothing to act on mid-step. */
    for (i = 0, late = 0; i < 3; i++)
        if (s_wiz_found[i] && s_wiz_late[i] && at < sizeof text)
            at += (size_t)snprintf(text + at, sizeof text - at,
                                   "%s%s never went fully still; measured "
                                   "anyway.\n", late++ ? "" : "\n", role[i]);
    if (at < sizeof text)
        snprintf(text + at, sizeof text - at,
                 "\nSave this?\n"
                 "Nothing is written to vcthunder.ini until you press Apply.");
    if (MessageBoxA(launcher_window(), text, "What Set up measured",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        wizard_note("Nothing was changed.");
        return;
    }
    /* A role whose controller cannot be given a line is left as it was, as
     * Skip leaves it; axis_value() has said why. */
    for (i = 0, refused = 0; i < 3; i++)
        if (s_wiz_found[i] && !(i == 2 && combined) &&
            !axis_value(s_wiz_device[i], s_wiz_axis[i], 1, value[i],
                        sizeof value[i])) {
            s_wiz_found[i] = 0;
            s_wiz_skipped[i] = 1;
            refused++;
        }
    if (!s_wiz_found[0] && !s_wiz_found[1] && s_wiz_skipped[2])
        return;
    /* The axis before its invert, so the invert lands in the section of the
     * controller the role now names. */
    if (s_wiz_found[0])
        model_set("analog", "dinput_steering_axis", value[0]);
    if (s_wiz_found[1]) {
        model_set("analog", "dinput_throttle_axis", value[1]);
        model_set("analog", "dinput_throttle_invert",
                  s_wiz_invert[1] ? "true" : "false");
    }
    if (s_wiz_skipped[2]) {
        /* Left exactly as it was, pedal mode included. */
    } else if (combined) {
        model_set("analog", "dinput_brake_axis", "none");
        model_set("analog", "dinput_pedal_mode", "combined");
    } else {
        model_set("analog", "dinput_brake_axis", value[2]);
        model_set("analog", "dinput_brake_invert",
                  s_wiz_invert[2] ? "true" : "false");
        model_set("analog", "dinput_pedal_mode", "separate");
    }
    mark_dirty();
    /* After mark_dirty(), whose status line this replaces; held through the
     * rebuild. A refusal's own note is left standing. */
    if (!refused)
        hold_note("Set up finished. Press Apply to write it to vcthunder.ini.");
    request_relayout();
}

/* One step's result, taken from the guard's own measurement. */
static void wizard_took(int step)
{
    int i = step - WIZ_STEER;

    snprintf(s_wiz_axis[i], sizeof s_wiz_axis[i], "%s", s_axis_pick_name);
    s_wiz_device[i] = s_axis_pick_device;
    s_wiz_rest[i] = s_axis_rest;
    s_wiz_extreme[i] = s_axis_extreme;
    s_wiz_invert[i] = s_axis_extreme < s_axis_rest;
    s_wiz_lever[i] = s_axis_lever;
    s_wiz_late[i] = s_axis_settled_late;
    s_wiz_found[i] = 1;
    if (step == WIZ_BRAKE)
        wizard_finish();
    else
        wizard_start_step(step + 1);
}

/* Skip: this row stays as it was, and the next step starts. */
void wizard_skip(void)
{
    int step = s_wizard;

    if (!step)
        return;
    s_capture_kind = 0;
    s_axis_state = AXIS_DETECT_OFF;
    s_wiz_found[step - WIZ_STEER] = 0;
    s_wiz_skipped[step - WIZ_STEER] = 1;
    if (step == WIZ_BRAKE)
        wizard_finish();
    else
        wizard_start_step(step + 1);
}

int capture_wizard_running(void)
{
    return s_wizard != WIZ_OFF;
}

/* A step that measured nothing. On the brake it is an ANSWER, combined
 * pedals, and on the other two it is the end of the run. */
static void wizard_failed(const char *why)
{
    if (s_wizard == WIZ_BRAKE) {
        s_wiz_found[2] = 0;
        wizard_finish();
        return;
    }
    wizard_stop(why);
}

static void wizard_start_step(int step)
{
    int index = setting_find("analog", wizard_axis_key(step));

    if (index < 0) {
        wizard_stop("This build has no setting for that axis.");
        return;
    }
    s_wizard = step;
    s_note_say[0] = '\0';
    s_note_state = SAY_NONE;
    /* THE BRAKE STEP CAN LEGITIMATELY MEASURE NOTHING, and forty seconds of
     * staring at a screen is not a way to say "this wheel has one pedal axis".
     * The other two have no such answer and keep the guard's own patience. */
    s_axis_wait_ms = step == WIZ_BRAKE ? 12000u : AXIS_WAIT_MS;
    s_wiz_found[step - WIZ_STEER] = 0;
    begin_axis_capture((unsigned)index);
    if (s_capture_kind != 3) {          /* begin_axis_capture refused */
        char why[sizeof s_note_say];

        snprintf(why, sizeof why, "%s", s_note_say);
        s_wizard = WIZ_OFF;
        s_axis_wait_ms = AXIS_WAIT_MS;
        wizard_note(why);       /* guard_say() said why; now it is the result */
    }
}

void begin_wizard(void)
{
    const input_probe_device_t *d = launcher_probe_selected() >= 0 ?
        input_probe_device((unsigned)launcher_probe_selected()) : NULL;

    /* SET UP'S REFUSALS ARE SET UP'S WORDS, so they go where the rest of what
     * it says goes: the line under the bars, beside the button just pressed. */
    if (!d) {
        hold_note("Select the controller that drives the car first.");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (d->backend != INPUT_PROBE_DINPUT) {
        /* A GAMEPAD'S AXES ARE FIXED BY ITS OWN API and the shipped defaults
         * are already right for one; there is nothing to measure and the three
         * questions would be about controls it does not have. */
        hold_note("Set up measures a wheel's axes. A gamepad's sticks and "
                  "triggers are already mapped; to change them, turn on Show "
                  "advanced settings on Help.");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    memset(s_wiz_found, 0, sizeof s_wiz_found);
    memset(s_wiz_skipped, 0, sizeof s_wiz_skipped);
    memset(s_wiz_lever, 0, sizeof s_wiz_lever);
    memset(s_wiz_late, 0, sizeof s_wiz_late);
    wizard_start_step(WIZ_STEER);
}

/* ---- what the window asks for --------------------------------------------
 *
 * Functions rather than fields, so this machine's state is not reachable from
 * the window. The header's first paragraph is why. */

int capture_active(void)
{
    return s_capture_kind != 0;
}

void capture_reset(void)
{
    s_capture_kind = 0;
    s_wizard = WIZ_OFF;
    s_axis_state = AXIS_DETECT_OFF;
}

/* THE KEYBOARD HALF OF A CAPTURE, AND ESCAPE, IN ONE PLACE. The binding
 * capture takes the keyboard and the controllers at once, so the keyboard half
 * arrives here and the controller half on the timer; whichever comes first is
 * the one the user meant.
 *
 * ESCAPE IS HERE RATHER THAN IN THE WINDOW PROCEDURE, and that is not a
 * tidying. It is gated on `s_capture_kind`: if the window could zero that field
 * on its own, a capture could still be running with Escape no longer able to
 * end it. The branch and the flag it reads are the same translation unit's. */
int capture_key(WPARAM key, LPARAM detail)
{
    if (s_capture_kind == 4) {
        handle_key_capture(key, detail);
        return 1;
    }
    if (key == VK_ESCAPE && s_capture_kind) {
        s_capture_kind = 0;
        if (s_wizard)
            wizard_stop("Set up cancelled; nothing was changed.");
        else
            set_status("Cancelled.");
        return 1;
    }
    return 0;
}

/* ONE FRAME. `have_device` is whether the selected controller answered this tick: an
 * axis guard must not measure samples that are not a device's. */
void capture_tick_all(int have_device)
{
    if (s_capture_kind == 4)
        capture_tick();
    else if (s_capture_kind == 3 && have_device)
        axis_detect_tick();
}

int capture_slot_text(unsigned action, unsigned slot, char *out,
                      size_t out_size)
{
    int left;

    if (s_capture_kind != 4 || s_capture_action != (int)action ||
        s_capture_slot != (int)slot)
        return 0;
    left = (LONG)(s_capture_deadline - GetTickCount()) / 1000 + 1;
    if (left < 0)
        left = 0;
    /* Short: the slot can be narrow, and the status line has the sentence. */
    snprintf(out, out_size, "Press now...  %d", left);
    return 1;
}

const char *capture_note(void)
{
    if (s_wizard)
        return wizard_ask(s_wizard);
    return s_note_hold_ms ? s_note_hold : NULL;
}

const char *capture_note_say(void)
{
    return s_wizard ? s_note_say : "";
}

capture_say_t capture_note_state(void)
{
    return s_wizard ? s_note_state : SAY_NONE;
}

/* The live panel's decay. Set up owns both lines while it runs, untimed. */
int capture_note_tick(void)
{
    if (s_wizard)
        return 1;
    if (!s_note_hold_ms)
        return 0;
    s_note_hold_ms = s_note_hold_ms > NOTE_TICK_MS ?
                     s_note_hold_ms - NOTE_TICK_MS : 0;
    return 1;
}
