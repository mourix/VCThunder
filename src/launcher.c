/* launcher.c -- native settings launcher.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * What `VCThunder.exe` with no title argument opens: every shipped ini key,
 * per-title bindings, and non-exclusive live device and axis testing with a
 * bounded force-feedback test. Plain Win32, no dialog resources; pages are built
 * from src/settings.def and src/bindings.def, so a key or control added there
 * appears here with no edit to this file. This half is the VIEW only: catalogue,
 * value grammar, document, model, capture and palette are each another
 * file's, and so are the Controls page and the CHD import (launcher_page.h). */
#include "vcthunder.h"
#include "resource.h"
#include "io.h"
#include "settings.h"
#include "ini.h"
#include "ini_doc.h"
#include "launcher_model.h"
#include "launcher_capture.h"
#include "launcher_pad.h"
#include "launcher_theme.h"
#include "launcher_page.h"
#include "di_role.h"
#include "ingest.h"
#include "bezel.h"
#include "arctic.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
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

/* s_lit_slot[] is one bit per action. */
typedef char action_count_fits_a_mask[ACTION_COUNT <= 32 ? 1 : -1];
/* ...and every slot id fits below IDC_BIND_RESET. */
typedef char binding_slots_fit[ACTION_COUNT * BINDING_SLOTS <= 100 ? 1 : -1];

static const char *const k_nav_name[] = {
    "Play", "Graphics", "Sound", "Controls", "Game System", "Link Play",
    "Files & Logging", "Help"
};

static const int k_nav_page[] = {
    PAGE_HOME, PAGE_GRAPHICS, PAGE_AUDIO, PAGE_CONTROLS, PAGE_SYSTEM,
    PAGE_LINK, PAGE_FILES, PAGE_TROUBLE
};

static const char *const k_page_intro[] = {
    "Pick a game and press Play. Changed settings are saved first.",
    "How the game looks. These apply to every game unless a row says otherwise.",
    "How the game sounds. These apply to every game unless a row says otherwise.",
    "Every controller you plug in works here. Steering, pedals and buttons "
    "can each come from a different controller.",
    "During a race your wheel pushes back and your pad shakes, just like the "
    "arcade did. Each game keeps its own settings here. Press Test to check "
    "that it works.",
    "The arcade machine's own settings: its clock, its DIP switches and how "
    "the controls are read.",
    "Race two copies of the game against each other, the way linked arcade "
    "machines did.",
    "Where the games, your saves and the log files are kept. A path without "
    "a drive is inside the VCThunder folder.",
    "The version, what to send with a problem report, and going back to the "
    "default settings."
};

/* A box opens at the first shown row of a new group: the air between two
 * outlines (frame_topic() draws 8 above a box and 4 below one), then its
 * title, at group_header()'s height. */
#define BOX_GAP  22
#define BOX_HEAD 26

/* A single-line EDIT draws its text at its top, so it is shorter than the row
 * and centred in it. */
#define EDIT_H   28
#define BUTTON_W 90             /* Browse..., Detect, Choose...            */

/* A page's header: the intro's top, and on a Controls page the height of the
 * tab row under it. page_header_bottom() and the renderers both read these. */
#define PAGE_INTRO_Y   16
#define CONTROLS_TAB_H CTRL_H

/* Each box's heading, by VCT_GROUP_ID(). Box 0 is a page's one box of bare
 * rows, titled per page by settings_box_title(). */
static const char *const k_group_name[] = {
    NULL, "DISPLAY", "PICTURE QUALITY", "WIDESCREEN", "CABINET SIGNAL"
};

/* All four are indexed by their enum, and an entry added to one and not the
 * others reads off the end of it. */
typedef char page_table_check[
    sizeof k_group_name / sizeof k_group_name[0] == GROUP_COUNT &&
    sizeof k_page_intro / sizeof k_page_intro[0] == PAGE_COUNT &&
    sizeof k_nav_name / sizeof k_nav_name[0] == NAV_COUNT &&
    sizeof k_nav_page / sizeof k_nav_page[0] == NAV_COUNT ? 1 : -1];

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static HWND s_window, s_nav, s_page, s_status, s_apply, s_close;
/* The controller's navigation state (launcher_pad.h). */
static launcher_pad_t s_pad;
static int s_compact, s_tiny;
/* THE DESIGN'S OWN SIZE, in logical units. The scale fits it to the window's
 * client area and never drops below Windows' scale for the monitor
 * (launcher_dpi()), so a window of the size the launcher opens at draws at
 * Windows' scale, and a larger one -- maximised, or the whole screen --
 * magnifies the same design rather than spreading it out. A smaller one keeps
 * Windows' scale and falls back to the compact form. */
#define DESIGN_W 960
#define DESIGN_H 600
/* The widest page worth having; past it the window centres one column. */
#define PAGE_MAX_W 1000
/* Whether the window covers its monitor, which follows [graphics] fullscreen
 * (launcher_set_fullscreen()), and where it was before it did. */
static int s_fullscreen;
static WINDOWPLACEMENT s_windowed_place;
static int s_page_index = PAGE_HOME;
/* WHICH BINDING SLOTS ARE LIT RIGHT NOW, one bit per action per slot. A
 * wheel's buttons are numbered and unlabelled, so `Button 9` says nothing about
 * which thing is under a hand; pressing it lights its slot, with no mode to
 * enter. Bitmasks rather than per-slot flags, so a tick that changes nothing
 * costs a comparison at 33 ms. */
static uint32_t s_lit_slot[BINDING_SLOTS];
static int s_dirty, s_loading, s_page_scroll, s_content_height;
/* A press-and-drag scrolling the page: where it began, and the offset then. */
static int s_dragging, s_drag_y, s_drag_from;
/* The page's width when its children were last built, and whether a rebuild is
 * queued. A resize INSIDE one layout mode moves the page; without a rebuild its
 * children keep their creation coordinates, so anything laid out to the right
 * edge ends up outside the page and unreachable, it scrolls vertically only. */
static int s_rendered_page_w, s_relayout_pending;
/* Inside a drag of the window's edge or caption (WM_ENTERSIZEMOVE): the scale
 * waits for the drop rather than following every step. */
static int s_sizing;
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
 * a property of a page. It lives on Help, which is the page somebody
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
    case IDC_TITLE_HYDRO:   return model_controls_title() == MODEL_TITLE_HYDRO;
    case IDC_TITLE_OFFROAD: return model_controls_title() == MODEL_TITLE_OFFROAD;
    case IDC_TITLE_ARCTIC:  return model_controls_title() == MODEL_TITLE_ARCTIC;
    default:                return -1;
    }
}


/* WHICH BUTTON THIS IS, for the palette that has to paint it. The colour is
 * launcher_theme.c's; which of the five kinds a control id is, is this file's,
 * and that is the whole of the boundary between them. */
static int launcher_slot_lit(int id);
static int launcher_slot_empty(int id);

theme_button_t launcher_button_kind(int id)
{
    int tab = tab_is_selected(id);

    if (launcher_slot_lit(id))
        return THEME_BUTTON_LIT;
    if (launcher_slot_empty(id))
        return THEME_BUTTON_EMPTY;
    if (tab >= 0)
        return tab ? THEME_BUTTON_TAB_LIT : THEME_BUTTON_TAB;
    if (id == IDC_PLAY_HYDRO)
        return THEME_BUTTON_HYDRO;
    if (id == IDC_PLAY_OFFROAD)
        return THEME_BUTTON_OFFROAD;
    if (id == IDC_PLAY_ARCTIC)
        return THEME_BUTTON_ARCTIC;
    if (id == IDC_PLAY_UARCTIC)
        return THEME_BUTTON_UARCTIC;
    return THEME_BUTTON_PLAIN;
}

/* A caption is subordinate to the label above it, and captions have their own
 * id range for exactly this: so a static can be muted without asking what its
 * text says. */
int launcher_control_is_caption(int id)
{
    return (id >= IDC_HELP_BASE && id < IDC_HELP_BASE + (int)SETTING_COUNT) ||
           (id >= IDC_BEZEL_NOTE && id < IDC_BEZEL_NOTE + MODEL_BEZELS);
}

static int setting_inert(unsigned index);

/* Set up's line 2: red keep still, green press, amber watching, blue done.
 * And a greyed row's label: a disabled static draws embossed, not grey. */
theme_tone_t launcher_control_tone(int id)
{
    if (id == IDC_ABOUT_LINK)
        return THEME_TONE_BLUE;
    if ((id >= IDC_LABEL_BASE && id < IDC_LABEL_BASE + (int)SETTING_COUNT) ||
        (id >= IDC_VALUE_BASE && id < IDC_VALUE_BASE + (int)SETTING_COUNT) ||
        (id >= IDC_FLANK_BASE && id < IDC_FLANK_BASE + (int)SETTING_COUNT) ||
        (id >= IDC_SUB_BASE && id < IDC_SUB_BASE + (int)SETTING_COUNT)) {
        int index = id >= IDC_SUB_BASE ? id - IDC_SUB_BASE :
                    id >= IDC_FLANK_BASE ? id - IDC_FLANK_BASE :
                    id >= IDC_VALUE_BASE ? id - IDC_VALUE_BASE :
                                           id - IDC_LABEL_BASE;
        HWND c = s_page ? GetDlgItem(s_page, IDC_SETTING_BASE + index) : NULL;

        return setting_inert((unsigned)index) ||
               (c && !IsWindowEnabled(c)) ? THEME_TONE_DIM : THEME_TONE_NONE;
    }
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

/* A ROW OF THE LIST IS NOT A NAV ENTRY ONCE ROWS CAN BE LEFT OUT: each row
 * carries its NAV_ value as item data, and that is what is read and sought.
 * Anything driving the window from outside must select by it too. */
static int nav_selection(void)
{
    LRESULT row;

    if (!s_nav)
        return -1;
    row = SendMessage(s_nav, s_compact ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
    if (row < 0)
        return -1;
    return (int)SendMessage(s_nav, s_compact ? CB_GETITEMDATA : LB_GETITEMDATA,
                            (WPARAM)row, 0);
}

static int nav_row(int nav)
{
    LRESULT n, row;

    if (!s_nav)
        return -1;
    n = SendMessage(s_nav, s_compact ? CB_GETCOUNT : LB_GETCOUNT, 0, 0);
    for (row = 0; row < n; row++)
        if (SendMessage(s_nav, s_compact ? CB_GETITEMDATA : LB_GETITEMDATA,
                        (WPARAM)row, 0) == nav)
            return (int)row;
    return -1;
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

static const char *page_name(int page);

/* THREE PAGES ARE FOR SOMEBODY WHO KNOWS THE CABINET, and are listed only
 * while Show advanced settings is on, the switch that reveals every other
 * expert row: Game System and Files & Logging hold nothing else, and Link Play
 * is a command-line feature. Link Play stays listed while a link is set, so
 * the row that turns it off is never behind a switch that is off. */
static int page_listed(int page)
{
    const char *link;

    if (s_advanced)
        return 1;
    if (page == PAGE_SYSTEM || page == PAGE_FILES)
        return 0;
    if (page == PAGE_LINK) {
        link = model_value("link", "link");
        return link && *link && _stricmp(link, "off") != 0;
    }
    return 1;
}

/* WHERE A SETTING IS, SAID SO THAT IT CAN BE FOUND: a page left out of the
 * list also says how to bring it back. */
static void page_where(int page, char *out, size_t size)
{
    if (page_listed(page))
        snprintf(out, size, "%s", page_name(page));
    else
        snprintf(out, size, "%s (turn on Show advanced settings, on Help)",
                 page_name(page));
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
    snprintf(title, sizeof title, "VCThunder - %s", page_name(s_page_index));
    SetWindowTextA(s_window, title);
}

/* THE LIST IS AS WIDE AS ITS LONGEST LISTED NAME, so the page gets the rest
 * of the window: measured in the font it is drawn in, plus a tile's padding.
 * The tiles shrink to fit first; the scroll bar takes what still does not. */
/* A tile's text inset, and the air under each tile. */
#define NAV_TILE_PAD 16
#define NAV_TILE_GAP 6

/* ONE PAGE TILE: the frame's colour around it, a face that is the accent
 * when it is the page on screen, and the name in bold capitals. Under High
 * Contrast every colour is the system's own. */
static int draw_nav_tile(const DRAWITEMSTRUCT *d)
{
    RECT tile, text_rc;
    char text[48];
    int lit, dark = theme_dark();
    COLORREF face, edge, ink;
    HBRUSH brush;
    HPEN pen;
    HGDIOBJ old_brush, old_pen, old_font;

    if (!d || d->CtlType != ODT_LISTBOX || d->CtlID != IDC_NAV)
        return 0;
    if (d->itemID == (UINT)-1)
        return 1;
    lit = (d->itemState & ODS_SELECTED) != 0;
    FillRect(d->hDC, &d->rcItem,
             dark ? theme_main_brush() : GetSysColorBrush(COLOR_WINDOW));
    tile = d->rcItem;
    tile.bottom -= scale_px(NAV_TILE_GAP);
    face = lit ? (dark ? RGB(0, 120, 215) : GetSysColor(COLOR_HIGHLIGHT))
               : (dark ? RGB(48, 48, 52) : GetSysColor(COLOR_BTNFACE));
    edge = lit ? face : (dark ? RGB(72, 72, 76) : GetSysColor(COLOR_BTNTEXT));
    ink = lit ? (dark ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT))
              : (dark ? RGB(240, 240, 240) : GetSysColor(COLOR_BTNTEXT));
    brush = CreateSolidBrush(face);
    pen = CreatePen(PS_INSIDEFRAME, max(1, scale_px(THEME_EDGE)), edge);
    old_brush = SelectObject(d->hDC, brush);
    old_pen = SelectObject(d->hDC, pen);
    Rectangle(d->hDC, tile.left, tile.top, tile.right, tile.bottom);
    SelectObject(d->hDC, old_pen);
    SelectObject(d->hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    text[0] = '\0';
    if (SendMessageA(d->hwndItem, LB_GETTEXTLEN, d->itemID, 0) <
        (LRESULT)sizeof text)
        SendMessageA(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);
    text_rc = tile;
    text_rc.left += scale_px(NAV_TILE_PAD);
    text_rc.right -= scale_px(4);
    old_font = SelectObject(d->hDC, theme_heading_font());
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, ink);
    DrawTextA(d->hDC, text, -1, &text_rc,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX |
              DT_END_ELLIPSIS);
    SelectObject(d->hDC, old_font);
    /* The keyboard's place. On the lit tile only once the keyboard or a
     * controller has been used, or a mouse user would see it on every page. */
    if ((d->itemState & ODS_FOCUS) &&
        (!lit || !(d->itemState & ODS_NOFOCUSRECT))) {
        RECT focus = tile;

        InflateRect(&focus, -scale_px(3), -scale_px(3));
        DrawFocusRect(d->hDC, &focus);
    }
    return 1;
}

/* THE MARK IN THE ABOUT BOX: the window's own icon, loaded at the size it is
 * drawn at, so Windows scales it once from the largest image. */
static HICON s_logo_icon;
static int s_logo_size;

static int draw_logo(const DRAWITEMSTRUCT *d)
{
    RECT rc;
    int size;

    if (!d || d->CtlType != ODT_STATIC || d->CtlID != IDC_LOGO)
        return 0;
    rc = d->rcItem;
    FillRect(d->hDC, &rc, theme_page_brush());
    size = min(rc.right - rc.left, rc.bottom - rc.top);
    if (size != s_logo_size) {
        if (s_logo_icon)
            DestroyIcon(s_logo_icon);
        s_logo_icon = (HICON)LoadImageA(theme_instance(),
                                        MAKEINTRESOURCEA(IDI_VCTHUNDER),
                                        IMAGE_ICON, size, size, 0);
        s_logo_size = size;
    }
    if (s_logo_icon)
        DrawIconEx(d->hDC, rc.left + (rc.right - rc.left - size) / 2,
                   rc.top + (rc.bottom - rc.top - size) / 2, s_logo_icon,
                   size, size, 0, NULL, DI_NORMAL);
    return 1;
}

static int nav_width(void)
{
    HDC dc = GetDC(s_window);
    HFONT previous;
    SIZE size;
    int widest = 0;
    unsigned i;

    if (!dc)
        return scale_px(204);
    previous = (HFONT)SelectObject(dc, theme_heading_font());
    for (i = 0; i < NAV_COUNT; i++) {
        char upper[32];

        if (!page_listed(k_nav_page[i]))
            continue;
        snprintf(upper, sizeof upper, "%s", k_nav_name[i]);
        CharUpperBuffA(upper, (DWORD)strlen(upper));
        if (GetTextExtentPoint32A(dc, upper, (int)strlen(upper), &size) &&
            size.cx > widest)
            widest = size.cx;
    }
    SelectObject(dc, previous);
    ReleaseDC(s_window, dc);
    return widest + scale_px(NAV_TILE_PAD * 3);
}

static void create_navigation(HWND parent)
{
    unsigned i;
    LRESULT row;

    if (s_nav)
        DestroyWindow(s_nav);
    if (s_compact)
        s_nav = make_control(0, "COMBOBOX", "", CBS_DROPDOWNLIST |
            WS_VSCROLL | WS_TABSTOP, 12, 34, 460, 240, parent, IDC_NAV);
    else
        /* TILES OVER A LIST BOX: owner-drawn rows (draw_nav_tile()) on a
         * list box, which keeps selection and item data. WS_VSCROLL for a
         * window too short for even the smallest tiles: a list box shows the
         * bar only when the rows do not fit. */
        s_nav = make_control(0, "LISTBOX", "",
            LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
            LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, 12, 44, 204, 560,
            parent, IDC_NAV);
    /* THE PAGE NAMES IN THE BOX TITLES' STYLE, bold capitals. The title bar
     * keeps the name as it is spelled. */
    SendMessage(s_nav, WM_SETFONT, (WPARAM)theme_heading_font(), FALSE);
    for (i = 0; i < NAV_COUNT; i++) {
        char upper[32];

        if (!page_listed(k_nav_page[i]))
            continue;
        snprintf(upper, sizeof upper, "%s", k_nav_name[i]);
        CharUpperBuffA(upper, (DWORD)strlen(upper));
        row = SendMessageA(s_nav, s_compact ? CB_ADDSTRING : LB_ADDSTRING, 0,
                           (LPARAM)upper);
        SendMessage(s_nav, s_compact ? CB_SETITEMDATA : LB_SETITEMDATA,
                    (WPARAM)row, (LPARAM)i);
    }
    SendMessage(s_nav, s_compact ? CB_SETCURSEL : LB_SETCURSEL,
                (WPARAM)nav_row(page_to_nav(s_page_index)), 0);
}

void set_status(const char *text)
{
    if (s_status)
        SetWindowTextA(s_status, text ? text : "");
}

/* Set when the configured device has just CHANGED, and consumed by
 * render_page() after its own pull_page(). See adopt_probe_device(). */
static int  s_reload_devices;
/* A change of whose controller the page shows, or of whether each game has
 * its own, waiting for the next rebuild: it has to happen between that
 * rebuild's pull and its destroy, for s_reload_devices' reason, or the pull
 * would put the outgoing cabinet's rows into the incoming one. */
/* CABINET_HYDRO + a MODEL_TITLE_* shows that cabinet. */
enum { CABINET_NONE, CABINET_HYDRO, CABINET_OFFROAD, CABINET_ARCTIC,
       CABINET_SPLIT, CABINET_JOIN };
static int  s_cabinet_op;
static int  s_cabinet_failed;           /* the last op ran out of memory     */

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

/* WHAT AN XInput BUTTON SAYS ON THE PAD, after the ordinal the file holds.
 * Not a per-model table: XInput fixes the order (xi_axes.h), so button 7 is
 * Back on every controller that reports through it. */
static const char *xinput_face(const char *label, unsigned ordinal)
{
    static const char *const k_face[] = {
        "A", "B", "X", "Y", "LB", "RB", "Back", "Start",
        "left stick press", "right stick press"
    };
    int row = label[0] ? probe_row_for_label(label) : s_probe_selected;
    const input_probe_device_t *d = row >= 0 ?
        input_probe_device((unsigned)row) : NULL;

    if (!d || d->backend != INPUT_PROBE_XINPUT || ordinal < 1 ||
        ordinal > sizeof k_face / sizeof k_face[0])
        return NULL;
    return k_face[ordinal - 1];
}

size_t pad_binding_render(const char *value, char *out, size_t out_size)
{
    char item[64], label[INI_LABEL_CAP], face[32];
    unsigned i, ordinal;
    size_t at = 0;

    out[0] = '\0';
    for (i = 0; ini_list_item(value, i, item, sizeof item); i++) {
        const char *who, *name;
        int w;

        if (!ini_parse_pad_ref(item, label, sizeof label, &ordinal) || !ordinal)
            continue;
        who = label[0] ? label : primary_label();
        name = xinput_face(label, ordinal);
        if (name)
            snprintf(face, sizeof face, " (%s)", name);
        else
            face[0] = '\0';
        w = snprintf(out + at, out_size - at, "%s%s button %u%s%s",
                     at ? " or " : "", who, ordinal, face,
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

/* A KEY AS A PERSON CALLS IT: the ini spells `LCTRL` and `EQUALS`, the key
 * cap says Left Ctrl and =. Display only; a letter, a digit or an F key is
 * already its own name. */
const char *key_display_name(const char *ini_name)
{
    static const struct { const char *ini, *shown; } k_key_names[] = {
        { "LEFT", "Left arrow" },   { "RIGHT", "Right arrow" },
        { "UP", "Up arrow" },       { "DOWN", "Down arrow" },
        { "LCTRL", "Left Ctrl" },   { "LCONTROL", "Left Ctrl" },
        { "RCTRL", "Right Ctrl" },  { "RCONTROL", "Right Ctrl" },
        { "LALT", "Left Alt" },     { "LMENU", "Left Alt" },
        { "RALT", "Right Alt" },    { "RMENU", "Right Alt" },
        { "LSHIFT", "Left Shift" }, { "RSHIFT", "Right Shift" },
        { "SPACE", "Space" },       { "ENTER", "Enter" },
        { "ESC", "Esc" },           { "ESCAPE", "Esc" },
        { "TAB", "Tab" },           { "BACKSPACE", "Backspace" },
        { "MINUS", "-" },           { "EQUALS", "=" },
        { "COMMA", "," },           { "PERIOD", "." },
        { "HOME", "Home" },         { "END", "End" },
        { "PGUP", "Page Up" },      { "PGDN", "Page Down" },
        { "INSERT", "Insert" },     { "DELETE", "Delete" }
    };
    const char *name = ini_name;
    size_t i;

    if (!_strnicmp(name, "KEYCODE_", 8))
        name += 8;
    for (i = 0; i < sizeof k_key_names / sizeof k_key_names[0]; i++)
        if (!_stricmp(name, k_key_names[i].ini))
            return k_key_names[i].shown;
    return name;
}

/* WHAT ONE SLOT SAYS: its input as a person reads it, `empty` when it has none
 * (the dark theme also draws it as an outline, not a button face).
 * The keyboard half is the ini's own key name made readable, the controller
 * half always names its controller. The last slot also says how many more a
 * hand-edited file gave the row, so no input that works is out of sight. */
static void binding_slot_text(int title, unsigned action, unsigned slot,
                              char *out, size_t out_size)
{
    binding_item_t row[BINDING_SLOTS];
    unsigned held = model_binding_items(title, action, row, BINDING_SLOTS);

    if (slot >= held) {
        snprintf(out, out_size, "empty");
        return;
    }
    if (row[slot].pad)
        pad_binding_text(row[slot].value, out, out_size);
    else
        snprintf(out, out_size, "%s", key_display_name(row[slot].value));
    if (slot == BINDING_SLOTS - 1 && held > BINDING_SLOTS) {
        size_t at = strlen(out);
        snprintf(out + at, out_size - at, "  (+%u more)",
                 held - BINDING_SLOTS);
    }
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
    if (!changed && model_bezel_copied()) {
        set_status("Bezel art saved into the bezels folder.");
        return 1;
    }
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

static void pull_page(void);

/* Before anything that reads the file from disk: the page into the model,
 * and the model into the file if it changed. */
int launcher_save_if_dirty(void)
{
    pull_page();
    return !s_dirty || save_settings(s_window);
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

/* EVERY SETTING BACK TO WHAT A FRESH UNZIP GIVES: the shipped ini, whole.
 * It asks, because it replaces every line, and keeps the old file beside it
 * under a name no Apply overwrites. */
static int restore_defaults(HWND owner)
{
    char why[MAX_PATH + 64], question[512];
    const void *data;
    unsigned long size;

    if (!config_default_ini(&data, &size)) {
        set_status("This build carries no default settings to restore.");
        return 0;
    }
    snprintf(question, sizeof question,
             "Put every setting back to how VCThunder comes?\n\n"
             "Graphics, sound, controls and folders all go back to their "
             "defaults, in a clean vcthunder.ini; no game or save file is "
             "deleted. The file as it is now is kept as "
             "vcthunder.ini" MODEL_RESTORE_SUFFIX ".%s",
             s_dirty ? "\n\nChanges you have not applied are discarded." :
                       "");
    if (MessageBoxA(owner, question, "Restore default settings",
                    MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
        return 0;
    if (!model_defaults_restore(data, size, why, sizeof why)) {
        MessageBoxA(owner, why, "VCThunder settings", MB_OK | MB_ICONERROR);
        set_status(why);
        return 0;
    }
    if (!model_reopen()) {
        MessageBoxA(owner, "The default settings were written, but they could "
                    "not be read back. Close the launcher and reopen it.",
                    "VCThunder settings", MB_OK | MB_ICONERROR);
        return 0;
    }
    s_loading = 1;
    load_settings();
    scan_ini_problems();
    s_loading = 0;
    s_dirty = 0;
    if (s_apply)
        EnableWindow(s_apply, FALSE);
    set_status("Restored the default settings. The file as it was is "
               "vcthunder.ini" MODEL_RESTORE_SUFFIX ".");
    request_relayout();
    return 1;
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

/* An SF_CHOICES boolean's two choices, in item-data order: 0 is false. */
#define BOOL_CHOICES "false|true"

/* WHAT A CHOICE READS AS IN THE LAUNCHER: no ini spelling is shown to a
 * person. Display only: the item data still carries the ini value, and
 * pull_page() writes that. A row of axes or pedal modes is matched by its
 * family, so the three providers' rows cannot drift apart. */
static const struct { const char *key, *value, *text; } k_choice_text[] = {
    { "fullscreen",       "false",       "Windowed"             },
    { "fullscreen",       "true",        "Fullscreen"           },
    { "aspect",           "raster",      "As the game draws it" },
    { "aspect",           "4:3",         "4:3"                  },
    { "aspect",           "16:9",        "16:9"                 },
    { "aspect",           "21:9",        "21:9 - ultrawide"     },
    { "aspect",           "stretch",     "Stretch to the window" },
    { "vcglide_renderer", "auto",        "Automatic"            },
    { "vcglide_renderer", "gpu",         "Graphics card"        },
    { "vcglide_renderer", "cpu",         "Processor"            },
    { "upscale_2d",       "bilinear",    "Off - plain stretch (bilinear)" },
    { "upscale_2d",       "catmull_rom", "Smooth (Catmull-Rom)" },
    { "upscale_2d",       "fsr1",        "Sharp (AMD FSR 1.0)"  },
    { "texmag",           "bilinear",    "Off - plain stretch (bilinear)" },
    { "texmag",           "catmull_rom", "Smooth (Catmull-Rom)" },
    { "frame_rate",       "30",          "30 Hz"                },
    { "frame_rate",       "60",          "60 Hz"                },
    { "vsync",            "off",         "Off - for variable refresh" },
    { "vsync",            "auto",        "On"                   },
    { "widescreen_hud",   "centre",      "4:3 - original"       },
    { "widescreen_hud",   "edges",       "Wide - screen edges"  },
    { "widescreen_ratio", "16:9",        "16:9"                 },
    { "widescreen_ratio", "21:9",        "21:9 - ultrawide"     },
    { "video_mode",       "auto",        "Automatic"            },
    { "video_mode",       "0",           "Mode 0"               },
    { "video_mode",       "1",           "Mode 1"               },
    { "video_mode",       "2",           "Mode 2 (bench only)"  },
    { "output",           "mix",         "Mixed - stereo"       },
    { "output",           "cabinet",     "Arcade - front left, sub right" },
    { "provider",         "auto",        "Automatic"            },
    { "provider",         "dinput",      "Wheel or joystick (DirectInput)" },
    { "provider",         "xinput",      "Xbox controller (XInput)" },
    { "provider",         "winmm",       "Old joystick driver (WinMM)" },
    { "provider",         "none",        "None: keyboard only"  },
    { "pov_digital",      "auto",        "Automatic"            },
    { "pov_digital",      "true",        "Yes"                  },
    { "pov_digital",      "false",       "No"                   },
    { "menu_wheel",       "auto",        "Automatic"            },
    { "menu_wheel",       "wheel",       "My wheel holds its position" },
    { "menu_wheel",       "pad",         "My controller springs back" },
    { "menu_wheel",       "off",         "Off"                  },
    { "direction",        "normal",      "Normal"               },
    { "direction",        "reversed",    "Reversed"             },
    { "link",             "off",         "Off"                  },
    { "link",             "local",       "Two games on this computer" },
    { "link",             "lan",         "Two computers (untested)" },
    { "log_level",        "error",       "Errors only"          },
    { "log_level",        "warn",        "Errors and warnings"  },
    { "log_level",        "info",        "Normal"               },
    { "log_level",        "trace",       "Everything (very large)" },
    { "*pedal_mode",      "combined",    "Both pedals on one axis" },
    { "*pedal_mode",      "separate",    "One axis per pedal"   },
    { "xinput*axis",      "lx",          "Left stick, sideways" },
    { "xinput*axis",      "ly",          "Left stick, up and down" },
    { "xinput*axis",      "rx",          "Right stick, sideways" },
    { "xinput*axis",      "ry",          "Right stick, up and down" },
    { "xinput*axis",      "lt",          "Left trigger"         },
    { "xinput*axis",      "rt",          "Right trigger"        },
    { "dinput*axis",      "x",           "X axis"               },
    { "dinput*axis",      "y",           "Y axis"               },
    { "dinput*axis",      "z",           "Z axis"               },
    { "dinput*axis",      "rx",          "X rotation"           },
    { "dinput*axis",      "ry",          "Y rotation"           },
    { "dinput*axis",      "rz",          "Z rotation"           },
    { "dinput*axis",      "slider0",     "Slider 1"             },
    { "dinput*axis",      "slider1",     "Slider 2"             },
    { "winmm*axis",       "x",           "X axis"               },
    { "winmm*axis",       "y",           "Y axis"               },
    { "winmm*axis",       "z",           "Z axis"               },
    { "winmm*axis",       "r",           "R axis"               },
    { "winmm*axis",       "u",           "U axis"               },
    { "winmm*axis",       "v",           "V axis"               },
    { "*axis",            "none",        "None"                 },
};

/* `pattern` is a key, or a key with one '*' standing for any run. */
static int choice_key_matches(const char *pattern, const char *key)
{
    const char *star = strchr(pattern, '*');
    size_t head, tail, n = strlen(key);

    if (!star)
        return !strcmp(pattern, key);
    head = (size_t)(star - pattern);
    tail = strlen(star + 1);
    return n >= head + tail && !strncmp(key, pattern, head) &&
           !strcmp(key + n - tail, star + 1);
}

static const char *choice_text(const char *key, const char *value)
{
    size_t i;

    for (i = 0; i < sizeof k_choice_text / sizeof k_choice_text[0]; i++)
        if (choice_key_matches(k_choice_text[i].key, key) &&
            !strcmp(k_choice_text[i].value, value))
            return k_choice_text[i].text;
    return value;
}

/* THE ITEM'S TEXT MAY SAY MORE THAN THE INI CAN, because the VALUE travels as
 * item data and pull_page() reads that; annotating the text alone would write
 * "rz  -  Throttle" into the file on the way out.
 *
 * `axis_prefix` non-NULL turns a plain enum into an axis picker: every choice
 * the device reports carries the driver's name for it and every one it does
 * not is marked. `none` is always available; a combined-pedal wheel is
 * configured with no brake axis on purpose. */
static void combo_add_choices(HWND combo, const char *key,
                              const char *choices, const char *axis_prefix)
{
    char value[96];
    int i;

    for (i = 0; choice_at(choices, i, value, sizeof value); i++) {
        char item[160];
        const char *label;
        int reported, index;

        snprintf(item, sizeof item, "%s", choice_text(key, value));
        if (axis_prefix && _stricmp(value, "none")) {
            label = probe_axis_label_for(value, &reported);
            if (!reported)
                snprintf(item, sizeof item, "%s  (not on this device)",
                         choice_text(key, value));
            else if (label)
                snprintf(item, sizeof item, "%s  -  %s",
                         choice_text(key, value), label);
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

/* THE TWO SCALE ROWS ARE DROPDOWNS, and the lines beside each say what the
 * chosen scale produces for each game (scale_lines()). They stay
 * ST_UINT / ST_AUTO_UINT in settings.def: the file holds the same numbers
 * either way, so an item's data is the number (0 for automatic, -1 for a
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

/* The ETS picture, as glide_bind.c's resolve_geometry() works it out: both
 * titles ask for the cabinet's GR_RESOLUTION_512x384 (the mode-1 entry of the
 * game's own video-mode table), raster_height replaces the 384, and
 * widescreen widens to widescreen_ratio (16:9 or 21:9) of that height. 0 when
 * it cannot be known from
 * here: a forced video_mode takes the size from the game's own table, which
 * the launcher has not read, and a raster_height being typed may not be a
 * number yet. */
#define CABINET_RASTER_W 512
#define CABINET_RASTER_H 384

/* A graphics key as one game reads it: its [<title>.graphics] line first, as
 * ini_read_scoped() does, then the row. In `buf` when the game has its own. */
static const char *title_graphics(int title, const char *key, char *buf,
                                  size_t size)
{
    char section[64];

    snprintf(section, sizeof section, "%s.graphics", model_title_id(title));
    if (ini_doc_get(model_doc(), section, key, buf, size))
        return buf;
    return model_value("graphics", key);
}

static int title_bool(int title, const char *key)
{
    char buf[INI_VALUE_CAP];
    int on;

    return ini_parse_bool(title_graphics(title, key, buf, sizeof buf), &on) &&
           on;
}

/* The widescreen raster's width at `height` rows, as this game would get it. */
static int title_widescreen_width(int title, int height)
{
    char buf[INI_VALUE_CAP];

    return widescreen_ratio_width(title_graphics(title, "widescreen_ratio",
                                                 buf, sizeof buf), height);
}

static int picture_raster(int title, int *w, int *h)
{
    char height_buf[INI_VALUE_CAP], mode_buf[INI_VALUE_CAP];
    const char *height = title_graphics(title, "raster_height", height_buf,
                                        sizeof height_buf);
    unsigned rows;

    if (_stricmp(title_graphics(title, "video_mode", mode_buf,
                                sizeof mode_buf), "auto"))
        return 0;
    if (!_stricmp(height, "auto"))
        rows = CABINET_RASTER_H;
    else if (!ini_parse_uint(height, 1, 4096, &rows))
        return 0;
    *h = (int)rows;
    *w = CABINET_RASTER_W;
    if (title_bool(title, "widescreen") &&
        title_widescreen_width(title, *h) > *w)
        *w = title_widescreen_width(title, *h);
    return 1;
}

/* The Bezel box's row names, by MODEL_TITLE_* and then
 * MODEL_BEZEL_UARCTIC. */
static const char *const k_bezel_name[MODEL_BEZELS] = {
    "Hydro Thunder", "Offroad Thunder", "Arctic Thunder",
    "Ultimate Arctic Thunder" };

/* What a bezel file is: its size and its opening's height, or why it cannot
 * be used. Decoded once per
 * version of the file, by the game's own reader; a version is a path, a write
 * time and a size, so a file Apply replaces is read again. */
typedef struct {
    char     path[MAX_PATH];
    FILETIME written;
    DWORD    size;
    int      ok, w, h, hole_h;
    char     why[160];
} bezel_info_t;

static const bezel_info_t *bezel_info(const char *path)
{
    static bezel_info_t cache[8];
    static unsigned next;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    char full[MAX_PATH];
    bezel_info_t *e;
    bezel_t b;
    unsigned i;

    ini_resolve_path(path, full, sizeof full);
    memset(&fa, 0, sizeof fa);
    GetFileAttributesExA(full, GetFileExInfoStandard, &fa);
    for (i = 0; i < 8; i++)
        if (cache[i].path[0] && !_stricmp(cache[i].path, full) &&
            !CompareFileTime(&cache[i].written, &fa.ftLastWriteTime) &&
            cache[i].size == fa.nFileSizeLow)
            return &cache[i];
    e = &cache[next++ % 8];
    memset(e, 0, sizeof *e);
    snprintf(e->path, sizeof e->path, "%s", full);
    e->written = fa.ftLastWriteTime;
    e->size = fa.nFileSizeLow;
    e->ok = bezel_load(full, &b, e->why, sizeof e->why);
    if (e->ok) {
        e->w = b.w;
        e->h = b.h;
        e->hole_h = b.hole_h;
        bezel_free(&b);
    }
    return e;
}

/* The bezel art one cabinet would load, as its size and its hole's height;
 * 0 when it has none or the file cannot be used. As this window has it, a
 * choice not yet applied included. */
static int title_bezel(int title, int *art_w, int *art_h, int *hole_h)
{
    char path[MAX_PATH];
    const bezel_info_t *info;

    if (!model_bezel_get(title, path, sizeof path))
        return 0;
    info = bezel_info(path);
    if (!info->ok)
        return 0;
    *art_w = info->w;
    *art_h = info->h;
    *hole_h = info->hole_h;
    return 1;
}

/* Arctic's picture: 640x480, or its 16:9 or 21:9 raster under widescreen. It ignores
 * aspect, raster_height and video_mode (arctic.c's k_unused). */
#define ARCTIC_RASTER_W 640
#define ARCTIC_RASTER_H 480

/* One title's size for a scale row; 0 when it cannot be known from here. The
 * ETS window follows window.c's client_size_for(): its height is the raster's
 * times the scale and `aspect` decides the width. Arctic's is its picture
 * times the scale (arctic.c). Under a cabinet's bezel art bezel_client_size()
 * decides either. The 3D scene is the raster times the scale on the GPU. */
static int title_scale_size(int title, int row, int scale, int *w, int *h)
{
    int raster_h, aw, ah, hh;

    if (title == MODEL_TITLE_ARCTIC) {
        *h = ARCTIC_RASTER_H;
        *w = title_bool(title, "widescreen")
             ? title_widescreen_width(title, ARCTIC_RASTER_H) : ARCTIC_RASTER_W;
        raster_h = *h;
        *w *= scale;
        *h *= scale;
    } else {
        char buf[INI_VALUE_CAP];
        const char *aspect = title_graphics(title, "aspect", buf, sizeof buf);

        if (!picture_raster(title, w, h))
            return 0;
        raster_h = *h;
        *h *= scale;
        if (row == SCALE_ROW_WINDOW && aspect_is_4_3(aspect))
            *w = (*h * 4 + 1) / 3;
        else if (row == SCALE_ROW_WINDOW && aspect_is_16_9(aspect))
            *w = (*h * 16 + 4) / 9;
        else if (row == SCALE_ROW_WINDOW && aspect_is_21_9(aspect))
            *w = widescreen_ratio_width("21:9", *h);
        else
            *w *= scale;
    }
    if (row == SCALE_ROW_WINDOW && title_bezel(title, &aw, &ah, &hh))
        bezel_client_size(aw, ah, hh, raster_h, scale, w, h);
    return 1;
}

/* The largest window scale whose window, frame included, fits the work area
 * of the launcher's monitor, which is where Play opens the game, as
 * window.c's window_scale_for() and arctic.c's resolve_window_scale() pick
 * it. */
static int title_fit_scale(int title)
{
    MONITORINFO monitor;
    RECT work, r;
    int scale, w, h;

    memset(&monitor, 0, sizeof monitor);
    monitor.cbSize = sizeof monitor;
    if (GetMonitorInfoA(MonitorFromWindow(s_window, MONITOR_DEFAULTTOPRIMARY),
                        &monitor))
        work = monitor.rcWork;
    else if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0))
        return 0;
    for (scale = 8; scale > 1; scale--) {
        if (!title_scale_size(title, SCALE_ROW_WINDOW, scale, &w, &h))
            return 0;
        SetRect(&r, 0, 0, w, h);
        AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                             WS_MINIMIZEBOX, FALSE);
        if (r.right - r.left <= work.right - work.left &&
            r.bottom - r.top <= work.bottom - work.top)
            break;
    }
    return scale;
}

static void scale_item_text(int scale, char *out, size_t size)
{
    if (scale == 0)
        snprintf(out, size, "Automatic");
    else
        snprintf(out, size, "%dx", scale);
}

/* What a scale row's value gives one game. A window too big for the screen
 * is opened at the largest that fits, so the cell gives that size and says
 * why. */
/* One game's answer: a size, then `text`; or `text` alone where there is no
 * size. */
typedef struct {
    int w, h;
    char text[48];
    int warn;                   /* amber: a choice that did not hold        */
} scale_size_t;

static void scale_cell(int row, int title, scale_size_t *cell)
{
    char buf[INI_VALUE_CAP];
    const char *have = title_graphics(title, row == SCALE_ROW_WINDOW ?
                                      "window_scale" : "render_scale",
                                      buf, sizeof buf);
    int fit = 0, w, h;
    unsigned scale;

    if (!_stricmp(have, "auto")) {
        if (!(fit = title_fit_scale(title)) ||
            !title_scale_size(title, row, fit, &w, &h)) {
            snprintf(cell->text, sizeof cell->text, "the largest that fits");
            return;
        }
        cell->w = w;
        cell->h = h;
        snprintf(cell->text, sizeof cell->text, "%dx", fit);
        return;
    }
    if (!ini_parse_uint(have, 1, 8, &scale)) {
        snprintf(cell->text, sizeof cell->text, "not one of these scales");
        cell->warn = 1;
        return;
    }
    if (row == SCALE_ROW_WINDOW && (fit = title_fit_scale(title)) &&
        fit < (int)scale && title_scale_size(title, row, fit, &w, &h)) {
        cell->w = w;
        cell->h = h;
        snprintf(cell->text, sizeof cell->text, "%ux does not fit, %dx", scale,
                 fit);
        cell->warn = 1;
        return;
    }
    if (!title_scale_size(title, row, (int)scale, &w, &h)) {
        snprintf(cell->text, sizeof cell->text, "set by the video-mode table");
        return;
    }
    cell->w = w;
    cell->h = h;
}

/* Borderless fullscreen covers the monitor; the launcher's own stands in
 * for the one the game will open on. */
static int fullscreen_cell(int title, scale_size_t *cell)
{
    MONITORINFO mi;

    if (!title_bool(title, "fullscreen"))
        return 0;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(MonitorFromWindow(s_window, MONITOR_DEFAULTTOPRIMARY),
                        &mi)) {
        cell->w = mi.rcMonitor.right - mi.rcMonitor.left;
        cell->h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    snprintf(cell->text, sizeof cell->text, "fullscreen");
    return 1;
}

/* THE SIZES A SCALE ROW GIVES, beside its dropdown: a line per game, and
 * games that come out the same share one, so Hydro and Offroad are usually
 * one line and Arctic the other. Drawn as owner-drawn statics in wide text:
 * the multiplication sign is not in every ANSI code page. */
#define SCALE_LINES 3

static int text_lines_height(HFONT font, const char *text, int width);

/* A line is two columns, the games and the size, so the sizes of both rows
 * start at one x (draw_scale_line()). */
static wchar_t s_scale_name[3][SCALE_LINES][32], s_scale_size[3][SCALE_LINES][64];
static int s_scale_warn[3][SCALE_LINES], s_scale_n[3];

static int scale_lines(int row)
{
    static const int title[SCALE_LINES] = {
        MODEL_TITLE_HYDRO, MODEL_TITLE_OFFROAD, MODEL_TITLE_ARCTIC };
    static const char *const name[SCALE_LINES] = {
        "Hydro", "Offroad", "Arctic" };
    wchar_t size[SCALE_LINES][64];
    int warn[SCALE_LINES], used[SCALE_LINES] = { 0 }, i, j, n = 0;

    for (i = 0; i < SCALE_LINES; i++) {
        scale_size_t cell;

        memset(&cell, 0, sizeof cell);
        /* Fullscreen is the monitor's size alone: the Display row above
         * already says Fullscreen. */
        if (row == SCALE_ROW_WINDOW && fullscreen_cell(title[i], &cell))
            cell.text[0] = 0;
        else
            scale_cell(row, title[i], &cell);
        if (cell.w)
            _snwprintf(size[i], 64, L"%d \x00d7 %d%hs%hs", cell.w, cell.h,
                       cell.text[0] ? ", " : "", cell.text);
        else
            _snwprintf(size[i], 64, L"%hs", cell.text);
        size[i][63] = 0;
        warn[i] = cell.warn;
    }
    for (i = 0; i < SCALE_LINES; i++) {
        char names[64];
        int same = 1;

        if (used[i])
            continue;
        snprintf(names, sizeof names, "%s", name[i]);
        for (j = i + 1; j < SCALE_LINES; j++)
            if (!used[j] && warn[j] == warn[i] && !wcscmp(size[j], size[i])) {
                used[j] = 1;
                same++;
                snprintf(names + strlen(names), sizeof names - strlen(names),
                         "/%s", name[j]);
            }
        _snwprintf(s_scale_name[row][n], 32, L"%hs:",
                   same == SCALE_LINES ? "All" : names);
        s_scale_name[row][n][31] = 0;
        wcscpy(s_scale_size[row][n], size[i]);
        s_scale_warn[row][n++] = warn[i];
    }
    s_scale_n[row] = n;
    return n;
}

static int scale_line_h(void)
{
    return text_lines_height(theme_font(), "Ag", 400) + 2;
}

/* THE LINES SIT IN AN OUTLINE AS TALL AS THE DROPDOWN, taller only for more
 * lines than that holds; SCALE_PAD between the outline and the text. */
#define SCALE_PAD 10

static int scale_frame_h(int lines)
{
    return max(CTRL_H, lines * scale_line_h() + 4);
}

/* BOTH ROWS' LINES, MEASURED ONCE per page build or refresh, not per caller:
 * the lines, the widest line in logical units (the widest name, the gap, the
 * widest size, as draw_scale_line() lays them out, and the padding), the
 * widest name in pixels, and whether they sit beside the dropdown -- while
 * every line fits there, and under it otherwise: a line cut short is a size
 * nobody can read. render_page() and scale_lines_refresh() clear
 * s_scale_measured. */
static int s_scale_measured, s_scale_beside, s_scale_width, s_scale_name_px;

static void scale_measure(void)
{
    HDC dc;
    HGDIOBJ old;
    SIZE extent;
    int row, k, name = 0, size = 0, width = page_width();

    if (s_scale_measured || !s_page)
        return;
    for (row = SCALE_ROW_WINDOW; row <= SCALE_ROW_RENDER; row++)
        scale_lines(row);
    s_scale_width = INT_MAX;
    dc = GetDC(s_page);
    if (dc) {
        old = SelectObject(dc, theme_font());
        for (row = SCALE_ROW_WINDOW; row <= SCALE_ROW_RENDER; row++)
            for (k = 0; k < s_scale_n[row]; k++) {
                if (GetTextExtentPoint32W(dc, s_scale_name[row][k],
                                          (int)wcslen(s_scale_name[row][k]),
                                          &extent))
                    name = max(name, (int)extent.cx);
                if (GetTextExtentPoint32W(dc, s_scale_size[row][k],
                                          (int)wcslen(s_scale_size[row][k]),
                                          &extent))
                    size = max(size, (int)extent.cx);
            }
        SelectObject(dc, old);
        ReleaseDC(s_page, dc);
        s_scale_width = unscale_px(name + size) + 8 + 2 + 2 * SCALE_PAD;
    }
    s_scale_name_px = name;
    s_scale_beside = !s_compact &&
        width - 22 - (244 + min(310, width - 266) + 16) >= s_scale_width;
    s_scale_measured = 1;
}

/* How much taller the row is for its lines: under the dropdown all of them,
 * beside it whatever does not fit the dropdown's height. */
static int scale_lines_extra(unsigned index)
{
    int row = scale_row(index), block;

    if (!row || !s_page)
        return 0;
    scale_measure();
    block = scale_frame_h(s_scale_n[row]);
    return s_scale_beside ? block - CTRL_H : block + 4;
}

static void render_scale_lines(unsigned index, int y, int control_w)
{
    int row = scale_row(index), n, k, line = scale_line_h(), frame_h, x, w, top;
    HWND frame;

    scale_measure();
    n = s_scale_n[row];
    frame_h = scale_frame_h(n);
    if (s_scale_beside) {
        /* Both rows' outlines are as wide as the widest line of either. */
        x = 244 + control_w + 16;
        top = y;
        w = min(s_scale_width, page_width() - 22 - x);
    } else {
        /* Under the dropdown, as wide as it is, unless a line would not fit. */
        x = s_compact ? 22 : 244;
        top = (s_compact ? y + STACK_DY : y) + CTRL_H + 4;
        w = max(control_w, min(s_scale_width, page_width() - 22 - x));
    }
    for (k = 0; k < n; k++)
        make_control(0, "STATIC", "", SS_OWNERDRAW, x + SCALE_PAD,
                     top + (frame_h - n * line) / 2 + k * line,
                     w - 2 * SCALE_PAD, line, s_page,
                     IDC_SCALE_LINE_BASE + row * SCALE_LINES + k);
    /* Under the lines it frames: a later sibling clips an earlier one. */
    frame = make_control(0, "STATIC", "", SS_OWNERDRAW, x, top, w, frame_h,
                         s_page, IDC_SCALE_FRAME_BASE + row);
    SetWindowPos(frame, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static int draw_scale_line(const DRAWITEMSTRUCT *d)
{
    int at;

    if (!d || d->CtlType != ODT_STATIC)
        return 0;
    if ((int)d->CtlID >= IDC_SCALE_FRAME_BASE &&
        (int)d->CtlID < IDC_SCALE_FRAME_BASE + 3) {
        FillRect(d->hDC, &d->rcItem, theme_page_brush());
        FrameRect(d->hDC, &d->rcItem, theme_dark() ? theme_border_brush() :
                                      GetSysColorBrush(COLOR_WINDOWTEXT));
        return 1;
    }
    if ((int)d->CtlID < IDC_SCALE_LINE_BASE ||
        (int)d->CtlID >= IDC_SCALE_LINE_BASE + 3 * SCALE_LINES)
        return 0;
    at = (int)d->CtlID - IDC_SCALE_LINE_BASE;
    /* The sizes start where the widest name of either row ends. */
    theme_draw_line(d, s_scale_name[at / SCALE_LINES][at % SCALE_LINES],
                    s_scale_size[at / SCALE_LINES][at % SCALE_LINES],
                    s_scale_name_px + scale_px(8),
                    s_scale_warn[at / SCALE_LINES][at % SCALE_LINES]);
    return 1;
}

/* (Re)fill one scale dropdown and select the model's value. */
static void scale_combo_fill(HWND combo, unsigned index)
{
    const setting_desc_t *d = &k_settings[index];
    const char *have = model_slot(index);
    int first, n, selected = -1, at;
    char item[INI_VALUE_CAP + 32], value[16];

    SendMessage(combo, CB_RESETCONTENT, 0, 0);
    first = d->type == ST_AUTO_UINT ? 0 : (int)d->minimum;
    for (n = first; n <= (int)d->maximum; n++) {
        if (n == 0 && d->type != ST_AUTO_UINT)
            continue;
        if (n != 0 && n < (int)d->minimum)
            continue;
        scale_item_text(n, item, sizeof item);
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

/* THE SAME WIDTH WHETHER OR NOT THE PAGE SCROLLS. The client area loses the
 * scroll bar's width when the bar is shown, and a page is built before
 * update_scrollbar() decides that, so the bar's room is always kept: every
 * box on every page has the same right edge. */
int page_width(void)
{
    RECT rc;
    int w, bar = GetSystemMetrics(SM_CXVSCROLL);

    if (!s_page)
        return 700;
    GetClientRect(s_page, &rc);
    w = rc.right - rc.left;
    if (GetWindowLongA(s_page, GWL_STYLE) & WS_VSCROLL)
        w += bar;
    /* The bar's room once, and nothing more: the boxes' right edges then sit
     * as far from it as their left edges from the page's. */
    return max(unscale_px(w - bar), s_tiny ? 230 : 300);
}

/* AN OUTLINE `margin` IN FROM BOTH SIDES OF THE PAGE, placed by its pixel
 * edges: a scaled left plus a scaled width rounds, and the two gaps can then
 * differ by a pixel. */
static int page_right_px(int margin)
{
    RECT rc;
    int right;

    GetClientRect(s_page, &rc);
    right = rc.right;
    if (!(GetWindowLongA(s_page, GWL_STYLE) & WS_VSCROLL))
        right -= GetSystemMetrics(SM_CXVSCROLL);
    return max(right, scale_px(page_width())) - scale_px(margin);
}

static void place_outline(HWND c, int margin, int y, int h)
{
    if (!c || !s_page)
        return;
    MoveWindow(c, scale_px(margin), scale_px(y),
               page_right_px(margin) - scale_px(margin), scale_px(h), FALSE);
}


/* Where a page's content starts: under its header, MEASURED. A Controls page
 * has the tab row under the intro, and the intro wraps, one line at 1200
 * units, four at 400, so a constant that fits one clips the other. */
static int setting_on_page(unsigned index);

/* A page whose rows are all advanced is not listed until they show
 * (page_listed()), so no intro has to say where its rows went. */
static const char *page_intro(void)
{
    return k_page_intro[s_page_index];
}

static int page_intro_height(void)
{
    return text_block_height(theme_font(), page_intro(), page_width() - 40);
}

/* WHERE A CONTROLS PAGE'S TAB ROW GOES: under the intro, and under the
 * taller of the two tabs' intros, so switching tabs does not move the row
 * that was just clicked. Both intros are constants, so the answer changes
 * only with the width, the DPI or the font, and is measured once per those. */
static int controls_tab_y(void)
{
    static int s_width = -1, s_scale = -1, s_y;
    static HFONT s_font;
    int width = page_width() - 40, scale = scale_px(1000);
    HFONT font = theme_font();

    if (width != s_width || scale != s_scale || font != s_font) {
        int main_h = text_block_height(font, k_page_intro[PAGE_CONTROLS],
                                       width);
        int ffb_h = text_block_height(font, k_page_intro[PAGE_FFB], width);

        s_y = PAGE_INTRO_Y + max(main_h, ffb_h) + 8;
        s_width = width;
        s_scale = scale;
        s_font = font;
    }
    return s_y;
}

/* 22 UNITS UNDER THE INTRO, NOT 6: the first topic's card takes 8 as its own
 * air, so 6 put the frame's top line above the bottom of the text box, through
 * the descenders of an intro that had wrapped. A Controls page has its tab row
 * between the intro and the first card. */
int page_header_bottom(void)
{
    if (page_is_controls(s_page_index))
        return controls_tab_y() + CONTROLS_TAB_H + 22;
    return PAGE_INTRO_Y + page_intro_height() + 22;
}

/* The link page's fixed panel. Both statements are there because the setting
 * below them does NOT do what a player would assume: this page picks a
 * transport, while whether a cabinet links and which cabinet it is belong to
 * the game's operator menu, in NVRAM. MEASURED, the same two sentences wrap
 * to two lines at 1200 px and six in the compact form. */
static const char *const k_link_note[] = {
    "Whether a game links, and which machine it is, are set in the game's "
    "own operator menu (F2 during the game): Unit ID and Network Enabled. "
    "This page only chooses how the two games reach each other.",
    "Local runs both games on this computer. Start the second one from a "
    "command prompt, with a save folder and logs of its own:\r\n"
    "VCThunder.exe hydro --link local --save save2 --log link2.log "
    "--glide-log vcglide2.log"
};
#define LINK_NOTE_COUNT (sizeof k_link_note / sizeof k_link_note[0])

/* Logical-unit height of `text` wrapped to `width`, in `font`.
 *
 * ONE LINE PER CALL when it matters: a STATIC renders a string carrying its own
 * "\r\n" in more line-heights than DT_CALCRECT reports once a line comes close
 * to the edge, so a caller that would measure a two-line string gives each line
 * its own control instead and stacks them by this height. */
static int text_lines_height(HFONT font, const char *text, int width)
{
    HWND owner = s_page ? s_page : s_window;
    HDC dc = GetDC(owner);
    HFONT previous = NULL;
    RECT rc;

    if (!dc)
        return 20;
    SetRect(&rc, 0, 0, scale_px(width), 0);
    if (font)
        previous = (HFONT)SelectObject(dc, font);
    DrawTextA(dc, text, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (previous)
        SelectObject(dc, previous);
    ReleaseDC(owner, dc);
    return unscale_px(rc.bottom - rc.top);
}

/* The same, at least a row's 20 units: what a control is sized to. Above
 * 100% DPI the floor is taller than one line of text, so text stacked line
 * under line is measured by text_lines_height() instead. */
int text_block_height(HFONT font, const char *text, int width)
{
    int height = text_lines_height(font, text, width);

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
/* EVERY BOX'S TITLE IS ONE STYLE: capitals, in bold. The text is upper-cased
 * here, so a title that carries a product name passes it as it is spelled. */
static void box_title_text(const char *text, char *out, size_t size)
{
    snprintf(out, size, "%s", text);
    CharUpperBuffA(out, (DWORD)strlen(out));
}

/* Measured in the title's own face: bold capitals are wider than the prose
 * font, and a title that wraps there would be cut in half. */
static int box_title_height(const char *text, int width)
{
    char upper[MAX_PATH + 64];

    box_title_text(text, upper, sizeof upper);
    return text_block_height(theme_heading_font(), upper, width);
}

static void box_title(int x, int y, int w, int h, const char *text,
                      DWORD style)
{
    char upper[MAX_PATH + 64];
    HWND c;

    box_title_text(text, upper, sizeof upper);
    c = make_control(0, "STATIC", upper, SS_LEFT | style, x, y, w, h, s_page, 0);
    SendMessage(c, WM_SETFONT, (WPARAM)theme_heading_font(), TRUE);
}

int frame_topic(int top, int bottom)
{
    HWND frame;
    int w = page_width() - 24, h = bottom - top + 12;

    if (w <= 0 || h <= 0)
        return bottom;
    frame = make_control(0, theme_dark() ? "VCThunderLauncherCard" : "BUTTON",
                         "", theme_dark() ? 0 : BS_GROUPBOX,
                         12, top - 8, w, h, s_page, 0);
    place_outline(frame, 12, top - 8, h);
    SetWindowPos(frame, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    /* Where the next box's title goes: BOX_GAP, the same air between two
     * outlines as the settings rows' boxes have, on every page. */
    return bottom + BOX_GAP;
}

/* A group's heading and the one button belonging to it: right-aligned beside
 * the heading where there is room, on its own line under it where there is
 * not. Every constant is a maximum. */
int group_header(int y, const char *text, int button_id,
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
                              stacked ? 20 : x, stacked ? y + 22 : y - 4,
                              button_w, CTRL_H, s_page, button_id);
        EnableWindow(button, enabled);
    }
    box_title(20, y, (button_id && !stacked ? x - 30 : right - 20), 20, text,
              SS_ENDELLIPSIS);
    if (!button_id)
        return y + 26;
    return y + (stacked ? 22 + CTRL_H + 4 : CTRL_H + 2);
}

/* Where each note goes, and where the settings rows start below them. Both
 * out-parameters may be NULL: page_settings_top() wants only the return. */
static int link_panel_layout(int *note_y, int *note_h)
{
    int width = page_width() - 40;
    int y = page_header_bottom() + BOX_HEAD;
    unsigned i;

    for (i = 0; i < LINK_NOTE_COUNT; i++) {
        int h = text_block_height(theme_font(), k_link_note[i], width);
        if (note_y)
            note_y[i] = y;
        if (note_h)
            note_h[i] = h;
        y += h + 10;
    }
    /* The notes end 10 under the last one, which is the frame's bottom plus
     * 10; the settings box starts BOX_GAP under that bottom. */
    return y - 10 + BOX_GAP;
}

static void render_link_panel(void)
{
    int note_y[LINK_NOTE_COUNT], note_h[LINK_NOTE_COUNT];
    int width = page_width();
    unsigned i;

    link_panel_layout(note_y, note_h);
    box_title(20, page_header_bottom(), width - 40, 20, "How it works",
              SS_ENDELLIPSIS);
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
 * One button and the sentences saying what it does. On Help because
 * what it restores is the WHOLE file, and because that is the page somebody
 * opens when something is wrong. A card's note is two sentences, each
 * starting on its own line: two statics, see text_lines_height(). */
static const char *const k_reset_note[2] = {
    "Restore default settings writes a clean vcthunder.ini, the one a fresh "
    "download starts with.",
    "The file as it was is kept as vcthunder.ini" MODEL_RESTORE_SUFFIX "."
};

static const char k_advanced_note[] =
    "Shows every setting, and three more pages: Game System, Link Play and "
    "Files & Logging. Most players never need them.";

/* WHAT TO SEND, AND WHERE IT IS: the two logs are what a problem report
 * needs, and a newcomer does not know they exist. Each start overwrites
 * them, which is the one thing worth saying about them. */
static const char *const k_report_note[2] = {
    "The log files say what happened: attach vcthunder.log and vcglide.log "
    "to your report.",
    "Each game start replaces them, so copy them before playing again."
};

/* THE ADVANCED DISCLOSURE, AND THE ONE PROMISE ATTACHED TO IT: everything the
 * pages stopped showing is one click away, and the click is in one place. */
static void render_advanced_toggle(int y)
{
    int width = page_width();

    make_control(0, "BUTTON", "Show advanced settings on every page",
                 BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 20, y, width - 40,
                 CTRL_H, s_page, IDC_SHOW_ADVANCED);
    SendDlgItemMessage(s_page, IDC_SHOW_ADVANCED, BM_SETCHECK,
                       s_advanced ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* The advanced block's height: heading, the switch 24 under it, the sentence
 * 26 under that, MEASURED, it wraps to three lines below about 500 units.
 * Layout and render both ask here rather than agreeing by hand. */
#define ADVANCED_HEAD_DY  0
#define ADVANCED_CHECK_DY 24
#define ADVANCED_NOTE_DY  (ADVANCED_CHECK_DY + CTRL_H + 4)

static int trouble_advanced_height(void)
{
    return ADVANCED_NOTE_DY +
           text_block_height(theme_font(), k_advanced_note, page_width() - 40);
}

/* THE FILE'S OWN PROBLEMS GO FIRST UNDER ABOUT: "the line you edited is not
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
    h = box_title_height(k_problem_head, page_width() - 40) + 6;
    for (i = 0; i < model_problem_count(); i++)
        h += text_block_height(theme_font(), model_problem(i), width) + 6;
    if (model_problem_total() > model_problem_count())
        h += text_block_height(theme_font(), "Ag", width) + 6;
    return h - 6 + BOX_GAP;         /* the frame ends 6 above h */
}

static int render_trouble_problems(int y)
{
    int width = page_width();
    int top = y;
    unsigned i;

    if (!model_problem_total())
        return y;
    box_title(20, y, width - 40, box_title_height(k_problem_head, width - 40),
              k_problem_head, 0);
    y += box_title_height(k_problem_head, width - 40) + 6;
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

/* The About card: heading, the version 24 under it, then the author and the
 * link 22 apart. */
#define ABOUT_VERSION_DY 24
#define ABOUT_AUTHOR_DY  46
#define ABOUT_LINK_DY    68
#define ABOUT_H          (ABOUT_LINK_DY + 20)
#define ABOUT_LOGO       (ABOUT_H - ABOUT_VERSION_DY)

/* A card of a heading, two measured sentences and one button under them:
 * the first 22 under the heading, the second directly under the first, the
 * button 10 under that. Returns where the next card starts. */
static int button_card_layout(int y, const char *const note[2], int *note_y,
                              int note_h[2], int *button_y)
{
    unsigned i;

    *note_y = y + 22;
    *button_y = *note_y + 10;
    for (i = 0; i < 2; i++) {
        note_h[i] = text_lines_height(theme_font(), note[i], page_width() - 40);
        *button_y += note_h[i];
    }
    return *button_y + CTRL_H + BOX_GAP;
}

static void render_button_card(int y, const char *title,
                               const char *const note[2],
                               const char *button, int id)
{
    int note_y, note_h[2], button_y, width = page_width();

    button_card_layout(y, note, &note_y, note_h, &button_y);
    box_title(20, y, width - 40, 20, title, SS_ENDELLIPSIS);
    make_control(0, "STATIC", note[0], SS_LEFT, 20, note_y, width - 40,
                 note_h[0], s_page, 0);
    make_control(0, "STATIC", note[1], SS_LEFT, 20, note_y + note_h[0],
                 width - 40, note_h[1], s_page, 0);
    make_control(0, "BUTTON", button, BS_PUSHBUTTON | WS_TABSTOP, 20,
                 button_y, min(240, width - 40), CTRL_H, s_page, id);
    frame_topic(y, button_y + CTRL_H);
}

/* Top to bottom: About, the file's problems if it has any, Reporting a
 * problem, Default settings, Advanced settings, BOX_GAP apart like every
 * other box. */
static int trouble_panel_layout(int *problems_y, int *report_y, int *reset_y,
                                int *adv_y)
{
    int y = page_header_bottom() + ABOUT_H + BOX_GAP;
    int note_y, note_h[2], button_y;

    if (problems_y)
        *problems_y = y;
    y += trouble_problems_height();
    if (report_y)
        *report_y = y;
    y = button_card_layout(y, k_report_note, &note_y, note_h, &button_y);
    if (reset_y)
        *reset_y = y;
    y = button_card_layout(y, k_reset_note, &note_y, note_h, &button_y);
    if (adv_y)
        *adv_y = y;
    return y + trouble_advanced_height() + BOX_GAP;
}

/* THE LOG, SHOWN IN EXPLORER WITH THE FILE SELECTED. A relative name is
 * beside VCThunder.exe, which is the working directory (main.c chdirs there
 * before anything reads a path), so GetFullPathName resolves it as the host
 * will. No log yet opens the folder it will be written to. */
static void show_log_files(void)
{
    const char *log = model_value("diagnostics", "log_file");
    char path[MAX_PATH], args[MAX_PATH + 16], *name = NULL;
    INT_PTR shown;

    if (!log || !*log)
        log = "vcthunder.log";
    if (!GetFullPathNameA(log, sizeof path, path, &name))
        return;
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        snprintf(args, sizeof args, "/select,\"%s\"", path);
        shown = (INT_PTR)ShellExecuteA(s_window, "open", "explorer.exe", args,
                                       NULL, SW_SHOWNORMAL);
    } else {
        if (name)
            *name = 0;
        shown = (INT_PTR)ShellExecuteA(s_window, "open", path, NULL, NULL,
                                       SW_SHOWNORMAL);
        set_status("No log yet: a game writes one each time it starts.");
    }
    if (shown <= 32)
        set_status("Could not open Explorer on the log folder.");
}

static void render_trouble_panel(void)
{
    int problems_y, report_y, reset_y, adv_y;
    int width = page_width(), about_y = page_header_bottom();
    int logo = width - 40 >= 200 + ABOUT_LOGO + 12 ? ABOUT_LOGO : 0;
    int text_w = width - 40 - (logo ? logo + 12 : 0);
    HWND author;

    trouble_panel_layout(&problems_y, &report_y, &reset_y, &adv_y);
    /* ABOUT: the version, which only this box shows, where the project
     * lives, and the mark at the right, as tall as the three lines, centred
     * on the outline (frame_topic() draws it 8 above the title and 4 below
     * the last line), and only where the text keeps 200 px beside it. The
     * link is a static: SysLink needs common controls 6, which this window
     * does not load. The author line is set wide, so the copyright sign does
     * not depend on the ANSI code page. */
    box_title(20, about_y, text_w, 20, "About", SS_ENDELLIPSIS);
    make_control(0, "STATIC", "VCThunder " VCT_VERSION, SS_LEFT, 20,
                 about_y + ABOUT_VERSION_DY, text_w, 20, s_page, 0);
    author = make_control(0, "STATIC", "", SS_LEFT, 20,
                          about_y + ABOUT_AUTHOR_DY, text_w, 20, s_page, 0);
    SetWindowTextW(author, VCT_COPYRIGHT);
    make_control(0, "STATIC", VCT_REPO_URL, SS_LEFT | SS_NOTIFY, 20,
                 about_y + ABOUT_LINK_DY, min(320, text_w), 20, s_page,
                 IDC_ABOUT_LINK);
    if (logo)
        make_control(0, "STATIC", "", SS_OWNERDRAW, width - 20 - logo,
                     about_y + (ABOUT_H - 4 - logo) / 2, logo, logo, s_page,
                     IDC_LOGO);
    frame_topic(about_y, about_y + ABOUT_H);
    render_trouble_problems(problems_y);
    render_button_card(report_y, "Reporting a problem", k_report_note,
                       "Show the log files", IDC_SHOW_LOGS);
    render_button_card(reset_y, "Default settings", k_reset_note,
                       "Restore default settings", IDC_RESTORE_DEFAULTS);
    /* THE LAST TOPIC, and the only place this switch exists. Every page's
     * advanced keys are behind it, which is why the sentence under it says
     * "on every page". */
    box_title(20, adv_y + ADVANCED_HEAD_DY, width - 40, 20, "Advanced settings",
              SS_ENDELLIPSIS);
    render_advanced_toggle(adv_y + ADVANCED_CHECK_DY);
    {
        int adv_h = text_block_height(theme_font(), k_advanced_note, width - 40);

        make_control(0, "STATIC", k_advanced_note, SS_LEFT, 20,
                     adv_y + ADVANCED_NOTE_DY, width - 40, adv_h, s_page, 0);
        frame_topic(adv_y, adv_y + ADVANCED_NOTE_DY + adv_h);
    }
}

/* The raw table's rows: every axis the controller reports, as far as the
 * probe holds them. */
unsigned probe_axis_rows(void)
{
    unsigned n = input_probe_axis_count();

    return n > INPUT_PROBE_MAX_AXES ? INPUT_PROBE_MAX_AXES : n;
}

/* Where the first settings row goes: under whatever the hand-built panel
 * needed. render_settings_page() renders the panel first and the rows after,
 * which is the one ordering this static depends on. */
static int files_panel_layout(int *note_y, int *note_h, int *button_y,
                              int *state_y);


static int page_settings_top(void)
{
    if (s_page_index == PAGE_CONTROLS || s_page_index == PAGE_FFB)
        return s_panel_bottom;
    if (s_page_index == PAGE_LINK)
        return link_panel_layout(NULL, NULL);
    if (s_page_index == PAGE_TROUBLE)
        return trouble_panel_layout(NULL, NULL, NULL, NULL);
    if (s_page_index == PAGE_FILES)
        return files_panel_layout(NULL, NULL, NULL, NULL);
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
#define VCT_HELP_CAP (VCT_HELP_MAX + 32 + 192 + 64 + 160)

/* WHICH GAMES READ THIS KEY AT ALL, one bit per MODEL_TITLE_*: all of them,
 * less Arctic for a key it does not use yet (arctic_setting_unused()). */
static unsigned setting_users(unsigned index)
{
    unsigned all = (1u << MODEL_TITLES) - 1u;

    if (arctic_setting_unused(k_settings[index].section, k_settings[index].key))
        all &= ~(1u << MODEL_TITLE_ARCTIC);
    return all;
}

/* WHICH CABINETS SET THIS KEY FOR THEMSELVES, and to what. One bit per
 * MODEL_TITLE_*, of the games that read it; `out` gets "Hydro 100, Offroad 8".
 *
 * THE LAUNCHER EDITS THE SHARED SECTIONS FOR EVERY ROW BUT THE PER-GAME ONES,
 * and ini_read_scoped() tries [<title>.<section>] FIRST, so a key a per-title
 * section carries is one this window cannot change. Without this the row shows
 * the shared value and writing it reaches neither cabinet, silently. The
 * shipped per-title blocks are deliberate; what was wrong is that nothing
 * said so. */
static unsigned setting_title_override(unsigned index, char *out,
                                       size_t out_size)
{
    static const char *const k_name[MODEL_TITLES] = {
        "Hydro", "Offroad", "Arctic" };
    unsigned t, mask = 0, users = setting_users(index);
    size_t at = 0;

    if (out && out_size)
        out[0] = '\0';
    /* A choice or a force-feedback row each game makes for itself is not an
     * override: the game row over the page shows each cabinet's. */
    if (model_setting_per_cabinet(index))
        return 0;
    for (t = 0; t < MODEL_TITLES; t++) {
        char section[96], value[INI_VALUE_CAP];

        if (!(users & (1u << t)))
            continue;
        /* THE TITLE-SCOPED FORM OF THE SECTION THIS ROW WRITES, which for a
         * device key is [<title>.analog.<label>] and not [<title>.analog].
         * **Device beats title** (ini_read_device_scoped), so a [hydro.analog]
         * line does not block a row writing [analog.tmx]. */
        snprintf(section, sizeof section, "%s.%s", model_title_id((int)t),
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

/* Every game that reads this key sets it for itself, so the row reaches
 * none of them. */
static int setting_overridden_everywhere(unsigned index)
{
    unsigned mask = setting_title_override(index, NULL, 0);

    return mask && mask == setting_users(index);
}

static const char *setting_help(unsigned index, char *buf, size_t cap)
{
    const setting_desc_t *d = &k_settings[index];
    const char *lead = d->warning == WARNING_DANGER  ? "Warning: " :
                       d->warning == WARNING_CAUTION ? "Caution: " : "";
    char scoped[128];
    unsigned mask = setting_title_override(index, scoped, sizeof scoped);
    /* A per-game row says it only while Arctic is the game shown. */
    int arctic_unused = !(setting_users(index) & (1u << MODEL_TITLE_ARCTIC)) &&
                        (!model_setting_per_cabinet(index) ||
                         model_controls_title() == MODEL_TITLE_ARCTIC);
    size_t at;

    if (!d->help && !mask && !arctic_unused)
        return NULL;
    at = (size_t)snprintf(buf, cap, "%s%s", lead, d->help ? d->help : "");
    if (mask && at < cap) {
        if (mask == setting_users(index))
            /* EVERY game that reads it, and this row reaches none: it is
             * disabled, and saying "this row does nothing" without saying
             * where the value IS would send the reader to the launcher for a
             * setting the launcher cannot hold. */
            at += (size_t)snprintf(buf + at, cap - at,
                     "%sEach game sets this for itself in vcthunder.ini (%s), "
                     "so this row is not used.", at ? "  " : "", scoped);
        else
            /* `scoped` already names the cabinets, and their values. */
            at += (size_t)snprintf(buf + at, cap - at,
                     "%sSet per game in vcthunder.ini for %s; this row is what "
                     "the other games use.", at ? "  " : "", scoped);
    }
    if (arctic_unused && at < cap)
        at += (size_t)snprintf(buf + at, cap - at,
                               "%sArctic Thunder does not use this yet.",
                               at ? "  " : "");
    return buf;
}

/* Measured: a caption is one line at 1200 units and three at 400. Cached
 * against the width, layout mode and DPI that produced it, page_setting_y()
 * is O(rows^2) in this by construction and each miss is a GetDC and a
 * DrawText. */
static int s_help_h[SETTING_COUNT];
static int s_help_h_width = -1, s_help_h_compact = -1, s_help_h_title = -1;
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

    if (!setting_help(index, text, sizeof text))
        return 0;
    /* The shown game is an input too: a per-game row's caption follows it. */
    if (width != s_help_h_width || s_compact != s_help_h_compact ||
        theme_dpi() != s_help_h_dpi ||
        model_controls_title() != s_help_h_title) {
        memset(s_help_h, 0, sizeof s_help_h);
        s_help_h_width = width;
        s_help_h_compact = s_compact;
        s_help_h_dpi = theme_dpi();
        s_help_h_title = model_controls_title();
    }
    if (!s_help_h[index])
        s_help_h[index] = text_block_height(
            theme_font(), setting_help(index, text, sizeof text), width - 44) + 8;
    return s_help_h[index];
}

/* The lines follow the scale, fullscreen, raster, aspect, widescreen and
 * bezel settings and the launcher's monitor, so any change on the page, and a
 * move, recomputes them; a change in how many there are, where they sit or how
 * wide the widest is moves the rows under them, which is a rebuild. */
static void scale_lines_refresh(void)
{
    int beside = s_scale_beside, width = s_scale_width, row, k;
    int n[3] = { 0, s_scale_n[1], s_scale_n[2] };

    if (!s_page || s_page_index != PAGE_GRAPHICS)
        return;
    s_scale_measured = 0;
    scale_measure();
    /* Moved, a new widest line (the outlines are that wide) or another
     * number of lines: a rebuild. Otherwise the same lines repaint. */
    if (s_scale_beside != beside || s_scale_width != width ||
        s_scale_n[1] != n[1] || s_scale_n[2] != n[2]) {
        request_relayout();
        return;
    }
    for (row = SCALE_ROW_WINDOW; row <= SCALE_ROW_RENDER; row++)
        for (k = 0; k < s_scale_n[row]; k++) {
            HWND c = GetDlgItem(s_page, IDC_SCALE_LINE_BASE +
                                        row * SCALE_LINES + k);
            if (c)
                InvalidateRect(c, NULL, FALSE);
        }
}

/* A row drawn as a checkbox: a boolean, or an SF_TOGGLE enum while advanced
 * settings are hidden, which is ticked for any choice but the first. */
static int setting_is_check(unsigned index)
{
    return (k_settings[index].type == ST_BOOL &&
            !(k_settings[index].flags & SF_CHOICES)) ||
           ((k_settings[index].flags & SF_TOGGLE) && !s_advanced);
}

static int control_is_button(HWND c)
{
    char cls[16];

    return GetClassNameA(c, cls, sizeof cls) && !_stricmp(cls, "Button");
}

static int toggle_is_on(unsigned index)
{
    char off[INI_VALUE_CAP];

    return !choice_at(k_settings[index].choices, 0, off, sizeof off) ||
           _stricmp(model_slot(index), off) != 0;
}

/* Where a row's caption sits inside it: under the control, which in compact
 * mode is itself under the label. A checkbox is its own label in both modes. */
static int balance_stacked(unsigned index);

static int scale_lines_extra(unsigned index);

static int setting_caption_offset(unsigned index)
{
    if (balance_stacked(index))
        return STACK_DY + CTRL_H + 2;
    return ((s_compact && !setting_is_check(index)) ? STACK_DY + CTRL_H + 2 :
                                                      CTRL_H + 2) +
           scale_lines_extra(index);
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
/* WHICH CONTROLLER AN [ffb] KEY REACHES: the wheel's motor (dinput.c) or
 * the pad's derived rumble (rumble.c), which a wheel vetoes. `enabled` gates
 * both and is neither's alone. */
static const char *ffb_key_provider(const char *key)
{
    if (!strncmp(key, "rumble", 6))
        return "xinput";
    if (!strcmp(key, "strength_percent") ||
        !strcmp(key, "device_gain_percent") || !strcmp(key, "direction") ||
        !strcmp(key, "autocenter"))
        return "dinput";
    return NULL;
}

static int setting_hidden_here(unsigned index)
{
    const char *prefix = k_settings[index].page == PAGE_CONTROLS ?
        setting_provider_prefix(k_settings[index].key) :
        k_settings[index].page == PAGE_FFB &&
        !strcmp(k_settings[index].section, "ffb") ?
        ffb_key_provider(k_settings[index].key) : NULL;
    const char *provider;
    const input_probe_device_t *device;

    if (!prefix)
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
    return _stricmp(prefix, device->backend == INPUT_PROBE_DINPUT ? "dinput" :
                            device->backend == INPUT_PROBE_XINPUT ? "xinput" :
                                                                    "winmm") != 0;
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
    return (s_compact || balance_stacked(index) ? STACK_DY + CTRL_H + 9 :
                                                  CTRL_H + 12) +
           scale_lines_extra(index) + setting_help_height(index);
}


/* Rows are not a constant pitch apart, so this accumulates rather than
 * multiplying. The one other consumer of a row position is the boxes and the
 * scroll extent at the bottom of render_page(). */
/* EVERY BOX HAS A TITLE. A named group has its own; a page's one untitled
 * box, the rows SHOW() puts on a bare page, is titled here by page. Layout and
 * drawing both ask this, so a title cannot be drawn without its room. */
static const char *settings_box_title(int group)
{
    if (k_group_name[group])
        return k_group_name[group];
    switch (s_page_index) {
    case PAGE_AUDIO:    return "Sound";
    case PAGE_CONTROLS: return "Controller details";
    case PAGE_FFB: {
        static char title[64];

        snprintf(title, sizeof title, "Strength and rumble - %s",
                 model_title_name(model_controls_title()));
        return title;
    }
    case PAGE_SYSTEM:   return "Cabinet";
    case PAGE_LINK:     return "Connection";
    case PAGE_FILES:    return "Folders and logs";
    case PAGE_TROUBLE:  return "Diagnostics";
    default:            return "Settings";
    }
}

static int page_setting_y(unsigned index)
{
    unsigned i;
    int y = page_settings_top(), group = -1;

    for (i = 0; i <= index && i < SETTING_COUNT; i++) {
        if (!setting_on_page(i))
            continue;
        if (k_settings[i].group != group) {
            if (group >= 0)
                y += BOX_GAP;
            y += BOX_HEAD;
            group = k_settings[i].group;
        }
        if (i < index)
            y += setting_row_height(i);
    }
    return y;
}

/* A ROW THAT DOES NOTHING WITH THE PAGE AS IT STANDS IS GREYED, NOT HIDDEN:
 * hiding it would move every row under it whenever its cause changed. Its
 * value is still saved. Each cause is a row shown with this one by default. */
static int setting_inert(unsigned index)
{
    const setting_desc_t *d = &k_settings[index];
    const char *renderer;
    unsigned scale;

    /* A per-game row the shown game does not read. */
    if (model_setting_per_cabinet(index) &&
        !(setting_users(index) & (1u << model_controls_title())))
        return 1;
    if (strcmp(d->section, "graphics"))
        return 0;
    renderer = model_value("graphics", "vcglide_renderer");
    if (!strcmp(d->key, "vcglide_threads"))
        return !_stricmp(renderer, "gpu");
    if (!strcmp(d->key, "render_scale") || !strcmp(d->key, "vsync"))
        return !_stricmp(renderer, "cpu");
    if (!strcmp(d->key, "texmag") || !strcmp(d->key, "upscale_2d"))
        return !_stricmp(renderer, "cpu") ||
               (ini_parse_uint(model_value("graphics", "render_scale"), 1, 8,
                               &scale) && scale == 1);
    if (!strcmp(d->key, "widescreen_hud") ||
        !strcmp(d->key, "widescreen_ratio"))
        return !model_bool("graphics", "widescreen");
    if (!strcmp(d->key, "window_scale"))
        return model_bool("graphics", "fullscreen");
    return 0;
}

/* The control follows setting_inert() and the label is dimmed to match
 * (launcher_control_tone()); the help caption stays as it is, because it is
 * what says why. A row disabled for its own reason (setting_title_override())
 * is left alone. */
static void setting_show_inert(unsigned index)
{
    int enable = !setting_inert(index);
    HWND c;

    if (!s_page || setting_overridden_everywhere(index))
        return;
    if ((c = GetDlgItem(s_page, IDC_SETTING_BASE + (int)index)))
        EnableWindow(c, enable);
    if ((c = GetDlgItem(s_page, IDC_LABEL_BASE + (int)index)))
        InvalidateRect(c, NULL, TRUE);
    if ((c = GetDlgItem(s_page, IDC_VALUE_BASE + (int)index)))
        InvalidateRect(c, NULL, TRUE);
    if ((c = GetDlgItem(s_page, IDC_FLANK_BASE + (int)index)))
        InvalidateRect(c, NULL, TRUE);
    if ((c = GetDlgItem(s_page, IDC_SUB_BASE + (int)index)))
        InvalidateRect(c, NULL, TRUE);
}

static void page_show_inert(void)
{
    unsigned i;

    if (!s_page)
        return;
    for (i = 0; i < SETTING_COUNT; i++)
        if (setting_on_page(i))
            setting_show_inert(i);
}

/* A setting checkbox is the box alone; its label is a static beside it. */
#define CHECK_BOX_W 32

/* ---- sliders -------------------------------------------------------------
 *
 * A NUMBER A PERSON SETS BY FEEL IS DRAGGED, NOT TYPED: SF_SLIDER rows are a
 * trackbar over [minimum, maximum], the value in words to its right and a
 * mark at the default. The file keeps the number; the position is only ever
 * written back when it differs from what the file said, so a value outside
 * the range is shown clamped and never rewritten by opening the page. */
#define SLIDER_VALUE_W  72
#define BALANCE_FRONT_W 52
#define BALANCE_SUB_W   84
#define BALANCE_VALUE_W 132

static int slider_row(unsigned index)
{
    return (k_settings[index].flags & SF_SLIDER) &&
           k_settings[index].type == ST_UINT;
}

/* [audio] balance, audio.c's balance_set(). Its two ends are
 * labelled with the bus each one favours, so the slider says what it does. */
static int balance_row(unsigned index)
{
    return !strcmp(k_settings[index].section, "audio") &&
           !strcmp(k_settings[index].key, "balance");
}

/* THE SLIDER'S TOP, which may be lower than the file's: [audio] volume
 * accepts up to 400 and the launcher offers 100, the game's own level, so
 * nobody drags the game into distortion. A file asking for more is shown at
 * the top and kept, never rewritten (pull_page()). */
static unsigned long slider_max(unsigned index)
{
    const setting_desc_t *d = &k_settings[index];

    if (!strcmp(d->section, "audio") && !strcmp(d->key, "volume"))
        return min(d->maximum, 100ul);
    return d->maximum;
}

/* THE BALANCE NEEDS ITS TWO ENDS, ITS WORDS AND A SLIDER WORTH DRAGGING, and
 * a two-column page under about 700 units has no room for all of that beside
 * the label: the slider shrank to a stub and the words ran over the box's
 * edge. Then the row stacks, as every row does on a compact page. */
#define BALANCE_MIN_BAR 160

static int balance_stacked(unsigned index)
{
    return !s_compact && balance_row(index) &&
           min(500, page_width() - 244 - 22) <
               BALANCE_FRONT_W + BALANCE_SUB_W + BALANCE_VALUE_W +
               BALANCE_MIN_BAR;
}

static int slider_clamp(unsigned index, unsigned long v)
{
    const setting_desc_t *d = &k_settings[index];

    if (v < d->minimum)
        v = d->minimum;
    if (v > slider_max(index))
        v = slider_max(index);
    return (int)v;
}

/* Where the model's value sits; the default when the text is not a number. */
static int slider_model_pos(unsigned index)
{
    unsigned v;

    if (!ini_parse_uint(model_slot(index), 0, 0xFFFFFFFFu, &v) &&
        !ini_parse_uint(k_settings[index].fallback, 0, 0xFFFFFFFFu, &v))
        v = (unsigned)k_settings[index].minimum;
    return slider_clamp(index, v);
}

/* The value in words. The balance's position means nothing to a player, so
 * it says which speaker is turned down and the level it then plays at, from
 * audio.c's balance_set(): at 15 the subwoofer plays at 30%. */
static void slider_text(unsigned index, int pos, char *right, size_t size)
{
    const setting_desc_t *d = &k_settings[index];

    if (balance_row(index)) {
        if (pos < 50)
            snprintf(right, size, "Subwoofer at %d%%", pos * 2);
        else if (pos > 50)
            snprintf(right, size, "Front at %d%%", (100 - pos) * 2);
        else
            snprintf(right, size, "Both at full");
    } else if (d->flags & SF_PERCENT) {
        snprintf(right, size, "%d%%", pos);
    } else if (!strcmp(d->key, "xinput_device")) {
        snprintf(right, size, "Pad %d", pos + 1);
    } else if (!strcmp(d->key, "rumble_agc_percent") &&
               pos == (int)d->minimum) {
        snprintf(right, size, "Off");
    } else if (!strcmp(d->key, "vcglide_threads") && pos == 0) {
        snprintf(right, size, "Automatic");
    } else {
        snprintf(right, size, "%d", pos);
    }
}

static void slider_show_value(unsigned index)
{
    HWND c = s_page ? GetDlgItem(s_page, IDC_SETTING_BASE + (int)index) : NULL;
    char right[48];

    if (!c)
        return;
    slider_text(index, (int)SendMessage(c, TBM_GETPOS, 0, 0), right,
                sizeof right);
    SetDlgItemTextA(s_page, IDC_VALUE_BASE + (int)index, right);
}

/* THE WHEEL SCROLLS THE PAGE, NOT THE SLIDER UNDER THE POINTER: a reader
 * scrolling past the Sound page would otherwise change the volume. Every
 * trackbar shares one class procedure, so one saved pointer serves them all. */
static WNDPROC s_trackbar_proc;

static LRESULT CALLBACK slider_proc(HWND h, UINT message, WPARAM wparam,
                                    LPARAM lparam)
{
    if (message == WM_MOUSEWHEEL)
        return SendMessage(GetParent(h), message, wparam, lparam);
    return CallWindowProcA(s_trackbar_proc, h, message, wparam, lparam);
}

/* A RADIO THAT GETS THE FOCUS CLICKS ITSELF: Windows sends BN_CLICKED from an
 * unchecked radio's WM_SETFOCUS unless a mouse press is under way. Tabbing or
 * moving past the controller list would pick each one passed. So the focus
 * only focuses: handle_command() drops the click while this is set, and every
 * check it moved is put back -- checking itself also unchecked the group's
 * picked radio. Space, Enter, A or a click picks.
 * And BM_SETCHECK on a radio sets or clears its WS_TABSTOP with the check,
 * which would leave every unpicked radio out of focusable(); the tab stop is
 * kept whatever the check. */
static int s_radio_focus;
/* Each radio's own previous procedure: under this one is make_control()'s
 * painter for a radio with a mark and the plain button for a tab, and one
 * stored procedure for both would hand every radio the first one's. */
static const char k_radio_next[] = "VCThunderRadioNext";

static WNDPROC radio_next(HWND h)
{
    return (WNDPROC)GetPropA(h, k_radio_next);
}

static LRESULT CALLBACK quiet_radio_proc(HWND, UINT, WPARAM, LPARAM);

static unsigned quiet_radio_checks(HWND h, HWND *radio, LRESULT *check,
                                   unsigned cap)
{
    HWND c = GetWindow(GetParent(h), GW_CHILD);
    unsigned n = 0;

    for (; c && n < cap; c = GetWindow(c, GW_HWNDNEXT)) {
        if ((WNDPROC)GetWindowLongPtrA(c, GWLP_WNDPROC) != quiet_radio_proc)
            continue;
        radio[n] = c;
        check[n++] = SendMessage(c, BM_GETCHECK, 0, 0);
    }
    return n;
}

static LRESULT CALLBACK quiet_radio_proc(HWND h, UINT message, WPARAM wparam,
                                         LPARAM lparam)
{
    if (message == WM_NCDESTROY) {
        WNDPROC next = radio_next(h);

        RemovePropA(h, k_radio_next);
        return CallWindowProcA(next, h, message, wparam, lparam);
    }
    if (message == WM_SETFOCUS) {
        HWND radio[64];
        LRESULT check[64], result;
        unsigned n = quiet_radio_checks(h, radio, check, 64), i, pass;

        s_radio_focus++;
        result = CallWindowProcA(radio_next(h), h, message, wparam, lparam);
        /* Unchecks first: putting a check back unchecks its group again. */
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < n; i++)
                if ((check[i] == BST_CHECKED) == (pass == 1) &&
                    SendMessage(radio[i], BM_GETCHECK, 0, 0) != check[i])
                    SendMessage(radio[i], BM_SETCHECK, (WPARAM)check[i], 0);
        s_radio_focus--;
        return result;
    }
    if (message == BM_SETCHECK) {
        LONG_PTR style = GetWindowLongPtrA(h, GWL_STYLE), now;
        LRESULT result = CallWindowProcA(radio_next(h), h, message, wparam,
                                         lparam);

        now = GetWindowLongPtrA(h, GWL_STYLE);
        if ((now ^ style) & WS_TABSTOP)
            SetWindowLongPtrA(h, GWL_STYLE,
                              (now & ~WS_TABSTOP) | (style & WS_TABSTOP));
        return result;
    }
    return CallWindowProcA(radio_next(h), h, message, wparam, lparam);
}

void quiet_radio(HWND radio)
{
    if (!radio)
        return;
    SetPropA(radio, k_radio_next,
             (HANDLE)GetWindowLongPtrA(radio, GWLP_WNDPROC));
    SetWindowLongPtrA(radio, GWLP_WNDPROC, (LONG_PTR)quiet_radio_proc);
}

/* Returns the trackbar. `x`, `y`, `w` are the whole value column: the
 * balance row's "Front" and "Subwoofer" at its two ends, then the value,
 * which every slider has last. */
static HWND render_slider(unsigned index, int x, int y, int w)
{
    const setting_desc_t *d = &k_settings[index];
    int balance = balance_row(index), pos = slider_model_pos(index);
    /* Too narrow for the end words even stacked (a 400-unit window): the
     * value still names the speaker, so the slider keeps the room. */
    int ends = balance && w >= BALANCE_FRONT_W + BALANCE_SUB_W +
                               BALANCE_VALUE_W + BALANCE_MIN_BAR;
    int left_w = ends ? BALANCE_FRONT_W : 0;
    int end_w = ends ? BALANCE_SUB_W : 0;
    int value_w = balance ? BALANCE_VALUE_W : SLIDER_VALUE_W;
    int bar_w = max(w - left_w - end_w - value_w, 40);
    int range = (int)(slider_max(index) - d->minimum);
    unsigned def;
    char right[48];
    HWND bar;

    slider_text(index, pos, right, sizeof right);
    if (ends) {
        make_control(0, "STATIC", "Front", SS_RIGHT, x, y + LABEL_DY,
                     left_w - 6, 20, s_page, IDC_FLANK_BASE + (int)index);
        make_control(0, "STATIC", "Subwoofer", SS_LEFT,
                     x + left_w + bar_w + 6, y + LABEL_DY, end_w - 6, 20,
                     s_page, IDC_SUB_BASE + (int)index);
    }
    bar = make_control(0, TRACKBAR_CLASSA, "",
                       TBS_HORZ | TBS_BOTTOM | WS_TABSTOP, x + left_w, y,
                       bar_w, CTRL_H, s_page, IDC_SETTING_BASE + (int)index);
    SendMessage(bar, TBM_SETRANGEMIN, FALSE, (LPARAM)d->minimum);
    SendMessage(bar, TBM_SETRANGEMAX, FALSE, (LPARAM)slider_max(index));
    SendMessage(bar, TBM_SETLINESIZE, 0, 1);
    SendMessage(bar, TBM_SETPAGESIZE, 0, range > 100 ? 10 : range > 20 ? 5 : 1);
    /* The mark is the default, except on the balance, where the one point
     * that means something is the middle: neither side turned down. */
    if (balance)
        SendMessage(bar, TBM_SETTIC, 0, 50);
    else if (ini_parse_uint(d->fallback, (unsigned)d->minimum,
                            (unsigned)d->maximum, &def) &&
             def > d->minimum && def < slider_max(index))
        SendMessage(bar, TBM_SETTIC, 0, (LPARAM)def);
    SendMessage(bar, TBM_SETPOS, TRUE, pos);
    if (!s_trackbar_proc)
        s_trackbar_proc = (WNDPROC)GetWindowLongPtrA(bar, GWLP_WNDPROC);
    SetWindowLongPtrA(bar, GWLP_WNDPROC, (LONG_PTR)slider_proc);
    make_control(0, "STATIC", right, SS_LEFT,
                 x + left_w + bar_w + end_w + 6, y + LABEL_DY, value_w - 6, 20,
                 s_page, IDC_VALUE_BASE + (int)index);
    return bar;
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
    int scoped_both = setting_overridden_everywhere(index);

    if (setting_is_check(index)) {
        int on = d->type == ST_BOOL ? model_bool(d->section, d->key)
                                    : toggle_is_on(index);

        /* THE BOX AND ITS LABEL ARE TWO CONTROLS. A disabled checkbox draws
         * its own text embossed whatever colour it is handed, which reads as
         * bold rather than greyed; a static of its own is dimmed by
         * launcher_control_tone(), and a click on it presses the box. */
        control = make_control(0, "BUTTON", "",
            BS_AUTOCHECKBOX | WS_TABSTOP, 22, y, CHECK_BOX_W, CTRL_H,
            s_page, IDC_SETTING_BASE + (int)index);
        SendMessage(control, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        make_control(0, "STATIC", d->label, SS_LEFT | SS_NOTIFY | SS_ENDELLIPSIS,
                     22 + CHECK_BOX_W, y + LABEL_DY,
                     (s_compact ? width - 44 : min(510, width - 44)) -
                     CHECK_BOX_W, 20, s_page, IDC_LABEL_BASE + (int)index);
        if (scoped_both)
            EnableWindow(control, FALSE);
    } else {
        /* SS_ENDELLIPSIS, and the checker caps the label length: a label
         * wider than its column WRAPS into the help caption underneath and
         * the two overlap, which is worse than a truncation that says so.
         * Caught on "Wheel: how much of its rotation to use (%)". */
        make_control(0, "STATIC", d->label, SS_LEFT | SS_ENDELLIPSIS, 22,
                     s_compact ? y + 4 : y + LABEL_DY,
                     s_compact ? width - 44 : 215, 22,
                     s_page, IDC_LABEL_BASE + (int)index);
        control_w = s_compact ? width - 44 - (has_button ? BUTTON_W + 8 : 0)
                              : min(310, width - 244 - 22 -
                                         (has_button ? BUTTON_W + 8 : 0));
        if (control_w < 120)
            control_w = 120;
        if (d->type == ST_BOOL) {
            /* SF_CHOICES: both answers named, the value spelled canonically
             * whatever spelling of a boolean the file has. */
            control = make_control(0, "COMBOBOX", "",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                s_compact ? 22 : 244, s_compact ? y + STACK_DY : y,
                control_w, 240, s_page,
                IDC_SETTING_BASE + (int)index);
            combo_add_choices(control, d->key, BOOL_CHOICES, NULL);
            combo_select_value(control, BOOL_CHOICES,
                               model_bool(d->section, d->key) ? "true" : "false");
        } else if (d->type == ST_ENUM) {
            control = make_control(0, "COMBOBOX", "",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                s_compact ? 22 : 244, s_compact ? y + STACK_DY : y,
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
            combo_add_choices(control, d->key, d->choices, axis_prefix);
            combo_select_value(control, d->choices, model_slot(index));
        } else if (scale_row(index)) {
            control = make_control(0, "COMBOBOX", "",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                s_compact ? 22 : 244, s_compact ? y + STACK_DY : y,
                control_w, 240, s_page,
                IDC_SETTING_BASE + (int)index);
            scale_combo_fill(control, index);
            render_scale_lines(index, y, control_w);
        } else if (slider_row(index)) {
            /* The balance row's two labelled ends need more than a plain
             * value column; nothing sits beside it, so it takes the room a
             * Browse button would. */
            int stack = s_compact || balance_stacked(index);
            int w = stack ? width - 44 :
                    balance_row(index) ? min(500, width - 244 - 22) :
                                         control_w;

            control = render_slider(index, stack ? 22 : 244,
                                    stack ? y + STACK_DY : y, w);
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
                (s_compact ? y + STACK_DY : y) + (CTRL_H - EDIT_H) / 2,
                control_w, EDIT_H, s_page, IDC_SETTING_BASE + (int)index);
            SendMessage(control, EM_SETLIMITTEXT, INI_VALUE_CAP - 1, 0);
        }
        button_x = s_compact ? width - 22 - BUTTON_W : 244 + control_w + 8;
        if (d->flags & (SF_DIR | SF_OPEN_FILE | SF_SAVE_FILE))
            make_control(0, "BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP,
                         button_x, s_compact ? y + STACK_DY : y, BUTTON_W,
                         CTRL_H, s_page, IDC_BROWSE_BASE + (int)index);
        if (d->flags & SF_AXIS)
            make_control(0, "BUTTON", "Detect", BS_PUSHBUTTON | WS_TABSTOP,
                         button_x, s_compact ? y + STACK_DY : y, BUTTON_W,
                         CTRL_H, s_page, IDC_DETECT_BASE + (int)index);
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
 * higher than a titled page needs. A Controls page adds its tab row under
 * the intro. */
static void add_page_header(void)
{
    make_control(0, "STATIC", page_intro(), SS_LEFT,
                 20, PAGE_INTRO_Y, page_width() - 40, page_intro_height(),
                 s_page, 0);
}


/* A ROW OF TABS, NOT A ROW OF DOTS: every subpage switch here is a place to
 * go, and a radio dot is this launcher's shape for a SETTING the page will
 * save. So: buttons laid out as a tab strip, owner-drawn in dark mode; under
 * High Contrast there is no owner-draw, so the row falls back to a PUSHLIKE
 * radio group the SYSTEM draws pressed in the user's own colours. Both take
 * which is lit from tab_is_selected(). The width is a MAXIMUM, as everything
 * here is. Returns the row's bottom edge. */
/* THE TWO TAB ROWS ARE ONE WIDTH: Controls / Force Feedback above, the three
 * games in the box under it, their outer edges in line. */
int tab_rows_width(void)
{
    return min(MODEL_TITLES * 160 + (MODEL_TITLES - 1) * 4, page_width() - 44);
}

/* `count` tabs filling `total` from `x`, 4 apart. Each edge is placed rather
 * than each width repeated, so the last tab ends exactly at x + total. */
int add_tab_row_at(int x, int y, int height, int total,
                   const char *const *labels, const int *ids,
                   unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        int left = x + (int)i * (total + 4) / (int)count;
        int right = x + (int)(i + 1) * (total + 4) / (int)count - 4;
        HWND tab = make_control(0, "BUTTON", labels[i],
            theme_dark() ? BS_PUSHBUTTON | WS_TABSTOP :
                          BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_TABSTOP |
                          (i ? 0 : WS_GROUP),
            left, y, right - left, height, s_page, ids[i]);
        /* By its scaled edges: a scaled left plus a scaled width can land a
         * pixel off the other row's edge. */
        MoveWindow(tab, scale_px(left), scale_px(y),
                   scale_px(right) - scale_px(left), scale_px(height), FALSE);
        if (!theme_dark()) {
            quiet_radio(tab);
            SendMessage(tab, BM_SETCHECK,
                        tab_is_selected(ids[i]) ? BST_CHECKED : BST_UNCHECKED,
                        0);
        }
    }
    return y + height;
}

static int add_tab_row(int y, int height, int total,
                       const char *const *labels, const int *ids,
                       unsigned count)
{
    return add_tab_row_at(20, y, height, total, labels, ids, count);
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
    add_page_header();
    add_tab_row(controls_tab_y(), CONTROLS_TAB_H, tab_rows_width(), labels, ids, 2);
}

static void layout_window(void);

void show_page(int page)
{
    pull_page();
    s_page_index = page;
    if (s_nav)
        SendMessage(s_nav, s_compact ? CB_SETCURSEL : LB_SETCURSEL,
                    (WPARAM)nav_row(page_to_nav(page)), 0);
    abandon_capture();
    render_page();
    layout_window();
}

/* The Launch page's cards and Files & Logging's list, in this order. Ultimate
 * Arctic Thunder runs in the Arctic cabinet and reads that cabinet's settings,
 * so a Play card is all it adds here. It is imported from its update disc,
 * laid over Arctic Thunder, which its card says. */
static const struct {
    const char *id;
    int play;                   /* the card's button                      */
    launcher_result_t result;   /* what pressing it returns to main.c     */
    int importable;             /* can "Import..." produce it?            */
    const char *from;           /* where it is imported from, if not a CHD */
} k_play[] = {
    { "hydro",   IDC_PLAY_HYDRO,   LAUNCHER_HYDRO,   1, 0 },
    { "offroad", IDC_PLAY_OFFROAD, LAUNCHER_OFFROAD, 1, 0 },
    { "arctic",  IDC_PLAY_ARCTIC,  LAUNCHER_ARCTIC,  1, 0 },
    { "uarctic", IDC_PLAY_UARCTIC, LAUNCHER_UARCTIC, 1,
      "Not imported yet: import its update disc, laid over Arctic Thunder." },
};
#define PLAY_TITLES ((int)(sizeof k_play / sizeof k_play[0]))

static int play_index(int id)
{
    int i;

    for (i = 0; i < PLAY_TITLES; i++)
        if (k_play[i].play == id)
            return i;
    return -1;
}

/* Files & Logging: where a game comes from, above the folder it goes to. */
static const char k_import_note[] =
    "VCThunder plays your own copy of each game. Choose its MAME CHD: the "
    "game is read straight out of it, checked to be the build VCThunder "
    "supports, and written into the game-data folder. Nothing is changed "
    "if it is not. Ultimate Arctic Thunder comes from its update disc (MAME's "
    "040503_1309 CHD, or your own .cue and .bin), laid over Arctic Thunder.";

static int files_panel_layout(int *note_y, int *note_h, int *button_y,
                              int *state_y)
{
    int width = page_width(), top = page_header_bottom();
    int nh = text_block_height(theme_font(), k_import_note, width - 40);
    if (note_y) *note_y = top + 26;
    if (note_h) *note_h = nh;
    if (button_y) *button_y = top + 26 + nh + 8;
    if (state_y) *state_y = top + 26 + nh + 8 + CTRL_H + 8;
    return top + 26 + nh + 8 + CTRL_H + 8 + PLAY_TITLES * 20 + BOX_GAP;
}

static int game_path(const char *title, char *out, size_t out_size);
static int title_files(const char *title, const char **name, const char **exe);

static void render_files_panel(void)
{
    int note_y, note_h, button_y, state_y, i, width = page_width();
    HWND button;
    files_panel_layout(&note_y, &note_h, &button_y, &state_y);
    box_title(20, page_header_bottom(), width - 40, 20, "Your games",
              SS_ENDELLIPSIS);
    make_control(0, "STATIC", k_import_note, SS_LEFT, 20, note_y, width - 40,
                 note_h, s_page, 0);
    button = make_control(0, "BUTTON", import_running() ? "Importing..." :
                          "Import from CHD...", BS_PUSHBUTTON | WS_TABSTOP,
                          20, button_y, min(240, width - 40), CTRL_H, s_page,
                          IDC_IMPORT);
    EnableWindow(button, !import_running());
    for (i = 0; i < PLAY_TITLES; i++) {
        char path[MAX_PATH + 64], line[MAX_PATH + 128];
        const char *name, *exe;
        int ready = game_path(k_play[i].id, path, sizeof path);

        title_files(k_play[i].id, &name, &exe);
        snprintf(line, sizeof line, "%s: %s", name,
                 ready ? "imported" : "not imported yet");
        make_control(0, "STATIC", line, SS_LEFT, 20, state_y + i * 20,
                     width - 40, 20, s_page, 0);
    }
    frame_topic(page_header_bottom(), state_y + PLAY_TITLES * 20);
}


/* A title's name and the executable that says it is installed: an ETS
 * profile's, or Arctic's, which has no profile. 0 for no such title. */
static int title_files(const char *title, const char **name, const char **exe)
{
    const game_profile_t *p = game_find(title);
    const arctic_title_t *w;

    if (p) {
        *name = p->name;
        *exe = p->exe;
        return 1;
    }
    if ((w = arctic_title_by_id(title)) != 0) {
        *name = w->ids->name;
        *exe = w->ids->file[ARCTIC_SNOW].name;
        return 1;
    }
    return 0;
}

static int game_path(const char *title, char *out, size_t out_size)
{
    const char *root = model_value("paths", "data_dir"), *name, *exe;
    char path[MAX_PATH];

    if (!title_files(title, &name, &exe))
        return 0;
    /* Exact first, then the case the filesystem uses: the launcher greys out a
     * title it cannot find, so without the fallback a good pack on a
     * case-sensitive data directory offers nothing to play. fs_resolve_ci(). */
    snprintf(path, sizeof path, "%s\\%s\\%s", root, title, exe);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, out_size, "%s", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s\\%s", title, exe);
    if (fs_resolve_ci(root, path, out, out_size))
        return 1;
    snprintf(out, out_size, "%s\\%s\\%s", root, title, exe);
    return 0;
}

static void render_home(void)
{
    int i;
    char validation[256];
    int invalid_page = PAGE_HOME;
    int settings_valid = validate_settings(validation, sizeof validation,
                                           &invalid_page);
    int width = page_width();
    /* THE SAME OUTLINE AS EVERY OTHER PAGE'S BOXES: frame_topic()'s x and
     * width, so the cards line up with the boxes on the page next door. */
    int group_x = s_tiny ? 6 : 12;
    int group_w = s_tiny ? width - 12 : width - 24;
    /* The first card's outline where every other page's first box is. */
    int next_card_y = s_tiny ? 15 : (s_compact ? 54 : page_header_bottom() - 8);

    if (s_compact) {
        if (!s_tiny)
            make_control(0, "STATIC", k_page_intro[PAGE_HOME], SS_LEFT,
                         12, 10, width - 24, 20, s_page, 0);
    } else {
        add_page_header();
    }
    /* THE COUNT OF LINES NOTHING READS (scan_ini_problems()), above the
     * cards. Why Play is off is said on each card instead, where Play is.
     * MEASURED, and it pushes the cards down: this page scrolls. */
    {
        char note[2][416];
        int notes = 0, n;
        int note_y = s_tiny ? 6 : (s_compact ? 32 : page_intro_height() + 18);

        if (!s_tiny && model_problem_total())
            snprintf(note[notes++], sizeof note[0],
                     model_problem_total() == 1 ?
                     "%u line in vcthunder.ini is not used. Help "
                     "says which." :
                     "%u lines in vcthunder.ini are not used. Help "
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
    {
        int block_x = s_compact ? 12 : 20;
        int top = next_card_y - (s_tiny ? 6 : 14);
        int after = render_import_block(top, block_x, group_w);
        if (after != top)
            next_card_y = after + (s_tiny ? 6 : 14);
    }
    /* TWO COLUMNS IN THE WIDE FORM, one in the compact: all four games on a
     * screen the size of a handheld's, each Play button as wide as its card.
     * The compact form keeps the button beside the text. Each card is as tall
     * as what is in it, and a row as tall as its taller card: at a constant
     * height a larger title face or a long status line puts the second line
     * outside the box, where it is simply not drawn. */
    {
        int columns = s_compact ? 1 : 2;
        int card_w = columns == 1 ? group_w : (group_w - 12) / 2;
        int play_h = columns == 1 ? CTRL_H : 60;
        int play_w = columns == 1 ? (s_tiny ? 72 : 92) : card_w - 36;
        int play_inset = s_tiny ? 10 : 18;
        int pad_top = s_tiny ? 6 : (s_compact ? 8 : 18);
        int pad_bottom = s_tiny ? 6 : (s_compact ? 10 : 18);
        /* The text ends where the button begins in one column, and spans
         * the card above the button in two. */
        int status_w = columns == 1 ? card_w - play_w - play_inset - 30 :
                                      card_w - 36;
        int floor_h = pad_top + CTRL_H + pad_bottom;
        HFONT title_font = s_tiny ? theme_title_small_font() : theme_title_font();
        int title_h[PLAY_TITLES], status_h[PLAY_TITLES], card_h[PLAY_TITLES];
        int ready[PLAY_TITLES];
        char status[PLAY_TITLES][MAX_PATH + 96];
        const char *name[PLAY_TITLES];

        if (!title_font)
            title_font = theme_font();
        for (i = 0; i < PLAY_TITLES; i++) {
            char path[MAX_PATH + 64];
            const char *exe;
            int importable = k_play[i].importable;

            name[i] = k_play[i].id;
            ready[i] = game_path(k_play[i].id, path, sizeof path);
            title_files(k_play[i].id, &name[i], &exe);
            /* ONE LINE UNDER THE TITLE: ready, or what stops Play. */
            if (!ready[i] && !importable)
                snprintf(status[i], sizeof status[i], "%s", s_tiny || s_compact ?
                         "Not installed." :
                         "Not installed, and it cannot be imported from here yet.");
            else if (!ready[i])
                snprintf(status[i], sizeof status[i], "%s",
                         s_tiny || s_compact ? "Not imported yet." :
                         k_play[i].from ? k_play[i].from :
                         "Not imported yet: import it from your own MAME CHD.");
            else if (import_running())
                snprintf(status[i], sizeof status[i],
                         "Play is off while a game is being imported.");
            else if (!settings_valid) {
                char where[96];

                page_where(invalid_page, where, sizeof where);
                if (invalid_page != PAGE_HOME)
                    snprintf(status[i], sizeof status[i],
                             "Play is off until this is fixed on %s: %s", where,
                             validation);
                else
                    snprintf(status[i], sizeof status[i],
                             "Play is off until this is fixed: %s", validation);
            } else
                snprintf(status[i], sizeof status[i], "Ready to play.");
            title_h[i] = text_block_height(title_font, name[i], status_w);
            status_h[i] = text_block_height(theme_font(), status[i], status_w);
            card_h[i] = pad_top + title_h[i] + 6 + status_h[i] + pad_bottom +
                        (columns == 1 ? 0 : 14 + play_h);
            if (card_h[i] < floor_h)
                card_h[i] = floor_h;
        }
        for (i = 0; i < PLAY_TITLES; i += columns) {
            int row_h = card_h[i], c;

            for (c = 1; c < columns && i + c < PLAY_TITLES; c++)
                row_h = max(row_h, card_h[i + c]);
            for (c = 0; c < columns && i + c < PLAY_TITLES; c++) {
                int t = i + c;
                int importable = k_play[t].importable;
                int card_x = group_x + c * (card_w + 12);
                int card_y = next_card_y;
                int title_y = card_y + pad_top;
                int path_y = title_y + title_h[t] + 6;
                int play_x = columns == 1 ?
                    card_x + card_w - play_w - play_inset : card_x + 18;
                int play_y = columns == 1 ? card_y + (row_h - play_h) / 2 :
                                            card_y + row_h - pad_bottom - play_h;
                HWND play, title_label, card = NULL;

                if (theme_dark()) {
                    card = make_control(0, "VCThunderLauncherCard", "", 0,
                                        card_x, card_y, card_w, row_h, s_page, 0);
                    /* The last column ends where every box on the page ends. */
                    if (c == columns - 1)
                        MoveWindow(card, scale_px(card_x), scale_px(card_y),
                                   page_right_px(group_x) - scale_px(card_x),
                                   scale_px(row_h), FALSE);
                } else {
                    HWND box = make_control(0, "BUTTON", "", BS_GROUPBOX,
                                 card_x, card_y - (s_tiny ? 8 : 14), card_w,
                                 row_h + (s_tiny ? 8 : 16), s_page, 0);

                    if (c == columns - 1)
                        MoveWindow(box, scale_px(card_x),
                                   scale_px(card_y - (s_tiny ? 8 : 14)),
                                   page_right_px(group_x) - scale_px(card_x),
                                   scale_px(row_h + (s_tiny ? 8 : 16)), FALSE);
                }
                title_label = make_control(0, "STATIC", name[t], SS_LEFT,
                                           card_x + 18, title_y, status_w,
                                           title_h[t], s_page, 0);
                SendMessage(title_label, WM_SETFONT, (WPARAM)title_font, TRUE);
                make_control(0, "STATIC", status[t], SS_LEFT, card_x + 18,
                             path_y, status_w, status_h[t], s_page, 0);
                /* A game that is not there yet is imported from this button. */
                play = make_control(0, "BUTTON",
                                    ready[t] || !importable ? "Play" : "Import...",
                                    BS_DEFPUSHBUTTON | WS_TABSTOP,
                                    play_x, play_y, play_w, play_h, s_page,
                                    k_play[t].play);
                if (!s_compact && theme_play_font())
                    SendMessage(play, WM_SETFONT, (WPARAM)theme_play_font(), TRUE);
                EnableWindow(play, !import_running() &&
                                   (ready[t] ? settings_valid : importable));
                if (card)
                    SetWindowPos(card, HWND_BOTTOM, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            /* The air between two outlines, as between any two boxes. */
            next_card_y += row_h + (s_tiny ? 9 : BOX_GAP - 12);
        }
    }
    s_content_height = next_card_y + 8;
}

/* A control Hydro does not have is hidden while Hydro is selected, and a NULL
 * Hydro label is the only thing that says so. */
int action_visible(unsigned action)
{
    return k_actions[action].label[model_controls_title()] != NULL;
}

/* THE PRODUCT NAME THE INI WOULD CARRY, which is what `pick_device()` matches
 * against, the probe prefixes its rows with "DirectInput: " and the host
 * never sees that. */
const char *probe_product_name(const input_probe_device_t *device)
{
    const char *colon = device ? strstr(device->name, ": ") : NULL;

    return colon ? colon + 2 : (device ? device->name : "");
}

const char *probe_provider(const input_probe_device_t *device)
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
int probe_id_of(const input_probe_device_t *d, ini_device_id_t *ids,
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

/* THE BEZEL BOX, last on the Graphics page: each game its own art, so a row
 * per game, each a switch, the file and Choose..., and a line saying what the
 * file is. The file is copied into bezels\ on Apply (model_save()). Ultimate
 * Arctic Thunder's row shows Arctic's art until it is given its own. */
static int render_bezel_panel(int top)
{
    const char *const *name = k_bezel_name;
    static const char intro[] = "PNG art with a transparent screen opening.";
    int width = page_width(), y, t;
    int control_w = s_compact ? width - 44 - BUTTON_W - 8 :
                                min(310, width - 244 - 22 - BUTTON_W - 8);

    if (control_w < 120)
        control_w = 120;
    y = group_header(top, "Bezel", 0, NULL, 0, 0);
    {
        int h = text_block_height(theme_font(), intro, width - 44);
        make_control(0, "STATIC", intro, SS_LEFT, 22, y, width - 44, h,
                     s_page, 0);
        y += h + 10;
    }
    for (t = 0; t < MODEL_BEZELS; t++) {
        char path[MAX_PATH], note[224], art[192];
        int inherits = model_bezel_inherits(t);
        const char *base;
        int on = model_bezel_get(t, path, sizeof path);
        int value_y = s_compact ? y + CTRL_H + 2 : y;
        HWND box;

        box = make_control(0, "BUTTON", name[t], BS_AUTOCHECKBOX | WS_TABSTOP,
                           22, y, s_compact ? width - 44 : 215, CTRL_H, s_page,
                           IDC_BEZEL_ENABLE + t);
        SendMessage(box, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        base = strrchr(path, '\\');
        if (!base || (strrchr(path, '/') && strrchr(path, '/') > base))
            base = strrchr(path, '/');
        base = base ? base + 1 : path;
        make_control(0, "STATIC", path[0] ? base : "None",
                     SS_LEFT | SS_ENDELLIPSIS, s_compact ? 22 : 244,
                     value_y + LABEL_DY, control_w, 20, s_page,
                     IDC_BEZEL_FILE + t);
        make_control(0, "BUTTON", "Choose...", BS_PUSHBUTTON | WS_TABSTOP,
                     s_compact ? width - 22 - BUTTON_W : 244 + control_w + 8,
                     value_y, BUTTON_W, CTRL_H, s_page, IDC_BEZEL_CHOOSE + t);
        if (!path[0]) {
            snprintf(art, sizeof art, "No art chosen.");
        } else {
            const bezel_info_t *info = bezel_info(path);
            if (info->ok)
                snprintf(art, sizeof art, "%dx%d.", info->w, info->h);
            else
                snprintf(art, sizeof art, "This file cannot be used: %s.",
                         info->why);
        }
        snprintf(note, sizeof note, "%s%s",
                 inherits ? "Same as Arctic Thunder. " : "", art);
        y = value_y + CTRL_H + 4;
        {
            int h = text_block_height(theme_font(), note, width - 44);
            make_control(0, "STATIC", note, SS_LEFT, 22, y, width - 44, h,
                         s_page, IDC_BEZEL_NOTE + t);
            y += h;
        }
        /* Ultimate's own art, or its own off, can go back to Arctic's. */
        if (t == MODEL_BEZEL_UARCTIC && !inherits) {
            make_control(0, "BUTTON", "Same as Arctic Thunder",
                         BS_PUSHBUTTON | WS_TABSTOP, 22, y + 6,
                         min(200, width - 44), CTRL_H, s_page,
                         IDC_BEZEL_INHERIT);
            y += 6 + CTRL_H;
        }
        y += t < MODEL_BEZELS - 1 ? 10 : 0;
    }
    frame_topic(top, y);
    return y;
}

static void render_settings_page(void)
{
    unsigned i;

    if (page_is_controls(s_page_index))
        add_controls_header();
    else
        add_page_header();
    if (s_page_index == PAGE_CONTROLS)
        s_panel_bottom = render_controls_panel();
    else if (s_page_index == PAGE_FFB)
        s_panel_bottom = render_ffb_panel();
    else if (s_page_index == PAGE_LINK)
        render_link_panel();
    else if (s_page_index == PAGE_TROUBLE)
        render_trouble_panel();
    else if (s_page_index == PAGE_FILES)
        render_files_panel();
    {
        int top = page_settings_top(), bottom = top, group = -1;

        /* THE ROWS ARE TOPICS TOO, one box per run of a group; on Audio,
         * Game System and Files the rows are the only box the page has. The
         * disclosure above them stays outside, as on the Controls page. Each
         * box is framed once its rows exist, as frame_topic() requires. */
        for (i = 0; i < SETTING_COUNT; i++) {
            int y;

            if (!setting_on_page(i))
                continue;
            y = page_setting_y(i);
            if (k_settings[i].group != group) {
                if (group >= 0)
                    frame_topic(top, bottom);
                group = k_settings[i].group;
                top = y;
                top = y - BOX_HEAD;
                group_header(top, settings_box_title(group), 0, NULL, 0, 0);
            }
            render_setting(i);
            setting_show_inert(i);
            bottom = y + setting_row_height(i);
        }
        if (group >= 0)
            frame_topic(top, bottom);
        if (s_page_index == PAGE_GRAPHICS)
            bottom = render_bezel_panel(bottom + BOX_GAP);
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
        if (k_settings[i].type == ST_BOOL && control_is_button(c))
            snprintf(model_slot(i), INI_VALUE_CAP, "%s",
                     SendMessage(c, BM_GETCHECK, 0, 0) == BST_CHECKED ?
                     "true" : "false");
        else if (k_settings[i].type == ST_BOOL) {
            /* Unchanged is left as the file spells it, so opening the page
             * and pressing Apply writes nothing. */
            LRESULT sel = SendMessage(c, CB_GETCURSEL, 0, 0);
            int on = sel != CB_ERR &&
                     SendMessage(c, CB_GETITEMDATA, (WPARAM)sel, 0) == 1;

            if (on != model_bool(k_settings[i].section, k_settings[i].key))
                snprintf(model_slot(i), INI_VALUE_CAP, "%s",
                         on ? "true" : "false");
        } else if ((k_settings[i].flags & SF_TOGGLE) && control_is_button(c)) {
            /* By the control on screen, not by s_advanced: the switch flips
             * before the pull that precedes the rebuild. Ticked keeps a
             * filter already chosen (fsr1 stays fsr1) and otherwise takes
             * the row's default; unticked is the first choice. */
            int on = SendMessage(c, BM_GETCHECK, 0, 0) == BST_CHECKED;

            if (!on)
                choice_at(k_settings[i].choices, 0, model_slot(i),
                          INI_VALUE_CAP);
            else if (!toggle_is_on(i))
                snprintf(model_slot(i), INI_VALUE_CAP, "%s",
                         k_settings[i].fallback);
        } else if (scale_row(i))
            scale_combo_value(c, model_slot(i), INI_VALUE_CAP);
        else if (slider_row(i)) {
            int pos = (int)SendMessage(c, TBM_GETPOS, 0, 0);

            if (pos != slider_model_pos(i))
                snprintf(model_slot(i), INI_VALUE_CAP, "%d", pos);
        } else if (k_settings[i].type == ST_MHZ_UINT) {
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
    /* A box's background is the page's: a press on it drags the page. */
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
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

/* THE KEYBOARD'S PLACE, ROUND A CONTROL THAT DOES NOT DRAW IT. A setting's
 * checkbox has no text of its own -- its label is a static beside it -- so the
 * system's dotted outline round the text has nothing to go round, and a
 * slider's and a list's are faint on the dark page. An owner-drawn button draws
 * its own, except where it would not show. The ring takes in the row's label,
 * and shows only once the keyboard or a controller has been used. 0 when there
 * is none to draw. */
static int focus_ring(RECT *ring)
{
    HWND f = GetFocus(), label;
    int id;
    RECT part;

    if (!s_page || !f || GetParent(f) != s_page ||
        (SendMessage(s_window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        return 0;
    id = GetDlgCtrlID(f);
    /* A Play button is the game's colour, and its own blue edge is lost on
     * it. */
    if (control_is_button(f) && play_index(id) < 0 &&
        (GetWindowLongPtrA(f, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
        return 0;
    GetWindowRect(f, ring);
    label = id >= IDC_SETTING_BASE && id < IDC_SETTING_BASE + (int)SETTING_COUNT ?
            GetDlgItem(s_page, id - IDC_SETTING_BASE + IDC_LABEL_BASE) : NULL;
    if (label && IsWindowVisible(label)) {
        GetWindowRect(label, &part);
        UnionRect(ring, ring, &part);
    }
    MapWindowPoints(NULL, s_page, (POINT *)ring, 2);
    InflateRect(ring, scale_px(5), scale_px(3));
    return 1;
}

/* FOUR EDGES, NOT A PAINT. Every box on a page is a frame child covering it,
 * and the page clips its children, so nothing the page paints shows inside a
 * box. The edges are children made after everything else, which puts them on
 * top (a sibling created later clips the ones before it), and
 * as children they scroll with the page. A rebuild destroys them with it. */
static HWND s_ring[4];

/* By the edges themselves: a rebuilt page can be given the handle value of
 * the one it replaced. */
static int focus_ring_alive(void)
{
    return s_ring[0] && IsWindow(s_ring[0]) && GetParent(s_ring[0]) == s_page &&
           GetDlgCtrlID(s_ring[0]) == IDC_FOCUS_RING;
}

static void place_focus_ring(void)
{
    RECT ring;
    int i, t = scale_px(2);

    if (!s_page)
        return;
    if (!focus_ring(&ring)) {
        if (focus_ring_alive())
            for (i = 0; i < 4; i++)
                ShowWindow(s_ring[i], SW_HIDE);
        return;
    }
    if (!focus_ring_alive())
        for (i = 0; i < 4; i++)
            s_ring[i] = make_control(0, "STATIC", "", SS_LEFT, 0, 0, 1, 1,
                                     s_page, IDC_FOCUS_RING);
    MoveWindow(s_ring[0], ring.left, ring.top, ring.right - ring.left, t, TRUE);
    MoveWindow(s_ring[1], ring.left, ring.bottom - t, ring.right - ring.left, t,
               TRUE);
    MoveWindow(s_ring[2], ring.left, ring.top, t, ring.bottom - ring.top, TRUE);
    MoveWindow(s_ring[3], ring.right - t, ring.top, t, ring.bottom - ring.top,
               TRUE);
    for (i = 0; i < 4; i++) {
        SetWindowPos(s_ring[i], HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(s_ring[i], SW_SHOWNA);
    }
}

static LRESULT CALLBACK page_proc(HWND h, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    if (message == WM_DRAWITEM &&
        (draw_dark_button((DRAWITEMSTRUCT *)lparam) ||
         theme_draw_combo_item((DRAWITEMSTRUCT *)lparam) ||
         draw_scale_line((DRAWITEMSTRUCT *)lparam) ||
         draw_logo((DRAWITEMSTRUCT *)lparam)))
        return TRUE;
    /* PRESS AND DRAG ON THE PAGE ITSELF SCROLLS IT, for everyone: a finger
     * has no wheel, and gamescope delivers a touchscreen as a mouse. A press
     * on a control is the control's; one on a caption or a box's background
     * reaches the page, because neither answers a hit test. */
    if (message == WM_LBUTTONDOWN) {
        s_drag_y = GET_Y_LPARAM(lparam);
        s_drag_from = s_page_scroll;
        s_dragging = 1;
        SetCapture(h);
        return 0;
    }
    /* Only a move made with the button down: moving the children under the
     * pointer makes Windows send a move of its own, and the capture delivers
     * the release however the drag ends. */
    if (message == WM_MOUSEMOVE && s_dragging && (wparam & MK_LBUTTON)) {
        set_page_scroll(h, s_drag_from - (GET_Y_LPARAM(lparam) - s_drag_y));
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        if (s_dragging)
            ReleaseCapture();
        s_dragging = 0;
        return 0;
    }
    /* A CAPTURE TAKEN AWAY ENDS THE DRAG: Alt+Tab or a message box takes it,
     * the release then goes elsewhere, and a drag left standing would jump
     * the page from its old anchor at the next press-and-move. */
    if (message == WM_CAPTURECHANGED && (HWND)lparam != h) {
        s_dragging = 0;
        return 0;
    }
    if (message == WM_CTLCOLORSTATIC &&
        GetDlgCtrlID((HWND)lparam) == IDC_FOCUS_RING) {
        static HBRUSH ring;

        if (!ring)
            ring = theme_dark() ? CreateSolidBrush(RGB(0, 120, 215)) :
                                  GetSysColorBrush(COLOR_HIGHLIGHT);
        return (LRESULT)ring;
    }
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
    if (message == WM_SETCURSOR &&
        GetDlgCtrlID((HWND)wparam) == IDC_ABOUT_LINK) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    /* A slider moved. Only a real change marks the file dirty: a click on
     * the thumb, or End on a slider already at its end, writes nothing. */
    if (message == WM_HSCROLL && lparam) {
        int id = GetDlgCtrlID((HWND)lparam);

        if (id >= IDC_SETTING_BASE && id < IDC_SETTING_BASE + (int)SETTING_COUNT &&
            slider_row((unsigned)(id - IDC_SETTING_BASE))) {
            unsigned index = (unsigned)(id - IDC_SETTING_BASE);

            slider_show_value(index);
            if ((int)SendMessage((HWND)lparam, TBM_GETPOS, 0, 0) !=
                slider_model_pos(index)) {
                pull_page();
                mark_dirty();
                page_show_inert();
            }
        }
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

static void choose_initial_probe(void);

/* What s_cabinet_op asked for, now that the page has been pulled. */
static void run_cabinet_op(void)
{
    int op = s_cabinet_op, ok;

    s_cabinet_op = CABINET_NONE;
    s_cabinet_failed = 0;
    if (op == CABINET_SPLIT)
        ok = model_controls_split_on();
    else if (op == CABINET_JOIN)
        ok = model_controls_join();
    else
        ok = model_controls_show(op - CABINET_HYDRO);
    if (!ok) {
        s_cabinet_failed = 1;
        set_status(op == CABINET_SPLIT || op == CABINET_JOIN ?
                   "Not enough memory to change which controller each game "
                   "uses." : "Not enough memory to switch games.");
        return;
    }
    /* The tick is the controller the SHOWN cabinet drives with; shared, it
     * is the same controller and stays ticked. */
    if (!model_controls_split() && op != CABINET_JOIN)
        return;
    s_probe_selected = -1;
    choose_initial_probe();
}

void render_page(void)
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
    /* FORGET WHAT WAS LIT before the rebuild, or a slot whose input is
     * still held keeps its bit, the scanner sees no change, and the light
     * never returns. The next timer tick re-lights whatever is down. */
    memset(s_lit_slot, 0, sizeof s_lit_slot);
    if (s_page) {
        pull_page();
        /* The rows just read belonged to the device that is being replaced.
         * Between the pull and the rebuild is the only moment the new
         * device's own values can be put in front of either. */
        if (s_reload_devices) {
            reload_device_settings();
            s_reload_devices = 0;
        }
        if (s_cabinet_op != CABINET_NONE)
            run_cabinet_op();
        DestroyWindow(s_page);
    }
    GetClientRect(s_window, &rc);
    /* A CONTROL PARENT, NOT A TAB STOP: Tab walks into the page's own
     * controls instead of stopping on the page as one. */
    s_page = make_control(WS_EX_CLIENTEDGE | WS_EX_CONTROLPARENT,
        "VCThunderLauncherPage", "", WS_VSCROLL | WS_CLIPCHILDREN,
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
    s_scale_measured = 0;
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

static UINT launcher_dpi(HWND h)
{
    RECT rc;
    UINT fit;

    GetClientRect(h, &rc);
    fit = (UINT)min(MulDiv(rc.right, 96, DESIGN_W),
                    MulDiv(rc.bottom, 96, DESIGN_H));
    return max(theme_floor_dpi(h), fit);
}

/* BORDERLESS OVER THE MONITOR THE WINDOW IS ON, as window.c's
 * apply_fullscreen() covers it for a game; the display mode is never touched.
 * VCT_LAUNCHER_SCREEN=WxH stands in for a screen this one is not: the window
 * takes that size in the monitor's corner, which is how a handheld's screen is
 * shot from a desktop. */
static void cover_monitor(void)
{
    MONITORINFO monitor;
    const char *screen = getenv("VCT_LAUNCHER_SCREEN");
    RECT r;
    int w, h;

    memset(&monitor, 0, sizeof monitor);
    monitor.cbSize = sizeof monitor;
    if (!GetMonitorInfoA(MonitorFromWindow(s_window, MONITOR_DEFAULTTONEAREST),
                         &monitor))
        return;
    r = monitor.rcMonitor;
    if (screen && sscanf(screen, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
        r.right = r.left + w;
        r.bottom = r.top + h;
    }
    SetWindowPos(s_window, HWND_TOP, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
}

/* THE LAUNCHER IS FULLSCREEN WHEN THE GAMES ARE: [graphics] fullscreen as the
 * file says it, when the window opens and after an Apply. One switch for both,
 * and no key of the launcher's own, so the menu shows what Play will do. */
static void launcher_set_fullscreen(int on)
{
    LONG_PTR style;

    on = on ? 1 : 0;
    if (!s_window || on == s_fullscreen)
        return;
    style = GetWindowLongPtrA(s_window, GWL_STYLE);
    if (on) {
        s_windowed_place.length = sizeof s_windowed_place;
        GetWindowPlacement(s_window, &s_windowed_place);
        s_fullscreen = 1;
        SetWindowLongPtrA(s_window, GWL_STYLE,
                          (style & ~(LONG_PTR)(WS_OVERLAPPEDWINDOW |
                                               WS_MAXIMIZE | WS_MINIMIZE)) |
                          WS_POPUP);
        cover_monitor();
    } else {
        s_fullscreen = 0;
        SetWindowLongPtrA(s_window, GWL_STYLE,
                          (style & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(s_window, &s_windowed_place);
        SetWindowPos(s_window, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static void layout_window(void)
{
    RECT rc;
    int margin = scale_px(12), nav_w = s_compact ? 0 : nav_width();
    int top = scale_px(s_compact ? 8 + CTRL_H + 8 : 12);
    int footer = scale_px(s_tiny ? CTRL_H + 8 : CTRL_H + 26);
    int button_w = scale_px(s_tiny ? 80 : 104);
    int button_h = scale_px(CTRL_H);
    int button_y, left, width;

    GetClientRect(s_window, &rc);
    button_y = rc.bottom - (footer + button_h) / 2;
    /* ONE COLUMN, CENTRED, once the page would be wider than it can use: a
     * screen wider than 16:10 grows the design sideways, and a box stretched
     * across an ultrawide is a line nobody can follow. */
    left = margin;
    width = rc.right - margin * 2;
    if (!s_compact && width > nav_w + scale_px(12 + PAGE_MAX_W)) {
        width = nav_w + scale_px(12 + PAGE_MAX_W);
        left = (rc.right - width) / 2;
    }
    if (s_compact) {
        MoveWindow(s_nav, left, scale_px(s_tiny ? 6 : 8), width,
                   scale_px(240), TRUE);
        if (s_page)
            MoveWindow(s_page, left, top, width, rc.bottom - top - footer,
                       TRUE);
    } else {
        int rows = (int)SendMessage(s_nav, LB_GETCOUNT, 0, 0);
        int nav_h = rc.bottom - top - footer, tiles_h = nav_h;

        /* TALL TILES WHILE THEY FIT, never shorter than a control. */
        if (rows > 0) {
            int item_h = min(scale_px(60),
                             max(scale_px(CTRL_H + NAV_TILE_GAP), nav_h / rows));

            SendMessage(s_nav, LB_SETITEMHEIGHT, 0, item_h);
            tiles_h = min(nav_h, rows * item_h);
        }
        /* The list ends at its last tile. */
        MoveWindow(s_nav, left, top, nav_w, tiles_h, TRUE);
        InvalidateRect(s_nav, NULL, TRUE);
        if (s_page)
            MoveWindow(s_page, left + nav_w + scale_px(12), top,
                       width - nav_w - scale_px(12),
                       rc.bottom - top - footer, TRUE);
    }
    ShowWindow(s_status, s_tiny ? SW_HIDE : SW_SHOW);
    if (!s_tiny)
        MoveWindow(s_status, left, button_y + (button_h - scale_px(24)) / 2,
                   width - button_w * 2 - scale_px(16), scale_px(24), TRUE);
    MoveWindow(s_apply, left + width - button_w * 2 - scale_px(8), button_y,
               button_w, button_h, TRUE);
    MoveWindow(s_close, left + width - button_w, button_y,
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

/* Choose a game's bezel art: a PNG, checked by the game's own reader before
 * it is accepted. Returns 1 when one was chosen. Nothing is copied or written
 * until Apply. */
static int choose_bezel(int title)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "", dest[MAX_PATH], start[MAX_PATH], text[512];
    char a[MAX_PATH], b[MAX_PATH];
    const bezel_info_t *info;
    char *slash;

    model_bezel_destination(title, dest, sizeof dest);
    snprintf(start, sizeof start, "%s", dest);
    if ((slash = strrchr(start, '\\')))
        *slash = '\0';
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = s_window;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof path;
    ofn.lpstrFilter = "PNG images (*.png)\0*.png\0\0";
    ofn.lpstrInitialDir = GetFileAttributesA(start) != INVALID_FILE_ATTRIBUTES
                              ? start : NULL;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST |
                OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn))
        return 0;
    info = bezel_info(path);
    if (!info->ok) {
        snprintf(text, sizeof text, "%s cannot be used as bezel art: %s.",
                 path, info->why);
        MessageBoxA(s_window, text, "VCThunder settings",
                    MB_OK | MB_ICONWARNING);
        return 0;
    }
    ini_resolve_path(path, a, sizeof a);
    ini_resolve_path(dest, b, sizeof b);
    if (_stricmp(a, b) && GetFileAttributesA(b) != INVALID_FILE_ATTRIBUTES) {
        snprintf(text, sizeof text, "Replace %s with %s?\n\nThe file is "
                 "replaced when you press Apply.", b, a);
        if (MessageBoxA(s_window, text, "VCThunder settings",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
    }
    model_bezel_choose(title, path);
    snprintf(text, sizeof text, "%s's bezel art is chosen. Press Apply to keep "
             "it.", k_bezel_name[title]);
    mark_dirty();
    set_status(text);
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
    if (k_settings[index].type == ST_BOOL && control_is_button(c))
        SendMessage(c, BM_SETCHECK,
                    model_bool(section, key) ? BST_CHECKED : BST_UNCHECKED, 0);
    else if (k_settings[index].type == ST_BOOL)
        combo_select_value(c, BOOL_CHOICES,
                           model_bool(section, key) ? "true" : "false");
    else if ((k_settings[index].flags & SF_TOGGLE) && control_is_button(c))
        SendMessage(c, BM_SETCHECK,
                    toggle_is_on((unsigned)index) ? BST_CHECKED : BST_UNCHECKED,
                    0);
    else if (k_settings[index].type == ST_ENUM)
        combo_select_value(c, k_settings[index].choices, model_slot(index));
    else if (scale_row((unsigned)index))
        scale_combo_fill(c, (unsigned)index);
    else if (slider_row((unsigned)index)) {
        SendMessage(c, TBM_SETPOS, TRUE, slider_model_pos((unsigned)index));
        slider_show_value((unsigned)index);
    } else
        SetWindowTextA(c, model_slot(index));
    scale_lines_refresh();
    page_show_inert();
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
    snprintf(text, sizeof text, "%s gets the %s%s%s. Press Apply to write that "
             "to vcthunder.ini.", probe_product_name(d),
             d->backend == INPUT_PROBE_XINPUT ? "rumble" : "force feedback",
             !model_controls_split() ? "" : " in ",
             !model_controls_split() ? "" :
             model_title_name(model_controls_title()));
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
 * src/launcher_capture.c holds the slots, Detect and Set up, and none of its
 * fields are reachable from here on purpose (launcher_capture.h). These are the
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

HWND launcher_page(void)
{
    return s_page;
}

int launcher_show_raw(void)
{
    return s_show_raw;
}

int launcher_show_legacy(void)
{
    return s_show_legacy;
}

int launcher_device_moving(unsigned index)
{
    return index < INPUT_PROBE_MAX_DEVICES && s_dev_active[index] &&
           (DWORD)(GetTickCount() - s_dev_active[index]) < DEVICE_ACTIVE_MS;
}

int launcher_binding_title(void)
{
    return model_controls_title();
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

int pad_refs_same(const char *a, const char *b)
{
    char la[INI_LABEL_CAP], lb[INI_LABEL_CAP];
    unsigned oa, ob;
    int ra, rb;

    if (!ini_parse_pad_ref(a, la, sizeof la, &oa) ||
        !ini_parse_pad_ref(b, lb, sizeof lb, &ob))
        return !_stricmp(a, b);
    if (oa != ob)
        return 0;
    if (!_stricmp(la, lb))
        return 1;
    /* Two labels can name one controller, and a bare ordinal is whichever
     * drives the car: ask the probe first, then compare the names. */
    ra = la[0] ? device_for_label(la) : s_probe_selected;
    rb = lb[0] ? device_for_label(lb) : s_probe_selected;
    if (ra >= 0 && ra == rb)
        return 1;
    return !_stricmp(la[0] ? la : primary_label(), lb[0] ? lb : primary_label());
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
    uint32_t now[BINDING_SLOTS];
    static int lit_title = -1;
    unsigned i, s;

    if (s_page_index != PAGE_CONTROLS || !s_page) {
        /* The controls are gone; forget what was lit, or the next render of
         * this page inherits lights for slots nothing is holding. */
        memset(s_lit_slot, 0, sizeof s_lit_slot);
        lit_title = -1;
        return;
    }
    if (lit_title != model_controls_title()) {
        memset(s_lit_slot, 0, sizeof s_lit_slot);   /* the other cabinet's */
        lit_title = model_controls_title();
    }
    memset(now, 0, sizeof now);
    /* lit_title is the cabinet shown from here down. */
    for (i = 0; i < ACTION_COUNT; i++) {
        binding_item_t row[IO_MAX_KEY_REFS + IO_MAX_PAD_REFS];
        unsigned e, held;

        if (!action_visible(i))
            continue;
        held = model_binding_items(lit_title, i, row,
                                   sizeof row / sizeof row[0]);
        if (held > sizeof row / sizeof row[0])
            held = sizeof row / sizeof row[0];
        for (e = 0; e < held; e++) {
            int down = 0;

            if (!row[e].pad) {
                /* GetAsyncKeyState is process-agnostic, so the keyboard is
                 * only asked while this window is the foreground one, the
                 * same trap `input_background` documents for the host. */
                int vk = GetForegroundWindow() == s_window ?
                         io_config_key_code(row[e].value) : 0;
                down = vk && (GetAsyncKeyState(vk) & 0x8000);
            } else {
                char label[INI_LABEL_CAP];
                unsigned ordinal;

                down = ini_parse_pad_ref(row[e].value, label, sizeof label,
                                         &ordinal) &&
                       ordinal && pad_ref_down(label, ordinal);
            }
            /* An input past the last slot is shown, counted, in that slot. */
            if (down)
                now[e < BINDING_SLOTS ? e : BINDING_SLOTS - 1] |= 1u << i;
        }
    }
    for (i = 0; i < ACTION_COUNT; i++) {
        uint32_t bit = 1u << i;
        int changed = 0;

        for (s = 0; s < BINDING_SLOTS; s++)
            if ((now[s] ^ s_lit_slot[s]) & bit) {
                s_lit_slot[s] ^= bit;
                changed = 1;
            }
        if (changed)
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
    char text[256];

    if (n > INPUT_PROBE_MAX_DEVICES)
        n = INPUT_PROBE_MAX_DEVICES;
    for (i = 0; i < n; i++) {
        int on = launcher_device_moving(i);
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
        /* The driver's name for the axis; the ini's only when the axis is
         * on another controller, whose label is the part worth reading. */
        if (drv && drv[0] && !role->label[0])
            snprintf(what[r], sizeof what[r], "%s", drv);
        else if (drv && drv[0])
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
            snprintf(what[i], sizeof what[i], "%s", drv);
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
    if (missing[0] && d->backend == INPUT_PROBE_DINPUT)
        snprintf(live, sizeof live,
                 "No axis found for: %s. Press Set up... and let it measure "
                 "them.", missing);
    else if (missing[0])
        snprintf(live, sizeof live,
                 "No axis found for: %s. Turn on Show advanced settings on "
                 "Help to choose another.", missing);
    else
        snprintf(live, sizeof live, "%s", k_driving_hint);
    launcher_note_lines(live, "");
}


/* High Contrast only: the dark theme lights the slot itself (theme_button_t).
 * ASCII on purpose, these controls are written with the ...A entry points. */
#define BINDING_ACTIVE_MARK "  <"

void update_binding_display(unsigned action)
{
    HWND control;
    unsigned s;
    /* The longest slot: a 15-character label, " button 128", a pad face and
     * " (not connected)", plus the "(+N more)" and the activity mark. */
    char text[160];

    if (s_page_index != PAGE_CONTROLS || !s_page || action >= ACTION_COUNT)
        return;
    for (s = 0; s < BINDING_SLOTS; s++) {
        control = GetDlgItem(s_page, IDC_BIND_SLOT_BASE +
                                     (int)(action * BINDING_SLOTS + s));
        if (!control)
            continue;
        if (!capture_slot_text(action, s, text, sizeof text)) {
            binding_slot_text(model_controls_title(), action, s, text,
                              sizeof text);
            if (!theme_dark() && (s_lit_slot[s] & (1u << action)))
                strncat(text, BINDING_ACTIVE_MARK,
                        sizeof text - strlen(text) - 1);
        }
        SetWindowTextA(control, text);
        /* The owner-drawn face changes with the light even when the text
         * does not. */
        InvalidateRect(control, NULL, FALSE);
    }
    update_reset_button();
}

/* Greyed while every row of the game shown is already on its default. */
void update_reset_button(void)
{
    HWND control = s_page ? GetDlgItem(s_page, IDC_BIND_RESET) : NULL;
    BOOL enable = FALSE;
    unsigned i;

    if (!control)
        return;
    for (i = 0; i < ACTION_COUNT && !enable; i++)
        enable = !model_binding_is_default(model_controls_title(), i);
    /* Greying the button that has the focus strands the keyboard on
     * nothing; hand it to the page. */
    if (!enable && GetFocus() == control)
        SetFocus(s_page);
    EnableWindow(control, enable);
}

/* IS THIS SLOT EMPTY? The dark theme draws an empty slot as an outline, so it
 * reads as a place to put something rather than as a button that adds. */
static int launcher_slot_empty(int id)
{
    binding_item_t row[BINDING_SLOTS];
    unsigned index;

    if (id < IDC_BIND_SLOT_BASE ||
        id >= IDC_BIND_SLOT_BASE + (int)(ACTION_COUNT * BINDING_SLOTS))
        return 0;
    index = (unsigned)(id - IDC_BIND_SLOT_BASE);
    return model_binding_items(model_controls_title(), index / BINDING_SLOTS,
                               row, BINDING_SLOTS) <= index % BINDING_SLOTS;
}

/* IS THIS SLOT LIT? The dark theme's painter asks: lit while its input is
 * held, and while it is the slot a capture is filling. */
static int launcher_slot_lit(int id)
{
    unsigned index, action, slot;
    char text[8];

    if (id < IDC_BIND_SLOT_BASE ||
        id >= IDC_BIND_SLOT_BASE + (int)(ACTION_COUNT * BINDING_SLOTS))
        return 0;
    index = (unsigned)(id - IDC_BIND_SLOT_BASE);
    action = index / BINDING_SLOTS;
    slot = index % BINDING_SLOTS;
    return (s_lit_slot[slot] & (1u << action)) != 0 ||
           capture_slot_text(action, slot, text, sizeof text);
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
    int i;

    if (!launcher_save_if_dirty())
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
            "This game's files are not in the game-data folder. Import it "
            "again, or choose the folder: both are on Files & Logging, once "
            "Show advanced settings on Help is on.",
            "Game data not found", MB_OK | MB_ICONWARNING);
        return 0;
    }
    for (i = 0; i < PLAY_TITLES; i++)
        if (!_stricmp(title, k_play[i].id)) {
            const char *name = title, *exe;

            s_result = k_play[i].result;
            title_files(k_play[i].id, &name, &exe);
            handoff_play(s_window, name, theme_game_colour(
                launcher_button_kind(k_play[i].play)));
        }
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
    /* The rows are numbered afresh, so no controller is known to be at rest. */
    launcher_pad_reset(&s_pad);
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

/* The page list's selection changed: show that page. */
static void nav_changed(void)
{
    int selected, page;

    pull_page();
    selected = nav_selection();
    if (selected < 0 || selected >= NAV_COUNT)
        return;
    /* Controls in the list is the Controls tab, even from Force Feedback: the
     * row says Controls. */
    page = k_nav_page[selected];
    if (page != s_page_index) {
        s_page_index = page;
        abandon_capture();
        render_page();
        layout_window();
    }
}

/* The page `delta` rows down the list, if there is one. */
static void nav_step(int delta)
{
    LRESULT n = SendMessage(s_nav, s_compact ? CB_GETCOUNT : LB_GETCOUNT, 0, 0);
    LRESULT row = SendMessage(s_nav, s_compact ? CB_GETCURSEL : LB_GETCURSEL,
                              0, 0) + delta;

    if (row < 0 || row >= n)
        return;
    SendMessage(s_nav, s_compact ? CB_SETCURSEL : LB_SETCURSEL, (WPARAM)row, 0);
    nav_changed();
}

/* ---- the keyboard's place, and moving it ---------------------------------
 *
 * WHERE THE KEYBOARD WAS IS REMEMBERED BY ID, because the page is rebuilt for
 * a great many things that are not a move, and every rebuild
 * destroys the control that had it. A focus that lands nowhere -- on the
 * window, the page itself, or no control at all -- is put back on the same id
 * of the same page, or on the page's first control when the page changed. */
static int s_focus_id;              /* a control's id; 0 before the first   */
static int s_focus_page = -1;       /* its page, -1 for the list and footer */
static HWND s_focus_seen;

static int focusable(HWND c)
{
    return c && IsWindowVisible(c) && IsWindowEnabled(c) &&
           (GetWindowLongPtrA(c, GWL_STYLE) & WS_TABSTOP);
}

static int class_is(HWND c, const char *name)
{
    char cls[32];

    return c && GetClassNameA(c, cls, sizeof cls) && !_stricmp(cls, name);
}

typedef struct {
    HWND stop[512];
    unsigned n;
} page_stops_t;

static BOOL CALLBACK collect_stop(HWND c, LPARAM data)
{
    page_stops_t *s = (page_stops_t *)data;

    if (s->n < sizeof s->stop / sizeof s->stop[0] && focusable(c))
        s->stop[s->n++] = c;
    return TRUE;
}

static void page_stops(page_stops_t *s)
{
    s->n = 0;
    if (s_page)
        EnumChildWindows(s_page, collect_stop, (LPARAM)s);
}

static pad_rect_t screen_rect(HWND c)
{
    RECT rc;
    pad_rect_t r;

    GetWindowRect(c, &rc);
    r.left = rc.left; r.top = rc.top; r.right = rc.right; r.bottom = rc.bottom;
    return r;
}

/* The page's first control on screen, top then left; the first at all on a
 * page scrolled past every one. */
static HWND first_page_stop(void)
{
    page_stops_t s;
    RECT view;
    HWND best = NULL;
    pad_rect_t b = { 0, 0, 0, 0 };
    unsigned i;

    page_stops(&s);
    if (!s.n)
        return NULL;
    GetWindowRect(s_page, &view);
    for (i = 0; i < s.n; i++) {
        pad_rect_t r = screen_rect(s.stop[i]);

        if (r.top < view.top || r.bottom > view.bottom)
            continue;
        if (!best || r.top < b.top || (r.top == b.top && r.left < b.left)) {
            best = s.stop[i];
            b = r;
        }
    }
    return best ? best : s.stop[0];
}

/* Moved to by the keyboard or a controller, a control below the fold or above
 * the top comes into view through set_page_scroll(), the one writer. */
static void scroll_into_view(HWND c)
{
    RECT rc, page;
    int margin = scale_px(24), next = s_page_scroll;

    if (!s_page || !IsChild(s_page, c))
        return;
    GetWindowRect(c, &rc);
    MapWindowPoints(NULL, s_page, (POINT *)&rc, 2);
    GetClientRect(s_page, &page);
    if (rc.bottom > page.bottom - margin)
        next += rc.bottom - (page.bottom - margin);
    if (rc.top - (next - s_page_scroll) < margin)
        next = s_page_scroll + rc.top - margin;
    set_page_scroll(s_page, next);
}

static void restore_focus(void)
{
    HWND c = NULL;

    if (s_focus_page < 0 && s_focus_id)
        c = GetDlgItem(s_window, s_focus_id);
    else if (s_focus_page >= 0 && s_focus_page == s_page_index && s_page)
        c = GetDlgItem(s_page, s_focus_id);
    if (!focusable(c) && s_focus_page >= 0)
        c = first_page_stop();
    if (!focusable(c))
        c = s_nav;
    SetFocus(c);
}

/* AFTER EVERY MESSAGE: record where the focus is, scroll it into view when it
 * MOVED (a rebuild putting it back on the same id is not a move, and must not
 * undo the reader's own scrolling), and put back a focus that landed nowhere.
 * Not while a capture holds the window: it takes the focus on purpose. */
static void track_focus(void)
{
    HWND f = GetFocus();
    int page, moved;

    if (!s_window || GetActiveWindow() != s_window)
        return;
    if (!f || f == s_window || f == s_page) {
        s_focus_seen = NULL;
        if (!capture_active() && !capture_wizard_running())
            restore_focus();
        return;
    }
    if (f == s_focus_seen || !IsChild(s_window, f))
        return;
    s_focus_seen = f;
    place_focus_ring();
    page = s_page && IsChild(s_page, f) ? s_page_index : -1;
    moved = GetDlgCtrlID(f) != s_focus_id || page != s_focus_page;
    s_focus_id = GetDlgCtrlID(f);
    s_focus_page = page;
    if (moved && page >= 0)
        scroll_into_view(f);
}

static void show_focus_cues(HWND top)
{
    if (!(SendMessage(top, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        return;
    SendMessage(top, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS),
                0);
    /* The page list draws its own tiles and does not repaint for this. */
    if (top == s_window && s_nav)
        InvalidateRect(s_nav, NULL, FALSE);
    if (top == s_window)
        place_focus_ring();
}

/* Into the page: where the keyboard last was on it, or its first control. */
static void enter_page(void)
{
    HWND c = s_focus_page >= 0 && s_focus_page == s_page_index && s_page ?
             GetDlgItem(s_page, s_focus_id) : NULL;

    if (!focusable(c))
        c = first_page_stop();
    if (c)
        SetFocus(c);
}

static void send_key(HWND c, UINT vk)
{
    LPARAM scan = (LPARAM)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16;

    SendMessage(c, WM_KEYDOWN, vk, scan | 1);
    SendMessage(c, WM_KEYUP, vk, scan | 1 | (LPARAM)0xC0000000u);
}

static int combo_dropped(HWND c)
{
    return class_is(c, "ComboBox") &&
           SendMessage(c, CB_GETDROPPEDSTATE, 0, 0);
}

/* Left and right CHANGE a slider or a closed list rather than leave it. */
static int adjust_sideways(HWND c, pad_event_t dir)
{
    if (class_is(c, TRACKBAR_CLASSA)) {
        send_key(c, dir == PAD_LEFT ? VK_LEFT : VK_RIGHT);
        return 1;
    }
    if (class_is(c, "ComboBox")) {
        LRESULT n = SendMessage(c, CB_GETCOUNT, 0, 0);
        LRESULT row = SendMessage(c, CB_GETCURSEL, 0, 0) +
                      (dir == PAD_LEFT ? -1 : 1);

        if (row >= 0 && row < n) {
            SendMessage(c, CB_SETCURSEL, (WPARAM)row, 0);
            SendMessage(GetParent(c), WM_COMMAND,
                        MAKEWPARAM(GetDlgCtrlID(c), CBN_SELCHANGE), (LPARAM)c);
        }
        return 1;
    }
    return 0;
}

/* ONE MOVE, from the D-pad or an arrow key on a button. The page list, the
 * page and the footer are three places: the list's up and down change page and
 * right enters it; in the page a move goes to the control that way, and off
 * its bottom to the footer, off its top or left side to the list. */
static void move_focus(pad_event_t dir)
{
    HWND f = GetFocus(), apply_or_close = focusable(s_apply) ? s_apply : s_close;
    page_stops_t s;
    pad_rect_t from, r[512];
    unsigned i, n = 0;
    int pick;

    if (f == s_nav) {
        if (dir == PAD_UP || dir == PAD_DOWN)
            nav_step(dir == PAD_DOWN ? 1 : -1);
        else if (dir == PAD_RIGHT)
            enter_page();
        return;
    }
    if (f == s_apply || f == s_close) {
        if (dir == PAD_UP)
            enter_page();
        else if (dir == PAD_LEFT)
            SetFocus(f == s_close && focusable(s_apply) ? s_apply : s_nav);
        else if (dir == PAD_RIGHT && f == s_apply)
            SetFocus(s_close);
        return;
    }
    if (!f || !s_page || !IsChild(s_page, f)) {
        SetFocus(s_nav);
        return;
    }
    if ((dir == PAD_LEFT || dir == PAD_RIGHT) && adjust_sideways(f, dir))
        return;
    page_stops(&s);
    for (i = 0; i < s.n; i++)
        if (s.stop[i] != f)
            s.stop[n] = s.stop[i], r[n++] = screen_rect(s.stop[i]);
    from = screen_rect(f);
    pick = launcher_pad_pick(&from, r, n, dir);
    if (pick >= 0)
        SetFocus(s.stop[pick]);
    else if (dir == PAD_DOWN)
        SetFocus(apply_or_close);
    else if (dir == PAD_UP || dir == PAD_LEFT)
        SetFocus(s_nav);
}

/* A or Enter: what a click on the focused control would do. */
static void activate(HWND f)
{
    if (f == s_nav)
        enter_page();
    else if (control_is_button(f))
        SendMessage(f, BM_CLICK, 0, 0);
    else if (class_is(f, "ComboBox"))
        SendMessage(f, CB_SHOWDROPDOWN, TRUE, 0);
}

/* A MESSAGE BOX OR A DIALOG OF OURS IS UP: move between its buttons, press
 * one, or cancel it. */
static void pad_dialog(HWND box, HWND f, pad_event_t e)
{
    if (!class_is(box, "#32770"))
        return;
    show_focus_cues(box);
    switch (e) {
    case PAD_UP:
    case PAD_LEFT:
        SendMessage(box, WM_NEXTDLGCTL, 1, FALSE);
        break;
    case PAD_DOWN:
    case PAD_RIGHT:
        SendMessage(box, WM_NEXTDLGCTL, 0, FALSE);
        break;
    case PAD_ACCEPT:
        if (f && control_is_button(f))
            SendMessage(f, BM_CLICK, 0, 0);
        break;
    case PAD_BACK:
        PostMessage(box, WM_COMMAND, IDCANCEL, 0);
        break;
    default:
        break;
    }
}

/* ONE CONTROLLER EVENT, posted by pad_tick() so it runs from a message loop
 * and not from inside the tick. Dropped if a capture started since. */
static void pad_act(pad_event_t e)
{
    HWND active = GetActiveWindow(), f = GetFocus();

    if (!active)
        return;
    if (active != s_window) {
        if (GetWindowThreadProcessId(active, NULL) == GetCurrentThreadId())
            pad_dialog(active, f, e);
        return;
    }
    if (capture_active() || capture_wizard_running())
        return;
    show_focus_cues(s_window);
    if (combo_dropped(f)) {
        if (e == PAD_UP || e == PAD_DOWN)
            send_key(f, e == PAD_UP ? VK_UP : VK_DOWN);
        else if (e == PAD_ACCEPT)
            send_key(f, VK_RETURN);
        else if (e == PAD_BACK)
            send_key(f, VK_ESCAPE);
        return;
    }
    switch (e) {
    case PAD_UP:
    case PAD_DOWN:
    case PAD_LEFT:
    case PAD_RIGHT:
        move_focus(e);
        break;
    case PAD_ACCEPT:
        activate(f);
        break;
    case PAD_BACK:
        if (f != s_nav)
            SetFocus(s_nav);
        break;
    case PAD_PREV_PAGE:
    case PAD_NEXT_PAGE:
        nav_step(e == PAD_NEXT_PAGE ? 1 : -1);
        break;
    case PAD_START:
        /* Play on the Play page, from the card that has the focus; Apply
         * everywhere else. */
        if (s_page_index == PAGE_HOME) {
            if (f && s_page && IsChild(s_page, f) &&
                play_index(GetDlgCtrlID(f)) >= 0 && IsWindowEnabled(f))
                SendMessage(f, BM_CLICK, 0, 0);
        } else if (IsWindowEnabled(s_apply)) {
            SendMessage(s_apply, BM_CLICK, 0, 0);
        }
        break;
    default:
        break;
    }
}

/* EVERY TICK, ON EVERY PAGE. The Controls pages poll every controller
 * already; the others do it here. Gated while a capture or Set up has the
 * controllers, and while the foreground is not one of this thread's windows:
 * XInput answers whatever window is in front. */
static void pad_tick(void)
{
    launcher_pad_device_t d[INPUT_PROBE_MAX_DEVICES];
    pad_event_t out[8];
    HWND front = GetForegroundWindow();
    unsigned i, n = input_probe_count(), got;
    int gated = capture_active() || capture_wizard_running() || !front ||
                GetWindowThreadProcessId(front, NULL) != GetCurrentThreadId();

    if (n > INPUT_PROBE_MAX_DEVICES)
        n = INPUT_PROBE_MAX_DEVICES;
    /* A message box of ours in front takes the foreground, and DirectInput
     * reads only for the window it was tied to. */
    if (front && !gated)
        input_probe_follow_window(front == s_window ? NULL : front);
    if (!gated && !page_is_controls(s_page_index))
        poll_every_device();
    for (i = 0; i < n; i++) {
        const input_probe_device_t *dev = input_probe_device(i);
        const input_probe_state_t *st = launcher_device_state(i);

        d[i].present = dev && st;
        d[i].xinput = dev && dev->backend == INPUT_PROBE_XINPUT;
        d[i].pov = st ? st->pov : -1;
        d[i].buttons = st ? st->buttons : NULL;
    }
    got = launcher_pad_tick(&s_pad, gated, d, n, GetTickCount(), out,
                            sizeof out / sizeof out[0]);
    for (i = 0; i < got; i++)
        PostMessage(s_window, WM_APP + 4, (WPARAM)out[i], 0);
}

/* THE KEYS THE DIALOG MANAGER DOES NOT HAVE, ahead of IsDialogMessage(). 1
 * when the key was taken. */
static int launcher_key(const MSG *m)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    int vertical;
    HWND f;

    if (m->message != WM_KEYDOWN || !s_window ||
        (m->hwnd != s_window && !IsChild(s_window, m->hwnd)))
        return 0;
    /* ESCAPE CLOSES THE LAUNCHER, but not by being held: the press that
     * cancelled a capture repeats into a window with no capture. */
    if (m->wParam == VK_ESCAPE && (m->lParam & (1 << 30)))
        return 1;
    if (ctrl && (m->wParam == VK_TAB || m->wParam == VK_PRIOR ||
                 m->wParam == VK_NEXT)) {
        show_focus_cues(s_window);
        nav_step(m->wParam == VK_PRIOR || (m->wParam == VK_TAB && shift) ?
                 -1 : 1);
        return 1;
    }
    /* THE ARROW KEYS ARE THE D-PAD, on every control it reaches. A control's
     * own arrows disagree: a list box, a dropdown and a slider all change
     * their value on Up and Down as on Left and Right, so the keyboard changed
     * a setting, or the page, where the D-pad moved on. Up and Down move
     * (on the page list they change page, as the D-pad does); Left and Right
     * change a slider or a dropdown, move an edit's caret, move between
     * buttons, and enter the page from the list. An open dropdown keeps all
     * four. */
    f = GetFocus();
    if (ctrl || shift || !f || !IsChild(s_window, f) || combo_dropped(f) ||
        (m->wParam != VK_UP && m->wParam != VK_DOWN &&
         m->wParam != VK_LEFT && m->wParam != VK_RIGHT))
        return 0;
    vertical = m->wParam == VK_UP || m->wParam == VK_DOWN;
    if (f == s_nav) {
        if (vertical)
            return 0;
        show_focus_cues(s_window);
        if (m->wParam == VK_RIGHT)
            enter_page();
        return 1;
    }
    if (!vertical && !control_is_button(f))
        return 0;
    show_focus_cues(s_window);
    move_focus(m->wParam == VK_UP ? PAD_UP : m->wParam == VK_DOWN ?
               PAD_DOWN : m->wParam == VK_LEFT ? PAD_LEFT : PAD_RIGHT);
    return 1;
}

static void handle_command(WPARAM wparam, LPARAM lparam)
{
    int id = LOWORD(wparam), notify = HIWORD(wparam);

    /* Every control this window acts on is addressed by id now: the one that
     * needed its HWND was the device combo, and the combo is gone. */
    (void)lparam;

    /* The click a radio sends itself on getting the focus (quiet_radio). */
    if (s_radio_focus && notify == BN_CLICKED)
        return;
    /* What IsDialogMessage() makes of Escape and of Enter on a control that
     * is not a plain push button (an owner-drawn one, a checkbox). */
    if (id == IDCANCEL) {
        SendMessage(s_window, WM_CLOSE, 0, 0);
        return;
    }
    if (id == IDOK) {
        HWND f = GetFocus();

        if (f && IsChild(s_window, f))
            activate(f);
        return;
    }
    if (id == IDC_NAV && notify == (s_compact ? CBN_SELCHANGE : LBN_SELCHANGE)) {
        nav_changed();
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
        if (save_settings(s_window))
            launcher_set_fullscreen(model_bool("graphics", "fullscreen"));
        return;
    }
    if (id == IDC_CLOSE && notify == BN_CLICKED) {
        SendMessage(s_window, WM_CLOSE, 0, 0);
        return;
    }
    if (play_index(id) >= 0 && notify == BN_CLICKED) {
        int t = play_index(id);
        char path[MAX_PATH];

        if (game_path(k_play[t].id, path, sizeof path))
            prepare_play(k_play[t].id);
        else if (k_play[t].importable && !import_beside_exe())
            import_pick();
        return;
    }
    if (id == IDC_IMPORT && notify == BN_CLICKED) {
        import_pick();
        return;
    }
    if (id == IDC_IMPORT_CANCEL && notify == BN_CLICKED) {
        import_cancel();
        SetDlgItemTextA(s_page, IDC_IMPORT_TEXT, "Stopping; nothing will be "
                        "kept...");
        EnableWindow(GetDlgItem(s_page, IDC_IMPORT_CANCEL), FALSE);
        return;
    }
    /* THE GAME ROW: the model follows, so while each game has its own
     * controller the top of the page changes with the buttons. */
    if ((id == IDC_TITLE_HYDRO || id == IDC_TITLE_OFFROAD ||
         id == IDC_TITLE_ARCTIC) && notify == BN_CLICKED) {
        int title = id == IDC_TITLE_ARCTIC ? MODEL_TITLE_ARCTIC :
                    id == IDC_TITLE_OFFROAD ? MODEL_TITLE_OFFROAD :
                                              MODEL_TITLE_HYDRO;

        /* THE MODEL HOLDS WHICH CABINET IS SHOWN, and switching commits the
         * outgoing cabinet's force feedback (and, split, its controller), so
         * it waits for the rebuild's pull. */
        s_cabinet_op = CABINET_HYDRO + title;
        abandon_capture();
        render_page();
        layout_window();
        return;
    }
    if (id >= IDC_BEZEL_CHOOSE && id < IDC_BEZEL_CHOOSE + MODEL_BEZELS &&
        notify == BN_CLICKED) {
        if (choose_bezel(id - IDC_BEZEL_CHOOSE))
            render_page();
        return;
    }
    if (id == IDC_BEZEL_INHERIT && notify == BN_CLICKED) {
        model_bezel_inherit(MODEL_BEZEL_UARCTIC);
        mark_dirty();
        set_status("Ultimate Arctic Thunder's bezel is Arctic Thunder's. "
                   "Press Apply to keep it.");
        render_page();
        return;
    }
    /* Toggled from the model, not read off the box. Turning on a game that
     * has no art of its own yet asks for the art; cancelling leaves it as it
     * was. Ultimate's row that is Arctic's becomes its own either way. */
    if (id >= IDC_BEZEL_ENABLE && id < IDC_BEZEL_ENABLE + MODEL_BEZELS &&
        notify == BN_CLICKED) {
        int title = id - IDC_BEZEL_ENABLE;
        char path[MAX_PATH], own[MAX_PATH];
        int on = model_bezel_get(title, path, sizeof path);

        model_bezel_own(title, own, sizeof own);
        if (!on && !own[0]) {
            choose_bezel(title);
        } else {
            char text[96];

            model_bezel_set_enabled(title, !on);
            mark_dirty();
            snprintf(text, sizeof text, "%s's bezel is %s. Press Apply to keep "
                     "it.", k_bezel_name[title],
                     on ? "off" : "on");
            set_status(text);
        }
        render_page();
        return;
    }
    /* Toggled from the model, not read off the box: see IDC_SHOW_RAW below. */
    if (id == IDC_SEPARATE_CONTROLLER && notify == BN_CLICKED) {
        int split = model_controls_split();

        abandon_capture();
        s_cabinet_op = split ? CABINET_JOIN : CABINET_SPLIT;
        mark_dirty();
        render_page();
        layout_window();
        if (s_cabinet_failed)
            return;
        if (split) {
            char text[160];

            snprintf(text, sizeof text, "All games now use %s's controller. "
                     "Press Apply to keep it.",
                     model_title_name(model_controls_title()));
            set_status(text);
        } else {
            set_status("Each game now has its own controller. Press Apply to "
                       "keep it.");
        }
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
        create_navigation(s_window);
        request_relayout();
        return;
    }
    if (id == IDC_ABOUT_LINK && notify == STN_CLICKED) {
        if ((INT_PTR)ShellExecuteA(s_window, "open", VCT_REPO_URL, NULL, NULL,
                                   SW_SHOWNORMAL) <= 32)
            set_status("Could not open a browser: the address is "
                       VCT_REPO_URL);
        return;
    }
    if (id == IDC_RESTORE_DEFAULTS && notify == BN_CLICKED) {
        /* So the question knows whether anything unapplied is about to go. */
        pull_page();
        /* Only a restored file moves the launcher: a declined or failed one
         * leaves the page's unapplied Display choice in the model. */
        if (restore_defaults(s_window))
            launcher_set_fullscreen(model_bool("graphics", "fullscreen"));
        return;
    }
    if (id == IDC_SHOW_LOGS && notify == BN_CLICKED) {
        show_log_files();
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
    if (id >= IDC_BIND_SLOT_BASE &&
        id < IDC_BIND_SLOT_BASE + (int)(ACTION_COUNT * BINDING_SLOTS) &&
        notify == BN_CLICKED) {
        unsigned index = (unsigned)(id - IDC_BIND_SLOT_BASE);

        abandon_capture();
        begin_capture(index / BINDING_SLOTS, index % BINDING_SLOTS);
        return;
    }
    if (id == IDC_BIND_RESET && notify == BN_CLICKED) {
        abandon_capture();
        restore_all_bindings();
        return;
    }
    /* A checkbox row's label is a static of its own; a click on it is a click
     * on the box, unless the box is greyed. */
    if (id >= IDC_LABEL_BASE && id < IDC_LABEL_BASE + (int)SETTING_COUNT &&
        notify == STN_CLICKED) {
        HWND box = GetDlgItem(s_page, id - IDC_LABEL_BASE + IDC_SETTING_BASE);

        if (box && IsWindowEnabled(box) && control_is_button(box))
            SendMessage(box, BM_CLICK, 0, 0);
        return;
    }
    if (id >= IDC_SETTING_BASE && id < IDC_SETTING_BASE + (int)SETTING_COUNT &&
        (notify == EN_CHANGE || notify == CBN_SELCHANGE || notify == BN_CLICKED)) {
        pull_page();
        mark_dirty();
        scale_lines_refresh();
        page_show_inert();
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
        theme_set_dpi(launcher_dpi(h), NULL);
        GetClientRect(h, &rc);
        s_compact = rc.right < scale_px(760);
        s_tiny = rc.right < scale_px(420) || rc.bottom < scale_px(300);
        SetWindowTextA(h, "VCThunder");
        /* NO FOCUS MARKS UNTIL A KEY OR A CONTROLLER IS USED, as in a dialog:
         * a plain window opens with them shown. Before the children, which
         * take their state from here. */
        SendMessage(h, WM_CHANGEUISTATE,
                    MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
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
        if (draw_nav_tile((DRAWITEMSTRUCT *)lparam) ||
            draw_dark_button((DRAWITEMSTRUCT *)lparam) ||
            theme_draw_combo_item((DRAWITEMSTRUCT *)lparam))
            return TRUE;
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        LRESULT colour;

        /* The air between the tiles is the frame, not a list's field. */
        if (message == WM_CTLCOLORLISTBOX && (HWND)lparam == s_nav &&
            theme_dark())
            return (LRESULT)theme_main_brush();
        colour = control_colour(0, message, wparam, lparam);
        if (colour)
            return colour;
        break;
    }
    case WM_TIMER:
        if (wparam == 1) {
            update_live_input();
            pad_tick();
        } else if (wparam == IMPORT_TIMER)
            import_tick();
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
                     "Help." :
                     "%u lines in vcthunder.ini are not used, see "
                     "Help.", model_problem_total());
            set_status(said);
        }
        /* CHDs dropped on VCThunder.exe start importing as the window
         * opens; with none, a game not imported yet is looked for beside
         * it. */
        import_open();
        return 0;
    case WM_APP + 3:
        confirm_axis_detect();
        return 0;
    case WM_APP + 4:
        pad_act((pad_event_t)wparam);
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
            int compact, tiny, compact_changed, tiny_changed, rescaled;
            UINT dpi = launcher_dpi(h);

            /* A NEW SCALE IS A NEW LAYOUT: the forms are chosen in logical
             * units, so they are decided after it. The page is pulled first,
             * while its controls still hold what was typed into them. */
            rescaled = s_nav && !s_sizing && dpi != theme_dpi();
            if (rescaled) {
                pull_page();
                theme_set_dpi(dpi, h);
            }
            GetClientRect(h, &rc);
            compact = rc.right < scale_px(760);
            tiny = rc.right < scale_px(420) || rc.bottom < scale_px(300);
            compact_changed = compact != s_compact;
            tiny_changed = tiny != s_tiny;
            if (s_nav && (compact_changed || tiny_changed || rescaled)) {
                if (!rescaled)
                    pull_page();
                s_compact = compact;
                s_tiny = tiny;
                if (compact_changed || rescaled)
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
    case WM_ENTERSIZEMOVE:
        s_sizing = 1;
        return 0;
    case WM_EXITSIZEMOVE:
        /* The drop: the scale the window now has, once, and the size lines
         * for the monitor it is now on. */
        s_sizing = 0;
        if (s_nav && launcher_dpi(h) != theme_dpi())
            SendMessage(h, WM_SIZE, SIZE_RESTORED, 0);
        else
            scale_lines_refresh();
        return 0;
    case WM_DISPLAYCHANGE:
        scale_lines_refresh();
        break;
    case WM_DPICHANGED:
        /* Another monitor, another Windows scale: the floor under the fit
         * moves, and WM_SIZE re-decides the scale from it. A fullscreen
         * window covers its new monitor instead of taking the suggestion. */
        if (s_fullscreen)
            cover_monitor();
        else {
            const RECT *r = (const RECT *)lparam;

            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    /* A WHEEL THAT REACHES THE WINDOW SCROLLS THE PAGE: the compact form's
     * closed page dropdown hands its wheel here, as every closed dropdown
     * hands its own to its parent, and a top-level window has no parent to
     * pass it on to. */
    case WM_MOUSEWHEEL:
        if (s_page)
            SendMessage(s_page, message, wparam, lparam);
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
        /* At Windows' scale for the monitor, not the fitted one: after a
         * large window the fit would hold a restored one large. */
        mm->ptMinTrackSize.x = min(MulDiv(320, (int)theme_floor_dpi(h), 96),
                                   available_w);
        mm->ptMinTrackSize.y = min(MulDiv(240, (int)theme_floor_dpi(h), 96),
                                   available_h);
        return 0;
    }
    case WM_CLOSE:
        if (import_stop_for_close() && close_with_dirty_prompt())
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
    RECT back;
    HMONITOR display;
    RECT work;
    int width, height, x, y;

    s_result = LAUNCHER_CANCEL;
    s_dirty = s_loading = 0;
    s_page_index = PAGE_HOME;
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

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)&launcher_run, &instance);
    /* BEFORE THE WORK AREA IS MEASURED, or it comes back in the scaled
     * coordinates this call is about to stop using. */
    theme_enable_dpi_awareness();
    /* The monitor a game was on when it reopened the launcher, or else the
     * one under the pointer. */
    if (handoff_return_monitor(&back))
        display = MonitorFromRect(&back, MONITOR_DEFAULTTONEAREST);
    else {
        GetCursorPos(&cursor);
        display = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    }
    memset(&monitor, 0, sizeof monitor);
    monitor.cbSize = sizeof monitor;
    if (GetMonitorInfoA(display, &monitor))
        work = monitor.rcWork;
    else
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    memset(&controls, 0, sizeof controls);
    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
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
        "VCThunder", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, width, height, NULL, NULL, theme_instance(), NULL);
    if (!s_window)
        goto unregister_main;
    enable_dark_title_bar(s_window);
    SendMessage(s_window, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(s_window, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    s_fullscreen = 0;
    launcher_set_fullscreen(model_bool("graphics", "fullscreen"));
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
        /* A HELD KEY IS NOT A CAPTURE'S PRESS: Enter starts a capture on its
         * first press, and its auto-repeat would be captured as the binding. */
        if (to_capture && (message.lParam & (1 << 30)))
            continue;
        if (!to_capture && launcher_key(&message)) {
            track_focus();
            continue;
        }
        if (to_capture || !s_window || !IsDialogMessageA(s_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        track_focus();
    }

unregister_main:
    if (s_logo_icon)
        DestroyIcon(s_logo_icon);
    s_logo_icon = NULL;
    s_logo_size = 0;
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
