/* launcher.c -- native settings launcher.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * What `VCThunder.exe` with no title argument opens: every shipped ini key,
 * per-title bindings, and non-exclusive live device and axis testing with a
 * bounded force-feedback test. Plain Win32, no dialog resources; pages are built
 * from src/settings.def and src/bindings.def, so a key or control added there
 * appears here with no edit to this file. This half is the VIEW only: catalogue,
 * value grammar, document, model, capture and palette are each another
 * file's. */
#include "vcthunder.h"
#include "resource.h"
#include "io.h"
#include "settings.h"
#include "ini.h"
#include "ini_doc.h"
#include "launcher_model.h"
#include "launcher_capture.h"
#include "launcher_theme.h"
#include "di_role.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NAV_LAUNCH,
    NAV_GRAPHICS,
    NAV_AUDIO,
    NAV_CONTROLS,
    NAV_SYSTEM,
    NAV_LINK,
    NAV_FILES,
    NAV_TROUBLE,
    NAV_COUNT
};

/* s_active_key / s_active_pad are one bit per action. */
typedef char action_count_fits_a_mask[ACTION_COUNT <= 32 ? 1 : -1];

enum {
    IDC_NAV = 100,
    IDC_STATUS,
    IDC_APPLY,
    IDC_CLOSE,
    IDC_PAGE,
    IDC_PLAY_HYDRO = 200,
    IDC_PLAY_OFFROAD,
    /* TWO TABS, NOT THREE. Bindings and Devices & Axes were one job split by
     * mechanism; they are one page now. */
    IDC_CONTROL_MAIN = 210,
    IDC_CONTROL_FFB,
    IDC_TITLE_HYDRO = 220,
    IDC_TITLE_OFFROAD,
    IDC_PROBE_REFRESH = 230,
    IDC_FFB_TEST,
    /* The three disclosures. Every one of them is a checkbox whose state is
     * this window's and is never written to the file: what a user chose to
     * LOOK at is not configuration. */
    IDC_SHOW_RAW,
    IDC_SHOW_ADVANCED,
    IDC_SHOW_LEGACY,
    IDC_SETUP_AXES,
    /* The known-good copy of the whole ini: one button to make one, one to go
     * back to it. See known_good_path(). */
    IDC_GOOD_SAVE,
    IDC_GOOD_REVERT,
    IDC_LIVE_STEER = 240,
    IDC_LIVE_THROTTLE,
    IDC_LIVE_BRAKE,
    /* The line under the three bars: the hint, the missing-axis sentence, or
     * the step Set up is asking for. Directly under the thing it is about, and
     * not on the status bar, which is where three of this project's own
     * answers went unread. */
    IDC_LIVE_NOTE,
    /* The raw disclosure's own read-out: which buttons are down, and the hat. */
    IDC_LIVE_BUTTONS,
    /* The second line under the bars: what the axis guard is measuring. */
    IDC_LIVE_SAY,
    IDC_AXIS_TABLE_HEAD = 250,
    /* The live control table, three controls per row: the ini's name for the
     * axis beside the driver's, a bar, and the value. One range each, so a row
     * is addressable without asking what a control is. */
    IDC_AXIS_NAME_BASE = 260,
    IDC_AXIS_BAR_BASE = 280,
    IDC_AXIS_VALUE_BASE = 300,
    /* One controller per row: the radio that says which one drives the car,
     * and the muted line under it that says what the thing is and whether it
     * is moving right now. INPUT_PROBE_MAX_DEVICES is 24. */
    IDC_DEVICE_BASE = 320,
    IDC_DEVICE_CAP_BASE = 350,
    IDC_LIVE_ROLE_BASE = 380,       /* which axis each of the three is on   */
    IDC_SETTING_BASE = 1000,
    IDC_BROWSE_BASE = 2000,
    IDC_DETECT_BASE = 3000,
    /* ONE ROW PER ACTION, NOT TWO COLUMNS: the keyboard and controller halves
     * share one value column, `LCTRL or thrustmaster button 1`, because
     * which KIND of thing you press is not a question to answer first. */
    IDC_BIND_TEXT_BASE = 4000,
    /* TWO BUTTONS, ONE MEANING EACH. A button that REPLACES and one that ADDS
     * are two ways to say the same thing; the replace case is Clear then Add.
     * Both halves are lists, so `Add` means the same to a key as to a
     * button. */
    IDC_BIND_ADD_BASE = 4100,
    IDC_BIND_CLEAR_BASE = 4200,
    /* The per-setting help caption. Its own range so control_colour() can mute
     * it without asking what a static's text says. */
    IDC_HELP_BASE = 5000
};

static const char *const k_nav_name[] = {
    "Launch", "Graphics", "Audio", "Controls", "Game System", "Link Play",
    "Files & Logging", "Troubleshooting"
};

static const int k_nav_page[] = {
    PAGE_HOME, PAGE_GRAPHICS, PAGE_AUDIO, PAGE_CONTROLS, PAGE_SYSTEM,
    PAGE_LINK, PAGE_FILES, PAGE_TROUBLE
};

static const char *const k_page_intro[] = {
    "Choose a title. Settings are saved before the selected game starts.",
    "Display, renderer, and cabinet-raster settings shared by both titles.",
    "Host audio and the cabinets' front/seat bus routing.",
    "Every controller you plug in works here. Steering, pedals and buttons "
    "can each come from a different controller.",
    "The game supplies the motor force. The launcher never commands a test force.",
    "Cabinet identity, timing, DIP switches, and input sampling.",
    "The transport the host puts under the games' own link protocol.",
    "Paths and the two runtime logs. Paths are relative to VCThunder.exe.",
    "Recovering settings that worked, and the capture and diagnostic keys."
};

/* All three are indexed by their enum, and a page added to one and not the
 * others reads off the end of it. */
typedef char page_table_check[
    sizeof k_page_intro / sizeof k_page_intro[0] == PAGE_COUNT &&
    sizeof k_nav_name / sizeof k_nav_name[0] == NAV_COUNT &&
    sizeof k_nav_page / sizeof k_nav_page[0] == NAV_COUNT ? 1 : -1];

static HWND s_window, s_nav, s_page, s_status, s_apply, s_close;
static int s_compact, s_tiny;
static int s_page_index = PAGE_HOME;
static int s_binding_title;
/* WHICH BINDING ROWS ARE LIT RIGHT NOW, one bit per action. A wheel's buttons
 * are numbered and unlabelled, so `Button 9` says nothing about which thing is
 * under a hand; pressing it lights its row, with no mode to enter. Bitmasks
 * rather than per-row flags, so a tick that changes nothing costs one
 * comparison at 33 ms. */
static uint32_t s_active_key, s_active_pad;
static int s_dirty, s_loading, s_page_scroll, s_content_height;
/* The page's width when its children were last built, and whether a rebuild is
 * queued. A resize INSIDE one layout mode moves the page; without a rebuild its
 * children keep their creation coordinates, so anything laid out to the right
 * edge ends up outside the page and unreachable, it scrolls vertically only. */
static int s_rendered_page_w, s_relayout_pending;
/* THE LINES UNDER THE LIVE BARS and the raw-view checkbox: the note and Set
 * up's line 2, reserved at exactly the height of what is showing. The render
 * and place_note_lines() measure at the same stored width and text, or they
 * could disagree about a wrap and rebuild forever. */
#define NOTE_GAP 12             /* from the control above to the note      */
static int s_note_text_h, s_note_w;
static char s_note_last[512];   /* the last note shown, redrawn by a rebuild */
/* WHICH PAGE THE CHILDREN ON SCREEN BELONG TO, so a rebuild can tell "the same
 * page again" from "a different page". -1 until the first render. */
static int s_rendered_page = -1;
static launcher_result_t s_result = LAUNCHER_CANCEL;

static int s_probe_selected = -1;
static input_probe_state_t s_probe_state;
/* WHAT THE USER CHOSE TO LOOK AT, never written to the file. One flag per
 * page for Advanced, because Controls and Force feedback are different
 * questions and a user who opened one has not asked about the other. */
/* ONE SWITCH, NOT ONE PER PAGE: it is a way of looking at the whole file, not
 * a property of a page. It lives on Troubleshooting, which is the page somebody
 * opens when they need the keys it reveals. */
static int s_advanced;
static int s_show_raw;              /* every axis the controller reports    */
static int s_show_legacy;           /* the WinMM view of devices already listed */
/* Where the hand-built panel ended, in logical units, so settings rows start
 * under what the panel actually needed rather than under a constant somebody
 * has to remember. Safe as a static: the panel is always rendered before the
 * rows, inside one render_page(). */
static int s_panel_bottom;
/* WHEN EACH CONTROLLER LAST MOVED, so a row can say `moving` and a user can
 * find which of three identically named things is under their hand. A tick
 * count rather than a flag, so the mark lingers long enough to be seen. */
static DWORD s_dev_active[INPUT_PROBE_MAX_DEVICES];
static uint32_t s_dev_buttons[INPUT_PROBE_MAX_DEVICES][4];
static int s_dev_seen[INPUT_PROBE_MAX_DEVICES];
#define DEVICE_ACTIVE_MS 700u
static void settings_help_cache_flush(void);
static int probe_row_for_label(const char *label);
static void derive_label(const char *name, const char *provider,
                         char *out, size_t out_size);



/* WHICH TAB IS LIT: 1 selected, 0 unselected, -1 "not a tab". The row that
 * builds the tabs and the owner-draw that paints them both ask this, so the lit
 * tab cannot disagree with the subpage shown. */
static int tab_is_selected(int id)
{
    switch (id) {
    case IDC_CONTROL_MAIN:  return s_page_index == PAGE_CONTROLS;
    case IDC_CONTROL_FFB:   return s_page_index == PAGE_FFB;
    case IDC_TITLE_HYDRO:   return !s_binding_title;
    case IDC_TITLE_OFFROAD: return s_binding_title != 0;
    default:                return -1;
    }
}


/* WHICH BUTTON THIS IS, for the palette that has to paint it. The colour is
 * launcher_theme.c's; which of the five kinds a control id is, is this file's,
 * and that is the whole of the boundary between them. */
theme_button_t launcher_button_kind(int id)
{
    int tab = tab_is_selected(id);

    if (tab >= 0)
        return tab ? THEME_BUTTON_TAB_LIT : THEME_BUTTON_TAB;
    if (id == IDC_PLAY_HYDRO)
        return THEME_BUTTON_HYDRO;
    if (id == IDC_PLAY_OFFROAD)
        return THEME_BUTTON_OFFROAD;
    return THEME_BUTTON_PLAIN;
}

/* A caption is subordinate to the label above it, and captions have their own
 * id range for exactly this: so a static can be muted without asking what its
 * text says. */
int launcher_control_is_caption(int id)
{
    return id >= IDC_HELP_BASE;
}

/* Set up's line 2: red keep still, green press, amber watching, blue done. */
theme_tone_t launcher_control_tone(int id)
{
    if (id != IDC_LIVE_SAY)
        return THEME_TONE_NONE;
    switch (capture_note_state()) {
    case SAY_WAIT:  return THEME_TONE_RED;
    case SAY_GO:    return THEME_TONE_GREEN;
    case SAY_SEEN:
    case SAY_AGAIN: return THEME_TONE_AMBER;
    case SAY_DONE:  return THEME_TONE_BLUE;
    default:        return THEME_TONE_NONE;
    }
}

static int nav_selection(void)
{
    if (!s_nav)
        return -1;
    return (int)SendMessage(s_nav, s_compact ? CB_GETCURSEL : LB_GETCURSEL,
                            0, 0);
}

static int page_is_controls(int page)
{
    return page == PAGE_CONTROLS || page == PAGE_FFB;
}

static int page_to_nav(int page)
{
    unsigned i;
    if (page_is_controls(page))
        return NAV_CONTROLS;
    for (i = 0; i < NAV_COUNT; i++)
        if (k_nav_page[i] == page)
            return (int)i;
    return NAV_LAUNCH;
}

/* THE NAME OF A PAGE, AND THE ONLY PLACE ONE IS WRITTEN DOWN: the nav list's
 * own label, so a page renamed in the list is renamed everywhere. Force
 * feedback is the exception, being a subpage with no nav row. */
static const char *page_name(int page)
{
    int nav;

    if (page == PAGE_FFB)
        return "Force Feedback";
    nav = page_to_nav(page);
    return nav >= 0 && nav < NAV_COUNT ? k_nav_name[nav] : "";
}

/* TWO RULES GOVERN EVERY GEOMETRY CONSTANT BELOW. A constant is only ever a
 * MAXIMUM, `min(constant, what the page has)`, because the two-column form
 * begins at a 760-unit window and leaves the page about 500. And the page is
 * rebuilt whenever its WIDTH changes, not only when the layout mode does. */

/* WHICH PAGE IS OPEN, IN THE TITLE BAR. The prefix is left alone: automation
 * finds this window by a title that starts with "VCThunder". */
static void update_window_title(void)
{
    char title[128];

    if (!s_window)
        return;
    snprintf(title, sizeof title, "VCThunder " VCT_VERSION " - %s",
             page_name(s_page_index));
    SetWindowTextA(s_window, title);
}

static void create_navigation(HWND parent)
{
    unsigned i;
    if (s_nav)
        DestroyWindow(s_nav);
    if (s_compact) {
        s_nav = make_control(0, "COMBOBOX", "", CBS_DROPDOWNLIST |
            WS_VSCROLL | WS_TABSTOP, 12, 34, 460, 240, parent, IDC_NAV);
        for (i = 0; i < NAV_COUNT; i++)
            SendMessageA(s_nav, CB_ADDSTRING, 0, (LPARAM)k_nav_name[i]);
        SendMessage(s_nav, CB_SETCURSEL, page_to_nav(s_page_index), 0);
    } else {
        s_nav = make_control(WS_EX_CLIENTEDGE, "LISTBOX", "",
            LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 12, 44, 204, 560,
            parent, IDC_NAV);
        for (i = 0; i < NAV_COUNT; i++)
            SendMessageA(s_nav, LB_ADDSTRING, 0, (LPARAM)k_nav_name[i]);
        SendMessage(s_nav, LB_SETCURSEL, page_to_nav(s_page_index), 0);
    }
}

void set_status(const char *text)
{
    if (s_status)
        SetWindowTextA(s_status, text ? text : "");
}

/* Set when the configured device has just CHANGED, and consumed by
 * render_page() after its own pull_page(). See adopt_probe_device(). */
static int  s_reload_devices;

/* HOW A CONTROLLER BUTTON READS, AND IT ALWAYS SAYS WHICH CONTROLLER.
 * `thrustmaster button 1`, `rumblepad button 3`, never a bare `Button 1`: a
 * binding made on one controller and read back while another was selected
 * would otherwise show the same text, a number about an unnamed thing.
 *
 * An unlabelled reference means the controller that drives the car, named with
 * the label [devices] gives it. Before anything has been adopted there is no
 * label and it reads `Controller button 1`, the only case where the row cannot
 * name the thing.
 *
 * Returns the number of characters written, 0 for a value that binds nothing,
 * so a caller can join it to the keyboard half without testing for a word.
 *
 * ONE COPY, not a strtoul() and a ternary at each site: strtoul("th8:1") is 0,
 * so a copy would draw a device-qualified binding as "Unbound" while the host
 * reads it perfectly. */
/* WHAT TO CALL THE CONTROLLER THAT DRIVES THE CAR. Its [devices] label once it
 * has one, and otherwise the label one would be derived from, the brand, out
 * of the product name. Never "Controller", which is a word about a category
 * where the row wants a word about a thing. */
static const char *primary_label(void)
{
    static char out[INI_LABEL_CAP];
    const input_probe_device_t *d = s_probe_selected >= 0 ?
        input_probe_device((unsigned)s_probe_selected) : NULL;

    if (model_profile()[0])
        return model_profile();
    if (!d)
        return "controller";
    derive_label(probe_product_name(d), "controller", out, sizeof out);
    return out;
}

/* IS THERE A CONTROLLER ATTACHED THAT THIS `[devices]` LABEL NAMES?
 *
 * `bind_one()` (io_config.c) warns once per binding that the label resolves to
 * nothing, and its census renders the reference as `thrustmaster:7(absent)` so
 * that a warning is not followed by a line that reads as confirmation. This
 * page must not be that line: rendering the binding as `thrustmaster button 7`
 * in the same grey as one that works reads as confirmation with the device
 * unplugged and two WARNs already in the log about it.
 *
 * A binding is a LIST, so the answer to an absent half is to ADD the button you
 * do have rather than lose the one you will plug back in, which the row can
 * only suggest once it admits which half is dead. */
static int label_is_attached(const char *label)
{
    /* No label is the primary controller and is not this question. */
    if (!label || !label[0])
        return 1;
    return probe_row_for_label(label) >= 0;
}

size_t pad_binding_render(const char *value, char *out, size_t out_size)
{
    char item[64], label[INI_LABEL_CAP];
    unsigned i, ordinal;
    size_t at = 0;

    out[0] = '\0';
    for (i = 0; ini_list_item(value, i, item, sizeof item); i++) {
        const char *who;
        int w;

        if (!ini_parse_pad_ref(item, label, sizeof label, &ordinal) || !ordinal)
            continue;
        who = label[0] ? label : primary_label();
        w = snprintf(out + at, out_size - at, "%s%s button %u%s",
                     at ? " or " : "", who, ordinal,
                     label_is_attached(label) ? "" : " (not connected)");
        if (w < 0 || (size_t)w >= out_size - at)
            break;
        at += (size_t)w;
    }
    return at;
}

void pad_binding_text(const char *value, char *out, size_t out_size)
{
    if (!pad_binding_render(value, out, out_size))
        snprintf(out, out_size, "Unbound");
}

/* ONE ROW, BOTH HALVES. `LCTRL or thrustmaster button 1`. The keyboard half is
 * the ini's own name for the key and is deliberately not prettified: a binding
 * is a name the user can read and type, and what is on screen is what is in
 * the file. */
static void binding_row_text(int title, unsigned action, char *out,
                             size_t out_size)
{
    const action_desc_t *a = &k_actions[action];
    const char *key = (title ? a->offroad_key : a->hydro_key) ?
                      model_key_binding(title, action) : NULL;
    size_t at = 0;

    out[0] = '\0';
    if (key && *key) {
        char item[64];
        unsigned e;

        for (e = 0; ini_list_item(key, e, item, sizeof item); e++) {
            int w;
            if (!_stricmp(item, "NONE"))
                continue;
            w = snprintf(out + at, out_size - at, "%s%s", at ? " or " : "",
                         item);
            if (w < 0 || (size_t)w >= out_size - at)
                break;
            at += (size_t)w;
        }
    }
    if (a->has_pad && at + 5 < out_size) {
        size_t n;
        if (at)
            at += (size_t)snprintf(out + at, out_size - at, " or ");
        n = pad_binding_render(model_pad_binding(title, action), out + at,
                               out_size - at);
        if (!n && at >= 4)
            out[at - 4] = '\0';         /* the " or " we had just added */
        at += n;
    }
    if (!out[0])
        snprintf(out, out_size, "Unbound");
}

/* The model, plus what this window measured from the old document. The cache
 * is the launcher's own: setting_help() appends the per-title sentence, so a
 * reload that changes which keys a [<title>.section] carries changes how tall
 * a row is, the fourth cache input, and the only one that is not a property
 * of the window. */
static void load_settings(void)
{
    settings_help_cache_flush();
    model_load();
}

static int save_settings(HWND owner)
{
    char why[512];
    int changed = 0, result;

    if (!validate_settings(why, sizeof why, NULL)) {
        MessageBoxA(owner, why, "VCThunder settings", MB_OK | MB_ICONWARNING);
        set_status(why);
        return 0;
    }
    if (model_unsafe_video_mode() &&
        MessageBoxA(owner,
            "Video mode 2 selects 640x480. It is for a bench LCD or emulator "
            "and may damage an original arcade monitor.\n\nSave this value?",
            "Unsafe arcade-monitor mode", MB_YESNO | MB_DEFBUTTON2 |
            MB_ICONWARNING) != IDYES)
        return 0;
    result = model_save(why, sizeof why, &changed);
    if (result != MODEL_SAVE_OK) {
        MessageBoxA(owner, why, "VCThunder settings", MB_OK | MB_ICONERROR);
        /* ini_doc_save()'s reason names a file and a Windows error and is
         * worth keeping on screen after the modal is gone; "not enough
         * memory" is not, and the status bar is where a user looks for the
         * state of their settings. */
        if (result == MODEL_SAVE_FAILED)
            set_status(why);
        return 0;
    }
    s_dirty = 0;
    EnableWindow(s_apply, FALSE);
    if (!changed) {
        /* AND "ALREADY SAYS" IS A CLAIM ABOUT A FILE, so it may only be made
         * about a file that is there. config_seed_ini() writes one before this
         * window opens, so the absent case now means the folder refused it,
         * and reporting "vcthunder.ini already says all of this" about a file
         * that does not exist sent the reader looking for the wrong thing. */
        if (!model_ini_exists())
            set_status("Nothing to save: every setting is still its default, "
                       "and vcthunder.ini could not be created in this "
                       "folder. Move the pack somewhere you can write to.");
        else
            set_status("Nothing to save: vcthunder.ini already says all of "
                       "this.");
        return 1;
    }
    snprintf(why, sizeof why, "Settings saved: %d key%s changed. A backup is "
             "available as vcthunder.ini.bak.", changed,
             changed == 1 ? "" : "s");
    set_status(why);
    return 1;
}

void mark_dirty(void)
{
    char why[256];
    int valid;
    if (s_loading)
        return;
    s_dirty = 1;
    valid = validate_settings(why, sizeof why, NULL);
    if (s_apply)
        EnableWindow(s_apply, valid);
    if (valid)
        set_status("Settings have not been saved.");
    else
        set_status(why);
}

/* REBUILD THE PAGE, ONCE, AFTER THE HANDLER HAS RETURNED. Four controls ask
 * for this from inside a notification of a control the rebuild destroys. It is
 * also why none of them updates a control's text in place: every line involved
 * is prose that WRAPS, so its height is part of what changed, and SetWindowText
 * on a static sized for one line clips the second silently. */
void request_relayout(void)
{
    if (s_window && !s_relayout_pending) {
        s_relayout_pending = 1;
        PostMessage(s_window, WM_APP + 2, 0, 0);
    }
}

/* ---- the known-good copy, the half that speaks ---------------------------
 *
 * WHY THERE IS ONE AT ALL, and what the two buttons are, is in
 * launcher_model.h and the file half in launcher_model.c. What is here is the
 * asking: both of these destroy something a user may want, so both of them
 * confirm, and the confirmation is the only reason these are not one call. */
static void save_known_good(HWND owner)
{
    char why[MAX_PATH + 64];

    /* FROM THE FILE, NOT FROM THIS WINDOW, and the difference is the whole
     * value of the copy: unsaved edits are exactly what a user is about to
     * regret. Saying so beats copying a state they never applied. */
    if (s_dirty) {
        set_status("Apply or discard the unsaved changes first: the known-good "
                   "copy is made from the FILE, not from this window.");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (!model_ini_exists()) {
        set_status("There is no vcthunder.ini to copy yet. Press Apply first.");
        return;
    }
    /* THE BUTTON THAT DESTROYS THE RECOVERY COPY SITS BESIDE THE ONE THAT
     * USES IT, on the page somebody opens because something is broken, so
     * the press that costs you the last working state looks exactly like the
     * press that saves you. It asks. The silent branch below is only for a
     * copy deleted from under the open window: seed_known_good() has made one
     * by the time this page is reachable. */
    if (model_known_good_exists()) {
        char ask[320], when[160];

        known_good_state(when, sizeof when);
        snprintf(ask, sizeof ask,
                 "Replace the known-good copy with the settings as they are "
                 "now?\n\n%s Reverting will come back to what you save here, "
                 "and the copy it has now is not kept.", when);
        if (MessageBoxA(owner, ask, "Replace the known-good copy",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) {
            set_status("The known-good copy was left as it was.");
            return;
        }
    }
    if (model_known_good_write(why, sizeof why))
        set_status("Saved these settings as the known-good copy.");
    else
        set_status(why);
    request_relayout();
}

static void revert_to_known_good(HWND owner)
{
    char why[MAX_PATH + 64];

    if (!model_known_good_exists()) {
        set_status("There is no known-good copy to go back to.");
        return;
    }
    if (MessageBoxA(owner,
            "Replace vcthunder.ini with the known-good copy?\n\n"
            "Everything on every page goes back to what that copy says. The "
            "file as it is now is kept as vcthunder.ini.bak.",
            "Revert to known-good settings",
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
        return;
    if (!model_known_good_restore(why, sizeof why)) {
        MessageBoxA(owner, why, "VCThunder settings", MB_OK | MB_ICONERROR);
        set_status(why);
        return;
    }
    if (!model_reopen()) {
        MessageBoxA(owner, "The known-good copy was restored, but it could "
                    "not be read back. Close the launcher and reopen it.",
                    "VCThunder settings", MB_OK | MB_ICONERROR);
        return;
    }
    s_loading = 1;
    load_settings();
    scan_ini_problems();
    s_loading = 0;
    s_dirty = 0;
    if (s_apply)
        EnableWindow(s_apply, FALSE);
    set_status("Reverted to the known-good copy. The file as it was is "
               "vcthunder.ini.bak.");
    /* EVERY control on every page is showing a value that just changed, so
     * this one does need the full rebuild, deferred, because the button
     * that asked for it is a child of the page being destroyed. */
    request_relayout();
}

/* The nth choice of an `a|b|c` set, into `out`. One walk of that grammar, used
 * by the population below and by pull_page()'s read-back: the item a user
 * picked has to come back as the value the ini spells. 0 when there is none. */
static int choice_at(const char *choices, int index, char *out, size_t size)
{
    const char *p = choices;
    int i = 0;

    while (p && *p) {
        const char *end = strchr(p, '|');
        size_t n = end ? (size_t)(end - p) : strlen(p);

        if (i == index) {
            if (n >= size)
                n = size - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return 1;
        }
        i++;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

/* Whether device `row` reports an axis by this ini name, and what its driver
 * calls it, in that device's own names. */
static const char *probe_axis_label_on(int row, const char *name,
                                       int *reported)
{
    unsigned i;

    *reported = 0;
    if (row < 0)
        return NULL;
    for (i = 0; i < input_probe_device_axis_count((unsigned)row); i++) {
        const char *have = input_probe_device_axis_name((unsigned)row, i);
        if (have && !_stricmp(have, name)) {
            *reported = 1;
            return input_probe_device_axis_label((unsigned)row, i);
        }
    }
    return NULL;
}

/* ...and the selected device's, which is what the axis combos annotate. */
static const char *probe_axis_label_for(const char *name, int *reported)
{
    return probe_axis_label_on(s_probe_selected, name, reported);
}

/* THE ITEM'S TEXT MAY SAY MORE THAN THE INI CAN, because the VALUE travels as
 * item data and pull_page() reads that; annotating the text alone would write
 * "rz  -  Throttle" into the file on the way out.
 *
 * `axis_prefix` non-NULL turns a plain enum into an axis picker: every choice
 * the device reports carries the driver's name for it and every one it does
 * not is marked. `none` is always available; a combined-pedal wheel is
 * configured with no brake axis on purpose. */
static void combo_add_choices(HWND combo, const char *choices,
                              const char *axis_prefix)
{
    char value[96];
    int i;

    for (i = 0; choice_at(choices, i, value, sizeof value); i++) {
        char item[160];
        const char *label;
        int reported, index;

        snprintf(item, sizeof item, "%s", value);
        if (axis_prefix && _stricmp(value, "none")) {
            label = probe_axis_label_for(value, &reported);
            if (!reported)
                snprintf(item, sizeof item, "%s  (not on this device)", value);
            else if (label)
                snprintf(item, sizeof item, "%s  -  %s", value, label);
        }
        index = (int)SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)item);
        if (index >= 0)
            SendMessage(combo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)i);
    }
}

/* Select by value, not by the item's text: an annotated item reads
 * "rz  -  Throttle" where the file says "rz". `choices` may be NULL, and then
 * the text is the value. A value no item spells (`twcs:z`) gets an item of its
 * own, item data -1, which pull_page() reads back as text; SetWindowText on a
 * drop-down list sets nothing. */
static void combo_select_value(HWND combo, const char *choices,
                               const char *text)
{
    int n = (int)SendMessage(combo, CB_GETCOUNT, 0, 0), i;
    char item[160], value[96];

    for (i = n - 1; i >= 0; i--)
        if (SendMessage(combo, CB_GETITEMDATA, (WPARAM)i, 0) == (LRESULT)-1)
            SendMessage(combo, CB_DELETESTRING, (WPARAM)i, 0);
    n = (int)SendMessage(combo, CB_GETCOUNT, 0, 0);
    for (i = 0; i < n; i++) {
        LRESULT data = SendMessage(combo, CB_GETITEMDATA, (WPARAM)i, 0);
        if (choices && data != CB_ERR &&
            choice_at(choices, (int)data, value, sizeof value)) {
            if (!_stricmp(value, text)) {
                SendMessage(combo, CB_SETCURSEL, i, 0);
                return;
            }
            continue;
        }
        SendMessageA(combo, CB_GETLBTEXT, i, (LPARAM)item);
        if (!_stricmp(item, text)) {
            SendMessage(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
    if (!*text) {
        SendMessage(combo, CB_SETCURSEL, (WPARAM)-1, 0);
        return;
    }
    i = (int)SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)text);
    if (i >= 0) {
        SendMessage(combo, CB_SETITEMDATA, (WPARAM)i, (LPARAM)-1);
        SendMessage(combo, CB_SETCURSEL, (WPARAM)i, 0);
    }
}

/* THE TWO SCALE ROWS ARE DROPDOWNS THAT SAY WHAT THEY PRODUCE, "2x  -
 * 1024x800", which an edit box cannot. They stay ST_UINT / ST_AUTO_UINT in
 * settings.def: the file holds the same numbers either way, so the item's text
 * is annotation and its item data is the number (0 for automatic, -1 for a
 * value the file holds that no item is, kept verbatim so opening the page
 * never rewrites it). */
enum { SCALE_ROW_NONE, SCALE_ROW_WINDOW, SCALE_ROW_RENDER };

static int scale_row(unsigned index)
{
    const setting_desc_t *d = &k_settings[index];

    if (strcmp(d->section, "graphics"))
        return SCALE_ROW_NONE;
    if (!strcmp(d->key, "window_scale"))
        return SCALE_ROW_WINDOW;
    if (!strcmp(d->key, "render_scale"))
        return SCALE_ROW_RENDER;
    return SCALE_ROW_NONE;
}

/* The picture the game will compose, as glide_bind.c's resolve_geometry()
 * works it out: both titles ask for the cabinet's GR_RESOLUTION_512x384
 * (the mode-1 entry of the game's own video-mode table; 512x400 is what both
 * titles' logs show), raster_height replaces the 384, and widescreen widens
 * to 16:9 of that height. 0 when it
 * cannot be known from here: a forced video_mode takes the size from the
 * game's own table, which the launcher has not read, and a raster_height
 * being typed may not be a number yet. */
#define CABINET_RASTER_W 512
#define CABINET_RASTER_H 384

static int picture_raster(int *w, int *h)
{
    const char *height = model_value("graphics", "raster_height");
    unsigned rows;

    if (_stricmp(model_value("graphics", "video_mode"), "auto"))
        return 0;
    if (!_stricmp(height, "auto"))
        rows = CABINET_RASTER_H;
    else if (!ini_parse_uint(height, 1, 4096, &rows))
        return 0;
    *h = (int)rows;
    *w = CABINET_RASTER_W;
    if (model_bool("graphics", "widescreen") && (*h * 16 + 4) / 9 > *w)
        *w = (*h * 16 + 4) / 9;
    return 1;
}

/* One item's text. The window follows window.c's client_size_for(): its
 * height is the raster's times the scale and `aspect` decides the width;
 * the 3D scene is the raster times the scale on the GPU. */
static void scale_item_text(int row, int scale, char *out, size_t size)
{
    int w, h;

    if (scale == 0) {
        snprintf(out, size, "Automatic  -  the largest that fits");
        return;
    }
    if (!picture_raster(&w, &h)) {
        snprintf(out, size, "%dx", scale);
        return;
    }
    h *= scale;
    if (row == SCALE_ROW_WINDOW) {
        const char *aspect = model_value("graphics", "aspect");

        if (!_stricmp(aspect, "4:3"))
            w = (h * 4 + 1) / 3;
        else if (!_stricmp(aspect, "16:9"))
            w = (h * 16 + 4) / 9;
        else
            w *= scale;
    } else {
        w *= scale;
    }
    snprintf(out, size, "%dx  -  %dx%d", scale, w, h);
}

/* (Re)fill one scale dropdown and select the model's value. */
static void scale_combo_fill(HWND combo, unsigned index)
{
    const setting_desc_t *d = &k_settings[index];
    const char *have = model_slot(index);
    int row = scale_row(index), first, n, selected = -1, at;
    char item[INI_VALUE_CAP + 32], value[16];

    SendMessage(combo, CB_RESETCONTENT, 0, 0);
    first = d->type == ST_AUTO_UINT ? 0 : (int)d->minimum;
    for (n = first; n <= (int)d->maximum; n++) {
        if (n == 0 && d->type != ST_AUTO_UINT)
            continue;
        if (n != 0 && n < (int)d->minimum)
            continue;
        scale_item_text(row, n, item, sizeof item);
        at = (int)SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)item);
        if (at < 0)
            continue;
        SendMessage(combo, CB_SETITEMDATA, (WPARAM)at, (LPARAM)n);
        if (n == 0)
            snprintf(value, sizeof value, "auto");
        else
            snprintf(value, sizeof value, "%d", n);
        if (!_stricmp(have, value))
            selected = at;
    }
    if (selected < 0) {
        snprintf(item, sizeof item, "%s", have);
        at = (int)SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)item);
        if (at >= 0) {
            SendMessage(combo, CB_SETITEMDATA, (WPARAM)at, (LPARAM)-1);
            selected = at;
        }
    }
    SendMessage(combo, CB_SETCURSEL, (WPARAM)selected, 0);
}

/* What the selected item puts in the file. */
static void scale_combo_value(HWND combo, char *out, size_t size)
{
    LRESULT sel = SendMessage(combo, CB_GETCURSEL, 0, 0);
    LRESULT data = sel == CB_ERR ? CB_ERR :
                   SendMessage(combo, CB_GETITEMDATA, (WPARAM)sel, 0);

    if (data == CB_ERR || (int)data < 0) {
        char item[INI_VALUE_CAP + 32];

        item[0] = '\0';
        if (sel != CB_ERR)
            SendMessageA(combo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        snprintf(out, size, "%s", item);
    } else if ((int)data == 0) {
        snprintf(out, size, "auto");
    } else {
        snprintf(out, size, "%d", (int)data);
    }
}

/* The sizes follow raster_height, widescreen, aspect and video_mode, so a
 * change to any row but a scale row re-labels both. */
static void scale_combos_relabel(void)
{
    unsigned i;

    if (!s_page || s_page_index != PAGE_GRAPHICS)
        return;
    for (i = 0; i < SETTING_COUNT; i++) {
        HWND c;

        if (!scale_row(i) || k_settings[i].page != s_page_index)
            continue;
        if ((c = GetDlgItem(s_page, IDC_SETTING_BASE + (int)i)))
            scale_combo_fill(c, i);
    }
}

static int page_width(void)
{
    RECT rc;
    if (!s_page)
        return 700;
    GetClientRect(s_page, &rc);
    return max(unscale_px(rc.right - rc.left) - 18, s_tiny ? 230 : 300);
}

static int text_block_height(HFONT font, const char *text, int width);
static void place_note_lines(void);
static int  note_text_height(const char *first, const char *second);

/* Where a page's content starts: under its header, MEASURED. A Controls page
 * has the tab row above the intro, and the intro wraps, one line at 1200
 * units, four at 400, so a constant that fits one clips the other. */
static int page_intro_height(void)
{
    return text_block_height(theme_font(), k_page_intro[s_page_index],
                             page_width() - 40);
}

/* 22 UNITS UNDER THE INTRO, NOT 6: the first topic's card takes 8 as its own
 * air, so 6 put the frame's top line above the bottom of the text box, through
 * the descenders of an intro that had wrapped. The two starting offsets differ
 * because a Controls page has the tab row above its intro. */
static int page_header_bottom(void)
{
    return (page_is_controls(s_page_index) ? 42 : 16) +
           page_intro_height() + 22;
}

/* The link page's fixed panel. Both statements are there because the setting
 * below them does NOT do what a player would assume: this page picks a
 * transport, while whether a cabinet links and which cabinet it is belong to
 * the game's operator menu, in NVRAM. MEASURED, the same two sentences wrap
 * to two lines at 1200 px and six in the compact form. */
static const char *const k_link_note[] = {
    "UNIT ID and NETWORK ENABLED are the game's own operator settings and live "
    "in its NVRAM: set them in the service menu (F2), not here. This page only "
    "chooses the transport the host puts underneath them.",
    "Local links two instances on this machine, and the second one needs a save "
    "folder and logs of its own, because one save folder is one cabinet's "
    "settings:\r\n"
    "VCThunder.exe hydro --link local --save save2 --log link2.log "
    "--glide-log vcglide2.log"
};
#define LINK_NOTE_COUNT (sizeof k_link_note / sizeof k_link_note[0])

/* Logical-unit height of `text` wrapped to `width`, in `font`.
 *
 * ONE LINE PER CALL when it matters: a STATIC renders a string carrying its own
 * "\r\n" in more line-heights than DT_CALCRECT reports once a line comes close
 * to the edge, so a caller that would measure a two-line string gives each line
 * its own control instead. */
static int text_block_height(HFONT font, const char *text, int width)
{
    HWND owner = s_page ? s_page : s_window;
    HDC dc = GetDC(owner);
    HFONT previous = NULL;
    RECT rc;
    int height;

    if (!dc)
        return 20;
    SetRect(&rc, 0, 0, scale_px(width), 0);
    if (font)
        previous = (HFONT)SelectObject(dc, font);
    DrawTextA(dc, text, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (previous)
        SelectObject(dc, previous);
    ReleaseDC(owner, dc);
    height = unscale_px(rc.bottom - rc.top);
    return height < 20 ? 20 : height;
}

/* THE PAGE'S ONE SHAPE FOR A TOPIC, AND IT IS THE LAUNCH PAGE'S CARD: a
 * one-pixel frame in the border colour over the page's background (card_proc),
 * or a BS_GROUPBOX under High Contrast, where nothing here paints its own
 * colours.
 *
 * BUILT AFTER THE BLOCK IT FRAMES, THEN DROPPED TO THE BOTTOM OF THE Z-ORDER:
 * every control is WS_CLIPSIBLINGS, so a later sibling clips the earlier ones
 * and a frame built first would take the whole group with it.
 *
 * `top` is where the block started, `bottom` what its renderer returned; the
 * frame takes 8 units of air above, 4 below and 8 outside the text margin.
 * Returns where the next topic starts. */
static int frame_topic(int top, int bottom)
{
    HWND frame;
    int w = page_width() - 24, h = bottom - top + 12;

    if (w <= 0 || h <= 0)
        return bottom;
    frame = make_control(0, theme_dark() ? "VCThunderLauncherCard" : "BUTTON",
                         "", theme_dark() ? 0 : BS_GROUPBOX,
                         12, top - 8, w, h, s_page, 0);
    SetWindowPos(frame, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return bottom + 12;
}

/* Where each note goes, and where the settings rows start below them. Both
 * out-parameters may be NULL: page_settings_top() wants only the return. */
static int link_panel_layout(int *note_y, int *note_h)
{
    int width = page_width() - 40;
    int y = page_header_bottom();
    unsigned i;

    for (i = 0; i < LINK_NOTE_COUNT; i++) {
        int h = text_block_height(theme_font(), k_link_note[i], width);
        if (note_y)
            note_y[i] = y;
        if (note_h)
            note_h[i] = h;
        y += h + 10;
    }
    return y + 6;
}

static void render_link_panel(void)
{
    int note_y[LINK_NOTE_COUNT], note_h[LINK_NOTE_COUNT];
    int width = page_width();
    unsigned i;

    link_panel_layout(note_y, note_h);
    for (i = 0; i < LINK_NOTE_COUNT; i++)
        make_control(0, "STATIC", k_link_note[i], SS_LEFT, 20, note_y[i],
                     width - 40, note_h[i], s_page, 0);
    /* The two notes are this page's one topic. The frame is drawn to the
     * note's own bottom rather than to link_panel_layout()'s return, because
     * that return is where the SETTINGS ROWS go. */
    frame_topic(page_header_bottom(),
                note_y[LINK_NOTE_COUNT - 1] + note_h[LINK_NOTE_COUNT - 1]);
}

/* ---- the recovery panel -------------------------------------------------
 *
 * Two buttons and the sentence saying whether there is anything to go back to.
 * On Troubleshooting because what it restores is the WHOLE file, and because
 * that is the page somebody opens when something is wrong. */
static const char k_good_note[] =
    "A known-good copy of vcthunder.ini is made the first time this launcher "
    "opens a settings file.";

static const char k_advanced_note[] =
    "Advanced settings for deadzones, axis maps, the renderer, "
    "diagnostic keys and more.";

/* THE ADVANCED DISCLOSURE, AND THE ONE PROMISE ATTACHED TO IT: everything the
 * pages stopped showing is one click away, and the click is in one place. */
static void render_advanced_toggle(int y)
{
    int width = page_width();

    make_control(0, "BUTTON", "Show advanced settings on every page",
                 BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 20, y, width - 40,
                 22, s_page, IDC_SHOW_ADVANCED);
    SendDlgItemMessage(s_page, IDC_SHOW_ADVANCED, BM_SETCHECK,
                       s_advanced ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* The advanced block's height: heading, the switch 24 under it, the sentence
 * 26 under that, MEASURED, it wraps to three lines below about 500 units.
 * Layout and render both ask here rather than agreeing by hand. */
#define ADVANCED_HEAD_DY  0
#define ADVANCED_CHECK_DY 24
#define ADVANCED_NOTE_DY  50

static int trouble_advanced_height(void)
{
    return ADVANCED_NOTE_DY +
           text_block_height(theme_font(), k_advanced_note, page_width() - 40);
}

/* Side by side needs room for two readable labels; below about 460 units they
 * would each be under 220 and the text is what gets clipped, not the button. */
static int trouble_buttons_stacked(void)
{
    return page_width() < 460;
}

/* THE FILE'S OWN PROBLEMS GO FIRST ON THIS PAGE: "the line you edited is not
 * read by anything" is the answer more often than any switch below it. Nothing
 * repairs the file. Measured, like every block here, each problem wraps, and
 * a static sized for one line swallows the rest. scan_ini_problems(). */
static const char k_problem_head[] = "Lines in vcthunder.ini that nothing reads";

static int trouble_problems_height(void)
{
    int width = page_width() - 52;
    int h;
    unsigned i;

    if (!model_problem_total())
        return 0;
    /* MEASURED, not 20: this heading is the longest on the page and at a
     * 400-unit desktop a fixed-height static cut it off mid-word. */
    h = text_block_height(theme_font(), k_problem_head, page_width() - 40) + 6;
    for (i = 0; i < model_problem_count(); i++)
        h += text_block_height(theme_font(), model_problem(i), width) + 6;
    if (model_problem_total() > model_problem_count())
        h += text_block_height(theme_font(), "Ag", width) + 6;
    return h + 14;
}

static int render_trouble_problems(int y)
{
    int width = page_width();
    int top = y;
    unsigned i;

    if (!model_problem_total())
        return y;
    make_control(0, "STATIC", k_problem_head, SS_LEFT, 20, y, width - 40,
                 text_block_height(theme_font(), k_problem_head, width - 40),
                 s_page, 0);
    y += text_block_height(theme_font(), k_problem_head, width - 40) + 6;
    for (i = 0; i < model_problem_count(); i++) {
        int h = text_block_height(theme_font(), model_problem(i), width - 52);

        make_control(0, "STATIC", model_problem(i), SS_LEFT, 32, y,
                     width - 52, h, s_page, 0);
        y += h + 6;
    }
    if (model_problem_total() > model_problem_count()) {
        char more[96];
        int h;

        snprintf(more, sizeof more, "...and %u more.",
                 model_problem_total() - model_problem_count());
        h = text_block_height(theme_font(), more, width - 52);
        make_control(0, "STATIC", more, SS_LEFT, 32, y, width - 52, h,
                     s_page, 0);
        y += h + 6;
    }
    frame_topic(top, y - 6);
    return y + 8;
}

static int trouble_panel_layout(int *note_y, int *note_h, int *buttons_y,
                                int *state_y, int *state_h, int *adv_y)
{
    int width = page_width() - 40;
    int y = page_header_bottom() + trouble_problems_height();
    int h = text_block_height(theme_font(), k_good_note, width);
    char state[160];
    int sh;

    /* THE STATE LINE IS MEASURED TOO: the known-good copy's timestamp is one
     * line at 1200 units and two at 400, so a constant height shows the date
     * and swallows the time. It is also why saving a copy REBUILDS the page
     * instead of setting this text. */
    known_good_state(state, sizeof state);
    sh = text_block_height(theme_font(), state, width);
    y += 22;
    if (note_y)
        *note_y = y;
    if (note_h)
        *note_h = h;
    y += h + 10;
    if (buttons_y)
        *buttons_y = y;
    y += trouble_buttons_stacked() ? 62 : 32;
    if (state_y)
        *state_y = y;
    if (state_h)
        *state_h = sh;
    /* 24 clears the known-good card's own bottom line and leaves the advanced
     * card 12 units of its own. */
    y += sh + 24;
    if (adv_y)
        *adv_y = y;
    return y + trouble_advanced_height() + 20;
}

static void render_trouble_panel(void)
{
    int note_y, note_h, buttons_y, state_y, state_h, adv_y;
    int width = page_width(), stacked = trouble_buttons_stacked();
    int half = stacked ? width - 40 : min(240, (width - 48) / 2);
    char state[160], path[MAX_PATH];
    HWND revert;

    trouble_panel_layout(&note_y, &note_h, &buttons_y, &state_y, &state_h,
                         &adv_y);
    render_trouble_problems(page_header_bottom());
    make_control(0, "STATIC", "Known-good settings", SS_LEFT,
                 20, page_header_bottom() + trouble_problems_height(),
                 width - 40, 20, s_page, 0);
    make_control(0, "STATIC", k_good_note, SS_LEFT, 20, note_y, width - 40,
                 note_h, s_page, 0);
    make_control(0, "BUTTON", "Save these as known-good",
                 BS_PUSHBUTTON | WS_TABSTOP, 20, buttons_y, half, 26,
                 s_page, IDC_GOOD_SAVE);
    revert = make_control(0, "BUTTON", "Revert to known-good",
                 BS_PUSHBUTTON | WS_TABSTOP,
                 stacked ? 20 : 20 + half + 8,
                 stacked ? buttons_y + 30 : buttons_y, half, 26,
                 s_page, IDC_GOOD_REVERT);
    known_good_state(state, sizeof state);
    make_control(0, "STATIC", state, SS_LEFT, 20, state_y, width - 40,
                 state_h, s_page, 0);
    /* A Revert with nothing to revert TO is refused by the handler anyway;
     * greying it is what says so before the press. */
    known_good_path(path, sizeof path);
    EnableWindow(revert, GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    frame_topic(page_header_bottom() + trouble_problems_height(),
                state_y + state_h);
    /* THE SECOND TOPIC, and the only place this switch exists. Every page's
     * advanced keys are behind it, which is why the sentence under it says
     * "on every page". */
    make_control(0, "STATIC", "Advanced settings", SS_LEFT, 20,
                 adv_y + ADVANCED_HEAD_DY, width - 40, 20, s_page, 0);
    render_advanced_toggle(adv_y + ADVANCED_CHECK_DY);
    {
        int adv_h = text_block_height(theme_font(), k_advanced_note, width - 40);

        make_control(0, "STATIC", k_advanced_note, SS_LEFT, 20,
                     adv_y + ADVANCED_NOTE_DY, width - 40, adv_h, s_page, 0);
        frame_topic(adv_y, adv_y + ADVANCED_NOTE_DY + adv_h);
    }
}

/* ---- the live panel ------------------------------------------------------
 *
 * ONE BAR PER ROLE, AND THE BAR IS THE MAPPED VALUE: what the cabinet will be
 * handed, with deadzone, pedal mode and inversion applied. Carrying a raw table
 * alongside it meant one pedal moved two bars in opposite directions under the
 * shipped `dinput_throttle_invert = true`. Only the mapped value answers "does
 * the car get my accelerator"; the raw table is one click away.
 *
 * MEASURED rather than laid out from constants: the row count is a property of
 * what is plugged in. */
#define AXIS_ROW_PITCH 22

static unsigned probe_axis_rows(void)
{
    unsigned n = input_probe_axis_count();

    return n > INPUT_PROBE_MAX_AXES ? INPUT_PROBE_MAX_AXES : n;
}

/* Where the first settings row goes: under whatever the hand-built panel
 * needed. render_settings_page() renders the panel first and the rows after,
 * which is the one ordering this static depends on. */
static int page_settings_top(void)
{
    if (s_page_index == PAGE_CONTROLS || s_page_index == PAGE_FFB)
        return s_panel_bottom;
    if (s_page_index == PAGE_LINK)
        return link_panel_layout(NULL, NULL);
    if (s_page_index == PAGE_TROUBLE)
        return trouble_panel_layout(NULL, NULL, NULL, NULL, NULL, NULL);
    return page_header_bottom();
}

/* THE `help` COLUMN, from settings.def. Twenty shipped keys carry a sentence
 * saying what the setting is for and what it costs; `video_mode`, whose help
 * says it can damage an arcade monitor, most of all.
 *
 * A `warning` is spelled into the caption's text rather than shown as a colour,
 * because a colour says nothing in High Contrast, which this launcher hands
 * back to the system rather than inventing colours for. */
/* Sized against the table, not guessed: the longest shipped help string is 253
 * characters and the "Caution: " lead adds nine. tools/check-launcher-settings.py
 * caps a help string at VCT_HELP_MAX, so a future one that would not fit fails
 * `make check` instead of appearing clipped on screen. */
#define VCT_HELP_MAX 400
/* The lead, plus the per-title sentence setting_title_override() appends. That
 * sentence is composed here rather than counted by the checker, which only
 * holds settings.def's own `help` column to VCT_HELP_MAX. */
#define VCT_HELP_CAP (VCT_HELP_MAX + 32 + 192)

/* WHICH CABINETS SET THIS KEY FOR THEMSELVES, and to what. Bit 0 Hydro, bit 1
 * Offroad; `out` gets "Hydro 100, Offroad 8".
 *
 * THE LAUNCHER EDITS THE SHARED SECTIONS AND HAS NO TITLE SCOPE, and
 * ini_read_scoped() tries [<title>.<section>] FIRST, so a key a per-title
 * section carries is one this window cannot change. Without this the row shows
 * the shared value and writing it reaches neither cabinet, silently. The
 * shipped per-title blocks are deliberate; what was wrong is that nothing
 * said so. */
static unsigned setting_title_override(unsigned index, char *out,
                                       size_t out_size)
{
    static const char *const k_id[2] = { "hydro", "offroad" };
    static const char *const k_name[2] = { "Hydro", "Offroad" };
    unsigned t, mask = 0;
    size_t at = 0;

    if (out && out_size)
        out[0] = '\0';
    for (t = 0; t < 2; t++) {
        char section[96], value[INI_VALUE_CAP];

        /* THE TITLE-SCOPED FORM OF THE SECTION THIS ROW WRITES, which for a
         * device key is [<title>.analog.<label>] and not [<title>.analog].
         * **Device beats title** (ini_read_device_scoped), so a [hydro.analog]
         * line does not block a row writing [analog.tmx]. */
        snprintf(section, sizeof section, "%s.%s", k_id[t],
                 setting_section(index));
        if (!ini_doc_get(model_doc(), section, k_settings[index].key,
                         value, sizeof value))
            continue;
        mask |= 1u << t;
        if (out && at < out_size)
            at += (size_t)snprintf(out + at, out_size - at, "%s%s %s",
                                   at ? ", " : "", k_name[t], value);
    }
    return mask;
}

static const char *setting_help(unsigned index, char *buf, size_t cap)
{
    const setting_desc_t *d = &k_settings[index];
    const char *lead = d->warning == WARNING_DANGER  ? "Warning: " :
                       d->warning == WARNING_CAUTION ? "Caution: " : "";
    char scoped[128];
    unsigned mask = setting_title_override(index, scoped, sizeof scoped);
    size_t at;

    if (!d->help && !mask)
        return NULL;
    at = (size_t)snprintf(buf, cap, "%s%s", lead, d->help ? d->help : "");
    if (!mask || at >= cap)
        return buf;
    /* BOTH cabinets, and this row reaches neither: it is disabled, and saying
     * "this row does nothing" without saying where the value IS would send the
     * reader to the launcher for a setting the launcher cannot hold. */
    if (mask == 3u)
        snprintf(buf + at, cap - at,
                 "%sEach cabinet sets this for itself in vcthunder.ini "
                 "(%s), so this row is not used.", at ? "  " : "", scoped);
    else {
        /* ONE cabinet: `scoped` is "Hydro 100" and the sentence already names
         * the cabinet, so only the value goes in the brackets. */
        const char *value = strchr(scoped, ' ');

        snprintf(buf + at, cap - at,
                 "%s%s sets this for itself in vcthunder.ini (%s); this row "
                 "is what the other cabinet uses.", at ? "  " : "",
                 (mask & 1u) ? "Hydro" : "Offroad",
                 value ? value + 1 : scoped);
    }
    return buf;
}

/* Measured: a caption is one line at 1200 units and three at 400. Cached
 * against the width, layout mode and DPI that produced it, page_setting_y()
 * is O(rows^2) in this by construction and each miss is a GetDC and a
 * DrawText. */
static int s_help_h[SETTING_COUNT];
static int s_help_h_width = -1, s_help_h_compact = -1;
static UINT s_help_h_dpi;

/* The fourth input to those heights is the DOCUMENT, and it is the one the
 * cache cannot see changing: load_settings() calls this. */
static void settings_help_cache_flush(void)
{
    s_help_h_width = -1;
}

static int setting_help_height(unsigned index)
{
    char text[VCT_HELP_CAP];
    int width = page_width();

    if (!k_settings[index].help)
        return 0;
    if (width != s_help_h_width || s_compact != s_help_h_compact ||
        theme_dpi() != s_help_h_dpi) {
        memset(s_help_h, 0, sizeof s_help_h);
        s_help_h_width = width;
        s_help_h_compact = s_compact;
        s_help_h_dpi = theme_dpi();
    }
    if (!s_help_h[index])
        s_help_h[index] = text_block_height(
            theme_font(), setting_help(index, text, sizeof text), width - 44) + 8;
    return s_help_h[index];
}

/* Where a row's caption sits inside it: under the control, which in compact
 * mode is itself under the label. A checkbox is its own label in both modes. */
static int setting_caption_offset(unsigned index)
{
    return (s_compact && k_settings[index].type != ST_BOOL) ? 51 : 26;
}

/* Which of the three provider prefixes a key belongs to, or NULL. One
 * spelling, shared with the matcher above: two lists of the same three prefixes
 * is how one ends up missing a provider. */
static const char *setting_provider_prefix(const char *key)
{
    if (!strncmp(key, "dinput_", 7)) return "dinput";
    if (!strncmp(key, "xinput_", 7)) return "xinput";
    if (!strncmp(key, "winmm_", 6))  return "winmm";
    return NULL;
}

/* ONE PROVIDER'S AXIS BLOCK AT A TIME. Only one provider can ever be live, and
 * three consecutive rows called "steering axis" with three Detect buttons is
 * not a longer form of one row: pressing Detect on the XInput block while the
 * wheel is on DirectInput reads as the button doing nothing.
 * begin_axis_capture() does refuse a mismatched provider and say so, but the
 * status line at the foot of the window is not where the eye is, and a control
 * that cannot apply should not be offered in the first place.
 *
 * WHAT DECIDES: `provider` when it names one, because that is the key that
 * decides which block the host will read. Under `auto` the host picks at run
 * time, so the selected test device stands in for it; with neither, every block
 * stays visible, which is what lets somebody configure a wheel that is not
 * plugged in yet. Hiding is never allowed to make a key unreachable: set
 * `provider` and the block it names appears. */
static int setting_hidden_here(unsigned index)
{
    const char *prefix = setting_provider_prefix(k_settings[index].key);
    const char *provider;
    const input_probe_device_t *device;

    if (!prefix || k_settings[index].page != PAGE_CONTROLS)
        return 0;
    provider = model_value("analog", "provider");
    if (provider && *provider && _stricmp(provider, "auto") &&
        _stricmp(provider, "none"))
        return _stricmp(provider, prefix) != 0;
    if (s_probe_selected < 0)
        return 0;                       /* nothing to filter by; show them all */
    device = input_probe_device((unsigned)s_probe_selected);
    if (!device)
        return 0;
    return !probe_backend_matches_setting(device, k_settings[index].key);
}

/* The page's own row test, so the filter cannot be applied at one of the four
 * sites and missed at another, the row heights, the y accumulator, the
 * render loop and the scroll extent must agree or the page lays out on top of
 * itself. */
static int setting_on_page(unsigned index)
{
    if (k_settings[index].page != s_page_index)
        return 0;
    /* BEHIND THE DISCLOSURE, AND ONE CLICK FROM IT. The keys that left the
     * page did not leave the build: `Advanced settings` on this same page
     * brings every one of them back, which is the promise that makes hiding
     * anything legitimate at all. */
    if ((k_settings[index].flags & SF_ADVANCED) && !s_advanced)
        return 0;
    return !setting_hidden_here(index);
}

static int setting_row_height(unsigned index)
{
    return (s_compact ? 58 : 36) + setting_help_height(index);
}

/* Rows are not a constant pitch apart, so this accumulates rather than
 * multiplying. The one other consumer of a row position is the scroll extent
 * at the bottom of render_page(). */
static int page_setting_y(unsigned index)
{
    unsigned i;
    int y = page_settings_top();

    for (i = 0; i < index; i++)
        if (setting_on_page(i))
            y += setting_row_height(i);
    return y;
}

static void render_setting(unsigned index)
{
    const setting_desc_t *d = &k_settings[index];
    HWND control;
    char display[INI_VALUE_CAP];
    const char *control_text = model_slot(index);
    int y = page_setting_y(index), control_w = 310;
    int width = page_width();
    int has_button = (d->flags & (SF_DIR | SF_OPEN_FILE | SF_SAVE_FILE |
                                  SF_AXIS)) != 0;
    /* Browse and Detect cannot sit at a fixed x: 564 is off the page below a
     * ~900-unit window, the same way the Launch page's Play buttons would be.
     * The value control gives way instead, down to a floor. */
    int button_x;

    /* A ROW NEITHER CABINET WOULD READ IS NOT OFFERED FOR EDITING. Its caption
     * says where the value lives; leaving the control live would let somebody
     * set a number, press Apply, see "1 key changed" and get nothing in either
     * game. setting_title_override(). */
    int scoped_both = setting_title_override(index, NULL, 0) == 3u;

    if (d->type == ST_BOOL) {
        control = make_control(0, "BUTTON", d->label,
            BS_AUTOCHECKBOX | WS_TABSTOP, 22, y,
            s_compact ? width - 44 : min(510, width - 44), 24,
            s_page, IDC_SETTING_BASE + (int)index);
        SendMessage(control, BM_SETCHECK,
                    model_bool(d->section, d->key) ? BST_CHECKED : BST_UNCHECKED,
                    0);
        if (scoped_both)
            EnableWindow(control, FALSE);
    } else {
        /* SS_ENDELLIPSIS, and the checker caps the label length: a label
         * wider than its column WRAPS into the help caption underneath and
         * the two overlap, which is worse than a truncation that says so.
         * Caught on "Wheel: how much of its rotation to use (%)". */
        make_control(0, "STATIC", d->label, SS_LEFT | SS_ENDELLIPSIS, 22, y + 4,
                     s_compact ? width - 44 : 215, 22,
                     s_page, 0);
        control_w = s_compact ? width - 44 - (has_button ? 90 : 0)
                              : min(310, width - 244 - 22 -
                                         (has_button ? 90 : 0));
        if (control_w < 120)
            control_w = 120;
        if (d->type == ST_ENUM) {
            control = make_control(0, "COMBOBOX", "",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                s_compact ? 22 : 244, s_compact ? y + 25 : y,
                control_w, 240, s_page,
                IDC_SETTING_BASE + (int)index);
            /* AN AXIS ROW IS ANNOTATED FROM THE SELECTED DEVICE, and only
             * when that device is the one the row is about: a dinput row
             * annotated from a WinMM device marks every DirectInput axis "not
             * on this device". setting_hidden_here() normally keeps the two in
             * step; this does not rely on it. */
            const char *axis_prefix = NULL;
            if ((d->flags & SF_AXIS) && s_probe_selected >= 0) {
                const input_probe_device_t *device =
                    input_probe_device((unsigned)s_probe_selected);
                if (device && probe_backend_matches_setting(device, d->key))
                    axis_prefix = setting_provider_prefix(d->key);
            }
            combo_add_choices(control, d->choices, axis_prefix);
            combo_select_value(control, d->choices, model_slot(index));
        } else if (scale_row(index)) {
            control = make_control(0, "COMBOBOX", "",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                s_compact ? 22 : 244, s_compact ? y + 25 : y,
                control_w, 240, s_page,
                IDC_SETTING_BASE + (int)index);
            scale_combo_fill(control, index);
        } else {
            if (d->type == ST_MHZ_UINT) {
                char *end;
                unsigned long hz = strtoul(model_slot(index), &end, 10);
                if (end != model_slot(index) && !*end &&
                    hz >= d->minimum && hz <= d->maximum) {
                    snprintf(display, sizeof display, "%lu", hz / 1000000ul);
                    control_text = display;
                }
            }
            control = make_control(WS_EX_CLIENTEDGE, "EDIT", control_text,
                ES_AUTOHSCROLL | WS_TABSTOP, s_compact ? 22 : 244,
                s_compact ? y + 25 : y, control_w, 24,
                s_page, IDC_SETTING_BASE + (int)index);
            SendMessage(control, EM_SETLIMITTEXT, INI_VALUE_CAP - 1, 0);
        }
        button_x = s_compact ? width - 104 : 244 + control_w + 8;
        if (d->flags & (SF_DIR | SF_OPEN_FILE | SF_SAVE_FILE))
            make_control(0, "BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP,
                         button_x, s_compact ? y + 25 : y, 82, 24,
                         s_page, IDC_BROWSE_BASE + (int)index);
        if (d->flags & SF_AXIS)
            make_control(0, "BUTTON", "Detect", BS_PUSHBUTTON | WS_TABSTOP,
                         button_x, s_compact ? y + 25 : y, 82, 24,
                         s_page, IDC_DETECT_BASE + (int)index);
        if (scoped_both) {
            HWND button;

            EnableWindow(control, FALSE);
            if ((button = GetDlgItem(s_page, IDC_BROWSE_BASE + (int)index)))
                EnableWindow(button, FALSE);
            if ((button = GetDlgItem(s_page, IDC_DETECT_BASE + (int)index)))
                EnableWindow(button, FALSE);
        }
    }
    {
        char text[VCT_HELP_CAP];
        const char *caption = setting_help(index, text, sizeof text);

        if (caption)
            make_control(0, "STATIC", caption, SS_LEFT, 22,
                         y + setting_caption_offset(index), width - 44,
                         setting_help_height(index) - 8, s_page,
                         IDC_HELP_BASE + (int)index);
    }
}

/* The page does not repeat its own name: the navigation list carries it
 * highlighted, and the compact form's collapsed combo shows it too. The intro
 * takes the space a page title would, which is why PAGE_TOP sits 24 units
 * higher than a titled page needs. The three Controls pages are unaffected;
 * their header is the tab row. */
static void add_page_header(void)
{
    make_control(0, "STATIC", k_page_intro[s_page_index], SS_LEFT,
                 20, 16, page_width() - 40, page_intro_height(), s_page, 0);
}


/* A ROW OF TABS, NOT A ROW OF DOTS: every subpage switch here is a place to
 * go, and a radio dot is this launcher's shape for a SETTING the page will
 * save. So: buttons laid out as a tab strip, owner-drawn in dark mode; under
 * High Contrast there is no owner-draw, so the row falls back to a PUSHLIKE
 * radio group the SYSTEM draws pressed in the user's own colours. Both take
 * which is lit from tab_is_selected(). The width is a MAXIMUM, as everything
 * here is. Returns the row's bottom edge. */
static int add_tab_row(int y, int height, int max_w,
                       const char *const *labels, const int *ids,
                       unsigned count)
{
    int tab_w = min(max_w, (page_width() - 44) / (int)count);
    unsigned i;

    for (i = 0; i < count; i++) {
        HWND tab = make_control(0, "BUTTON", labels[i],
            theme_dark() ? BS_PUSHBUTTON | WS_TABSTOP :
                          BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_TABSTOP |
                          (i ? 0 : WS_GROUP),
            20 + (int)i * (tab_w + 4), y, tab_w, height, s_page, ids[i]);
        if (!theme_dark())
            SendMessage(tab, BM_SETCHECK,
                        tab_is_selected(ids[i]) ? BST_CHECKED : BST_UNCHECKED,
                        0);
    }
    return y + height;
}

/* TWO TABS. Devices and axes are the top half of the page this one heads. */
static void add_controls_header(void)
{
    static const int ids[] = { IDC_CONTROL_MAIN, IDC_CONTROL_FFB };
    const char *labels[2];

    /* THE TAB IS THE PAGE'S OWN NAME, read from the one table that has it: a
     * tab that says something other than the title bar is two names for one
     * page. */
    labels[0] = page_name(PAGE_CONTROLS);
    labels[1] = page_name(PAGE_FFB);
    add_tab_row(10, 24, 220, labels, ids, 2);
    make_control(0, "STATIC", k_page_intro[s_page_index], SS_LEFT,
                 20, 42, page_width() - 40, page_intro_height(), s_page, 0);
}

static int game_path(const char *title, char *out, size_t out_size)
{
    const game_profile_t *p = game_find(title);
    const char *root = model_value("paths", "data_dir");
    char path[MAX_PATH];

    if (!p)
        return 0;
    /* Exact first, then the case the filesystem uses: the launcher greys out a
     * title it cannot find, so without the fallback a good pack on a
     * case-sensitive data directory offers nothing to play. fs_resolve_ci(). */
    snprintf(path, sizeof path, "%s\\%s\\%s", root, p->id, p->exe);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, out_size, "%s", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s\\%s", p->id, p->exe);
    if (fs_resolve_ci(root, path, out, out_size))
        return 1;
    snprintf(out, out_size, "%s\\%s\\%s", root, p->id, p->exe);
    return 0;
}

static void render_home(void)
{
    unsigned i;
    const char *ids[] = { "hydro", "offroad" };
    char validation[256];
    int invalid_page = PAGE_HOME;
    int settings_valid = validate_settings(validation, sizeof validation,
                                           &invalid_page);
    int width = page_width();
    int group_x = s_tiny ? 6 : (s_compact ? 12 : 20);
    /* 650 is what a card wants; the page is what it has. The two-column layout
     * starts at a 760-unit window and leaves the page about 500, so a card
     * insisting on 650 hung its right edge, and the Play button aligned to it,
     * off the page, with nothing to scroll it back. */
    int group_w = s_tiny ? width - 12 :
                  (s_compact ? width - 24 : min(650, width - group_x * 2));
    int next_card_y = s_tiny ? 15 : (s_compact ? 54 : 72);

    if (s_compact) {
        if (!s_tiny)
            make_control(0, "STATIC", k_page_intro[PAGE_HOME], SS_LEFT,
                         12, 10, width - 24, 20, s_page, 0);
    } else {
        add_page_header();
    }
    /* THE TWO THINGS THIS PAGE SAYS ABOUT THE FILE, IN BOTH LAYOUTS.
     *
     * WHY PLAY IS OFF comes first: it is what stops the user doing the thing
     * the page is for. validate_settings() produces that sentence, and it names
     * the page too, because the value is not on this one.
     *
     * The second is the count of lines nothing reads (scan_ini_problems()).
     *
     * Both are MEASURED and both push the cards down: this page scrolls. */
    {
        char note[2][256];
        int notes = 0, n;
        int note_y = s_tiny ? 6 : (s_compact ? 32 : page_intro_height() + 18);

        if (!settings_valid) {
            const char *where = page_name(invalid_page);

            if (invalid_page != PAGE_HOME && where)
                snprintf(note[notes++], sizeof note[0],
                         "Play is off until this is fixed on %s: %s",
                         where, validation);
            else
                snprintf(note[notes++], sizeof note[0],
                         "Play is off until this is fixed: %s", validation);
        }
        if (!s_tiny && model_problem_total())
            snprintf(note[notes++], sizeof note[0],
                     model_problem_total() == 1 ?
                     "%u line in vcthunder.ini is not used. Troubleshooting "
                     "says which." :
                     "%u lines in vcthunder.ini are not used. Troubleshooting "
                     "says which.", model_problem_total());
        for (n = 0; n < notes; n++) {
            int note_w = width - (s_compact ? 24 : 40);
            int note_h = text_block_height(theme_font(), note[n], note_w);

            make_control(0, "STATIC", note[n], SS_LEFT, s_compact ? 12 : 20,
                         note_y, note_w, note_h, s_page, 0);
            note_y += note_h + 4;
        }
        if (notes && note_y + 8 > next_card_y)
            next_card_y = note_y + 8;
    }
    /* Each card is as tall as what is in it. At a constant height, a larger
     * title face or a save directory spelled out in full puts the second line
     * outside the box, where it is simply not drawn. */
    for (i = 0; i < 2; i++) {
        const game_profile_t *p = game_find(ids[i]);
        char path[MAX_PATH + 64], status[MAX_PATH + 96];
        int ready = game_path(ids[i], path, sizeof path);
        int play_w = s_tiny ? 72 : (s_compact ? 92 : 184);
        int play_h = s_tiny ? 28 : (s_compact ? 30 : 60);
        int play_inset = s_tiny ? 10 : 18;
        /* The text ends where the button begins, whatever the card's width. */
        int status_w = group_w - play_w - play_inset - 30;
        /* The title line has a face of its own, so it is a control of its own
         * and the block below it is what is left of the old one. */
        int pad_top = s_tiny ? 6 : (s_compact ? 8 : 22);
        int pad_bottom = s_tiny ? 6 : (s_compact ? 10 : 22);
        int floor_h = s_tiny ? 48 : (s_compact ? 72 : 130);
        int card_y = next_card_y;
        int title_y = card_y + pad_top;
        int title_h, line_h, path_y, save_y, bottom, card_h, play_y;
        HFONT title_font = s_tiny ? theme_title_small_font() : theme_title_font();
        HWND play, title_label, card = NULL;

        if (!title_font)
            title_font = theme_font();

        /* "data not found" moves off the title line and onto the path it
         * could not find: on a 320x240 desktop, leaving it on the title line
         * wraps that line in two and pushes the path out of the box. */
        if (s_tiny)
            snprintf(status, sizeof status, "%s",
                     ready ? path : "data not found");
        else
            snprintf(status, sizeof status, "%s%s",
                     ready ? "" : "data not found: ", path);
        title_h = text_block_height(title_font, p ? p->name : ids[i],
                                    status_w);
        line_h = text_block_height(theme_font(), "Ag", status_w);
        path_y = title_y + title_h + 6;
        save_y = path_y + line_h + 2;
        bottom = (s_tiny ? path_y : save_y) + line_h;
        card_h = bottom + pad_bottom - card_y;
        if (card_h < floor_h)
            card_h = floor_h;
        play_y = card_y + (card_h - play_h) / 2;

        if (theme_dark()) {
            card = make_control(0, "VCThunderLauncherCard", "", 0,
                                group_x, card_y, group_w, card_h, s_page, 0);
        } else {
            make_control(0, "BUTTON", "", BS_GROUPBOX,
                         group_x, card_y - (s_tiny ? 8 : 14), group_w,
                         card_h + (s_tiny ? 8 : 16), s_page, 0);
        }
        title_label = make_control(0, "STATIC", p ? p->name : ids[i], SS_LEFT,
                                   group_x + 18, title_y, status_w, title_h,
                                   s_page, 0);
        if (title_font)
            SendMessage(title_label, WM_SETFONT, (WPARAM)title_font, TRUE);
        make_control(0, "STATIC", status, SS_LEFT, group_x + 18, path_y,
                     status_w, line_h, s_page, 0);
        if (!s_tiny) {
            char save_line[MAX_PATH + 32];
            snprintf(save_line, sizeof save_line, "Save data: %s",
                     model_value("paths", "save_dir"));
            make_control(0, "STATIC", save_line, SS_LEFT, group_x + 18,
                         save_y, status_w, line_h, s_page, 0);
        }
        play = make_control(0, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP,
                            group_x + group_w - play_w - play_inset, play_y,
                            play_w, play_h, s_page,
                            i ? IDC_PLAY_OFFROAD : IDC_PLAY_HYDRO);
        if (!s_compact && theme_play_font())
            SendMessage(play, WM_SETFONT, (WPARAM)theme_play_font(), TRUE);
        EnableWindow(play, ready && settings_valid);
        if (card)
            SetWindowPos(card, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        next_card_y = card_y + card_h + (s_tiny ? 9 : (s_compact ? 16 : 42));
    }
    s_content_height = next_card_y + 8;
}

/* A control Hydro does not have is hidden while Hydro is selected, and a NULL
 * Hydro label is the only thing that says so. */
int action_visible(unsigned action)
{
    return k_actions[action].hydro_label != NULL || s_binding_title;
}

/* THE PRODUCT NAME THE INI WOULD CARRY, which is what `pick_device()` matches
 * against, the probe prefixes its rows with "DirectInput: " and the host
 * never sees that. */
const char *probe_product_name(const input_probe_device_t *device)
{
    const char *colon = device ? strstr(device->name, ": ") : NULL;

    return colon ? colon + 2 : (device ? device->name : "");
}

static const char *probe_provider(const input_probe_device_t *device)
{
    return device->backend == INPUT_PROBE_DINPUT ? "dinput" :
           device->backend == INPUT_PROBE_XINPUT ? "xinput" : "winmm";
}

/* One provider's controllers in enumeration order, for ini_devices_resolve():
 * the ID on DirectInput, the number elsewhere (a display string would let
 * `xinput:0` match "XInput controller 0"). `row[k]` is the probe row of
 * `ids[k]`. */
static unsigned probe_ids(const char *provider, ini_device_id_t *ids,
                          unsigned *row)
{
    unsigned i, n = 0;

    for (i = 0; i < input_probe_count() && n < INPUT_PROBE_MAX_DEVICES; i++) {
        const input_probe_device_t *d = input_probe_device(i);

        if (!d || _stricmp(probe_provider(d), provider) != 0)
            continue;
        ids[n].id = d->backend == INPUT_PROBE_DINPUT ? d->id : "";
        ids[n].name = d->backend == INPUT_PROBE_DINPUT ? d->id_name : "";
        ids[n].index = d->backend_index;
        row[n++] = i;
    }
    return n;
}

/* `d`'s place in its provider's list, or -1. */
static int probe_id_of(const input_probe_device_t *d, ini_device_id_t *ids,
                       unsigned *count)
{
    unsigned row[INPUT_PROBE_MAX_DEVICES], k;

    *count = probe_ids(probe_provider(d), ids, row);
    for (k = 0; k < *count; k++)
        if (input_probe_device(row[k]) == d)
            return (int)k;
    return -1;
}

/* The probe row a [devices] label opens, by the host's resolver over the
 * whole table, or -1 when it names nothing attached. */
static int probe_row_for_label(const char *label)
{
    ini_device_id_t ids[INPUT_PROBE_MAX_DEVICES];
    unsigned row[INPUT_PROBE_MAX_DEVICES], n;
    char value[INI_VALUE_CAP], provider[16], match[INI_MATCH_CAP];
    int k;

    if (!label || !label[0] ||
        !ini_doc_get(model_doc(), "devices", label, value, sizeof value) ||
        !ini_device_entry(value, provider, sizeof provider,
                          match, sizeof match))
        return -1;
    n = probe_ids(provider, ids, row);
    k = model_device_for_label(label, provider, ids, n);
    return k < 0 ? -1 : (int)row[k];
}

/* What resolve_profile() is told is attached. */
static unsigned probe_attached(const char *provider, ini_device_id_t *ids,
                               int *ffb, unsigned max)
{
    unsigned row[INPUT_PROBE_MAX_DEVICES], n, k;

    (void)max;               /* MODEL_ATTACHED_MAX == INPUT_PROBE_MAX_DEVICES */
    n = probe_ids(provider, ids, row);
    for (k = 0; k < n; k++)
        ffb[k] = input_probe_device(row[k])->force_feedback;
    return n;
}
typedef char attached_cap_check[MODEL_ATTACHED_MAX == INPUT_PROBE_MAX_DEVICES ?
                                1 : -1];

/* The DirectInput row the file configures, by the host's rule, or -1. */
static int probe_configured_row(void)
{
    ini_device_id_t ids[INPUT_PROBE_MAX_DEVICES];
    unsigned row[INPUT_PROBE_MAX_DEVICES], n;
    int ffb[INPUT_PROBE_MAX_DEVICES], k;

    n = probe_ids("dinput", ids, row);
    for (k = 0; k < (int)n; k++)
        ffb[k] = input_probe_device(row[k])->force_feedback;
    k = model_configured_device(ids, ffb, n);
    return k < 0 ? -1 : (int)row[k];
}

/* The DirectInput controller the host drives with: the one an unlabelled
 * role axis is on. -1 for none. */
int launcher_default_row(void)
{
    return probe_configured_row();
}

/* ---- adopting a device, as against testing one --------------------------
 *
 * WHAT ADOPTING `d` WOULD PUT IN THE FILE, computed rather than written, so one
 * walk answers both "do it" and "would it change anything". The second is not
 * decoration: a button offering to use a device the file already names is an
 * action that appears to do something while the file quietly moves.
 *
 * Three keys and no more: `provider`, the one device key belonging to that
 * provider, and, on DirectInput alone, the product name. Everything else on the
 * page, the axis map included, belongs to whoever set it. */
typedef struct {
    const char *provider;           /* [analog] provider                    */
    const char *name_key;           /* dinput_name, or NULL on the others   */
    char        name[INI_MATCH_CAP];
    const char *index_key;          /* xinput_ / winmm_device, or NULL      */
    char        index[16];
} probe_adopt_t;

/* On DirectInput the device is named, never numbered: `dinput_name` gets what
 * a [devices] line would (model_device_match()), its name, or its full ID when
 * the name opens another controller. */
static int probe_adopt_plan(const input_probe_device_t *d, probe_adopt_t *out)
{
    ini_device_id_t ids[INPUT_PROBE_MAX_DEVICES];
    unsigned count;
    int which;

    if (!d)
        return 0;
    memset(out, 0, sizeof *out);
    out->provider = probe_provider(d);
    if (d->backend != INPUT_PROBE_DINPUT) {
        snprintf(out->index, sizeof out->index, "%u", d->backend_index);
        out->index_key = d->backend == INPUT_PROBE_XINPUT ?
                         "xinput_device" : "winmm_device";
        return 1;
    }
    out->name_key = "dinput_name";
    return (which = probe_id_of(d, ids, &count)) >= 0 &&
           model_device_match("dinput", ids, count, (unsigned)which, 0,
                              out->name, sizeof out->name);
}

static int probe_adopt_pending(const probe_adopt_t *p)
{
    return strcmp(model_value("analog", "provider"), p->provider) != 0 ||
           (p->index_key &&
            strcmp(model_value("analog", p->index_key), p->index) != 0) ||
           (p->name_key &&
            strcmp(model_value("analog", p->name_key), p->name) != 0);
}

/* ---- the Controls page ----------------------------------------------------
 *
 * Three groups down one page, in the order somebody sets a wheel up: which
 * controllers are attached, what drives the car, what the buttons do. It
 * replaces the Bindings and Devices & Axes tabs, the same job cut by
 * mechanism, and the `Test controller` combo, which was a mode nothing on
 * screen declared. Every attached controller is polled every tick, so nothing
 * has to be selected before it can be used. */

/* A WinMM row is a second view of a controller DirectInput already listed, and
 * this host reads one provider at a time, so two of them are two rows nobody
 * can tell apart. Behind the legacy disclosure rather than gone: a device no
 * other backend enumerates is still one click away. */
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

static int device_listed(unsigned i)
{
    const input_probe_device_t *d = input_probe_device(i);

    if (!d)
        return 0;
    return d->backend != INPUT_PROBE_WINMM || s_show_legacy;
}

static int device_winmm_hidden(void)
{
    unsigned i, n = 0;

    if (s_show_legacy)
        return 0;
    for (i = 0; i < input_probe_count(); i++) {
        const input_probe_device_t *d = input_probe_device(i);
        if (d && d->backend == INPUT_PROBE_WINMM)
            n++;
    }
    return (int)n;
}

/* WHAT THE CONTROLLER IS, in terms this host can answer: how many axes and
 * buttons it reports, and whether it publishes force feedback. Not "wheel,
 * pedals"; a guess here is the per-model table this project has twice
 * refused to build. */
static void device_what(unsigned i, char *out, size_t out_size)
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
    if (at < out_size && i < INPUT_PROBE_MAX_DEVICES && s_dev_active[i] &&
        (DWORD)(GetTickCount() - s_dev_active[i]) < DEVICE_ACTIVE_MS)
        snprintf(out + at, out_size - at, "   <- moving");
}

/* A group's heading, and the one button that belongs to it, right-aligned and
 * never off the page: every constant here is a maximum. */
/* A group's heading and the one button belonging to it: right-aligned beside
 * the heading where there is room, on its own line under it where there is not;
 * at 320 units the heading was ellipsised to make room. Every constant is a
 * maximum, which here was not sufficient: two things sharing a line need a rule
 * for when they stop sharing it. */
static int group_header(int y, const char *text, int button_id,
                        const char *button_text, int button_w, int enabled)
{
    int width = page_width();
    int right = width - 20;
    int stacked = button_id && width < 400;
    int x = right - min(button_w, max(90, width - 200));
    HWND button;

    if (button_id) {
        button_w = stacked ? min(button_w, width - 40) : right - x;
        button = make_control(0, "BUTTON", button_text,
                              BS_PUSHBUTTON | WS_TABSTOP,
                              stacked ? 20 : x, stacked ? y + 22 : y - 3,
                              button_w, 26, s_page, button_id);
        EnableWindow(button, enabled);
    }
    make_control(0, "STATIC", text, SS_LEFT | SS_ENDELLIPSIS, 20, y,
                 (button_id && !stacked ? x - 30 : right - 20), 20, s_page, 0);
    return y + (stacked ? 52 : 26);
}

static int muted_line(int y, const char *text, int id)
{
    int width = page_width();
    int h = text_block_height(theme_font(), text, width - 40);

    make_control(0, "STATIC", text, SS_LEFT, 20, y, width - 40, h, s_page, id);
    return y + h + 6;
}

/* One row per axis the controller reports, RAW. Behind its own disclosure and
 * headed with what it is: the Driving bars are the map's answer and these
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
                     name_w - 18, 18, s_page, IDC_AXIS_NAME_BASE + (int)i);
        make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, bar_x, y, bar_w, 16,
                     s_page, IDC_AXIS_BAR_BASE + (int)i);
        SendDlgItemMessage(s_page, IDC_AXIS_BAR_BASE + (int)i,
                           PBM_SETRANGE32, 0, 1000);
        make_control(0, "STATIC", "", SS_LEFT, value_x, y + 2, value_w, 18,
                     s_page, IDC_AXIS_VALUE_BASE + (int)i);
    }
    make_control(0, "STATIC", "Buttons down: none", SS_LEFT | SS_ENDELLIPSIS,
                 38, y + 4, width - 58, 20, s_page, IDC_LIVE_BUTTONS);
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

static int render_device_group(int y)
{
    unsigned i;
    int width = page_width();
    char text[256];

    y = group_header(y, "YOUR CONTROLLERS", IDC_PROBE_REFRESH, "Rescan", 92, 1);
    /* What the tick means, above the list it is about. Steering, pedals and
     * buttons can come from any controller; the tick picks the one that gets
     * the force feedback. */
    y = muted_line(y, devices_pad_only() ?
                      "Select your controller. It gets the rumble." :
                      "Select your wheel. It gets the force feedback.", 0);
    if (!input_probe_count()) {
        y = muted_line(y, "No controller detected. Plug one in and press "
                          "Rescan; the keyboard works either way.", 0);
        return y + 6;
    }
    for (i = 0; i < input_probe_count(); i++) {
        const input_probe_device_t *d = input_probe_device(i);
        HWND radio;

        if (!device_listed(i))
            continue;
        radio = make_control(0, "BUTTON", d->name,
            BS_AUTORADIOBUTTON | WS_TABSTOP | (y ? 0 : WS_GROUP),
            20, y, width - 40, 22, s_page, IDC_DEVICE_BASE + (int)i);
        /* The controller the host will use as the main one, which is not
         * necessarily the one the file pins: the shipped `provider = auto`
         * pins nothing and the host still picks one. Clicking another row
         * pins it. */
        SendMessage(radio, BM_SETCHECK,
                    (int)i == s_probe_selected ? BST_CHECKED : BST_UNCHECKED,
                    0);
        y += 21;
        device_what(i, text, sizeof text);
        make_control(0, "STATIC", text, SS_LEFT | SS_ENDELLIPSIS, 38, y,
                     width - 58, 18, s_page, IDC_DEVICE_CAP_BASE + (int)i);
        y += 22;
    }
    device_problem_text(text, sizeof text);
    y = *text ? muted_line(y + 2, text, 0) : y + 2;
    make_control(0, "BUTTON",
        device_winmm_hidden() ?
            "Also show the legacy joystick view of these controllers" :
            "Legacy joystick view",
        BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 20, y, width - 40, 22,
        s_page, IDC_SHOW_LEGACY);
    SendDlgItemMessage(s_page, IDC_SHOW_LEGACY, BM_SETCHECK,
                       s_show_legacy ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 30;
    /* The raw table is the selected controller's, so it sits under the list
     * that selects it. */
    if (s_probe_selected < 0)
        return y;
    make_control(0, "BUTTON", "Show everything the selected controller reports",
                 BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 20, y, width - 40,
                 22, s_page, IDC_SHOW_RAW);
    SendDlgItemMessage(s_page, IDC_SHOW_RAW, BM_SETCHECK,
                       s_show_raw ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 26;
    if (s_show_raw)
        y = render_axis_table(y);
    return y + 4;
}

/* Set up's button, whose words follow what pressing it would do. */
static const char *setup_button_text(void)
{
    return capture_wizard_running() ? "Skip step" : "Set up...";
}

static int render_driving_group(int y)
{
    static const char *const k_role[3] = { "Steering", "Accelerator", "Brake" };
    const input_probe_device_t *d = s_probe_selected >= 0 ?
        input_probe_device((unsigned)s_probe_selected) : NULL;
    int width = page_width();
    int right = width - 20;
    int label_w = min(104, max(70, (width - 40) / 3));
    int src_x = 20 + label_w + 8;
    int src_w = min(120, max(60, (right - src_x) / 3));
    int bar_x = src_x + src_w + 8;
    int bar_w = right - bar_x;
    unsigned i;

    y = group_header(y, "DRIVING - Both games", IDC_SETUP_AXES,
                     setup_button_text(), 108, d != NULL);
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
                     s_page, 0);
        if (src_w)
            make_control(0, "STATIC", "", SS_LEFT | SS_ENDELLIPSIS, src_x,
                         y + 2, src_w, 20, s_page,
                         IDC_LIVE_ROLE_BASE + (int)i);
        make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, bar_x, y, bar_w, 18,
                     s_page, IDC_LIVE_STEER + (int)i);
        SendDlgItemMessage(s_page, IDC_LIVE_STEER + (int)i, PBM_SETRANGE32,
                           0, 1000);
        y += 26;
    }
    /* Two controls, not one string with a "\r\n": text_block_height() says
     * why. place_note_lines() sizes and stacks them. */
    {
        HFONT font = theme_note_font();
        const char *first = capture_note() ? capture_note() :
                            s_note_last[0] ? s_note_last :
                            "Press a pedal: the bar that answers is the one "
                            "it is on. A pedal always fills its bar as you "
                            "press it.";
        HWND line;

        s_note_w = width - 40;
        s_note_text_h = note_text_height(first, capture_note_say());
        line = make_control(0, "STATIC", first, SS_LEFT, 20, y + NOTE_GAP,
                            s_note_w, 0, s_page, IDC_LIVE_NOTE);
        SendMessage(line, WM_SETFONT, (WPARAM)font, TRUE);
        line = make_control(0, "STATIC", capture_note_say(), SS_LEFT, 20,
                            y + NOTE_GAP, s_note_w, 0, s_page, IDC_LIVE_SAY);
        SendMessage(line, WM_SETFONT, (WPARAM)font, TRUE);
        place_note_lines();
        y += NOTE_GAP + s_note_text_h + 6;
    }
    return y + 4;
}

static int render_button_group(int y)
{
    int width = page_width();
    int right = width - 20;
    /* TWO BUTTONS NOW, so the row fits on one line again at every width the
     * three-button form had to stack at. */
    int stacked = width < 470;
    int label_w = min(132, max(90, (width - 40) / 3));
    int add_w, clear_w, value_x, add_x, clear_x, value_w;
    unsigned i;
    char head[64];

    add_w = min(58, max(46, (width - 40) / 8));
    clear_w = min(62, max(48, (width - 40) / 7));
    clear_x = right - clear_w;
    add_x = clear_x - 6 - add_w;
    if (stacked) {
        /* The label takes line one and the value shares line two with the two
         * buttons: the label is the short thing here, so it is the one that
         * gets a line to itself. */
        value_x = 20;
        value_w = max(add_x - 10 - value_x, 70);
    } else {
        value_x = 20 + label_w + 8;
        value_w = max(add_x - 10 - value_x, 70);
    }
    snprintf(head, sizeof head, "BUTTONS - %s",
             s_binding_title ? "Offroad Thunder" : "Hydro Thunder");
    y = group_header(y, head, 0, NULL, 0, 0);
    {
        /* WHICH CABINET'S MAP IS BELOW, the same shape as the Controls tabs
         * above it, because it is the same question: two subpages, one list. */
        static const int ids[] = { IDC_TITLE_HYDRO, IDC_TITLE_OFFROAD };
        static const char *const labels[] = { "Hydro Thunder",
                                              "Offroad Thunder" };

        y = add_tab_row(y, 22, 150, labels, ids, 2) + 4;
    }
    /* THE TWO CABINETS DO NOT SHARE A CONTROL PANEL and the file says so: six
     * of the fifteen shared actions have a different default on each, and
     * Offroad has five controls Hydro does not have at all. A "both games"
     * column would therefore be six rows reading "differs" and a shared line
     * that BEATS the per-title default rather than losing to it. One cabinet
     * at a time is what the file is. */
    y = muted_line(y, "Press Add and press a button to bind.", 0);
    for (i = 0; i < ACTION_COUNT; i++) {
        const action_desc_t *a = &k_actions[i];
        const char *label;
        char text[288];
        int row_y = stacked ? y + 22 : y;

        if (!action_visible(i))
            continue;
        label = s_binding_title ? a->offroad_label : a->hydro_label;
        make_control(0, "STATIC", label ? label : a->key,
                     SS_LEFT | SS_ENDELLIPSIS, 20, y + 4,
                     stacked ? width - 40 : label_w, 20, s_page, 0);
        binding_row_text(s_binding_title, i, text, sizeof text);
        make_control(WS_EX_CLIENTEDGE, "EDIT", text,
                     ES_READONLY | ES_AUTOHSCROLL, value_x, row_y, value_w, 24,
                     s_page, IDC_BIND_TEXT_BASE + (int)i);
        make_control(0, "BUTTON", "Add", BS_PUSHBUTTON | WS_TABSTOP,
                     add_x, row_y, add_w, 24, s_page,
                     IDC_BIND_ADD_BASE + (int)i);
        make_control(0, "BUTTON", "Clear", BS_PUSHBUTTON | WS_TABSTOP,
                     clear_x, row_y, clear_w, 24, s_page,
                     IDC_BIND_CLEAR_BASE + (int)i);
        y += stacked ? 52 : 28;
    }
    return y + 6;
}

static void render_controls_panel(void)
{
    int y = page_header_bottom(), top;

    /* THREE TOPICS, THREE CARDS. The disclosure below them is not one: it is
     * the door to the rest of this page, not a thing on it. */
    top = y;
    y = frame_topic(top, render_device_group(y));
    top = y;
    y = frame_topic(top, render_driving_group(y));
    top = y;
    y = frame_topic(top, render_button_group(y));
    /* 8 more than the gap between two cards: the settings rows get a card of
     * their own, and its top line would otherwise land exactly on the last
     * group's bottom line. */
    s_panel_bottom = y + 8;
}

/* WHAT THE GAME WILL ACTUALLY DO WITH THIS CONTROLLER, which is not what the
 * Test button proves. Test drives the DEVICE: it commands a DirectInput effect
 * or an XInput vibration and says nothing about whether the host will ever ask
 * for one, so a pad with `[ffb] rumble = false` buzzes on the bench and stays
 * dead in the race.
 *
 * The three cases are the three sinks. `[ffb] enabled` gates the wheel's motor
 * (dinput.c: `s_have_ffb && s_cfg.ffb_enabled`); `[ffb] rumble` gates the
 * derived pad model; and **the rumble sink is XInput and only XInput**
 * (io_host_rumble), so a controller that is neither an XInput device nor a
 * DirectInput one publishing effects has nowhere for either to go.
 *
 * It reads the MODEL, so it answers to what the page is showing rather than to
 * what the file last said, and it follows the selected controller for the same
 * reason the bars above do. */
static const char *ffb_reaches_device(const input_probe_device_t *d)
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

static void render_ffb_panel(void)
{
    const input_probe_device_t *d = s_probe_selected >= 0 ?
        input_probe_device((unsigned)s_probe_selected) : NULL;
    int y = page_header_bottom(), top = y;
    const char *gap = ffb_reaches_device(d);
    char label[MAX_PATH + 48];

    snprintf(label, sizeof label, "FORCE FEEDBACK - %s",
             d ? probe_product_name(d) : "no controller selected");
    y = group_header(y, label, IDC_FFB_TEST, "Test", 108,
                     d && d->force_feedback);
    y = muted_line(y, "Test will throw a shake event to your wheel/pad.", 0);
    if (gap)
        y = muted_line(y, gap, 0);
    s_panel_bottom = frame_topic(top, y) + 8;
}

static void render_settings_page(void)
{
    unsigned i;

    if (page_is_controls(s_page_index))
        add_controls_header();
    else
        add_page_header();
    if (s_page_index == PAGE_CONTROLS)
        render_controls_panel();
    else if (s_page_index == PAGE_FFB)
        render_ffb_panel();
    else if (s_page_index == PAGE_LINK)
        render_link_panel();
    else if (s_page_index == PAGE_TROUBLE)
        render_trouble_panel();
    for (i = 0; i < SETTING_COUNT; i++)
        if (setting_on_page(i))
            render_setting(i);
    {
        int rows = 0, top = page_settings_top(), bottom = top;
        for (i = 0; i < SETTING_COUNT; i++)
            if (setting_on_page(i)) {
                bottom += setting_row_height(i);
                rows++;
            }
        /* THE ROWS ARE A TOPIC TOO, and on Graphics, Audio, Game System and
         * Files they are the only one the page has. The disclosure above them
         * stays outside it, as on the Controls page. */
        if (rows)
            frame_topic(top, bottom);
        s_content_height = bottom + 20;
    }
}

static void pull_page(void)
{
    unsigned i;
    /* The binding rows are read-only displays and the model holds both
     * halves (launcher_model.h), so there is nothing on the Controls page to
     * pull back except its settings rows, which the loop below walks by id. */
    if (!s_page || s_page_index == PAGE_HOME)
        return;
    for (i = 0; i < SETTING_COUNT; i++) {
        HWND c;
        if (k_settings[i].page != s_page_index)
            continue;
        c = GetDlgItem(s_page, IDC_SETTING_BASE + (int)i);
        if (!c)
            continue;
        if (k_settings[i].type == ST_BOOL)
            snprintf(model_slot(i), INI_VALUE_CAP, "%s",
                     SendMessage(c, BM_GETCHECK, 0, 0) == BST_CHECKED ?
                     "true" : "false");
        else if (scale_row(i))
            scale_combo_value(c, model_slot(i), INI_VALUE_CAP);
        else if (k_settings[i].type == ST_MHZ_UINT) {
            char mhz[INI_VALUE_CAP], *end;
            unsigned long value;
            GetWindowTextA(c, mhz, sizeof mhz);
            errno = 0;
            value = strtoul(mhz, &end, 10);
            if (errno != ERANGE && end != mhz && !*end &&
                value >= 1 && value <= 4000)
                snprintf(model_slot(i), INI_VALUE_CAP, "%lu",
                         value * 1000000ul);
            else
                snprintf(model_slot(i), INI_VALUE_CAP, "%s", mhz);
        } else if (k_settings[i].type == ST_ENUM) {
            /* THE VALUE, NOT THE LABEL: an annotated axis item shows the
             * driver's name beside the ini's, and GetWindowText would put that
             * whole string into the file. The selection carries its choice's
             * index; anything else falls back to the text. */
            LRESULT sel = SendMessage(c, CB_GETCURSEL, 0, 0);
            LRESULT data = sel == CB_ERR ? CB_ERR :
                SendMessage(c, CB_GETITEMDATA, (WPARAM)sel, 0);
            char value[INI_VALUE_CAP];

            if (data != CB_ERR && k_settings[i].choices &&
                choice_at(k_settings[i].choices, (int)data, value, sizeof value))
                snprintf(model_slot(i), INI_VALUE_CAP, "%s", value);
            else
                GetWindowTextA(c, model_slot(i), INI_VALUE_CAP);
        } else
            GetWindowTextA(c, model_slot(i), INI_VALUE_CAP);
    }
    /* A role moved to another controller takes its facts with it; refresh
     * the rows that show them, or the next pull reads stale values back. */
    model_sync_role_facts(update_model_control);
}

static LRESULT CALLBACK card_proc(HWND h, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    if (message == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wparam, &rc, theme_page_brush());
        return TRUE;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT rc;
        HDC dc = BeginPaint(h, &paint);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, theme_page_brush());
        FrameRect(dc, &rc, theme_border_brush());
        EndPaint(h, &paint);
        return 0;
    }
    return DefWindowProcA(h, message, wparam, lparam);
}

/* MOVE THE PAGE TO AN ABSOLUTE OFFSET, in pixels, clamped to what the content
 * actually is. Two callers: the page's own WM_VSCROLL, and render_page()
 * putting a rebuilt page back where the reader had it. */
static void set_page_scroll(HWND h, int next)
{
    RECT rc;
    int old = s_page_scroll, limit;

    if (!h)
        return;
    GetClientRect(h, &rc);
    limit = scale_px(s_content_height) - (rc.bottom - rc.top);
    if (next > limit)
        next = limit;
    if (next < 0)
        next = 0;
    if (next == old)
        return;
    SendMessage(h, WM_SETREDRAW, FALSE, 0);
    s_page_scroll = next;
    ScrollWindowEx(h, 0, old - next, NULL, NULL, NULL, NULL,
                   SW_SCROLLCHILDREN);
    SetScrollPos(h, SB_VERT, next, TRUE);
    SendMessage(h, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE |
                 RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static LRESULT CALLBACK page_proc(HWND h, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    if (message == WM_DRAWITEM && draw_dark_button((DRAWITEMSTRUCT *)lparam))
        return TRUE;
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN) {
        LRESULT colour = control_colour(1, message, wparam, lparam);
        if (colour)
            return colour;
    }
    if (message == WM_COMMAND) {
        SendMessage(GetParent(h), message, wparam, lparam);
        return 0;
    }
    if (message == WM_VSCROLL) {
        SCROLLINFO si;
        RECT rc;
        int next = s_page_scroll;

        GetClientRect(h, &rc);
        switch (LOWORD(wparam)) {
        case SB_LINEUP: next -= scale_px(28); break;
        case SB_LINEDOWN: next += scale_px(28); break;
        case SB_PAGEUP: next -= rc.bottom - rc.top; break;
        case SB_PAGEDOWN: next += rc.bottom - rc.top; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            memset(&si, 0, sizeof si);
            si.cbSize = sizeof si;
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, SB_VERT, &si);
            next = si.nTrackPos;
            break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = scale_px(s_content_height); break;
        default: return 0;
        }
        set_page_scroll(h, next);
        return 0;
    }
    if (message == WM_MOUSEWHEEL) {
        int lines = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        while (lines > 0) {
            SendMessage(h, WM_VSCROLL, SB_LINEUP, 0);
            lines--;
        }
        while (lines < 0) {
            SendMessage(h, WM_VSCROLL, SB_LINEDOWN, 0);
            lines++;
        }
        return 0;
    }
    return DefWindowProcA(h, message, wparam, lparam);
}

/* THE BAR REPORTS WHERE THE PAGE IS; IT DOES NOT DECIDE IT. Setting
 * `s_page_scroll = 0` here while leaving the children where they were made the
 * variable and the screen disagree. layout_window() ends here and every
 * render_page() is followed by one, so a restored offset was thrown away a call
 * later: the cabinet tab kept its place on the first press and jumped to the
 * top on the second.
 *
 * The one thing this may MOVE is a page no longer reaching as far as the offset
 * it holds, and it moves it through set_page_scroll() like everything else. */
static void update_scrollbar(void)
{
    RECT rc;
    SCROLLINFO si;

    if (!s_page)
        return;
    GetClientRect(s_page, &rc);
    set_page_scroll(s_page, s_page_scroll);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = max(scale_px(s_content_height) - 1, 0);
    si.nPage = (UINT)(rc.bottom - rc.top);
    si.nPos = s_page_scroll;
    SetScrollInfo(s_page, SB_VERT, &si, TRUE);
    ShowScrollBar(s_page, SB_VERT,
                  scale_px(s_content_height) > rc.bottom - rc.top);
}

static void layout_window(void);

static void render_page(void)
{
    RECT rc;
    /* A REBUILD OF THE SAME PAGE KEEPS THE READER WHERE THEY WERE. This page
     * is rebuilt for things that are not a change of page, a cabinet tab, a
     * disclosure, a captured binding, a resize, and each threw the reader to
     * the top of a page that scrolls for most of its length. A different page
     * still starts at its top, and set_page_scroll() clamps: Offroad's list is
     * five rows longer than Hydro's. */
    int keep = s_page && s_rendered_page == s_page_index ? s_page_scroll : 0;

    s_loading = 1;
    /* REBUILT FROM THE MODEL, WITHOUT THEIR ACTIVITY MARK; this path writes
     * each control's text inline rather than through update_binding_display().
     * Forget what was lit, or a row whose input is still held keeps its bit,
     * the scanner sees no change, and the mark never returns. The next timer
     * tick re-lights whatever is down. */
    s_active_key = s_active_pad = 0;
    if (s_page) {
        pull_page();
        /* The rows just read belonged to the device that is being replaced.
         * Between the pull and the rebuild is the only moment the new
         * device's own values can be put in front of either. */
        if (s_reload_devices) {
            reload_device_settings();
            s_reload_devices = 0;
        }
        DestroyWindow(s_page);
    }
    GetClientRect(s_window, &rc);
    s_page = make_control(WS_EX_CLIENTEDGE, "VCThunderLauncherPage", "",
        WS_VSCROLL | WS_CLIPCHILDREN | WS_TABSTOP,
        s_compact ? 12 : 228, s_compact ? 40 : 12,
        unscale_px(rc.right - scale_px(s_compact ? 24 : 240)),
        unscale_px(rc.bottom - scale_px(s_compact ? 88 : 70)),
        s_window, IDC_PAGE);
    /* THE NEW PAGE'S CHILDREN ARE AT THE TOP, whatever the destroyed one held;
     * `keep` above is the only memory of that. Say so before anything reads the
     * offset, the layout_window() on the next line trusts it. */
    s_page_scroll = 0;
    /* A WM_SIZE mode switch can arrive while Windows still reports the old
     * child's rectangle. Put the new page in its final rectangle before any
     * responsive child computes page_width(). */
    layout_window();
    s_rendered_page_w = page_width();
    if (s_page_index == PAGE_HOME)
        render_home();
    else
        render_settings_page();
    update_scrollbar();
    set_page_scroll(s_page, keep);
    s_rendered_page = s_page_index;
    update_window_title();
    s_loading = 0;
}

static void layout_window(void)
{
    RECT rc;
    int margin = scale_px(12), nav_w = scale_px(204);
    int top = scale_px(s_compact ? 40 : 12);
    int footer = scale_px(s_tiny ? 36 : (s_compact ? 48 : 54));
    int button_w = scale_px(s_tiny ? 72 : 88);
    int button_h = scale_px(s_tiny ? 26 : 28);
    GetClientRect(s_window, &rc);
    if (s_compact) {
        MoveWindow(s_nav, margin, scale_px(s_tiny ? 6 : 8),
                   rc.right - margin * 2,
                   scale_px(240), TRUE);
        if (s_page)
            MoveWindow(s_page, margin, top, rc.right - margin * 2,
                       rc.bottom - top - footer, TRUE);
    } else {
        MoveWindow(s_nav, margin, top, nav_w, rc.bottom - top - footer, TRUE);
        if (s_page)
            MoveWindow(s_page, margin + nav_w + scale_px(12), top,
                       rc.right - margin * 2 - nav_w - scale_px(12),
                       rc.bottom - top - footer, TRUE);
    }
    ShowWindow(s_status, s_tiny ? SW_HIDE : SW_SHOW);
    if (!s_tiny)
        MoveWindow(s_status, margin, rc.bottom - scale_px(38),
                   rc.right - margin * 2 - button_w * 2 - scale_px(16),
                   scale_px(24), TRUE);
    MoveWindow(s_apply, rc.right - margin - button_w * 2 - scale_px(8),
               rc.bottom - scale_px(s_tiny ? 32 : 42),
               button_w, button_h, TRUE);
    MoveWindow(s_close, rc.right - margin - button_w,
               rc.bottom - scale_px(s_tiny ? 32 : 42),
               button_w, button_h, TRUE);
    update_scrollbar();
}

static void compact_selected_path(char *path, size_t size)
{
    char cwd[MAX_PATH];
    size_t n;
    if (!GetCurrentDirectoryA(sizeof cwd, cwd))
        return;
    n = strlen(cwd);
    if (!_strnicmp(path, cwd, n) && (path[n] == '\\' || path[n] == '/'))
        memmove(path, path + n + 1, strlen(path + n + 1) + 1);
    path[size - 1] = '\0';
}

static int browse_directory(HWND owner, char *path, size_t path_size)
{
    BROWSEINFOA bi;
    LPITEMIDLIST item;
    char selected[MAX_PATH];
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int uninitialise = SUCCEEDED(com);

    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner;
    bi.lpszTitle = "Select folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    item = SHBrowseForFolderA(&bi);
    if (!item) {
        if (uninitialise)
            CoUninitialize();
        return 0;
    }
    if (!SHGetPathFromIDListA(item, selected)) {
        CoTaskMemFree(item);
        if (uninitialise)
            CoUninitialize();
        return 0;
    }
    CoTaskMemFree(item);
    if (uninitialise)
        CoUninitialize();
    compact_selected_path(selected, sizeof selected);
    snprintf(path, path_size, "%s", selected);
    return 1;
}

static int browse_file(HWND owner, char *path, size_t path_size, int open_file)
{
    OPENFILENAMEA ofn;
    char selected[MAX_PATH];

    snprintf(selected, sizeof selected, "%.*s",
             (int)(sizeof selected - 1), path);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFile = selected;
    ofn.nMaxFile = sizeof selected;
    ofn.lpstrFilter = "All files\0*.*\0\0";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                (open_file ? OFN_FILEMUSTEXIST : OFN_OVERWRITEPROMPT);
    if (!(open_file ? GetOpenFileNameA(&ofn) : GetSaveFileNameA(&ofn)))
        return 0;
    compact_selected_path(selected, sizeof selected);
    snprintf(path, path_size, "%s", selected);
    return 1;
}

static void browse_setting(unsigned index)
{
    const setting_desc_t *d;
    char selected[INI_VALUE_CAP];
    HWND control;

    if (index >= SETTING_COUNT)
        return;
    d = &k_settings[index];
    snprintf(selected, sizeof selected, "%s", model_slot(index));
    if (d->flags & SF_DIR) {
        if (!browse_directory(s_window, selected, sizeof selected))
            return;
    } else if (!browse_file(s_window, selected, sizeof selected,
                            (d->flags & SF_OPEN_FILE) != 0)) {
        return;
    }
    snprintf(model_slot(index), INI_VALUE_CAP, "%s", selected);
    control = GetDlgItem(s_page, IDC_SETTING_BASE + (int)index);
    if (control)
        SetWindowTextA(control, selected);
    mark_dirty();
}

int probe_backend_matches_setting(const input_probe_device_t *device,
                                         const char *key)
{
    if (!device)
        return 0;
    if (!strncmp(key, "dinput_", 7)) return device->backend == INPUT_PROBE_DINPUT;
    if (!strncmp(key, "xinput_", 7)) return device->backend == INPUT_PROBE_XINPUT;
    if (!strncmp(key, "winmm_", 6)) return device->backend == INPUT_PROBE_WINMM;
    return 0;
}



void update_model_control(const char *section, const char *key)
{
    int index = setting_find(section, key);
    HWND c;
    if (index < 0 || !s_page || k_settings[index].page != s_page_index)
        return;
    c = GetDlgItem(s_page, IDC_SETTING_BASE + index);
    if (!c)
        return;
    if (k_settings[index].type == ST_BOOL)
        SendMessage(c, BM_SETCHECK,
                    model_bool(section, key) ? BST_CHECKED : BST_UNCHECKED, 0);
    else if (k_settings[index].type == ST_ENUM)
        combo_select_value(c, k_settings[index].choices, model_slot(index));
    else if (scale_row((unsigned)index))
        scale_combo_fill(c, (unsigned)index);
    else
        SetWindowTextA(c, model_slot(index));
    if (!scale_row((unsigned)index))
        scale_combos_relabel();
}

/* Write one value into the model and refresh whatever control shows it.
 *
 * setting_find() answers -1 for a key settings.def does not carry, so the call
 * sites below must not hand it to model_slot() unchecked: a key removed from
 * the table would turn selecting a controller into a write before the array. */
void model_set(const char *section, const char *key, const char *value)
{
    int i = setting_find(section, key);

    if (i < 0) {
        /* The status bar, not the log: this window runs before log_open(), so
         * a LOGW here would go nowhere a user could read it. */
        set_status("This build has no setting for the selected device; it was "
                   "selected but nothing was written to the file.");
        return;
    }
    snprintf(model_slot(i), INI_VALUE_CAP, "%s", value);
    update_model_control(section, key);
    model_sync_role_facts(update_model_control);
}

/* A LABEL IS DERIVED FROM THE PRODUCT NAME AND THEN SHOWN, never inferred and
 * kept quiet. The first word is the brand: "Thrustmaster TMX Racing Wheel"
 * gives `thrustmaster`. Capped at 12 so a collision suffix still fits inside
 * ini_label_ok()'s 15; a name with no letters or digits falls back to the
 * provider, because an empty label binds nothing. */
static void derive_label(const char *name, const char *provider,
                         char *out, size_t out_size)
{
    size_t n = 0;
    const char *p;

    for (p = name; *p && n + 1 < out_size && n < 12; p++) {
        if (isalnum((unsigned char)*p))
            out[n++] = (char)tolower((unsigned char)*p);
        else if (n)
            break;
    }
    out[n] = '\0';
    if (!n)
        snprintf(out, out_size, "%s", provider);
}

/* Bind a label to this device in [devices] and hand it back: an existing line
 * that opens it, whatever it says, or a new one model_device_label_ensure()
 * has proved. 0 when no line can name it. */
static int ensure_device_entry(const input_probe_device_t *d,
                               char *out, size_t out_size)
{
    ini_device_id_t ids[INPUT_PROBE_MAX_DEVICES];
    char base[INI_LABEL_CAP];
    unsigned count;
    int which;

    if (!d || (which = probe_id_of(d, ids, &count)) < 0)
        return 0;
    /* THE PRODUCT NAME, NOT THE ROW: input_probe_device()->name is prefixed
     * with the backend, so deriving from it gave every DirectInput device the
     * label `directinput`, a label naming the API rather than the
     * controller, which is the one thing it is for. */
    derive_label(probe_product_name(d), probe_provider(d), base, sizeof base);
    return model_device_label_ensure(probe_provider(d), ids, count,
                                     (unsigned)which, base, out, out_size);
}

/* A [devices] label for a controller, so a button or an axis on it can be
 * named `<label>:<n>`. DirectInput controllers and XInput pads only: a WinMM
 * view is the same device one of those lists. */
int ensure_device_label(const input_probe_device_t *d,
                               char *out, size_t out_size)
{
    if (!d || d->backend == INPUT_PROBE_WINMM)
        return 0;
    return ensure_device_entry(d, out, out_size);
}

static void adopt_probe_device(unsigned index)
{
    const input_probe_device_t *d = input_probe_device(index);
    probe_adopt_t plan;
    char text[MAX_PATH + 96];

    if (!probe_adopt_plan(d, &plan)) {
        set_status("That controller could not be selected.");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (input_probe_select(index))
        s_probe_selected = (int)index;
    if (!probe_adopt_pending(&plan)) {
        /* Already the one the file names: a re-render, not a write. Selecting
         * is how a person LOOKS at something, and writing on the way past is
         * what costs a working wheel its identity. */
        request_relayout();
        return;
    }
    model_set("analog", "provider", plan.provider);
    if (plan.name_key)
        model_set("analog", plan.name_key, plan.name);
    if (plan.index_key)
        model_set("analog", plan.index_key, plan.index);
    /* R7: THIS DEVICE'S SETTINGS ARE THIS DEVICE'S. Binding a label in
     * [devices] gives it a section of its own, and re-resolving puts ITS axis
     * map on the page, so adopting a second controller does not quietly
     * inherit the first one's values and then overwrite them. */
    ensure_device_entry(d, NULL, 0);
    resolve_profile();
    /* AND THE REREAD IS DEFERRED, because the rebuild below PULLS THE PAGE IT
     * IS ABOUT TO DESTROY: render_page() calls pull_page() first; it has to,
     * or a rebuild throws away what the user typed, and the rows on screen
     * are the OUTGOING device's. Re-reading here put the old wheel's axis map
     * back into the model, and Apply wrote it into the shared [analog] block
     * under the new device's name. */
    s_reload_devices = 1;
    /* mark_dirty() writes the status line itself, so it goes first and the
     * sentence that says what happened goes after it. */
    mark_dirty();
    snprintf(text, sizeof text, "%s gets the %s. Press Apply to write that to "
             "vcthunder.ini.", probe_product_name(d),
             d->backend == INPUT_PROBE_XINPUT ? "rumble" : "force feedback");
    set_status(text);
    request_relayout();
}

static float probe_axis_value(const char *name, int *found)
{
    unsigned i;
    for (i = 0; i < s_probe_state.axis_count; i++)
        if (!_stricmp(s_probe_state.axis_name[i], name)) {
            *found = 1;
            return s_probe_state.axis[i];
        }
    *found = 0;
    return 0.0f;
}

/* EVERY ATTACHED CONTROLLER, POLLED EVERY TICK, so a binding can be captured
 * from whatever the user presses and a row can light for whichever controller
 * holds it. One GetDeviceState per device at 30 Hz, only while a Controls page
 * is on screen. */
static input_probe_state_t s_all[INPUT_PROBE_MAX_DEVICES];
static int s_all_ok[INPUT_PROBE_MAX_DEVICES];

static void poll_every_device(void)
{
    unsigned i, n = input_probe_count();

    if (n > INPUT_PROBE_MAX_DEVICES)
        n = INPUT_PROBE_MAX_DEVICES;
    for (i = 0; i < n; i++) {
        int changed = 0;
        unsigned w;

        s_all_ok[i] = input_probe_poll_device(i, &s_all[i]);
        if (!s_all_ok[i])
            continue;
        for (w = 0; w < 4; w++)
            if (s_all[i].buttons[w] != s_dev_buttons[i][w])
                changed = 1;
        /* A CHANGE, NOT A STATE: a controller with a button taped down would
         * read `moving` for ever, which is the opposite of what this mark is
         * for. */
        if (changed && s_dev_seen[i])
            s_dev_active[i] = GetTickCount();
        for (w = 0; w < 4; w++)
            s_dev_buttons[i][w] = s_all[i].buttons[w];
        s_dev_seen[i] = 1;
    }
    for (; i < INPUT_PROBE_MAX_DEVICES; i++)
        s_all_ok[i] = 0;
}

/* ---- what the capture machine asks this window --------------------------
 *
 * src/launcher_capture.c holds Add, Detect and Set up, and none of its fields
 * are reachable from here on purpose (launcher_capture.h). These are the
 * reads it still needs: what is ticked, what it is doing this tick, and where
 * to put a sentence. Everything else it does, it does to the model. */

HWND launcher_window(void)
{
    return s_window;
}

/* THE LINE UNDER THE LIVE BARS, which exists only while the Controls page is
 * built. A capture writes to it through here rather than holding s_page,
 * because a page it wrote to while another was on screen is a control that
 * has been destroyed. */
int launcher_note_visible(void)
{
    return s_page && s_page_index == PAGE_CONTROLS;
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
    one = GetDlgItem(s_page, IDC_LIVE_NOTE);
    two = GetDlgItem(s_page, IDC_LIVE_SAY);
    if (!one || !two)
        return;
    GetWindowRect(one, &r1);
    GetWindowRect(two, &r2);
    MapWindowPoints(NULL, s_page, (POINT *)&r1, 2);
    MapWindowPoints(NULL, s_page, (POINT *)&r2, 2);
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
    GetDlgItemTextA(s_page, IDC_LIVE_NOTE, now1, sizeof now1);
    GetDlgItemTextA(s_page, IDC_LIVE_SAY, now2, sizeof now2);
    snprintf(s_note_last, sizeof s_note_last, "%s", first);
    if (!strcmp(now1, first) && !strcmp(now2, second))
        return;
    SetDlgItemTextA(s_page, IDC_LIVE_NOTE, first);
    SetDlgItemTextA(s_page, IDC_LIVE_SAY, second);
    place_note_lines();
}

int launcher_binding_title(void)
{
    return s_binding_title;
}

int launcher_probe_selected(void)
{
    return s_probe_selected;
}

int launcher_probe_poll(void)
{
    return input_probe_poll(&s_probe_state);
}

const input_probe_state_t *launcher_probe_state(void)
{
    return &s_probe_state;
}

/* NULL for a device that did not answer this tick, so a caller cannot read a
 * stale button mask by forgetting to test the flag beside it. */
const input_probe_state_t *launcher_device_state(unsigned index)
{
    if (index >= INPUT_PROBE_MAX_DEVICES || !s_all_ok[index])
        return NULL;
    return &s_all[index];
}

/* WHICH ATTACHED CONTROLLER A [devices] LABEL NAMES, by the predicate the host
 * resolves it with, so a row lights for the controller the game would read and
 * not one with a similar name. -1 when the file names a device that is not
 * plugged in, which is a real and common state. */
static int device_for_label(const char *label)
{
    return probe_row_for_label(label);
}

/* Is this reference held right now, on the controller it names? An unlabelled
 * one means the controller that drives the car. */
static int pad_ref_down(const char *label, unsigned ordinal)
{
    int index = label[0] ? device_for_label(label) : s_probe_selected;

    if (index < 0 || index >= INPUT_PROBE_MAX_DEVICES || !s_all_ok[index])
        return 0;
    return io_button_down(s_all[index].buttons, ordinal);
}

/* Light the binding rows whose input is held, so a numbered wheel button can be
 * identified by pressing it. No mode to enter: it is true while the control is
 * held, for every controller.
 *
 * ONLY THE ROWS THAT CHANGED ARE REWRITTEN: twenty SetWindowTextA calls thirty
 * times a second is a flickering page, and the masks make a quiet tick cost one
 * comparison. The keyboard half is gated on this window being foreground
 * because GetAsyncKeyState is process-agnostic, the same trap
 * `input_background` documents for the host.
 *
 * It cannot corrupt a binding: these controls are display-only and pull_page()
 * reads none of them. */
static void refresh_binding_activity(void)
{
    uint32_t key_now = 0, pad_now = 0;
    static int lit_title = -1;
    unsigned i;

    if (s_page_index != PAGE_CONTROLS || !s_page) {
        /* The controls are gone; forget what was lit, or the next render of
         * this page inherits marks for rows nothing is holding. */
        s_active_key = s_active_pad = 0;
        lit_title = -1;
        return;
    }
    if (lit_title != s_binding_title) {
        s_active_key = s_active_pad = 0;      /* the other cabinet's rows */
        lit_title = s_binding_title;
    }
    for (i = 0; i < ACTION_COUNT; i++) {
        if (!action_visible(i))
            continue;
        if (GetForegroundWindow() == s_window) {
            /* ANY OF THE KEYS THE BINDING NAMES, because [digital] is a
             * list: io_config_key_code() takes ONE name, so a value with two
             * in it parsed as nothing and the row never lit. */
            char item[64];
            unsigned e;

            for (e = 0; ini_list_item(model_key_binding(s_binding_title, i), e,
                                      item, sizeof item); e++) {
                int vk = io_config_key_code(item);
                if (vk && (GetAsyncKeyState(vk) & 0x8000)) {
                    key_now |= 1u << i;
                    break;
                }
            }
        }
        if (k_actions[i].has_pad) {
            char item[64], label[INI_LABEL_CAP];
            unsigned e, ordinal;

            for (e = 0; ini_list_item(model_pad_binding(s_binding_title, i), e,
                                      item, sizeof item); e++)
                if (ini_parse_pad_ref(item, label, sizeof label, &ordinal) &&
                    ordinal && pad_ref_down(label, ordinal)) {
                    pad_now |= 1u << i;
                    break;
                }
        }
    }
    if (key_now == s_active_key && pad_now == s_active_pad)
        return;
    for (i = 0; i < ACTION_COUNT; i++) {
        uint32_t bit = 1u << i;
        int was = ((s_active_key | s_active_pad) & bit) != 0;
        int now = ((key_now | pad_now) & bit) != 0;

        s_active_key = (s_active_key & ~bit) | (key_now & bit);
        s_active_pad = (s_active_pad & ~bit) | (pad_now & bit);
        if (was != now)
            update_binding_display(i);
    }
}

/* One poll of the Detect guard, from update_live_input() at the preview's rate,
 * so every window below is in wall-clock milliseconds and not in samples: a
 * device that answers slowly must not get a shorter test.
 *
 * REFUSING IS A RESULT. Every path out either reaches a measurement the user is
 * shown and asked about, or says in words why it could not. */

/* The `moving` mark on the controller rows, rewritten only when it changes:
 * twenty-four SetWindowTextA calls thirty times a second is a flickering page.
 */
static void update_device_rows(void)
{
    static int lit[INPUT_PROBE_MAX_DEVICES];
    unsigned i, n = input_probe_count();
    DWORD now = GetTickCount();
    char text[256];

    if (n > INPUT_PROBE_MAX_DEVICES)
        n = INPUT_PROBE_MAX_DEVICES;
    for (i = 0; i < n; i++) {
        int on = s_dev_active[i] &&
                 (DWORD)(now - s_dev_active[i]) < DEVICE_ACTIVE_MS;
        if (on == lit[i])
            continue;
        lit[i] = on;
        if (!GetDlgItem(s_page, IDC_DEVICE_CAP_BASE + (int)i))
            continue;
        device_what(i, text, sizeof text);
        SetDlgItemTextA(s_page, IDC_DEVICE_CAP_BASE + (int)i, text);
    }
}

/* ONE CAPTURE, EVERY CONTROLLER. The keyboard half arrives through WM_KEYDOWN
 * and the controller half through here, and which one the user pressed is what
 * decides which half of the binding moves; the page never asks.
 *
 * ARMED ONLY AFTER EVERYTHING IS RELEASED, across every device: a wheel with a
 * paddle held, or the button the user is still holding from the last capture,
 * would otherwise bind itself instantly. */
/* Is this reference already in what the capture has collected? Pressing the
 * same button twice is the easiest thing in the world to do, and `1, 1` is a
 * binding that reads as two and is one. */

/* "x (steering)" onto the missing list. */
static void missing_add(char *missing, size_t size, const char *value,
                        const char *role)
{
    size_t at = strlen(missing);

    snprintf(missing + at, size - at, "%s%s (%s)", at ? ", " : "", value,
             role);
}

/* The driving rows for a DirectInput default device, by the host's rule: each
 * role read from the controller its value names, in that controller's axis
 * names, with its facts from its section, through di_roles_sample() itself. A
 * role whose controller is missing or lacks the axis reads released and says
 * which. */
static void preview_dinput(float bar[3], char what[3][96], char *missing,
                           size_t missing_size)
{
    di_roles_t roles;
    di_slot_reading_t reading[IO_MAX_SLOTS];
    int slot_row[IO_MAX_SLOTS];
    unsigned slots = 1, r, k, a;
    float steer, drive;

    memset(&roles, 0, sizeof roles);
    memset(reading, 0, sizeof reading);
    missing[0] = '\0';
    slot_row[0] = s_probe_selected;
    for (r = 0; r < DI_ROLE_COUNT; r++) {
        di_role_t *role = &roles.role[r];
        const char *value = model_value("analog", di_role_desc[r].axis_key);
        const char *drv;
        char axis[32];
        int row, reported, ok;

        role->slot = -1;
        row = -1;
        ok = 0;
        if (ini_parse_axis_ref(value, role->label, sizeof role->label,
                               axis, sizeof axis)) {
            row = role->label[0] ? probe_row_for_label(role->label)
                                 : s_probe_selected;
            /* An XInput pad's axes have XInput's names; a controller that
             * is not attached may have either. */
            ok = row >= 0 ?
                 di_axis_parse(axis, input_probe_device((unsigned)row)->backend
                                     == INPUT_PROBE_XINPUT, &role->axis) :
                 di_axis_parse(axis, 0, &role->axis) ||
                 di_axis_parse(axis, 1, &role->axis);
        }
        if (!ok) {
            snprintf(what[r], sizeof what[r], "%s - not an axis", value);
            role->axis = DI_AXIS_NONE;
            continue;
        }
        if (row == s_probe_selected) {
            role->slot = 0;
        } else if (row >= 0) {
            for (k = 1; k < slots && slot_row[k] != row; k++)
                ;
            if (k == slots && slots < IO_MAX_SLOTS)
                slot_row[slots++] = row;
            if (k < slots)
                role->slot = (int)k;
        }
        role->invert = model_bool("analog", di_role_desc[r].invert_key);
        role->deadzone_percent = (unsigned)atoi(
            model_device_value(role->label, "dinput_deadzone_percent"));
        role->range_percent = r == DI_ROLE_STEER ?
            model_uint("analog", "dinput_steering_range_percent", 100) : 100;
        if (role->range_percent < 5)
            role->range_percent = 5;
        if (role->axis == DI_AXIS_NONE) {
            snprintf(what[r], sizeof what[r], "not used");
            continue;
        }
        if (row < 0) {
            snprintf(what[r], sizeof what[r], "%s - not connected", value);
            missing_add(missing, missing_size, value, di_role_desc[r].what);
            continue;
        }
        drv = probe_axis_label_on(row, di_axis_name(role->axis), &reported);
        if (!reported) {
            /* As validate_role(): a missing axis is none, never a constant
             * centre in a pedal sum. */
            snprintf(what[r], sizeof what[r], "%s - not on it", value);
            missing_add(missing, missing_size, value, di_role_desc[r].what);
            role->axis = DI_AXIS_NONE;
            continue;
        }
        if (drv && drv[0])
            snprintf(what[r], sizeof what[r], "%s - %s", value, drv);
        else
            snprintf(what[r], sizeof what[r], "%s", value);
    }
    roles.pedals_separate =
        _stricmp(model_value("analog", "dinput_pedal_mode"), "combined") != 0;
    if (di_roles_split_combined(&roles))
        roles.pedals_separate = 1;
    for (k = 0; k < slots; k++) {
        const input_probe_state_t *st = slot_row[k] < 0 ? NULL :
            launcher_device_state((unsigned)slot_row[k]);

        if (!st)
            continue;
        reading[k].answered = 1;
        for (a = DI_AXIS_X; a < DI_AXIS_COUNT; a++) {
            unsigned q;

            for (q = 0; q < st->axis_count; q++)
                if (st->axis_name[q] &&
                    !_stricmp(st->axis_name[q], di_axis_name((di_axis_t)a)))
                    reading[k].unit[a] = st->axis[q];
        }
    }
    di_roles_sample(&roles, reading, slots, &steer, &drive);
    bar[0] = steer;
    bar[1] = drive > 0.0f ? drive : 0.0f;
    bar[2] = drive < 0.0f ? -drive : 0.0f;
}

/* The driving rows for an XInput or WinMM device: one controller, its own
 * reader's arithmetic (io_host.c). */
static void preview_one_device(const input_probe_device_t *d, float bar[3],
                               char what[3][96], char *missing,
                               size_t missing_size)
{
    static const char *const k_role[3] = { "steering", "accelerator", "brake" };
    static const char *const k_suffix[3] = {
        "_steering_axis", "_throttle_axis", "_brake_axis"
    };
    const char *prefix = d->backend == INPUT_PROBE_XINPUT ? "xinput" : "winmm";
    const char *name[3];
    char key[64];
    float raw[3], deadzone, drive;
    int found[3], separate = 1;
    unsigned i;

    missing[0] = '\0';
    for (i = 0; i < 3; i++) {
        const char *drv;
        int reported;

        snprintf(key, sizeof key, "%s%s", prefix, k_suffix[i]);
        name[i] = model_value("analog", key);
        raw[i] = probe_axis_value(name[i], &found[i]);
        if (!found[i])
            raw[i] = 0.0f;
        if (!_stricmp(name[i], "none")) {
            snprintf(what[i], sizeof what[i], "not used");
        } else if (!found[i]) {
            snprintf(what[i], sizeof what[i], "%s: not on this one", name[i]);
            missing_add(missing, missing_size, name[i], k_role[i]);
        } else if ((drv = probe_axis_label_for(name[i], &reported)) &&
                   reported && drv[0]) {
            snprintf(what[i], sizeof what[i], "%s - %s", name[i], drv);
        } else {
            snprintf(what[i], sizeof what[i], "%s", name[i]);
        }
    }
    deadzone = (float)model_uint("analog", "deadzone_percent", 0) / 100.0f;
    bar[0] = io_deadzone_centred(raw[0], deadzone);
    snprintf(key, sizeof key, "%s_steering_invert", prefix);
    if (model_bool("analog", key))
        bar[0] = -bar[0];
    if (d->backend != INPUT_PROBE_XINPUT) {
        snprintf(key, sizeof key, "%s_pedal_mode", prefix);
        separate = _stricmp(model_value("analog", key), "combined") != 0;
    }
    if (!_stricmp(name[2], "none"))
        separate = 0;
    if (!separate) {
        int trigger = d->backend == INPUT_PROBE_XINPUT &&
            (!_stricmp(name[1], "lt") || !_stricmp(name[1], "rt"));

        drive = trigger ?
            io_deadzone_unipolar((raw[1] + 1.0f) * 0.5f, deadzone) :
            io_deadzone_centred(raw[1], deadzone);
        snprintf(key, sizeof key, "%s_throttle_invert", prefix);
        if (model_bool("analog", key))
            drive = -drive;
    } else {
        /* The cabinet's law (io.h); an axis the device lacks contributes
         * nothing. */
        int t_inv, b_inv;

        snprintf(key, sizeof key, "%s_throttle_invert", prefix);
        t_inv = model_bool("analog", key);
        snprintf(key, sizeof key, "%s_brake_invert", prefix);
        b_inv = model_bool("analog", key);
        drive = (found[1] ? io_pedal((raw[1] + 1.0f) * 0.5f, t_inv, deadzone)
                          : 0.0f) -
                (found[2] ? io_pedal((raw[2] + 1.0f) * 0.5f, b_inv, deadzone)
                          : 0.0f);
    }
    bar[1] = drive > 0.0f ? drive : 0.0f;
    bar[2] = drive < 0.0f ? -drive : 0.0f;
}

static void update_live_input(void)
{
    const input_probe_device_t *d;
    /* AN AXIS THE DEVICE DOES NOT HAVE IS THE ONE FACT THAT EXPLAINS A
     * MOTIONLESS BAR. Folding every probe_axis_value() miss into `x = 0.0f` is
     * indistinguishable on screen from a control resting at centre, and a G29,
     * which has no `x` at all, hits it on the shipped defaults. */
    char what[3][96], buttons[192], missing[192], live[416];
    float bar[3];
    int have;
    unsigned i, shown = 0;

    if (!s_page || !page_is_controls(s_page_index)) {
        refresh_binding_activity();
        return;
    }
    poll_every_device();
    have = s_probe_selected >= 0 && input_probe_poll(&s_probe_state);
    if (!have)
        memset(&s_probe_state, 0, sizeof s_probe_state);
    refresh_binding_activity();
    update_device_rows();

    capture_tick_all(have);
    if (s_page_index != PAGE_CONTROLS)
        return;
    {
        HWND button = GetDlgItem(s_page, IDC_SETUP_AXES);
        char text[32];

        if (button) {
            GetWindowTextA(button, text, sizeof text);
            if (strcmp(text, setup_button_text()))
                SetWindowTextA(button, setup_button_text());
        }
    }
    d = have ? input_probe_device((unsigned)s_probe_selected) : NULL;
    if (!d)
        return;
    if (d->backend == INPUT_PROBE_DINPUT)
        preview_dinput(bar, what, missing, sizeof missing);
    else
        preview_one_device(d, bar, what, missing, sizeof missing);
    /* THE THREE BARS ARE THE MAPPED VALUE, one per role. A pedal fills its bar
     * as it is pressed, whichever way the axis under it counts, the single
     * decision that stops one pedal being two bars moving opposite ways. */
    SendDlgItemMessage(s_page, IDC_LIVE_STEER, PBM_SETPOS,
                       (WPARAM)((bar[0] + 1.0f) * 500.0f), 0);
    SendDlgItemMessage(s_page, IDC_LIVE_THROTTLE, PBM_SETPOS,
                       (WPARAM)(bar[1] * 1000.0f), 0);
    SendDlgItemMessage(s_page, IDC_LIVE_BRAKE, PBM_SETPOS,
                       (WPARAM)(bar[2] * 1000.0f), 0);
    /* Which axis each role is on, and on which controller when it is
     * another, in the driver's own words where there are any. */
    for (i = 0; i < 3; i++)
        if (GetDlgItem(s_page, IDC_LIVE_ROLE_BASE + (int)i))
            SetDlgItemTextA(s_page, IDC_LIVE_ROLE_BASE + (int)i, what[i]);
    /* THE TABLE, FROM THE RAW READING AND NOTHING ELSE: no deadzone, no
     * inversion, no steering range. Those belong to the map, and the bars above
     * already show the map's answer. A row here says only "this is what the
     * device is sending". */
    if (s_show_raw) {
        unsigned rows = probe_axis_rows();
        for (i = 0; i < rows; i++) {
            char value[24];
            float v = i < s_probe_state.axis_count ? s_probe_state.axis[i] : 0.0f;

            if (!GetDlgItem(s_page, IDC_AXIS_BAR_BASE + (int)i))
                break;
            SendDlgItemMessage(s_page, IDC_AXIS_BAR_BASE + (int)i, PBM_SETPOS,
                               (WPARAM)((v + 1.0f) * 500.0f), 0);
            /* "-0.00" is what %+.2f makes of a centred axis a hair below
             * zero, and it reads as a defect rather than a wheel at rest. Two
             * decimals is what the column shows, so anything it would round to
             * zero IS zero. */
            if (v > -0.005f && v < 0.005f)
                v = 0.0f;
            snprintf(value, sizeof value, "%+.2f", v);
            SetDlgItemTextA(s_page, IDC_AXIS_VALUE_BASE + (int)i, value);
        }
        snprintf(buttons, sizeof buttons, "Buttons down:");
        for (i = 0; i < IO_MAX_BUTTONS; i++)
            if (io_button_down(s_probe_state.buttons, i + 1)) {
                char one[12];
                snprintf(one, sizeof one, " %u", i + 1);
                strncat(buttons, one, sizeof buttons - strlen(buttons) - 1);
                shown++;
            }
        if (!shown)
            strncat(buttons, " none", sizeof buttons - strlen(buttons) - 1);
        if (s_probe_state.pov >= 0) {
            char pov[32];
            snprintf(pov, sizeof pov, "   Hat: %d degrees",
                     s_probe_state.pov / 100);
            strncat(buttons, pov, sizeof buttons - strlen(buttons) - 1);
        }
        SetDlgItemTextA(s_page, IDC_LIVE_BUTTONS, buttons);
    }

    /* `none` is a real setting, not an absence: a combined-pedal wheel is
     * configured with no brake axis on purpose. Only a NAMED axis the device
     * did not report is a defect worth interrupting the reader for. */
    /* THE NOTE LINE HAS TWO SPEAKERS AND ONE OF THEM OUTRANKS THE OTHER.
     * While a capture is holding it, this one says nothing. */
    if (capture_note_tick())
        return;
    if (missing[0])
        snprintf(live, sizeof live,
                 "No axis found for: %s. Press Set up... and let it measure "
                 "them.", missing);
    else
        snprintf(live, sizeof live,
                 "Press a pedal: the bar that answers is the one it is on. A "
                 "pedal always fills its bar as you press it.");
    launcher_note_lines(live, "");
}


/* ASCII on purpose: these controls are written with the ...A entry points, so
 * anything above 7 bits depends on the user's code page. Trailing rather than
 * leading, so a lit row does not shift its own text sideways. */
#define BINDING_ACTIVE_MARK "  <"

void update_binding_display(unsigned action)
{
    HWND control;
    /* Wide enough for the longest binding_row_text() plus the activity mark:
     * IO_MAX_PAD_REFS references of a 15-character label and " button 128", a
     * key name, the joining word, and " (not connected)" on every one. */
    char text[288];

    if (s_page_index != PAGE_CONTROLS || !s_page || action >= ACTION_COUNT)
        return;
    control = GetDlgItem(s_page, IDC_BIND_TEXT_BASE + (int)action);
    if (!control)
        return;
    if (capture_row_text(action, text, sizeof text)) {
        SetWindowTextA(control, text);
        return;
    }
    binding_row_text(s_binding_title, action, text, sizeof text);
    if ((s_active_key | s_active_pad) & (1u << action))
        strncat(text, BINDING_ACTIVE_MARK, sizeof text - strlen(text) - 1);
    SetWindowTextA(control, text);
}



static int close_with_dirty_prompt(void)
{
    int answer;
    pull_page();
    if (!s_dirty)
        return 1;
    answer = MessageBoxA(s_window,
        "Save the changes to vcthunder.ini before closing?",
        "VCThunder settings", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL)
        return 0;
    if (answer == IDYES)
        return save_settings(s_window);
    return 1;
}

static int prepare_play(const char *title)
{
    char path[MAX_PATH];
    pull_page();
    if (s_dirty && !save_settings(s_window))
        return 0;
    if (!s_dirty) {
        char why[256];
        if (!validate_settings(why, sizeof why, NULL)) {
            MessageBoxA(s_window, why, "VCThunder settings",
                        MB_OK | MB_ICONWARNING);
            return 0;
        }
    }
    if (!game_path(title, path, sizeof path)) {
        MessageBoxA(s_window,
            "The selected title's executable is not present under data_dir. "
            "Choose the correct game-data folder on Files & Logging.",
            "Game data not found", MB_OK | MB_ICONWARNING);
        return 0;
    }
    s_result = !_stricmp(title, "hydro") ? LAUNCHER_HYDRO : LAUNCHER_OFFROAD;
    DestroyWindow(s_window);
    return 1;
}

/* The row of `backend`'s device number `index`, or -1. */
static int probe_row_at(input_probe_backend_t backend, unsigned index)
{
    unsigned i;

    for (i = 0; i < input_probe_count(); i++) {
        const input_probe_device_t *d = input_probe_device(i);

        if (d && d->backend == backend && d->backend_index == index)
            return (int)i;
    }
    return -1;
}

/* Tick the controller the host will read, in the host's order: DirectInput by
 * pick_device()'s rule, then XInput, then WinMM, each at its configured number;
 * under `auto` all three, otherwise the one named. Nothing when the host would
 * read none. */
static void choose_initial_probe(void)
{
    const char *provider = model_value("analog", "provider");
    int any = !_stricmp(provider, "auto"), best = -1;

    if (any || !_stricmp(provider, "dinput"))
        best = probe_configured_row();
    if (best < 0 && (any || !_stricmp(provider, "xinput")))
        best = probe_row_at(INPUT_PROBE_XINPUT,
                            model_uint("analog", "xinput_device", 0));
    if (best < 0 && (any || !_stricmp(provider, "winmm")))
        best = probe_row_at(INPUT_PROBE_WINMM,
                            model_uint("analog", "winmm_device", 0));
    if (best >= 0 && input_probe_select((unsigned)best))
        s_probe_selected = best;
}

static void poll_every_device(void);

static void refresh_input_probe(int rerender)
{
    abandon_capture();
    s_probe_selected = -1;
    input_probe_start(s_window);
    /* Which line is the configured device's depends on what is attached, so
     * a change re-reads that device's settings after the rebuild's pull. */
    {
        char before[INI_LABEL_CAP];

        snprintf(before, sizeof before, "%s", model_profile());
        resolve_profile();
        if (strcmp(before, model_profile()))
            s_reload_devices = 1;
    }
    choose_initial_probe();
    /* OPEN EVERY CONTROLLER ONCE, HERE, BEFORE THE PAGE IS BUILT. A
     * DirectInput device's axis and button counts come from GetCapabilities(),
     * which needs the device open, and opening is lazy; it happens on the
     * first poll, a timer tick AFTER the render. So the list drew every device
     * except the selected one as "0 axes, 0 buttons". */
    poll_every_device();
    set_status(input_probe_count() ? "Controller list refreshed." :
                                     "No controller detected.");
    if (rerender && page_is_controls(s_page_index)) {
        render_page();
        layout_window();
    }
}

static void handle_command(WPARAM wparam, LPARAM lparam)
{
    int id = LOWORD(wparam), notify = HIWORD(wparam);

    /* Every control this window acts on is addressed by id now: the one that
     * needed its HWND was the device combo, and the combo is gone. */
    (void)lparam;

    if (id == IDC_NAV && notify == (s_compact ? CBN_SELCHANGE : LBN_SELCHANGE)) {
        int selected, page;
        pull_page();
        selected = nav_selection();
        if (selected < 0 || selected >= NAV_COUNT)
            return;
        page = selected == NAV_CONTROLS && page_is_controls(s_page_index) ?
               s_page_index : k_nav_page[selected];
        if (page != s_page_index) {
            s_page_index = page;
            abandon_capture();
            render_page();
            layout_window();
        }
        return;
    }
    if ((id == IDC_CONTROL_MAIN || id == IDC_CONTROL_FFB) &&
        notify == BN_CLICKED) {
        int page = id == IDC_CONTROL_MAIN ? PAGE_CONTROLS : PAGE_FFB;
        pull_page();
        if (page != s_page_index) {
            s_page_index = page;
            abandon_capture();
            render_page();
            layout_window();
        }
        return;
    }
    if (id == IDC_APPLY && notify == BN_CLICKED) {
        pull_page();
        save_settings(s_window);
        return;
    }
    if (id == IDC_CLOSE && notify == BN_CLICKED) {
        SendMessage(s_window, WM_CLOSE, 0, 0);
        return;
    }
    if (id == IDC_PLAY_HYDRO && notify == BN_CLICKED) {
        prepare_play("hydro");
        return;
    }
    if (id == IDC_PLAY_OFFROAD && notify == BN_CLICKED) {
        prepare_play("offroad");
        return;
    }
    if ((id == IDC_TITLE_HYDRO || id == IDC_TITLE_OFFROAD) &&
        notify == BN_CLICKED) {
        s_binding_title = id == IDC_TITLE_OFFROAD;
        abandon_capture();
        render_page();
        layout_window();
        return;
    }
    if (id >= IDC_DEVICE_BASE &&
        id < IDC_DEVICE_BASE + INPUT_PROBE_MAX_DEVICES &&
        notify == BN_CLICKED) {
        /* THE RADIO IS THE ADOPT. Selecting a device was once a different
         * action from using it, because selecting it to LOOK at it rewrote
         * three keys. Every controller is polled now, so there is nothing left
         * to select in order to look: this row says which one drives the car. */
        pull_page();
        adopt_probe_device((unsigned)(id - IDC_DEVICE_BASE));
        return;
    }
    if (id == IDC_PROBE_REFRESH && notify == BN_CLICKED) {
        refresh_input_probe(1);
        return;
    }
    if (id == IDC_SETUP_AXES && notify == BN_CLICKED) {
        /* While Set up runs this button reads "Skip step". */
        if (capture_wizard_running()) {
            wizard_skip();
            return;
        }
        pull_page();
        begin_wizard();
        return;
    }
    /* THE FLAG IS TOGGLED, NOT READ BACK OFF THE CHECKBOX, wrong twice: the
     * page is rebuilt from the flag immediately, so the control it would read
     * is about to be destroyed, and a WM_COMMAND that did not come from a real
     * click (which is how the screenshot harness drives this window) leaves the
     * checkbox untouched, so the read returns the state already there. */
    if (id == IDC_SHOW_RAW && notify == BN_CLICKED) {
        pull_page();
        s_show_raw = !s_show_raw;
        request_relayout();
        return;
    }
    if (id == IDC_SHOW_LEGACY && notify == BN_CLICKED) {
        pull_page();
        s_show_legacy = !s_show_legacy;
        request_relayout();
        return;
    }
    if (id == IDC_SHOW_ADVANCED && notify == BN_CLICKED) {
        pull_page();
        s_advanced = !s_advanced;
        request_relayout();
        return;
    }
    if (id == IDC_GOOD_SAVE && notify == BN_CLICKED) {
        save_known_good(s_window);
        return;
    }
    if (id == IDC_GOOD_REVERT && notify == BN_CLICKED) {
        revert_to_known_good(s_window);
        return;
    }
    if (id == IDC_FFB_TEST && notify == BN_CLICKED) {
        const input_probe_device_t *d = s_probe_selected >= 0 ?
            input_probe_device((unsigned)s_probe_selected) : NULL;
        const char *gap = ffb_reaches_device(d);

        if (input_probe_test_force())
            /* A TEST THAT PASSES IS THE WORST MOMENT TO SAY NOTHING: the
             * device just shook, which is the evidence a user takes away. */
            set_status(gap ? gap : "Force-feedback test running: brrr...");
        else {
            set_status("The selected device could not start a force-feedback test.");
            MessageBeep(MB_ICONWARNING);
        }
        return;
    }
    if (id >= IDC_BROWSE_BASE && id < IDC_BROWSE_BASE + (int)SETTING_COUNT &&
        notify == BN_CLICKED) {
        browse_setting((unsigned)(id - IDC_BROWSE_BASE));
        return;
    }
    if (id >= IDC_DETECT_BASE && id < IDC_DETECT_BASE + (int)SETTING_COUNT &&
        notify == BN_CLICKED) {
        pull_page();
        abandon_capture();
        begin_axis_capture((unsigned)(id - IDC_DETECT_BASE));
        return;
    }
    if (id >= IDC_BIND_ADD_BASE &&
        id < IDC_BIND_ADD_BASE + (int)ACTION_COUNT && notify == BN_CLICKED) {
        abandon_capture();
        begin_capture((unsigned)(id - IDC_BIND_ADD_BASE));
        return;
    }
    if (id >= IDC_BIND_CLEAR_BASE &&
        id < IDC_BIND_CLEAR_BASE + (int)ACTION_COUNT && notify == BN_CLICKED) {
        abandon_capture();
        clear_binding((unsigned)(id - IDC_BIND_CLEAR_BASE));
        return;
    }
    if (id >= IDC_SETTING_BASE && id < IDC_SETTING_BASE + (int)SETTING_COUNT &&
        (notify == EN_CHANGE || notify == CBN_SELCHANGE || notify == BN_CLICKED)) {
        pull_page();
        mark_dirty();
        if (!scale_row((unsigned)(id - IDC_SETTING_BASE)))
            scale_combos_relabel();
    }
}

static LRESULT CALLBACK launcher_proc(HWND h, UINT message, WPARAM wparam,
                                      LPARAM lparam)
{
    switch (message) {
    case WM_NCCREATE:
        s_window = h;
        return DefWindowProcA(h, message, wparam, lparam);
    case WM_CREATE: {
        RECT rc;
        GetClientRect(h, &rc);
        s_compact = rc.right < scale_px(760);
        s_tiny = rc.right < scale_px(420) || rc.bottom < scale_px(300);
        SetWindowTextA(h, "VCThunder " VCT_VERSION);
        create_navigation(h);
        s_status = make_control(0, "STATIC", "Ready", SS_LEFT,
                                12, 630, 600, 24, h, IDC_STATUS);
        s_apply = make_control(0, "BUTTON", "Apply", BS_PUSHBUTTON | WS_TABSTOP,
                               760, 626, 88, 28, h, IDC_APPLY);
        s_close = make_control(0, "BUTTON", "Close", BS_PUSHBUTTON | WS_TABSTOP,
                               856, 626, 88, 28, h, IDC_CLOSE);
        EnableWindow(s_apply, FALSE);
        SetTimer(h, 1, 33, NULL);
        render_page();
        layout_window();
        return 0;
    }
    case WM_COMMAND:
        handle_command(wparam, lparam);
        return 0;
    case WM_DRAWITEM:
        if (draw_dark_button((DRAWITEMSTRUCT *)lparam))
            return TRUE;
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        LRESULT colour = control_colour(0, message, wparam, lparam);
        if (colour)
            return colour;
        break;
    }
    case WM_TIMER:
        if (wparam == 1)
            update_live_input();
        return 0;
    case WM_APP + 1:
        refresh_input_probe(1);
        /* AFTER the probe, because the probe writes this line too and the
         * first thing a window says should be the thing that is wrong with
         * the file rather than how many controllers are plugged in. */
        if (model_problem_total()) {
            char said[160];

            snprintf(said, sizeof said,
                     model_problem_total() == 1 ?
                     "%u line in vcthunder.ini is not used, see "
                     "Troubleshooting." :
                     "%u lines in vcthunder.ini are not used, see "
                     "Troubleshooting.", model_problem_total());
            set_status(said);
        }
        return 0;
    case WM_APP + 3:
        confirm_axis_detect();
        return 0;
    case WM_APP + 2:
        /* Rebuild responsive children only after the top-level WM_SIZE has
         * returned. During a live mode switch Windows can still expose the
         * previous child client width from inside WM_SIZE, which placed
         * compact controls at their former wide coordinates. */
        s_relayout_pending = 0;
        render_page();
        layout_window();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* THE BINDING CAPTURE TAKES THE KEYBOARD AND THE CONTROLLERS AT ONCE,
         * so the keyboard half arrives here and the controller half on the
         * timer. Whichever comes first is the one the user meant. Escape goes
         * with it: it is gated on the capture's own flag, which is why the
         * branch lives beside it (launcher_capture.h). */
        if (capture_key(wparam, lparam))
            return 0;
        break;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            RECT rc;
            int compact, tiny, compact_changed, tiny_changed;
            GetClientRect(h, &rc);
            compact = rc.right < scale_px(760);
            tiny = rc.right < scale_px(420) || rc.bottom < scale_px(300);
            compact_changed = compact != s_compact;
            tiny_changed = tiny != s_tiny;
            if (s_nav && (compact_changed || tiny_changed)) {
                pull_page();
                s_compact = compact;
                s_tiny = tiny;
                if (compact_changed)
                    create_navigation(h);
                layout_window();
                s_relayout_pending = 1;
                PostMessage(h, WM_APP + 2, 0, 0);
                return 0;
            }
            s_compact = compact;
            s_tiny = tiny;
            layout_window();
            /* Same mode, different width: the children still have to be
             * rebuilt, once per drag rather than once per WM_SIZE. */
            if (s_page && !s_relayout_pending &&
                page_width() != s_rendered_page_w) {
                s_relayout_pending = 1;
                PostMessage(h, WM_APP + 2, 0, 0);
            }
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lparam;
        MONITORINFO monitor;
        HMONITOR display = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        int available_w, available_h;
        memset(&monitor, 0, sizeof monitor);
        monitor.cbSize = sizeof monitor;
        GetMonitorInfoA(display, &monitor);
        available_w = monitor.rcWork.right - monitor.rcWork.left;
        available_h = monitor.rcWork.bottom - monitor.rcWork.top;
        mm->ptMinTrackSize.x = min(scale_px(320), available_w);
        mm->ptMinTrackSize.y = min(scale_px(240), available_h);
        return 0;
    }
    case WM_CLOSE:
        if (close_with_dirty_prompt())
            DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        input_probe_stop();
        s_window = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, message, wparam, lparam);
}


/* THE ONLY WINDOW IN WHICH A WINDOW OF OUR OWN IS SAFE: this runs BEFORE the
 * game image is mapped and overwrites the host's address space. */
launcher_result_t launcher_run(const char *ini_path)
{
    WNDCLASSA wc;
    INITCOMMONCONTROLSEX controls;
    MSG message;
    HICON icon;
    MONITORINFO monitor;
    HINSTANCE instance = NULL;
    POINT cursor;
    HMONITOR display;
    RECT work;
    int width, height, x, y;

    s_result = LAUNCHER_CANCEL;
    s_dirty = s_loading = 0;
    s_page_index = PAGE_HOME;
    s_binding_title = 0;
    s_probe_selected = -1;
    capture_reset();
    model_set_attached(probe_attached);
    if (!model_open(ini_path)) {
        MessageBoxA(NULL, "vcthunder.ini could not be read.",
                    "VCThunder", MB_OK | MB_ICONERROR);
        return LAUNCHER_CANCEL;
    }
    load_settings();
    scan_ini_problems();
    /* ONCE, AND ONLY WHEN THERE IS NONE. The file as this window found it is
     * the last state this window cannot have damaged. See known_good_path(). */
    seed_known_good();

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)&launcher_run, &instance);
    /* BEFORE THE WORK AREA IS MEASURED, or it comes back in the scaled
     * coordinates this call is about to stop using. */
    theme_enable_dpi_awareness();
    GetCursorPos(&cursor);
    display = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    memset(&monitor, 0, sizeof monitor);
    monitor.cbSize = sizeof monitor;
    if (GetMonitorInfoA(display, &monitor))
        work = monitor.rcWork;
    else
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    memset(&controls, 0, sizeof controls);
    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);
    theme_open(instance, &work);
    icon = LoadIconA(theme_instance(), MAKEINTRESOURCEA(IDI_VCTHUNDER));

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = theme_instance();
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = theme_page_brush();
    wc.lpszClassName = "VCThunderLauncherPage";
    if (!RegisterClassA(&wc))
        goto done;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = card_proc;
    wc.hInstance = theme_instance();
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = theme_page_brush();
    wc.lpszClassName = "VCThunderLauncherCard";
    if (!RegisterClassA(&wc))
        goto unregister_page;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = launcher_proc;
    wc.hInstance = theme_instance();
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = icon;
    wc.hbrBackground = theme_main_brush();
    wc.lpszClassName = "VCThunderLauncher";
    if (!RegisterClassA(&wc))
        goto unregister_card;

    width = scale_px(960);
    height = scale_px(680);
    width = min(width, work.right - work.left);
    height = min(height, work.bottom - work.top);
    x = work.left + ((work.right - work.left) - width) / 2;
    y = work.top + ((work.bottom - work.top) - height) / 2;
    s_window = CreateWindowExA(WS_EX_APPWINDOW, "VCThunderLauncher",
        "VCThunder " VCT_VERSION, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, width, height, NULL, NULL, theme_instance(), NULL);
    if (!s_window)
        goto unregister_main;
    enable_dark_title_bar(s_window);
    SendMessage(s_window, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(s_window, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    ShowWindow(s_window, SW_SHOW);
    UpdateWindow(s_window);
    PostMessage(s_window, WM_APP + 1, 0, 0);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        /* ANY capture, not just the keyboard one. IsDialogMessageA() eats
         * Escape as a dialog cancel and Backspace as navigation, so a key
         * pressed during a capture never reached the top-level WM_KEYDOWN
         * where the cancel and clear branches are.
         *
         * TWO THINGS WERE WRONG AND NEITHER WAS SUFFICIENT ALONE: this filter,
         * and the focus; begin_pad_capture() and begin_axis_capture() did not
         * call SetFocus(s_window), so a message getting past IsDialogMessage
         * still went to the push button that held it. */
        int to_capture = capture_active() &&
            (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN);
        if (to_capture || !s_window || !IsDialogMessageA(s_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }

unregister_main:
    UnregisterClassA("VCThunderLauncher", theme_instance());
unregister_card:
    UnregisterClassA("VCThunderLauncherCard", theme_instance());
unregister_page:
    UnregisterClassA("VCThunderLauncherPage", theme_instance());
done:
    input_probe_stop();
    theme_close();
    model_close();
    return s_result;
}
