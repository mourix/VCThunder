/* launcher_controls.c -- the launcher's Controls page and Force feedback tab.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Which controllers are attached, what drives the car and what the buttons do,
 * and the two lines under the live bars. The window (launcher.c) owns the page
 * these are built on, the navigation and every message; launcher_page.h is all
 * that passes between them. Add, Detect and Set up, which run on this page,
 * are launcher_capture.c.
 */
#include "launcher_page.h"
#include "launcher_model.h"
#include "launcher_capture.h"
#include "settings.h"
#include "io.h"

#include <commctrl.h>
#include <stdio.h>
#include <string.h>

/* ---- the Controls page ----------------------------------------------------
 *
 * Three groups down one page, in the order somebody sets a wheel up: which
 * controllers are attached, what drives the car, what the buttons do. Every
 * attached controller is polled every tick, so nothing has to be selected
 * before it can be used. */

/* THE LIVE PANEL: ONE BAR PER ROLE, AND THE BAR IS THE MAPPED VALUE: what the
 * cabinet will be handed, with deadzone, pedal mode and inversion applied.
 * Only the mapped value answers "does the car get my accelerator"; the raw
 * table is one click away.
 *
 * MEASURED rather than laid out from constants: the row count is a property of
 * what is plugged in. */
#define AXIS_ROW_PITCH 22

/* THE LINES UNDER THE LIVE BARS and the raw-view checkbox: the note and Set
 * up's line 2, reserved at exactly the height of what is showing. The render
 * and place_note_lines() measure at the same stored width and text, or they
 * could disagree about a wrap and rebuild forever. */
#define NOTE_GAP 12             /* from the control above to the note      */
static int s_note_text_h, s_note_w;
static char s_note_last[512];   /* the last note shown, redrawn by a rebuild */
static int  note_text_height(const char *first, const char *second);
static void place_note_lines(void);

/* The [devices] label this controller ALREADY has, without creating one:
 * device_what() runs from the render and the 30 Hz refresh, and a lookup that
 * writes would put a line in the file for every controller merely looked at. */
static int device_label_of(const input_probe_device_t *d, char *out,
                           size_t out_size)
{
    ini_device_id_t ids[INPUT_PROBE_MAX_DEVICES];
    unsigned count;
    int which;

    if (!d || (which = probe_id_of(d, ids, &count)) < 0)
        return 0;
    return model_device_label_find(probe_provider(d), ids, count,
                                   (unsigned)which, out, out_size);
}

/* A WinMM row is a second view of a controller DirectInput already listed, and
 * this host reads one provider at a time, so two of them are two rows nobody
 * can tell apart. Behind the legacy disclosure rather than gone: a device no
 * other backend enumerates is still one click away. */
static int device_listed(unsigned i)
{
    const input_probe_device_t *d = input_probe_device(i);

    if (!d)
        return 0;
    return d->backend != INPUT_PROBE_WINMM || launcher_show_legacy();
}

/* WHAT THE CONTROLLER IS, in terms this host can answer: how many axes and
 * buttons it reports, and whether it publishes force feedback. Not "wheel,
 * pedals": that would take a per-model table, which this host does not keep. */
void device_what(unsigned i, char *out, size_t out_size)
{
    const input_probe_device_t *d = input_probe_device(i);
    size_t at;

    if (!d) {
        out[0] = '\0';
        return;
    }
    /* "FORCE FEEDBACK" MEANS THE WHEEL'S MOTOR, AND AN XInput PAD HAS NOT GOT
     * ONE. input_probe sets the flag for every XInput device because every one
     * can be TESTED, but the game's force is a DirectInput effect, and a pad
     * gets the derived model behind [ffb] rumble, a different switch on a
     * different page. */
    at = (size_t)snprintf(out, out_size, "%u axes, %u buttons%s",
                          d->axis_count, d->button_count,
                          !d->force_feedback ? "" :
                          d->backend == INPUT_PROBE_XINPUT ?
                          ", rumble motors" : ", force feedback");
    /* The label the device's bindings carry, so the list and the binding rows
     * share one vocabulary. An XInput or WinMM row names its [analog.<label>]
     * section instead. */
    if (at < out_size) {
        char label[INI_LABEL_CAP];
        if (device_label_of(d, label, sizeof label))
            at += (size_t)snprintf(out + at, out_size - at,
                                   d->backend == INPUT_PROBE_DINPUT ?
                                   "   called \"%s\" in bindings" :
                                   "   [analog.%s]; its buttons bind as plain "
                                   "numbers", label);
    }
    if (at < out_size && launcher_device_moving(i))
        snprintf(out + at, out_size - at, "   <- moving");
}

static int muted_line(int y, const char *text, int id)
{
    int width = page_width();
    int h = text_block_height(theme_font(), text, width - 40);

    make_control(0, "STATIC", text, SS_LEFT, 20, y, width - 40, h, launcher_page(), id);
    return y + h + 6;
}

/* One row per axis the controller reports, RAW. Behind its own disclosure and
 * headed with what it is: the analog bars are the map's answer and these
 * are the device's, and the two disagreeing is the finding rather than a
 * display fault. */
static int render_axis_table(int y)
{
    int width = page_width();
    unsigned rows = probe_axis_rows(), i;
    int name_w, bar_w, value_w = 56, bar_x, value_x;

    if (!rows)
        return y;
    name_w = min(178, (width - 40) / 2);
    bar_x = 20 + name_w + 10;
    value_x = width - 20 - value_w;
    bar_w = value_x - 10 - bar_x;
    if (bar_w < 50) {
        bar_w = 50;
        value_x = bar_x + bar_w + 10;
    }
    y = muted_line(y, "Raw values, before your mapping. A row that does not "
                      "move is an axis the controller is not writing.",
                   IDC_AXIS_TABLE_HEAD);
    y += 2;
    for (i = 0; i < rows; i++, y += AXIS_ROW_PITCH) {
        const char *name = input_probe_axis_name(i);
        const char *label = input_probe_axis_label(i);
        char text[96];

        /* THE DRIVER'S NAME, WHERE THERE IS ONE. WinMM publishes none at all,
         * so those rows carry the ini name alone rather than a label this
         * launcher invented for them. */
        if (label && label[0])
            snprintf(text, sizeof text, "%s  -  %s", name ? name : "?", label);
        else
            snprintf(text, sizeof text, "%s", name ? name : "?");
        make_control(0, "STATIC", text, SS_LEFT | SS_ENDELLIPSIS, 38, y + 2,
                     name_w - 18, 18, launcher_page(), IDC_AXIS_NAME_BASE + (int)i);
        make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, bar_x, y, bar_w, 16,
                     launcher_page(), IDC_AXIS_BAR_BASE + (int)i);
        SendDlgItemMessage(launcher_page(), IDC_AXIS_BAR_BASE + (int)i,
                           PBM_SETRANGE32, 0, 1000);
        make_control(0, "STATIC", "", SS_LEFT, value_x, y + 2, value_w, 18,
                     launcher_page(), IDC_AXIS_VALUE_BASE + (int)i);
    }
    make_control(0, "STATIC", "Buttons down: none", SS_LEFT | SS_ENDELLIPSIS,
                 38, y + 4, width - 58, 20, launcher_page(), IDC_LIVE_BUTTONS);
    return y + 28;
}

/* A machine with a pad and no force feedback wheel: the tick picks the pad,
 * and what it gets is the rumble. */
static int devices_pad_only(void)
{
    unsigned i;
    int pad = 0;

    for (i = 0; i < input_probe_count(); i++) {
        const input_probe_device_t *d = input_probe_device(i);

        if (!d || !device_listed(i))
            continue;
        if (d->backend == INPUT_PROBE_DINPUT && d->force_feedback)
            return 0;
        if (d->backend == INPUT_PROBE_XINPUT)
            pad = 1;
    }
    return pad;
}

/* Why nothing, or something unexpected, is ticked, when the file is the
 * reason: no controller at all, or a named one that is not attached. Empty
 * otherwise. */
static void device_problem_text(char *out, size_t out_size)
{
    const char *provider = model_value("analog", "provider");
    const char *name = model_value("analog", "dinput_name");

    out[0] = '\0';
    if (!_stricmp(provider, "none"))
        snprintf(out, out_size, "No controller is in use. Select one to use "
                 "it.");
    else if (*name && (!_stricmp(provider, "auto") ||
                       !_stricmp(provider, "dinput")) &&
             launcher_default_row() < 0)
        snprintf(out, out_size, "Your wheel, %s, is not plugged in. Plug it "
                 "in and press Rescan.", name);
}

/* A CHECKBOX WHOSE LABEL IS A SENTENCE: long enough to wrap on a narrow
 * page, and a checkbox does not wrap by itself, so multiline and as tall as
 * its text beside the box. Returns the y under it. */
static int sentence_checkbox(int y, const char *text, int id, int checked)
{
    int width = page_width();
    int h = max(CTRL_H, text_block_height(theme_font(), text,
                                          width - 40 - THEME_CHECK_TEXT_X) + 8);

    make_control(0, "BUTTON", text,
                 BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP |
                 WS_GROUP, 20, y, width - 40, h, launcher_page(), id);
    SendDlgItemMessage(launcher_page(), id, BM_SETCHECK,
                       checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return y + h + 8;
}

/* ONE CONTROLLER FOR BOTH GAMES, OR ONE EACH, LAST IN ITS BOX: what a
 * person with one wheel never needs to think about, so it is under the list
 * rather than over it. Unticked is the file with no [<title>.analog] choice
 * in it. Ticked, the game row at the top picks whose controller this card and
 * the analog card show. */
static int render_cabinet_choice(int y)
{
    y = sentence_checkbox(y, "Separate controller per game",
                          IDC_SEPARATE_CONTROLLER, model_controls_split()) - 2;
    if (!model_controls_split())
        return y;
    return muted_line(y, "Pick a game in the row at the top of the page, then "
                         "the controller it uses in the list above.", 0);
}

static int render_device_group(int y)
{
    unsigned i;
    int width = page_width(), first = 1;
    char text[256];

    snprintf(text, sizeof text, "YOUR CONTROLLERS - %s",
             !model_controls_split() ? "All games" :
             model_title_name(model_controls_title()));
    y = group_header(y, text, IDC_PROBE_REFRESH, "Rescan", 92, 1);
    if (!input_probe_count()) {
        y = muted_line(y, "No controller detected. Plug one in and press "
                          "Rescan; the keyboard works either way.", 0);
        return render_cabinet_choice(y) + 6;
    }
    /* What the tick means, above the list it is about. Steering, pedals and
     * buttons can come from any controller; the tick picks the one that gets
     * the force feedback. */
    y = muted_line(y, devices_pad_only() ?
                      "Pick the controller you drive with: it steers and it "
                      "shakes. Buttons and keys work on every controller, "
                      "picked or not." :
                      "Pick the wheel you drive with: it steers and it pushes "
                      "back. Buttons and keys work on every controller, "
                      "picked or not.", 0);
    for (i = 0; i < input_probe_count(); i++) {
        const input_probe_device_t *d = input_probe_device(i);
        HWND radio;

        if (!device_listed(i))
            continue;
        /* ITS OWN GROUP, or an auto radio would join the cabinet tabs above
         * it and clicking a controller would untick the game. */
        radio = make_control(0, "BUTTON", d->name,
            BS_AUTORADIOBUTTON | WS_TABSTOP | (first ? WS_GROUP : 0),
            20, y, width - 40, CTRL_H, launcher_page(), IDC_DEVICE_BASE + (int)i);
        quiet_radio(radio);
        first = 0;
        /* The controller the host will use as the main one, which is not
         * necessarily the one the file pins: the shipped `provider = auto`
         * pins nothing and the host still picks one. Clicking another row
         * pins it. */
        SendMessage(radio, BM_SETCHECK,
                    (int)i == launcher_probe_selected() ? BST_CHECKED : BST_UNCHECKED,
                    0);
        y += CTRL_H - 6;
        device_what(i, text, sizeof text);
        make_control(0, "STATIC", text, SS_LEFT | SS_ENDELLIPSIS,
                     20 + THEME_CHECK_TEXT_X, y, width - 40 - THEME_CHECK_TEXT_X,
                     18, launcher_page(), IDC_DEVICE_CAP_BASE + (int)i);
        y += 22;
    }
    device_problem_text(text, sizeof text);
    y = *text ? muted_line(y + 2, text, 0) : y + 2;
    y = sentence_checkbox(y, "Also list my controllers the old way (only "
                             "needed if yours is missing from the list above)",
                          IDC_SHOW_LEGACY, launcher_show_legacy());
    return render_cabinet_choice(y) - 4;
}

/* The line under the three bars when nothing more urgent holds it. */
const char k_driving_hint[] =
    "Move your controls and watch the bars follow. Steering rests in the "
    "middle, so its bar starts half full.";

/* Set up's button, whose words follow what pressing it would do. */
const char *setup_button_text(void)
{
    return capture_wizard_running() ? "Skip step" : "Set up...";
}

static int render_driving_group(int y)
{
    static const char *const k_role[3] = { "Steering", "Accelerator", "Brake" };
    const input_probe_device_t *d = launcher_probe_selected() >= 0 ?
        input_probe_device((unsigned)launcher_probe_selected()) : NULL;
    char head[96];
    int width = page_width();
    int right = width - 20;
    int label_w = min(104, max(70, (width - 40) / 3));
    int src_x = 20 + label_w + 8;
    int src_w = min(120, max(60, (right - src_x) / 3));
    int bar_x = src_x + src_w + 8;
    int bar_w = right - bar_x;
    unsigned i;

    /* SET UP ONLY WHERE IT CAN RUN. It measures a wheel's axes; a gamepad's
     * are fixed by its API and already mapped, and a Set up button that only
     * beeps reads as broken. */
    snprintf(head, sizeof head, "ANALOG CONTROLS - %s",
             !model_controls_split() ? "All games" :
             model_title_name(model_controls_title()));
    y = group_header(y, head,
                     d && d->backend == INPUT_PROBE_DINPUT ? IDC_SETUP_AXES : 0,
                     setup_button_text(), 108, 1);
    if (!d)
        return muted_line(y, "No controller is selected, so nothing drives "
                             "the car but the keyboard.", 0) + 6;
    if (bar_w < 60) {           /* a page too narrow for three columns drops */
        src_w = 0;              /* the middle one, which is the least urgent */
        bar_x = src_x;
        bar_w = right - bar_x;
    }
    for (i = 0; i < 3; i++) {
        make_control(0, "STATIC", k_role[i], SS_LEFT, 20, y + 2, label_w, 20,
                     launcher_page(), 0);
        if (src_w)
            make_control(0, "STATIC", "", SS_LEFT | SS_ENDELLIPSIS, src_x,
                         y + 2, src_w, 20, launcher_page(),
                         IDC_LIVE_ROLE_BASE + (int)i);
        make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, bar_x, y, bar_w, 18,
                     launcher_page(), IDC_LIVE_STEER + (int)i);
        SendDlgItemMessage(launcher_page(), IDC_LIVE_STEER + (int)i, PBM_SETRANGE32,
                           0, 1000);
        /* STEERING'S CENTRE, MARKED: at rest its bar is half full, which
         * otherwise reads as something held down. Made after the bar, so it
         * stays on top of every repaint (WS_CLIPSIBLINGS). */
        if (i == 0)
            make_control(0, "STATIC", "",
                         theme_dark() ? SS_WHITERECT : SS_BLACKRECT,
                         bar_x + bar_w / 2 - 1, y - 3, 2, 24, launcher_page(), 0);
        y += 26;
    }
    /* Two controls, not one string with a "\r\n": text_block_height() says
     * why. place_note_lines() sizes and stacks them. */
    {
        HFONT font = theme_note_font();
        const char *first = capture_note() ? capture_note() :
                            s_note_last[0] ? s_note_last :
                            k_driving_hint;
        HWND line;

        s_note_w = width - 40;
        s_note_text_h = note_text_height(first, capture_note_say());
        line = make_control(0, "STATIC", first, SS_LEFT, 20, y + NOTE_GAP,
                            s_note_w, 0, launcher_page(), IDC_LIVE_NOTE);
        SendMessage(line, WM_SETFONT, (WPARAM)font, TRUE);
        line = make_control(0, "STATIC", capture_note_say(), SS_LEFT, 20,
                            y + NOTE_GAP, s_note_w, 0, launcher_page(), IDC_LIVE_SAY);
        SendMessage(line, WM_SETFONT, (WPARAM)font, TRUE);
        place_note_lines();
        y += NOTE_GAP + s_note_text_h + 6;
    }
    /* THE RAW TABLE, UNDER THE BARS IT EXPLAINS: the question it answers is
     * "why does this bar not move", and this is where that is asked. */
    y = sentence_checkbox(y, "Bars not moving the way you expect? Show "
                             "everything this controller sends, before any "
                             "mapping", IDC_SHOW_RAW, launcher_show_raw()) - 4;
    if (launcher_show_raw())
        y = render_axis_table(y);
    return y + 4;
}

/* THE ORDER THE BUTTON LIST IS READ IN: what you drive with first, the
 * operator's buttons last. Display only; bindings.def keeps the order every
 * other consumer, the ini and the control ids follow. An action in no group
 * is still shown, under "Other", so a new row in bindings.def cannot vanish. */
static const struct {
    const char *head;
    const char *keys[11];
} k_button_groups[] = {
    { "Driving", { "steer_left", "steer_right", "throttle", "brake", "boost",
                   "shift_up", "shift_down", "shift1", "shift2", "shift3",
                   NULL } },
    { "Camera", { "view_high", "view_low", "view_pilot", NULL } },
    { "Select screens", { "menu_prev", "menu_next", NULL } },
    { "Starting a game", { "coin1", "start1", NULL } },
    { "Arcade cabinet", { "coin2", "service1", "service_mode", "volume_down",
                          "volume_up", NULL } },
};
#define BUTTON_GROUP_COUNT (sizeof k_button_groups / sizeof k_button_groups[0])

/* `order` gets every visible action once, grouped; `head[k]` is the heading
 * to draw before order[k], or NULL. A group with nothing visible gets no
 * heading. Returns how many entries were written. */
static unsigned button_order(unsigned order[ACTION_COUNT],
                             const char *head[ACTION_COUNT])
{
    unsigned char placed[ACTION_COUNT] = { 0 };
    const char *pending;
    unsigned n = 0, g, j, a;

    for (g = 0; g <= BUTTON_GROUP_COUNT; g++) {
        pending = g < BUTTON_GROUP_COUNT ? k_button_groups[g].head : "Other";
        for (j = 0; j < ACTION_COUNT; j++) {
            if (g < BUTTON_GROUP_COUNT) {
                const char *key = k_button_groups[g].keys[j];

                if (!key)
                    break;
                for (a = 0; a < ACTION_COUNT && strcmp(k_actions[a].key, key);
                     a++)
                    ;
            } else {
                a = j;          /* "Other": whatever no group named */
            }
            if (a >= ACTION_COUNT || placed[a] || !action_visible(a))
                continue;
            placed[a] = 1;
            head[n] = pending;
            pending = NULL;
            order[n++] = a;
        }
    }
    return n;
}

static int render_button_group(int y)
{
    int width = page_width();
    int right = width - 20;
    /* Below this the label gets a line of its own and the slots the second. */
    int stacked = width < 470;
    int label_w = min(132, max(90, (width - 40) / 3));
    int value_x, slot_w, reset_w;
    unsigned i, k, s, shown, order[ACTION_COUNT];
    const char *heads[ACTION_COUNT];
    char head[64];

    value_x = stacked ? 20 : 20 + label_w + 8;
    /* A slot is a control, not a line: wide enough for `thrustmaster button
     * 12`, and no wider however wide the page is. */
    slot_w = min(240, max((right - value_x - 6 * (BINDING_SLOTS - 1)) /
                          BINDING_SLOTS, 60));
    snprintf(head, sizeof head, "BUTTONS - %s",
             model_title_name(model_controls_title()));
    /* WHICH CABINET'S MAP IS BELOW is the game row's question, and only the
     * game row answers it: one place to switch, not two. */
    y = group_header(y, head, 0, NULL, 0, 1);
    /* ONE CABINET AT A TIME, as the file holds them: six of the fifteen
     * shared actions have a different default on each, and Offroad has five
     * controls Hydro does not have at all. */
    y = muted_line(y, "Click a slot, then press the key or button for it, on "
                      "any controller. Delete empties the slot; Backspace "
                      "puts back its default.", 0);
    shown = button_order(order, heads);
    for (k = 0; k < shown; k++) {
        const action_desc_t *a = &k_actions[order[k]];
        const char *label;
        int row_y;

        i = order[k];
        if (heads[k]) {
            HWND sub = make_control(0, "STATIC", heads[k], SS_LEFT,
                                    20, y + 6, width - 40, 20, launcher_page(), 0);
            SendMessage(sub, WM_SETFONT, (WPARAM)theme_note_font(), TRUE);
            y += 30;
        }
        row_y = stacked ? y + 22 : y;
        label = a->label[model_controls_title()];
        make_control(0, "STATIC", label ? label : a->key,
                     SS_LEFT | SS_ENDELLIPSIS, 20, stacked ? y + 4 : y + LABEL_DY,
                     stacked ? width - 40 : label_w, 20, launcher_page(), 0);
        /* Created captionless; update_binding_display() writes each slot's
         * text, the one place it is decided. */
        for (s = 0; s < BINDING_SLOTS; s++)
            make_control(0, "BUTTON", "", BS_PUSHBUTTON | WS_TABSTOP,
                         value_x + (int)s * (slot_w + 6), row_y, slot_w, CTRL_H,
                         launcher_page(),
                         IDC_BIND_SLOT_BASE + (int)(i * BINDING_SLOTS + s));
        update_binding_display(i);
        y += stacked ? 22 + CTRL_H + 6 : CTRL_H + 4;
    }
    reset_w = min(160, width - 40);
    make_control(0, "BUTTON", "Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP,
                 value_x, y + 8, reset_w, CTRL_H, launcher_page(), IDC_BIND_RESET);
    update_reset_button();
    return y + 8 + CTRL_H + 6;
}

/* THE GAME ROW: one row of tabs, one variable, above everything it
 * governs. Buttons always follow it; the controller cards only while each
 * game has its own, and their headings say which. */
static int render_game_row(int y)
{
    static const int ids[MODEL_TITLES] = {
        IDC_TITLE_HYDRO, IDC_TITLE_OFFROAD, IDC_TITLE_ARCTIC };
    static const char *const k_short[MODEL_TITLES] = {
        "Hydro", "Offroad", "Arctic" };
    const char *labels[MODEL_TITLES];
    /* A tab narrower than "Offroad Thunder" clips it, so the narrow window
     * names the games by their first word. */
    int narrow = (page_width() - 44) / MODEL_TITLES < 140;
    int t;

    for (t = 0; t < MODEL_TITLES; t++)
        labels[t] = narrow ? k_short[t] : model_title_name(t);
    /* No "Game:" label: the names say what the row is. */
    return add_tab_row_at(20, y, CTRL_H, tab_rows_width(), labels, ids,
                          MODEL_TITLES);
}

int render_controls_panel(void)
{
    int top = page_header_bottom(), y;

    /* THE GAME ROW IS A CARD OF ITS OWN, 8 of air above and below the tabs,
     * then three topics, three cards. The disclosure below them is not one:
     * it is the door to the rest of this page, not a thing on it. */
    y = frame_topic(top, render_game_row(top) + 4);
    top = y;
    y = frame_topic(top, render_device_group(y));
    top = y;
    y = frame_topic(top, render_driving_group(y));
    top = y;
    y = frame_topic(top, render_button_group(y));
    /* The settings rows' box comes next, the same gap under the last card. */
    return y;
}

/* WHAT THE GAME WILL ACTUALLY DO WITH THIS CONTROLLER, which is not what the
 * Test button proves. Test drives the DEVICE: it commands a DirectInput effect
 * or an XInput vibration and says nothing about whether the host will ever ask
 * for one, so a pad with `[ffb] rumble = false` buzzes on the bench and stays
 * dead in the race.
 *
 * The three cases are the three sinks. `[ffb] enabled` gates the wheel's motor
 * (dinput.c); `[ffb] rumble` gates the derived pad model, whose only sink is
 * XInput (io_host_rumble); a controller that is neither an XInput device nor a
 * DirectInput one publishing effects has nowhere for either to go.
 *
 * It reads the MODEL, so it answers to what the page is showing rather than to
 * what the file last said, and it follows the selected controller for the same
 * reason the bars above do. */
const char *ffb_reaches_device(const input_probe_device_t *d)
{
    if (!d)
        return NULL;
    if (d->backend == INPUT_PROBE_XINPUT)
        return model_bool("ffb", "rumble") ? NULL :
            "Gamepad rumble below is off, so the game will not drive this "
            "pad's motors, Test only proves the motors work.";
    if (d->backend == INPUT_PROBE_DINPUT && d->force_feedback)
        return model_bool("ffb", "enabled") ? NULL :
            "Enable force feedback below is off, so the game will not command "
            "this wheel; Test only proves the motor works.";
    return "This is not an XInput pad and it publishes no force-feedback "
           "effects, so neither the wheel force nor the derived rumble has "
           "anywhere to go on it.";
}

int render_ffb_panel(void)
{
    const input_probe_device_t *d = launcher_probe_selected() >= 0 ?
        input_probe_device((unsigned)launcher_probe_selected()) : NULL;
    int y = page_header_bottom(), top = y;
    const char *gap = ffb_reaches_device(d);
    char label[MAX_PATH + 48];

    /* THE SAME GAME ROW AS THE CONTROLS PAGE, one variable: every row below
     * is the shown game's, [<title>.ffb] over [ffb]. */
    y = frame_topic(top, render_game_row(top) + 4);
    top = y;
    snprintf(label, sizeof label, "FORCE FEEDBACK - %s",
             d ? probe_product_name(d) : "no controller selected");
    y = group_header(y, label, IDC_FFB_TEST, "Test", 108,
                     d && d->force_feedback);
    if (gap)
        y = muted_line(y, gap, 0);
    return frame_topic(top, y);
}

/* How tall the note and the line under it are together, at `s_note_w`. */
static int note_text_height(const char *first, const char *second)
{
    HFONT font = theme_note_font();
    int h = text_block_height(font, first ? first : "", s_note_w);

    if (second && second[0])
        h += 2 + text_block_height(font, second, s_note_w);
    return h;
}

/* Each line as tall as its text, line 2 directly under line 1, whose top
 * never moves. Text needing a different height than was reserved (Set up
 * starting or ending) rebuilds the page. */
static void place_note_lines(void)
{
    HWND one, two;
    RECT r1, r2;
    char t1[512], t2[256];
    int h1, h2, y2;

    if (!launcher_note_visible())
        return;
    one = GetDlgItem(launcher_page(), IDC_LIVE_NOTE);
    two = GetDlgItem(launcher_page(), IDC_LIVE_SAY);
    if (!one || !two)
        return;
    GetWindowRect(one, &r1);
    GetWindowRect(two, &r2);
    MapWindowPoints(NULL, launcher_page(), (POINT *)&r1, 2);
    MapWindowPoints(NULL, launcher_page(), (POINT *)&r2, 2);
    GetWindowTextA(one, t1, sizeof t1);
    GetWindowTextA(two, t2, sizeof t2);
    if (note_text_height(t1, t2) != s_note_text_h)
        request_relayout();
    h1 = scale_px(text_block_height(theme_note_font(), t1, s_note_w));
    h2 = t2[0] ? scale_px(text_block_height(theme_note_font(), t2, s_note_w))
               : 0;
    y2 = r1.top + h1 + (h2 ? scale_px(2) : 0);
    if (r1.bottom - r1.top != h1)
        SetWindowPos(one, NULL, r1.left, r1.top, r1.right - r1.left, h1,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    if (r2.top != y2 || r2.bottom - r2.top != h2)
        SetWindowPos(two, NULL, r2.left, y2, r2.right - r2.left, h2,
                     SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Re-placed only when a text changed: the idle hint is rewritten at 30 Hz. */
void launcher_note_lines(const char *first, const char *second)
{
    char now1[512], now2[256];

    if (!launcher_note_visible())
        return;
    first = first ? first : "";
    second = second ? second : "";
    GetDlgItemTextA(launcher_page(), IDC_LIVE_NOTE, now1, sizeof now1);
    GetDlgItemTextA(launcher_page(), IDC_LIVE_SAY, now2, sizeof now2);
    snprintf(s_note_last, sizeof s_note_last, "%s", first);
    if (!strcmp(now1, first) && !strcmp(now2, second))
        return;
    SetDlgItemTextA(launcher_page(), IDC_LIVE_NOTE, first);
    SetDlgItemTextA(launcher_page(), IDC_LIVE_SAY, second);
    place_note_lines();
}
