/* launcher.c -- native settings launcher.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * What `VCThunder.exe` with no title argument opens: every shipped ini key,
 * per-title keyboard and controller bindings, and non-exclusive live device and
 * axis testing with a bounded force-feedback test. Plain Win32, no dialog
 * resources -- the pages are built from the tables, so a key added to
 * src/settings.def or a control added to src/bindings.def appears here with no
 * edit to this file. It runs BEFORE the game overwrites the host image, which
 * is the only window in which a window of our own is safe.
 *
 * It carries none of the three things it might look like it does:
 *
 *   the catalogue      src/settings.def and src/bindings.def. This file reads
 *                      the SHOW half of one and the labels of the other.
 *   the value grammar  src/ini.h, shared with the loader's own parser and the
 *                      two Win32-profile readers.
 *   the file itself    src/ini_doc.h. The user's ini is held as a document and
 *                      changed a line at a time, so a save keeps their
 *                      comments, ordering and line endings. That half has no
 *                      window in it and is the half under test.
 *
 * WHAT IS ACTUALLY HARD HERE IS LAYOUT, and two rules govern it, both paid for
 * by controls that walked off the page. A geometry constant is only ever a
 * MAXIMUM -- `min(constant, what the page has)` -- because the two-column form
 * begins at a 760-unit window and leaves the page about 500. And the page is
 * rebuilt whenever its WIDTH changes, not only when the compact/wide mode
 * does, or no width formula can help.
 */
#include "vcthunder.h"
#include "resource.h"
#include "io.h"
#include "settings.h"
#include "ini.h"
#include "ini_doc.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The settings catalogue is src/settings.def, shared with config.c: section,
 * page, label, control type, choices, range and default all come from there.
 * This file supplies only the window that renders them.
 *
 * `vct_ui_setting` deliberately excludes the developer switches; see
 * save_settings(), which writes every entry of this array back to the file, so
 * a hidden key in it would be injected into the user's ini on first open. */
#define SETTING_COUNT VCT_UI_SETTING_COUNT
static const setting_desc_t *const k_settings = vct_ui_setting;

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
/* The launcher's half of src/bindings.def: the labels, and the same per-title
 * defaults the loader uses, spelled as the text this file writes back.
 *
 * The catalogue is NOT written out here. Twenty rows beside the loader's own
 * copy of the same defaults in io_config.c and the button chain in io_board.c
 * would be three lists with nothing holding them together, and a binding could
 * be editable here and unread by the cabinet. See the note in bindings.def. */
typedef struct {
    const char *key;                    /* the ini key, in BOTH sections    */
    const char *hydro_label;            /* NULL: Offroad's control alone    */
    const char *offroad_label;
    /* The keyboard default per title, or NULL where the control has no
     * keyboard half, which is the shifter paddles and nothing else. */
    const char *hydro_key, *offroad_key;
    int         has_pad;                /* is there a [gamepad] ordinal?    */
    unsigned    pad_default;
} action_desc_t;

static const action_desc_t k_actions[] = {
#define BINDING(key, hydro, offroad, pad_default, button, hl, ol)             \
    { #key, hl, ol, hydro, offroad, 1, pad_default },
#define BINDING_AXIS(key, hydro, offroad, hl, ol)                             \
    { #key, hl, ol, hydro, offroad, 0, 0 },
#define BINDING_PAD(key, pad_default, hl, ol)                                 \
    { #key, hl, ol, NULL, NULL, 1, pad_default },
#include "bindings.def"
#undef BINDING
#undef BINDING_AXIS
#undef BINDING_PAD
};

#define ACTION_COUNT (sizeof k_actions / sizeof k_actions[0])

enum {
    IDC_NAV = 100,
    IDC_STATUS,
    IDC_APPLY,
    IDC_CLOSE,
    IDC_PAGE,
    IDC_PLAY_HYDRO = 200,
    IDC_PLAY_OFFROAD,
    IDC_CONTROL_BINDINGS = 210,
    IDC_CONTROL_DEVICES,
    IDC_CONTROL_FFB,
    IDC_TITLE_HYDRO = 220,
    IDC_TITLE_OFFROAD,
    IDC_PROBE_DEVICE = 230,
    IDC_PROBE_REFRESH,
    IDC_FFB_TEST,
    IDC_LIVE_STEER = 240,
    IDC_LIVE_THROTTLE,
    IDC_LIVE_BRAKE,
    IDC_LIVE_BUTTONS,
    IDC_SETTING_BASE = 1000,
    IDC_BROWSE_BASE = 2000,
    IDC_DETECT_BASE = 3000,
    IDC_KEY_TEXT_BASE = 4000,
    IDC_KEY_BIND_BASE = 4100,
    IDC_PAD_TEXT_BASE = 4200,
    IDC_PAD_BIND_BASE = 4300,
    /* The per-setting help caption. Its own range so control_colour() can mute
     * it without asking what a static's text says. */
    IDC_HELP_BASE = 5000
};

static const char *const k_nav_name[] = {
    "Launch", "Graphics", "Audio", "Controls", "Game System", "Link Play",
    "Files & Logging", "Troubleshooting"
};

static const int k_nav_page[] = {
    PAGE_HOME, PAGE_GRAPHICS, PAGE_AUDIO, PAGE_BINDINGS, PAGE_SYSTEM,
    PAGE_LINK, PAGE_FILES, PAGE_TROUBLE
};

static const char *const k_page_intro[] = {
    "Choose a title. Settings are saved before the selected game starts.",
    "Display, renderer, and cabinet-raster settings shared by both titles.",
    "Host audio and the cabinets' front/seat bus routing.",
    "Each title has its own effective keyboard and controller-button map.",
    "Choose and test an attached controller, then map its axes.",
    "The game supplies the motor force. The launcher never commands a test force.",
    "Cabinet identity, timing, DIP switches, and input sampling.",
    "The transport the host puts under the games' own link protocol.",
    "Paths and the two runtime logs. Paths are relative to VCThunder.exe.",
    "Capture and diagnostic settings shipped in vcthunder.ini."
};

/* All three are indexed by their enum, and a page added to one and not the
 * others reads off the end of it. */
typedef char page_table_check[
    sizeof k_page_intro / sizeof k_page_intro[0] == PAGE_COUNT &&
    sizeof k_nav_name / sizeof k_nav_name[0] == NAV_COUNT &&
    sizeof k_nav_page / sizeof k_nav_page[0] == NAV_COUNT ? 1 : -1];

static ini_doc_t s_doc;
static char s_value[SETTING_COUNT][INI_VALUE_CAP];
static char s_key_binding[2][ACTION_COUNT][32];
static char s_pad_binding[2][ACTION_COUNT][16];

static HWND s_window, s_nav, s_page, s_status, s_apply, s_close;
static HFONT s_font, s_play_font, s_title_font, s_title_small_font;
static HINSTANCE s_instance;
static HBRUSH s_main_brush, s_page_brush, s_control_brush, s_border_brush;
static COLORREF s_main_color, s_page_color, s_control_color, s_text_color;
static UINT s_dpi = 96;
static UINT s_system_dpi = 96;
static int s_dark_mode, s_compact, s_tiny;
static int s_page_index = PAGE_HOME;
static int s_binding_title;
static int s_dirty, s_loading, s_page_scroll, s_content_height;
/* The page's width when its children were last built, and whether a rebuild
 * is already on the queue. A resize INSIDE one layout mode moves the page;
 * without a rebuild its children keep the coordinates they were created with,
 * so anything laid out to the right edge -- the Launch page's Play buttons, a
 * Browse button -- ends up outside the page and unreachable: the page scrolls
 * vertically only. */
static int s_rendered_page_w, s_relayout_pending;
static int s_external_overrides;
static launcher_result_t s_result = LAUNCHER_CANCEL;

static int s_probe_selected = -1;
static input_probe_state_t s_probe_state;
static uint32_t s_previous_buttons;
static int s_capture_kind, s_capture_action, s_capture_setting, s_capture_armed;
static float s_axis_baseline[INPUT_PROBE_MAX_AXES];
static char s_axis_baseline_name[INPUT_PROBE_MAX_AXES][16];
static unsigned s_axis_baseline_count;

static void update_binding_display(unsigned action, int pad);

static int scale_px(int value)
{
    return MulDiv(value, (int)s_dpi, 96);
}

static int unscale_px(int value)
{
    return MulDiv(value, 96, (int)s_dpi);
}

static void initialise_palette(void)
{
    HIGHCONTRASTA contrast;
    memset(&contrast, 0, sizeof contrast);
    contrast.cbSize = sizeof contrast;
    SystemParametersInfoA(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0);
    s_dark_mode = !(contrast.dwFlags & HCF_HIGHCONTRASTON);
    if (!s_dark_mode) {
        s_main_brush = GetSysColorBrush(COLOR_BTNFACE);
        s_page_brush = GetSysColorBrush(COLOR_WINDOW);
        s_control_brush = GetSysColorBrush(COLOR_WINDOW);
        return;
    }
    s_main_color = RGB(30, 30, 30);
    s_page_color = RGB(24, 24, 24);
    s_control_color = RGB(45, 45, 48);
    s_text_color = RGB(240, 240, 240);
    s_main_brush = CreateSolidBrush(s_main_color);
    s_page_brush = CreateSolidBrush(s_page_color);
    s_control_brush = CreateSolidBrush(s_control_color);
    s_border_brush = CreateSolidBrush(RGB(100, 100, 104));
}

static void release_palette(void)
{
    if (!s_dark_mode)
        return;
    DeleteObject(s_main_brush);
    DeleteObject(s_page_brush);
    DeleteObject(s_control_brush);
    DeleteObject(s_border_brush);
    s_main_brush = s_page_brush = s_control_brush = s_border_brush = NULL;
}

static void enable_dark_title_bar(HWND h)
{
    typedef HRESULT (WINAPI *dwm_set_fn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE module;
    dwm_set_fn set_attribute;
    BOOL enabled = TRUE;

    if (!s_dark_mode)
        return;
    module = LoadLibraryA("dwmapi.dll");
    if (!module)
        return;
    set_attribute = (dwm_set_fn)(uintptr_t)
        GetProcAddress(module, "DwmSetWindowAttribute");
    if (set_attribute && FAILED(set_attribute(h, 20, &enabled, sizeof enabled)))
        set_attribute(h, 19, &enabled, sizeof enabled);
    FreeLibrary(module);
}

static void theme_control(HWND h)
{
    typedef HRESULT (WINAPI *set_theme_fn)(HWND, LPCWSTR, LPCWSTR);
    HMODULE module;
    set_theme_fn set_theme;
    char class_name[32];

    if (!h || !s_dark_mode)
        return;
    GetClassNameA(h, class_name, sizeof class_name);
    module = LoadLibraryA("uxtheme.dll");
    if (module) {
        set_theme = (set_theme_fn)(uintptr_t)
            GetProcAddress(module, "SetWindowTheme");
        if (set_theme) {
            LONG_PTR style = GetWindowLongPtr(h, GWL_STYLE);
            if (!_stricmp(class_name, "Button") &&
                (style & BS_TYPEMASK) == BS_GROUPBOX)
                set_theme(h, L"", L"");
            else
                set_theme(h, L"DarkMode_Explorer", NULL);
        }
        FreeLibrary(module);
    }
    if (!_stricmp(class_name, PROGRESS_CLASSA)) {
        SendMessage(h, PBM_SETBKCOLOR, 0, s_control_color);
        SendMessage(h, PBM_SETBARCOLOR, 0, RGB(0, 120, 215));
    }
}

static int draw_dark_button(const DRAWITEMSTRUCT *draw)
{
    RECT rc;
    char text[128];
    COLORREF face, border, text_colour;
    HBRUSH brush;
    HPEN pen, old_pen;
    HGDIOBJ old_brush, old_font = NULL;

    if (!s_dark_mode || !draw || draw->CtlType != ODT_BUTTON)
        return 0;
    rc = draw->rcItem;
    if (!(draw->itemState & ODS_DISABLED) &&
        draw->CtlID == IDC_PLAY_HYDRO) {
        face = draw->itemState & ODS_SELECTED ? RGB(0, 107, 132) :
                                                RGB(0, 134, 165);
    } else if (!(draw->itemState & ODS_DISABLED) &&
               draw->CtlID == IDC_PLAY_OFFROAD) {
        face = draw->itemState & ODS_SELECTED ? RGB(151, 10, 13) :
                                                RGB(189, 12, 16);
    } else {
        face = draw->itemState & ODS_DISABLED ? RGB(38, 38, 40) :
               draw->itemState & ODS_SELECTED ? RGB(58, 58, 62) :
                                                RGB(48, 48, 52);
    }
    border = draw->itemState & ODS_FOCUS ? RGB(0, 120, 215) : RGB(100, 100, 104);
    text_colour = draw->itemState & ODS_DISABLED ? RGB(125, 125, 128) :
                                                   s_text_color;
    brush = CreateSolidBrush(face);
    pen = CreatePen(PS_SOLID, 1, border);
    old_brush = SelectObject(draw->hDC, brush);
    old_pen = SelectObject(draw->hDC, pen);
    Rectangle(draw->hDC, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(draw->hDC, old_pen);
    SelectObject(draw->hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    GetWindowTextA(draw->hwndItem, text, sizeof text);
    {
        HFONT font = (HFONT)SendMessage(draw->hwndItem, WM_GETFONT, 0, 0);
        if (font)
            old_font = SelectObject(draw->hDC, font);
    }
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, text_colour);
    if (draw->itemState & ODS_SELECTED)
        OffsetRect(&rc, 1, 1);
    DrawTextA(draw->hDC, text, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (draw->itemState & ODS_FOCUS) {
        InflateRect(&rc, -3, -3);
        DrawFocusRect(draw->hDC, &rc);
    }
    if (old_font)
        SelectObject(draw->hDC, old_font);
    return 1;
}

static LRESULT control_colour(HWND parent, UINT message, WPARAM wparam,
                              LPARAM lparam)
{
    HDC dc = (HDC)wparam;
    COLORREF background;
    HBRUSH brush;
    char class_name[32] = "";

    if (!s_dark_mode)
        return 0;
    GetClassNameA((HWND)lparam, class_name, sizeof class_name);
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
        !_stricmp(class_name, "Edit")) {
        background = s_control_color;
        brush = s_control_brush;
    } else if (parent == s_page) {
        background = s_page_color;
        brush = s_page_brush;
    } else {
        background = s_main_color;
        brush = s_main_brush;
    }
    SetBkColor(dc, background);
    /* A caption is subordinate to the label above it and should read that way.
     * Only here: High Contrast returns above, having handed its colours back to
     * the system, which is the one theme where inventing a second grey would be
     * wrong. The caution rows say "Caution:" in their text as well, so nothing
     * depends on a reader distinguishing two greys. */
    SetTextColor(dc, GetDlgCtrlID((HWND)lparam) >= IDC_HELP_BASE ?
                         RGB(176, 176, 180) : s_text_color);
    return (LRESULT)brush;
}

static void set_font(HWND h)
{
    if (h && s_font)
        SendMessage(h, WM_SETFONT, (WPARAM)s_font, TRUE);
}

static HWND make_control(DWORD ex, const char *class_name, const char *text,
                         DWORD style, int x, int y, int w, int h,
                         HWND parent, int id)
{
    if (s_dark_mode && !_stricmp(class_name, "BUTTON") &&
        ((style & BS_TYPEMASK) == BS_PUSHBUTTON ||
         (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON))
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    HWND control = CreateWindowExA(ex, class_name, text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
        scale_px(x), scale_px(y),
        scale_px(w), scale_px(h), parent, (HMENU)(INT_PTR)id,
        s_instance, NULL);
    set_font(control);
    theme_control(control);
    return control;
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
    return page == PAGE_BINDINGS || page == PAGE_DEVICES || page == PAGE_FFB;
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

static void set_status(const char *text)
{
    if (s_status)
        SetWindowTextA(s_status, text ? text : "");
}

static int setting_find(const char *section, const char *key)
{
    unsigned i;
    for (i = 0; i < SETTING_COUNT; i++)
        if (!_stricmp(k_settings[i].section, section) &&
            !_stricmp(k_settings[i].key, key))
            return (int)i;
    return -1;
}

static const char *model_value(const char *section, const char *key)
{
    int i = setting_find(section, key);
    return i >= 0 ? s_value[i] : "";
}

/* The model never holds an unparseable boolean -- load_settings() replaces one
 * with the table's default -- so the 0 here is unreachable and is only what
 * ini_bool_or() needs to be handed. */
static int model_bool(const char *section, const char *key)
{
    return ini_bool_or(model_value(section, key), 0);
}

static unsigned model_uint(const char *section, const char *key, unsigned dflt)
{
    return ini_uint_or(model_value(section, key), 0, UINT_MAX, dflt);
}

/* An action has up to two halves -- a key in [digital] and an ordinal in
 * [gamepad] -- and the loader and the saver want the same three facts about
 * each: which section it lives in, what it falls back to for this title, and
 * where this file keeps its current value. Stating that once is the point: the
 * PRECEDENCE (a title's own section, then the shared one, then the table) was
 * written out four times, and four copies of a precedence rule is four places
 * for it to go stale. */
typedef struct {
    const char *section;                /* "digital" | "gamepad"            */
    const char *fallback;               /* the table's default, as text     */
    char       *value;                  /* what the launcher currently holds*/
    size_t      value_sz;
} binding_half_t;

static unsigned action_halves(unsigned title, unsigned action,
                              binding_half_t *out, char *pad_text,
                              size_t pad_text_sz)
{
    const action_desc_t *a = &k_actions[action];
    const char *key_default = title ? a->offroad_key : a->hydro_key;
    unsigned n = 0;

    if (key_default) {
        out[n].section  = "digital";
        out[n].fallback = key_default;
        out[n].value    = s_key_binding[title][action];
        out[n].value_sz = sizeof s_key_binding[0][0];
        n++;
    }
    if (a->has_pad) {
        snprintf(pad_text, pad_text_sz, "%u", a->pad_default);
        out[n].section  = "gamepad";
        out[n].fallback = pad_text;
        out[n].value    = s_pad_binding[title][action];
        out[n].value_sz = sizeof s_pad_binding[0][0];
        n++;
    }
    return n;
}

static void load_settings(void)
{
    unsigned i, title, action;

    for (i = 0; i < SETTING_COUNT; i++) {
        int unused;

        if (!ini_doc_get(&s_doc, k_settings[i].section, k_settings[i].key,
                         s_value[i], sizeof s_value[i]))
            snprintf(s_value[i], sizeof s_value[i], "%s",
                     k_settings[i].fallback);
        /* A checkbox cannot show "neither", so a boolean the file spells
         * wrongly used to arrive here as unticked and be written back as
         * `false` on Apply -- silently turning a typo into a setting, where
         * the host would have refused the value and said so. An unparseable
         * boolean carries no more information than a missing one, so it is
         * treated as one. Every other type keeps its text and is caught by
         * validate_settings(), which can name it in a message. */
        if (k_settings[i].type == ST_BOOL &&
            !ini_parse_bool(s_value[i], &unused))
            snprintf(s_value[i], sizeof s_value[i], "%s",
                     k_settings[i].fallback);
    }
    for (title = 0; title < 2; title++) {
        const char *id = title ? "offroad" : "hydro";
        char section[64], shared[64], pad_text[16];
        for (action = 0; action < ACTION_COUNT; action++) {
            const char *key = k_actions[action].key;
            binding_half_t half[2];
            unsigned h, n = action_halves(title, action, half,
                                          pad_text, sizeof pad_text);

            for (h = 0; h < n; h++) {
                if (!ini_doc_get(&s_doc, half[h].section, key,
                             shared, sizeof shared))
                    snprintf(shared, sizeof shared, "%s", half[h].fallback);
                snprintf(section, sizeof section, "%s.%s", id,
                         half[h].section);
                if (!ini_doc_get(&s_doc, section, key,
                             half[h].value, half[h].value_sz))
                    snprintf(half[h].value, half[h].value_sz, "%.*s",
                             (int)(half[h].value_sz - 1), shared);
            }
        }
    }
}

static int is_title_section(const char *section, const char *title)
{
    size_t n = strlen(title);
    return !_strnicmp(section, title, n) &&
           (section[n] == '\0' || section[n] == '.');
}

static int contains_nocase(const char *text, const char *part)
{
    size_t n = strlen(part), i;
    if (!n)
        return 1;
    for (i = 0; text[i]; i++)
        if (!_strnicmp(text + i, part, n))
            return 1;
    return 0;
}

static void detect_external_overrides(void)
{
    size_t i;
    s_external_overrides = 0;
    for (i = 0; i < s_doc.count; i++) {
        char section[96], *dot;
        if (!ini_doc_section(s_doc.line[i], section, sizeof section))
            continue;
        if (!is_title_section(section, "hydro") &&
            !is_title_section(section, "offroad"))
            continue;
        dot = strchr(section, '.');
        if (!dot || (_stricmp(dot + 1, "digital") &&
                     _stricmp(dot + 1, "gamepad"))) {
            s_external_overrides = 1;
            return;
        }
    }
}

static int parse_unsigned_setting(const setting_desc_t *d, const char *value)
{
    unsigned n;

    if (d->type == ST_AUTO_UINT && !_stricmp(value, "auto"))
        return 1;
    return ini_parse_uint(value, (unsigned)d->minimum, (unsigned)d->maximum,
                          &n);
}

static int keyboard_binding_valid(const char *value)
{
    static const char *const names[] = {
        "NONE", "LEFT", "RIGHT", "UP", "DOWN",
        "LCTRL", "LCONTROL", "RCTRL", "RCONTROL",
        "LALT", "LMENU", "RALT", "RMENU", "LSHIFT", "RSHIFT",
        "SPACE", "ENTER", "ESC", "ESCAPE", "TAB", "BACKSPACE",
        "MINUS", "EQUALS", "COMMA", "PERIOD", "HOME", "END",
        "PGUP", "PGDN", "INSERT", "DELETE"
    };
    const char *s = value;
    char *end;
    unsigned long n;
    unsigned i;

    if (!_strnicmp(s, "KEYCODE_", 8))
        s += 8;
    if (s[0] && !s[1] && (isalnum((unsigned char)s[0]) ||
                           s[0] == '-' || s[0] == '='))
        return 1;
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        n = strtoul(s + 1, &end, 10);
        if (!*end && n >= 1 && n <= 24)
            return 1;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_stricmp(s, names[i]))
            return 1;
    n = strtoul(s, &end, 0);
    return end != s && !*end && n <= 0xFF;
}

static int validate_settings(char *why, size_t why_size)
{
    unsigned i, title, action, value;

    for (i = 0; i < SETTING_COUNT; i++) {
        const setting_desc_t *d = &k_settings[i];
        const char *v = s_value[i];
        if ((d->flags & SF_REQUIRED) && !*v) {
            snprintf(why, why_size, "%s cannot be empty.", d->label);
            return 0;
        }
        if (d->type == ST_ENUM && !vct_choice_contains(d->choices, v)) {
            snprintf(why, why_size, "%s is not a supported value.", d->label);
            return 0;
        }
        if ((d->type == ST_UINT || d->type == ST_AUTO_UINT ||
             d->type == ST_MHZ_UINT) &&
            !parse_unsigned_setting(d, v)) {
            if (d->type == ST_MHZ_UINT)
                snprintf(why, why_size, "%s must be 1..4000.", d->label);
            else
                snprintf(why, why_size, "%s must be %s%lu..%lu.", d->label,
                         d->type == ST_AUTO_UINT ? "automatic or " : "",
                         d->minimum, d->maximum);
            return 0;
        }
    }
    for (title = 0; title < 2; title++)
        for (action = 0; action < ACTION_COUNT; action++) {
            if ((title ? k_actions[action].offroad_key :
                         k_actions[action].hydro_key) &&
                !keyboard_binding_valid(s_key_binding[title][action])) {
                snprintf(why, why_size,
                         "%s keyboard binding is not supported.",
                         title ? k_actions[action].offroad_label :
                                 k_actions[action].hydro_label);
                return 0;
            }
            if (k_actions[action].has_pad) {
                char *end;
                value = (unsigned)strtoul(s_pad_binding[title][action], &end, 10);
                if (end == s_pad_binding[title][action] || *end || value > 32) {
                    snprintf(why, why_size,
                             "%s controller binding must be 0..32.",
                             title ? k_actions[action].offroad_label :
                                     k_actions[action].hydro_label);
                    return 0;
                }
            }
        }
    return 1;
}

/* A binding equal to what the title would inherit is REMOVED rather than
 * written, so the file keeps only what the user actually changed. */
static int save_bindings_to_doc(void)
{
    unsigned title, action;
    char section[64], shared[64], pad_text[16];

    for (title = 0; title < 2; title++) {
        const char *id = title ? "offroad" : "hydro";
        for (action = 0; action < ACTION_COUNT; action++) {
            const char *key = k_actions[action].key;
            binding_half_t half[2];
            unsigned h, n = action_halves(title, action, half,
                                          pad_text, sizeof pad_text);

            for (h = 0; h < n; h++) {
                if (!ini_doc_get(&s_doc, half[h].section, key,
                             shared, sizeof shared))
                    snprintf(shared, sizeof shared, "%s", half[h].fallback);
                snprintf(section, sizeof section, "%s.%s", id,
                         half[h].section);
                if (!_stricmp(shared, half[h].value)) {
                    if (!ini_doc_remove(&s_doc, section, key))
                        return 0;
                } else if (!ini_doc_set(&s_doc, section, key, half[h].value)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int save_settings(HWND owner)
{
    char why[512];
    unsigned i;

    if (!validate_settings(why, sizeof why)) {
        MessageBoxA(owner, why, "VCThunder settings", MB_OK | MB_ICONWARNING);
        set_status(why);
        return 0;
    }
    /* An absent key gives -1, and the comparison below is unsigned, so it
     * wraps to a value no index can reach. Said out loud rather than left as
     * a thing the reader has to notice. */
    i = (unsigned)setting_find("graphics", "video_mode");
    if (i < SETTING_COUNT && k_settings[i].warning == WARNING_DANGER &&
        !strcmp(s_value[i], "2") &&
        MessageBoxA(owner,
            "Video mode 2 selects 640x480. It is for a bench LCD or emulator "
            "and may damage an original arcade monitor.\n\nSave this value?",
            "Unsafe arcade-monitor mode", MB_YESNO | MB_DEFBUTTON2 |
            MB_ICONWARNING) != IDYES)
        return 0;
    for (i = 0; i < SETTING_COUNT; i++)
        if (!ini_doc_set(&s_doc, k_settings[i].section, k_settings[i].key,
                     s_value[i])) {
            MessageBoxA(owner, "Not enough memory to update vcthunder.ini.",
                        "VCThunder settings", MB_OK | MB_ICONERROR);
            return 0;
        }
    if (!save_bindings_to_doc()) {
        MessageBoxA(owner, "Not enough memory to update title bindings.",
                    "VCThunder settings", MB_OK | MB_ICONERROR);
        return 0;
    }
    if (!ini_doc_save(&s_doc, why, sizeof why)) {
        MessageBoxA(owner, why, "VCThunder settings", MB_OK | MB_ICONERROR);
        set_status(why);
        return 0;
    }
    s_dirty = 0;
    EnableWindow(s_apply, FALSE);
    set_status("Settings saved. A backup is available as vcthunder.ini.bak.");
    return 1;
}

static void mark_dirty(void)
{
    char why[256];
    int valid;
    if (s_loading)
        return;
    s_dirty = 1;
    valid = validate_settings(why, sizeof why);
    if (s_apply)
        EnableWindow(s_apply, valid);
    if (valid)
        set_status("Settings have not been saved.");
    else
        set_status(why);
}

static void combo_add_choices(HWND combo, const char *choices)
{
    const char *p = choices;
    while (p && *p) {
        const char *end = strchr(p, '|');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char item[96];
        if (n >= sizeof item)
            n = sizeof item - 1;
        memcpy(item, p, n);
        item[n] = '\0';
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)item);
        p = end ? end + 1 : NULL;
    }
}

static void combo_select_text(HWND combo, const char *text)
{
    int n = (int)SendMessage(combo, CB_GETCOUNT, 0, 0), i;
    char item[96];
    for (i = 0; i < n; i++) {
        SendMessageA(combo, CB_GETLBTEXT, i, (LPARAM)item);
        if (!_stricmp(item, text)) {
            SendMessage(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SetWindowTextA(combo, text);
}

static int page_width(void)
{
    RECT rc;
    if (!s_page)
        return 700;
    GetClientRect(s_page, &rc);
    return max(unscale_px(rc.right - rc.left) - 18, s_tiny ? 230 : 300);
}

/* Where a page's own content starts, under add_page_header()'s one line of
 * intro. */
static int page_header_bottom(void)
{
    return s_compact ? 62 : 38;
}

/* The link page's fixed panel. Both statements on it are there because the
 * setting below them does NOT do what a player would assume: this page picks a
 * transport, and the two things that decide whether a cabinet links and which
 * cabinet it is belong to the game's own operator menu, in NVRAM.
 *
 * They are MEASURED rather than given a height, because this page is the first
 * whose fixed panel is prose: the same two sentences wrap to two lines at
 * 1200 px and to six in the compact form, and a constant that fits one of them
 * clips the other under the first settings row. */
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
 * ONE LINE PER CALL when it matters. A STATIC renders a string carrying its
 * own "\r\n" in more line-heights than DT_CALCRECT reports for it once a line
 * comes close to the edge -- an absolute path in the Launch page's cards is
 * exactly that -- so a caller that would otherwise measure a two-line string
 * gives each line its own control instead. */
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

/* Where each note goes, and where the settings rows start below them. Both
 * out-parameters may be NULL: page_settings_top() wants only the return. */
static int link_panel_layout(int *note_y, int *note_h)
{
    int width = page_width() - 40;
    int y = page_header_bottom();
    unsigned i;

    for (i = 0; i < LINK_NOTE_COUNT; i++) {
        int h = text_block_height(s_font, k_link_note[i], width);
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
}

/* Where the first settings row goes: below whatever fixed panel the page
 * puts above it. render_settings_page() sizes the page from the same number,
 * so it is a function and not a third copy of the expression. */
static int page_settings_top(void)
{
    if (s_page_index == PAGE_DEVICES)
        return s_compact ? 278 : 190;
    if (s_page_index == PAGE_FFB)
        return s_compact ? 194 : 146;
    if (s_page_index == PAGE_LINK)
        return link_panel_layout(NULL, NULL);
    return page_header_bottom();
}

/* THE `help` COLUMN HAS BEEN IN settings.def SINCE IT WAS WRITTEN AND HAS
 * NEVER REACHED THE SCREEN. Twenty shipped keys carry a sentence saying what
 * the setting is for and what it costs, and a launcher that shows only the
 * label makes the reader guess exactly the settings the table already explains
 * -- `video_mode`, whose help says it can damage an arcade monitor, most of
 * all.
 *
 * The `warning` column was in the same state but worse: acted on in exactly
 * one hardcoded place (`video_mode`'s save-time modal), so the other two
 * warned nobody, and that one only warned AFTER the value had been chosen. It is
 * spelled into the caption's own text rather than shown as a colour, because a
 * colour says nothing in High Contrast and nothing to a reader who cannot see
 * it, and because this launcher deliberately hands High Contrast back to the
 * system rather than inventing colours for it. */
/* Sized against the table, not guessed: the longest shipped help string is 253
 * characters and the "Caution: " lead adds nine. A 256-byte buffer truncated
 * the longest one mid-word ON SCREEN and said nothing, which is why
 * tools/check-launcher-settings.py now caps a help string at
 * VCT_HELP_MAX characters -- a future one that would not fit fails `make
 * check` instead of appearing clipped in the launcher. */
#define VCT_HELP_MAX 400
#define VCT_HELP_CAP (VCT_HELP_MAX + 32)

static const char *setting_help(unsigned index, char *buf, size_t cap)
{
    const setting_desc_t *d = &k_settings[index];
    const char *lead = d->warning == WARNING_DANGER  ? "Warning: " :
                       d->warning == WARNING_CAUTION ? "Caution: " : "";

    if (!d->help)
        return NULL;
    snprintf(buf, cap, "%s%s", lead, d->help);
    return buf;
}

/* Measured, because a caption wraps and a wrapped caption is one line at 1200
 * units and three at 400. Cached against the width, the layout mode and the
 * DPI that produced it: page_setting_y() is O(rows^2) in this by construction
 * and each miss is a GetDC and a DrawText. */
static int s_help_h[SETTING_COUNT];
static int s_help_h_width = -1, s_help_h_compact = -1;
static UINT s_help_h_dpi;

static int setting_help_height(unsigned index)
{
    char text[VCT_HELP_CAP];
    int width = page_width();

    if (!k_settings[index].help)
        return 0;
    if (width != s_help_h_width || s_compact != s_help_h_compact ||
        s_dpi != s_help_h_dpi) {
        memset(s_help_h, 0, sizeof s_help_h);
        s_help_h_width = width;
        s_help_h_compact = s_compact;
        s_help_h_dpi = s_dpi;
    }
    if (!s_help_h[index])
        s_help_h[index] = text_block_height(
            s_font, setting_help(index, text, sizeof text), width - 44) + 8;
    return s_help_h[index];
}

/* Where a row's caption sits inside it: under the control, which in compact
 * mode is itself under the label. A checkbox is its own label in both modes. */
static int setting_caption_offset(unsigned index)
{
    return (s_compact && k_settings[index].type != ST_BOOL) ? 51 : 26;
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
        if (k_settings[i].page == s_page_index)
            y += setting_row_height(i);
    return y;
}

static void render_setting(unsigned index)
{
    const setting_desc_t *d = &k_settings[index];
    HWND control;
    char display[INI_VALUE_CAP];
    const char *control_text = s_value[index];
    int y = page_setting_y(index), control_w = 310;
    int width = page_width();
    int has_button = (d->flags & (SF_DIR | SF_OPEN_FILE | SF_SAVE_FILE |
                                  SF_AXIS)) != 0;
    /* Browse and Detect cannot sit at a fixed x: 564 is off the page below a
     * ~900-unit window, the same way the Launch page's Play buttons would be.
     * The value control gives way instead, down to a floor. */
    int button_x;

    if (d->type == ST_BOOL) {
        control = make_control(0, "BUTTON", d->label,
            BS_AUTOCHECKBOX | WS_TABSTOP, 22, y,
            s_compact ? width - 44 : min(510, width - 44), 24,
            s_page, IDC_SETTING_BASE + (int)index);
        SendMessage(control, BM_SETCHECK,
                    model_bool(d->section, d->key) ? BST_CHECKED : BST_UNCHECKED,
                    0);
    } else {
        make_control(0, "STATIC", d->label, SS_LEFT, 22, y + 4,
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
            combo_add_choices(control, d->choices);
            combo_select_text(control, s_value[index]);
        } else {
            if (d->type == ST_MHZ_UINT) {
                char *end;
                unsigned long hz = strtoul(s_value[index], &end, 10);
                if (end != s_value[index] && !*end &&
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

/* The page does not repeat its own name: the navigation list already carries
 * it highlighted, and the compact form's collapsed combo box shows it too. The
 * intro takes the space a page title would occupy, which is why PAGE_TOP sits
 * 24 logical units higher than a titled page would need. The three Controls
 * pages are not affected: their header is the tab row (add_controls_header),
 * which carries no page name. */
static void add_page_header(void)
{
    make_control(0, "STATIC", k_page_intro[s_page_index], SS_LEFT,
                 20, 16, page_width() - 40, s_compact ? 40 : 20, s_page, 0);
}


static void add_controls_header(void)
{
    static const int ids[] = {
        IDC_CONTROL_BINDINGS, IDC_CONTROL_DEVICES, IDC_CONTROL_FFB
    };
    const char *labels[] = {
        "Bindings", s_tiny ? "Devices" : "Devices && Axes",
        s_tiny ? "FFB" : "Force Feedback"
    };
    int width = page_width();
    int tab_w = (width - 48) / 3;
    unsigned i;

    for (i = 0; i < 3; i++) {
        HWND tab = make_control(0, "BUTTON", labels[i],
            BS_AUTORADIOBUTTON | WS_TABSTOP | (i ? 0 : WS_GROUP),
            20 + (int)i * (tab_w + 4), 10, tab_w, 24,
            s_page, ids[i]);
        SendMessage(tab, BM_SETCHECK,
            ((i == 0 && s_page_index == PAGE_BINDINGS) ||
             (i == 1 && s_page_index == PAGE_DEVICES) ||
             (i == 2 && s_page_index == PAGE_FFB)) ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
    make_control(0, "STATIC", k_page_intro[s_page_index], SS_LEFT,
                 20, 42, width - 40, s_compact ? 40 : 20, s_page, 0);
}

static int game_path(const char *title, char *out, size_t out_size)
{
    const game_profile_t *p = game_find(title);
    const char *root = model_value("paths", "data_dir");
    char path[MAX_PATH];

    if (!p)
        return 0;
    /* Exact first, then the case the filesystem actually uses -- the launcher
     * greys out a title it cannot find, so without the fallback a perfectly
     * good pack on a case-sensitive data directory offers nothing to play.
     * Same rule as game_resolve_paths(); see fs_resolve_ci(). */
    snprintf(path, sizeof path, "%s\\%s\\%s", root, p->id, p->exe);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, out_size, "%s", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s\\%s", root, p->exe);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, out_size, "%s", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s\\%s", p->id, p->exe);
    if (fs_resolve_ci(root, path, out, out_size) ||
        fs_resolve_ci(root, p->exe, out, out_size))
        return 1;
    snprintf(out, out_size, "%s\\%s\\%s", root, p->id, p->exe);
    return 0;
}

static void render_home(void)
{
    unsigned i;
    const char *ids[] = { "hydro", "offroad" };
    char validation[256];
    int settings_valid = validate_settings(validation, sizeof validation);
    int width = page_width();
    int group_x = s_tiny ? 6 : (s_compact ? 12 : 20);
    /* 650 is what a card wants; the page is what it has. The two-column
     * layout starts at a 760-unit window, which leaves the page about 500,
     * so a card that insisted on 650 hung its right edge -- and the Play
     * button right-aligned to it -- off the side of the page, with nothing
     * to scroll it back into view. */
    int group_w = s_tiny ? width - 12 :
                  (s_compact ? width - 24 : min(650, width - group_x * 2));
    int next_card_y = s_tiny ? 15 : (s_compact ? 54 : 72);

    if (s_compact) {
        if (!s_tiny)
            make_control(0, "STATIC",
                         s_external_overrides ?
                         "Per-title INI overrides remain active." :
                         k_page_intro[PAGE_HOME], SS_LEFT,
                         12, 10, width - 24, 20, s_page, 0);
    } else {
        add_page_header();
    }
    /* Each card is as tall as what is in it, and so is the detail block under
     * the title. At a constant height, a larger title face or a save directory
     * spelled out in full puts the second line outside the box, where it is
     * simply not drawn. Measure the block, size the card to it, and stack the
     * next card below where this one actually ended. */
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
        HFONT title_font = s_tiny ? s_title_small_font : s_title_font;
        HWND play, title_label, card = NULL;

        if (!title_font)
            title_font = s_font;

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
        line_h = text_block_height(s_font, "Ag", status_w);
        path_y = title_y + title_h + 6;
        save_y = path_y + line_h + 2;
        bottom = (s_tiny ? path_y : save_y) + line_h;
        card_h = bottom + pad_bottom - card_y;
        if (card_h < floor_h)
            card_h = floor_h;
        play_y = card_y + (card_h - play_h) / 2;

        if (s_dark_mode) {
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
        if (!s_compact && s_play_font)
            SendMessage(play, WM_SETFONT, (WPARAM)s_play_font, TRUE);
        EnableWindow(play, ready && settings_valid);
        if (card)
            SetWindowPos(card, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        next_card_y = card_y + card_h + (s_tiny ? 9 : (s_compact ? 16 : 42));
    }
    if (s_external_overrides && !s_compact)
        make_control(0, "STATIC",
            "This INI contains per-title settings outside the binding sections. "
            "They are preserved and still override the common launcher values.",
            SS_LEFT, group_x, next_card_y + 8, group_w, 48, s_page, 0);
    s_content_height = next_card_y +
                       (s_external_overrides && !s_compact ? 64 : 8);
}

/* A control Hydro does not have is hidden while Hydro is selected, and a NULL
 * Hydro label is the only thing that says so. */
static int action_visible(unsigned action)
{
    return k_actions[action].hydro_label != NULL || s_binding_title;
}

static void populate_probe_combo(HWND combo)
{
    unsigned i;
    if (!input_probe_count()) {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"No controller detected");
        SendMessage(combo, CB_SETCURSEL, 0, 0);
        EnableWindow(combo, FALSE);
        return;
    }
    for (i = 0; i < input_probe_count(); i++) {
        const input_probe_device_t *d = input_probe_device(i);
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)d->name);
    }
    if (s_probe_selected < 0 || (unsigned)s_probe_selected >= input_probe_count())
        s_probe_selected = 0;
    SendMessage(combo, CB_SETCURSEL, s_probe_selected, 0);
}

static void render_bindings(void)
{
    unsigned i;
    int row = 0, y;
    HWND combo;

    add_controls_header();
    if (s_compact) {
        int width = page_width();
        int half = (width - 40) / 2;
        int tab_w = (width - 44) / 2;
        make_control(0, "BUTTON", "Hydro Thunder", BS_AUTORADIOBUTTON |
                     WS_GROUP | WS_TABSTOP, 20, 82, tab_w, 24,
                     s_page, IDC_TITLE_HYDRO);
        make_control(0, "BUTTON", "Offroad Thunder", BS_AUTORADIOBUTTON |
                     WS_TABSTOP, 24 + tab_w, 82, tab_w, 24,
                     s_page, IDC_TITLE_OFFROAD);
        SendDlgItemMessage(s_page, s_binding_title ? IDC_TITLE_OFFROAD :
                           IDC_TITLE_HYDRO, BM_SETCHECK, BST_CHECKED, 0);
        make_control(0, "STATIC", "Test controller", SS_LEFT,
                     20, 112, width - 40, 20, s_page, 0);
        combo = make_control(0, "COMBOBOX", "", CBS_DROPDOWNLIST |
                             WS_VSCROLL | WS_TABSTOP, 20, 134,
                             width - 130, 240, s_page, IDC_PROBE_DEVICE);
        populate_probe_combo(combo);
        make_control(0, "BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP,
                     width - 102, 134, 82, 24, s_page, IDC_PROBE_REFRESH);
        make_control(0, "STATIC", "Keyboard", SS_LEFT,
                     20, 168, half - 8, 20, s_page, 0);
        make_control(0, "STATIC", "Controller", SS_LEFT,
                     20 + half, 168, half - 8, 20, s_page, 0);
        for (i = 0; i < ACTION_COUNT; i++) {
            const action_desc_t *a = &k_actions[i];
            const char *label;
            int key_w = max(half - 70, 70);
            int pad_x = 20 + half;
            if (!action_visible(i))
                continue;
            y = 192 + row++ * 58;
            label = s_binding_title ? a->offroad_label : a->hydro_label;
            make_control(0, "STATIC", label ? label : "", SS_LEFT,
                         20, y, width - 40, 20, s_page, 0);
            if (s_binding_title ? a->offroad_key : a->hydro_key) {
                make_control(WS_EX_CLIENTEDGE, "EDIT",
                    s_key_binding[s_binding_title][i],
                    ES_READONLY | ES_AUTOHSCROLL, 20, y + 23, key_w, 24,
                    s_page, IDC_KEY_TEXT_BASE + (int)i);
                make_control(0, "BUTTON", "Bind", BS_PUSHBUTTON | WS_TABSTOP,
                    26 + key_w, y + 23, 54, 24,
                    s_page, IDC_KEY_BIND_BASE + (int)i);
            } else {
                make_control(0, "STATIC", "--", SS_LEFT,
                             20, y + 27, half - 8, 20, s_page, 0);
            }
            if (a->has_pad) {
                char pad[32];
                unsigned n = (unsigned)strtoul(
                    s_pad_binding[s_binding_title][i], NULL, 10);
                snprintf(pad, sizeof pad, n ? "Button %u" : "Unbound", n);
                make_control(WS_EX_CLIENTEDGE, "EDIT", pad,
                    ES_READONLY | ES_AUTOHSCROLL, pad_x, y + 23,
                    key_w, 24, s_page, IDC_PAD_TEXT_BASE + (int)i);
                make_control(0, "BUTTON", "Bind", BS_PUSHBUTTON | WS_TABSTOP,
                    pad_x + key_w + 6, y + 23, 54, 24,
                    s_page, IDC_PAD_BIND_BASE + (int)i);
            } else {
                make_control(0, "STATIC", "Axis / POV", SS_LEFT,
                             pad_x, y + 27, half - 8, 20, s_page, 0);
            }
        }
        s_content_height = 214 + row * 58;
        return;
    }
    {
    /* The wide table, placed from the page's own width. No column is a
     * constant: a Bind button fixed at 560 leaves the page below a ~900-unit
     * window, and title radio buttons at 420/540 on the tab row overlap
     * "Force Feedback" at EVERY width. The radios have a row of their own, and
     * the columns keep their nominal positions whenever there is room. */
    int width = page_width();
    int right = width - 22;
    int bind_w = 58, gap = 6, col_gap = 14;
    int spare = right - 190 - (bind_w + gap) * 2 - col_gap;
    int key_w = min(112, max(spare * 112 / 242, 60));
    int pad_w = min(130, max(spare - key_w, 70));
    int key_x = 190, key_bind_x = key_x + key_w + gap;
    int pad_x = key_bind_x + bind_w + col_gap;
    int pad_bind_x, refresh_x, combo_w, header_y, first_row_y;

    if (pad_x < 424 && 424 + pad_w + gap + bind_w <= right)
        pad_x = 424;
    pad_bind_x = pad_x + pad_w + gap;
    refresh_x = min(564, right - 82);
    combo_w = min(409, refresh_x - 145 - 10);
    header_y = 135;
    first_row_y = 159;

    make_control(0, "BUTTON", "Hydro Thunder", BS_AUTORADIOBUTTON | WS_GROUP |
                 WS_TABSTOP, 20, 66, 130, 24, s_page, IDC_TITLE_HYDRO);
    make_control(0, "BUTTON", "Offroad Thunder", BS_AUTORADIOBUTTON |
                 WS_TABSTOP, 156, 66, 145, 24, s_page, IDC_TITLE_OFFROAD);
    SendDlgItemMessage(s_page, s_binding_title ? IDC_TITLE_OFFROAD :
                       IDC_TITLE_HYDRO, BM_SETCHECK, BST_CHECKED, 0);
    make_control(0, "STATIC", "Test controller", SS_LEFT, 20, 102, 120, 22,
                 s_page, 0);
    combo = make_control(0, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL |
                         WS_TABSTOP, 145, 98, combo_w, 240, s_page,
                         IDC_PROBE_DEVICE);
    populate_probe_combo(combo);
    make_control(0, "BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP,
                 refresh_x, 98, 82, 24, s_page, IDC_PROBE_REFRESH);
    make_control(0, "STATIC", "Action", SS_LEFT, 20, header_y, 155, 22,
                 s_page, 0);
    make_control(0, "STATIC", "Keyboard", SS_LEFT, key_x, header_y,
                 pad_x - key_x - col_gap, 22, s_page, 0);
    make_control(0, "STATIC", "Controller", SS_LEFT, pad_x, header_y,
                 right - pad_x, 22, s_page, 0);
    for (i = 0; i < ACTION_COUNT; i++) {
        const action_desc_t *a = &k_actions[i];
        const char *label;
        if (!action_visible(i))
            continue;
        y = first_row_y + row++ * 32;
        label = s_binding_title ? a->offroad_label : a->hydro_label;
        make_control(0, "STATIC", label ? label : "", SS_LEFT,
                     20, y + 4, 160, 22, s_page, 0);
        if (s_binding_title ? a->offroad_key : a->hydro_key) {
            make_control(WS_EX_CLIENTEDGE, "EDIT",
                s_key_binding[s_binding_title][i], ES_READONLY | ES_AUTOHSCROLL,
                key_x, y, key_w, 24, s_page, IDC_KEY_TEXT_BASE + (int)i);
            make_control(0, "BUTTON", "Bind", BS_PUSHBUTTON | WS_TABSTOP,
                key_bind_x, y, bind_w, 24, s_page,
                IDC_KEY_BIND_BASE + (int)i);
        } else {
            make_control(0, "STATIC", "--", SS_LEFT, key_x, y + 4,
                         key_w + gap + bind_w, 22, s_page, 0);
        }
        if (a->has_pad) {
            char pad[32];
            unsigned n = (unsigned)strtoul(s_pad_binding[s_binding_title][i],
                                            NULL, 10);
            snprintf(pad, sizeof pad, n ? "Button %u" : "Unbound", n);
            make_control(WS_EX_CLIENTEDGE, "EDIT", pad,
                ES_READONLY | ES_AUTOHSCROLL, pad_x, y, pad_w, 24,
                s_page, IDC_PAD_TEXT_BASE + (int)i);
            make_control(0, "BUTTON", "Bind", BS_PUSHBUTTON | WS_TABSTOP,
                pad_bind_x, y, bind_w, 24, s_page,
                IDC_PAD_BIND_BASE + (int)i);
        } else {
            make_control(0, "STATIC", "Axis / POV", SS_LEFT,
                         pad_x, y + 4, pad_w + gap + bind_w, 22, s_page, 0);
        }
    }
    s_content_height = first_row_y + 15 + row * 32;
    }
}

static void render_probe_panel(void)
{
    HWND combo;
    int width = page_width();
    if (s_compact) {
        make_control(0, "STATIC", "Configured/test device", SS_LEFT,
                     20, 86, width - 40, 20, s_page, 0);
        combo = make_control(0, "COMBOBOX", "", CBS_DROPDOWNLIST |
                             WS_VSCROLL | WS_TABSTOP, 20, 108,
                             width - 130, 240, s_page, IDC_PROBE_DEVICE);
        populate_probe_combo(combo);
        make_control(0, "BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP,
                     width - 102, 108, 82, 24, s_page, IDC_PROBE_REFRESH);
        make_control(0, "STATIC", "Steering", SS_LEFT,
                     20, 145, 76, 20, s_page, 0);
        make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH,
                     100, 145, width - 120, 18, s_page, IDC_LIVE_STEER);
        make_control(0, "STATIC", "Throttle", SS_LEFT,
                     20, 171, 76, 20, s_page, 0);
        make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH,
                     100, 171, width - 120, 18, s_page, IDC_LIVE_THROTTLE);
        make_control(0, "STATIC", "Brake", SS_LEFT,
                     20, 197, 76, 20, s_page, 0);
        make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH,
                     100, 197, width - 120, 18, s_page, IDC_LIVE_BRAKE);
        make_control(0, "STATIC", "No live input", SS_LEFT,
                     20, 225, width - 40, 42, s_page, IDC_LIVE_BUTTONS);
        SendDlgItemMessage(s_page, IDC_LIVE_STEER, PBM_SETRANGE32, 0, 1000);
        SendDlgItemMessage(s_page, IDC_LIVE_THROTTLE, PBM_SETRANGE32, 0, 1000);
        SendDlgItemMessage(s_page, IDC_LIVE_BRAKE, PBM_SETRANGE32, 0, 1000);
        return;
    }
    {
    /* Placed from the page's width for the same reason the settings rows and
     * the binding columns are: Refresh sat at 564 and the button read-out at
     * 438..648, both off a page narrower than about 670. */
    int right = width - 22;
    int refresh_x = min(564, right - 82);
    int combo_w = min(350, refresh_x - 204 - 10);
    int bar_w = min(300, right - 288);
    int live_x = 125 + bar_w + 13;

    make_control(0, "STATIC", "Configured/test device", SS_LEFT,
                 20, 66, 180, 22, s_page, 0);
    combo = make_control(0, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL |
                         WS_TABSTOP, 204, 62, combo_w, 240, s_page,
                         IDC_PROBE_DEVICE);
    populate_probe_combo(combo);
    make_control(0, "BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP,
                 refresh_x, 62, 82, 24, s_page, IDC_PROBE_REFRESH);
    make_control(0, "STATIC", "Steering", SS_LEFT, 20, 100, 100, 20, s_page, 0);
    make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, 125, 100, bar_w, 18,
                 s_page, IDC_LIVE_STEER);
    make_control(0, "STATIC", "Throttle", SS_LEFT, 20, 126, 100, 20, s_page, 0);
    make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, 125, 126, bar_w, 18,
                 s_page, IDC_LIVE_THROTTLE);
    make_control(0, "STATIC", "Brake", SS_LEFT, 20, 152, 100, 20, s_page, 0);
    make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, 125, 152, bar_w, 18,
                 s_page, IDC_LIVE_BRAKE);
    make_control(0, "STATIC", "No live input", SS_LEFT,
                 live_x, 100, right - live_x, 70, s_page, IDC_LIVE_BUTTONS);
    }
    SendDlgItemMessage(s_page, IDC_LIVE_STEER, PBM_SETRANGE32, 0, 1000);
    SendDlgItemMessage(s_page, IDC_LIVE_THROTTLE, PBM_SETRANGE32, 0, 1000);
    SendDlgItemMessage(s_page, IDC_LIVE_BRAKE, PBM_SETRANGE32, 0, 1000);
}

static void render_settings_page(void)
{
    unsigned i;

    if (page_is_controls(s_page_index))
        add_controls_header();
    else
        add_page_header();
    if (s_page_index == PAGE_DEVICES)
        render_probe_panel();
    if (s_page_index == PAGE_FFB) {
        const input_probe_device_t *device = s_probe_selected >= 0 ?
            input_probe_device((unsigned)s_probe_selected) : NULL;
        char label[MAX_PATH + 32];
        HWND test;
        snprintf(label, sizeof label, "Test device: %s",
                 device ? device->name : "none detected");
        make_control(0, "STATIC", label, SS_LEFT, 20,
                     s_compact ? 84 : 64,
                     s_compact ? page_width() - 40 : page_width() - 210,
                     20, s_page, 0);
        test = make_control(0, "BUTTON", "Test force feedback",
                            BS_PUSHBUTTON | WS_TABSTOP,
                            s_compact ? 20 : page_width() - 180,
                            s_compact ? 108 : 62,
                            s_compact ? min(page_width() - 40, 180) : 160,
                            28, s_page, IDC_FFB_TEST);
        EnableWindow(test, device && device->force_feedback);
        /* The paragraph that stood here said what the `autocenter` row's own
         * help column says, in different words, at a constant y. One table,
         * one statement: it is rendered with every other caption now. */
    }
    if (s_page_index == PAGE_LINK)
        render_link_panel();
    for (i = 0; i < SETTING_COUNT; i++)
        if (k_settings[i].page == s_page_index)
            render_setting(i);
    {
        int bottom = page_settings_top();
        for (i = 0; i < SETTING_COUNT; i++)
            if (k_settings[i].page == s_page_index)
                bottom += setting_row_height(i);
        s_content_height = bottom + 20;
    }
}

static void pull_page(void)
{
    unsigned i;
    if (!s_page || s_page_index == PAGE_HOME || s_page_index == PAGE_BINDINGS)
        return;
    for (i = 0; i < SETTING_COUNT; i++) {
        HWND c;
        if (k_settings[i].page != s_page_index)
            continue;
        c = GetDlgItem(s_page, IDC_SETTING_BASE + (int)i);
        if (!c)
            continue;
        if (k_settings[i].type == ST_BOOL)
            snprintf(s_value[i], sizeof s_value[i], "%s",
                     SendMessage(c, BM_GETCHECK, 0, 0) == BST_CHECKED ?
                     "true" : "false");
        else if (k_settings[i].type == ST_MHZ_UINT) {
            char mhz[INI_VALUE_CAP], *end;
            unsigned long value;
            GetWindowTextA(c, mhz, sizeof mhz);
            errno = 0;
            value = strtoul(mhz, &end, 10);
            if (errno != ERANGE && end != mhz && !*end &&
                value >= 1 && value <= 4000)
                snprintf(s_value[i], sizeof s_value[i], "%lu",
                         value * 1000000ul);
            else
                snprintf(s_value[i], sizeof s_value[i], "%s", mhz);
        } else
            GetWindowTextA(c, s_value[i], sizeof s_value[i]);
    }
}

static LRESULT CALLBACK card_proc(HWND h, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    if (message == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wparam, &rc, s_page_brush);
        return TRUE;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT rc;
        HDC dc = BeginPaint(h, &paint);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, s_page_brush);
        FrameRect(dc, &rc, s_border_brush);
        EndPaint(h, &paint);
        return 0;
    }
    return DefWindowProcA(h, message, wparam, lparam);
}

static LRESULT CALLBACK page_proc(HWND h, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    if (message == WM_DRAWITEM && draw_dark_button((DRAWITEMSTRUCT *)lparam))
        return TRUE;
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN) {
        LRESULT colour = control_colour(h, message, wparam, lparam);
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
        int old = s_page_scroll, next = old;

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
        if (next < 0) next = 0;
        if (next > scale_px(s_content_height) - (rc.bottom - rc.top))
            next = scale_px(s_content_height) - (rc.bottom - rc.top);
        if (next < 0) next = 0;
        if (next != old) {
            SendMessage(h, WM_SETREDRAW, FALSE, 0);
            s_page_scroll = next;
            ScrollWindowEx(h, 0, old - next, NULL, NULL, NULL, NULL,
                           SW_SCROLLCHILDREN);
            SetScrollPos(h, SB_VERT, next, TRUE);
            SendMessage(h, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE |
                         RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
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

static void update_scrollbar(void)
{
    RECT rc;
    SCROLLINFO si;
    if (!s_page)
        return;
    GetClientRect(s_page, &rc);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = max(scale_px(s_content_height) - 1, 0);
    si.nPage = (UINT)(rc.bottom - rc.top);
    si.nPos = 0;
    s_page_scroll = 0;
    SetScrollInfo(s_page, SB_VERT, &si, TRUE);
    ShowScrollBar(s_page, SB_VERT,
                  scale_px(s_content_height) > rc.bottom - rc.top);
}

static void layout_window(void);

static void render_page(void)
{
    RECT rc;
    s_loading = 1;
    if (s_page) {
        pull_page();
        DestroyWindow(s_page);
    }
    GetClientRect(s_window, &rc);
    s_page = make_control(WS_EX_CLIENTEDGE, "VCThunderLauncherPage", "",
        WS_VSCROLL | WS_CLIPCHILDREN | WS_TABSTOP,
        s_compact ? 12 : 228, s_compact ? 40 : 12,
        unscale_px(rc.right - scale_px(s_compact ? 24 : 240)),
        unscale_px(rc.bottom - scale_px(s_compact ? 88 : 70)),
        s_window, IDC_PAGE);
    /* A WM_SIZE mode switch can arrive while Windows still reports the old
     * child's rectangle. Put the new page in its final rectangle before any
     * responsive child computes page_width(). */
    layout_window();
    s_rendered_page_w = page_width();
    if (s_page_index == PAGE_HOME)
        render_home();
    else if (s_page_index == PAGE_BINDINGS)
        render_bindings();
    else
        render_settings_page();
    update_scrollbar();
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
    snprintf(selected, sizeof selected, "%s", s_value[index]);
    if (d->flags & SF_DIR) {
        if (!browse_directory(s_window, selected, sizeof selected))
            return;
    } else if (!browse_file(s_window, selected, sizeof selected,
                            (d->flags & SF_OPEN_FILE) != 0)) {
        return;
    }
    snprintf(s_value[index], sizeof s_value[index], "%s", selected);
    control = GetDlgItem(s_page, IDC_SETTING_BASE + (int)index);
    if (control)
        SetWindowTextA(control, selected);
    mark_dirty();
}

static int probe_backend_matches_setting(const input_probe_device_t *device,
                                         const char *key)
{
    if (!device)
        return 0;
    if (!strncmp(key, "dinput_", 7)) return device->backend == INPUT_PROBE_DINPUT;
    if (!strncmp(key, "xinput_", 7)) return device->backend == INPUT_PROBE_XINPUT;
    if (!strncmp(key, "winmm_", 6)) return device->backend == INPUT_PROBE_WINMM;
    return 0;
}

static void begin_axis_capture(unsigned index)
{
    const input_probe_device_t *device;
    unsigned i;

    if (index >= SETTING_COUNT || s_probe_selected < 0)
        return;
    device = input_probe_device((unsigned)s_probe_selected);
    if (!probe_backend_matches_setting(device, k_settings[index].key)) {
        set_status("Select a test device from the same provider before detecting this axis.");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (!input_probe_poll(&s_probe_state)) {
        set_status("The selected controller is not currently responding.");
        return;
    }
    s_axis_baseline_count = s_probe_state.axis_count;
    for (i = 0; i < s_axis_baseline_count; i++) {
        s_axis_baseline[i] = s_probe_state.axis[i];
        snprintf(s_axis_baseline_name[i], sizeof s_axis_baseline_name[i], "%s",
                 s_probe_state.axis_name[i]);
    }
    s_capture_kind = 3;
    s_capture_setting = (int)index;
    set_status("Move the requested axis through at least 40% of its range; press Escape to cancel.");
}

static void update_model_control(const char *section, const char *key)
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
        combo_select_text(c, s_value[index]);
    else
        SetWindowTextA(c, s_value[index]);
}

/* Write one value into the model and refresh whatever control is showing it.
 *
 * setting_find() answers -1 for a key settings.def does not carry, so the four
 * call sites below must not index s_value with it UNCHECKED. The table carries
 * all four today; a key removed from it would turn selecting a controller into
 * a write one slot before the array. */
static void model_set(const char *section, const char *key, const char *value)
{
    int i = setting_find(section, key);

    if (i < 0) {
        /* The status bar, not the log: this window runs before log_open(), so
         * a LOGW here would go nowhere a user could read it. */
        set_status("This build has no setting for the selected device; it was "
                   "selected but nothing was written to the file.");
        return;
    }
    snprintf(s_value[i], sizeof s_value[i], "%s", value);
    update_model_control(section, key);
}

static void select_probe_from_combo(HWND combo, int update_configuration)
{
    int selected = (int)SendMessage(combo, CB_GETCURSEL, 0, 0);
    const input_probe_device_t *d;
    char text[32];

    if (selected < 0 || (unsigned)selected >= input_probe_count() ||
        !input_probe_select((unsigned)selected))
        return;
    s_probe_selected = selected;
    s_previous_buttons = 0;
    if (!update_configuration)
        return;
    d = input_probe_device((unsigned)selected);
    model_set("analog", "provider",
              d->backend == INPUT_PROBE_DINPUT ? "dinput" :
              d->backend == INPUT_PROBE_XINPUT ? "xinput" : "winmm");
    if (d->backend == INPUT_PROBE_DINPUT) {
        const char *name = strstr(d->name, ": ");
        model_set("analog", "dinput_name", name ? name + 2 : d->name);
        model_set("analog", "dinput_device", "auto");
    } else {
        const char *key = d->backend == INPUT_PROBE_XINPUT ?
                          "xinput_device" : "winmm_device";
        snprintf(text, sizeof text, "%u", d->backend_index);
        model_set("analog", key, text);
    }
    mark_dirty();
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

static void report_duplicate_binding(int pad);

static void update_live_input(void)
{
    const input_probe_device_t *d;
    const char *prefix, *steer_name, *throttle_name, *brake_name;
    char key[64], buttons[192];
    float steer = 0.0f, throttle = 0.0f, brake = 0.0f, deadzone;
    float raw_throttle, raw_brake, drive;
    int found, separate = 1;
    unsigned i, shown = 0;

    if (s_probe_selected < 0 || !input_probe_poll(&s_probe_state))
        return;
    d = input_probe_device((unsigned)s_probe_selected);
    if (!d)
        return;

    if (s_capture_kind == 2) {
        if (!s_capture_armed && s_probe_state.buttons == 0)
            s_capture_armed = 1;
        if (s_capture_armed && s_probe_state.buttons) {
            for (i = 0; i < 32; i++)
                if (s_probe_state.buttons & (1u << i)) {
                    snprintf(s_pad_binding[s_binding_title][s_capture_action],
                             sizeof s_pad_binding[0][0], "%u", i + 1);
                    s_capture_kind = 0;
                    mark_dirty();
                    update_binding_display((unsigned)s_capture_action, 1);
                    report_duplicate_binding(1);
                    set_status("Controller button bound.");
                    break;
                }
        }
    } else if (s_capture_kind == 3) {
        for (i = 0; i < s_probe_state.axis_count; i++) {
            unsigned j;
            for (j = 0; j < s_axis_baseline_count; j++)
                if (!_stricmp(s_probe_state.axis_name[i],
                              s_axis_baseline_name[j]) &&
                    (s_probe_state.axis[i] - s_axis_baseline[j] > 0.4f ||
                     s_axis_baseline[j] - s_probe_state.axis[i] > 0.4f)) {
                    snprintf(s_value[s_capture_setting],
                             sizeof s_value[s_capture_setting], "%s",
                             s_probe_state.axis_name[i]);
                    update_model_control(k_settings[s_capture_setting].section,
                                         k_settings[s_capture_setting].key);
                    s_capture_kind = 0;
                    mark_dirty();
                    set_status("Axis detected and assigned.");
                    break;
                }
            if (!s_capture_kind)
                break;
        }
    }
    if (s_page_index != PAGE_DEVICES || !s_page)
        return;
    prefix = d->backend == INPUT_PROBE_DINPUT ? "dinput" :
             d->backend == INPUT_PROBE_XINPUT ? "xinput" : "winmm";
    snprintf(key, sizeof key, "%s_steering_axis", prefix);
    steer_name = model_value("analog", key);
    snprintf(key, sizeof key, "%s_throttle_axis", prefix);
    throttle_name = model_value("analog", key);
    snprintf(key, sizeof key, "%s_brake_axis", prefix);
    brake_name = model_value("analog", key);

    steer = probe_axis_value(steer_name, &found);
    if (!found) steer = 0.0f;
    deadzone = (float)model_uint("analog",
        d->backend == INPUT_PROBE_DINPUT ? "dinput_deadzone_percent" :
                                           "deadzone_percent", 0) / 100.0f;
    if (d->backend == INPUT_PROBE_DINPUT) {
        float range = (float)model_uint("analog",
                            "dinput_steering_range_percent", 100) / 100.0f;
        if (range > 0.0f) steer /= range;
        if (steer < -1.0f) steer = -1.0f;
        if (steer > 1.0f) steer = 1.0f;
    }
    steer = io_deadzone_centred(steer, deadzone);
    snprintf(key, sizeof key, "%s_steering_invert", prefix);
    if (model_bool("analog", key)) steer = -steer;

    raw_throttle = probe_axis_value(throttle_name, &found);
    if (!found) raw_throttle = 0.0f;
    raw_brake = probe_axis_value(brake_name, &found);
    if (!found) raw_brake = 0.0f;
    if (d->backend != INPUT_PROBE_XINPUT) {
        snprintf(key, sizeof key, "%s_pedal_mode", prefix);
        separate = _stricmp(model_value("analog", key), "combined") != 0;
    }
    if (!_stricmp(brake_name, "none"))
        separate = 0;
    if (!separate) {
        int trigger = d->backend == INPUT_PROBE_XINPUT &&
            (!_stricmp(throttle_name, "lt") || !_stricmp(throttle_name, "rt"));
        if (trigger) {
            drive = io_deadzone_unipolar((raw_throttle + 1.0f) * 0.5f,
                                         deadzone);
        } else {
            drive = io_deadzone_centred(raw_throttle, deadzone);
        }
        snprintf(key, sizeof key, "%s_throttle_invert", prefix);
        if (model_bool("analog", key))
            drive = -drive;
        throttle = drive > 0.0f ? drive : 0.0f;
        brake = drive < 0.0f ? -drive : 0.0f;
    } else {
        /* The same law the cabinet gets (io.h), so this preview cannot say one
         * thing while the game is handed another. */
        int t_inv, b_inv;

        snprintf(key, sizeof key, "%s_throttle_invert", prefix);
        t_inv = model_bool("analog", key);
        snprintf(key, sizeof key, "%s_brake_invert", prefix);
        b_inv = model_bool("analog", key);
        drive = io_pedals_separate((raw_throttle + 1.0f) * 0.5f,
                                   (raw_brake + 1.0f) * 0.5f,
                                   t_inv, b_inv, deadzone);
        throttle = drive > 0.0f ? drive : 0.0f;
        brake = drive < 0.0f ? -drive : 0.0f;
    }
    SendDlgItemMessage(s_page, IDC_LIVE_STEER, PBM_SETPOS,
                       (WPARAM)((steer + 1.0f) * 500.0f), 0);
    SendDlgItemMessage(s_page, IDC_LIVE_THROTTLE, PBM_SETPOS,
                       (WPARAM)(throttle * 1000.0f), 0);
    SendDlgItemMessage(s_page, IDC_LIVE_BRAKE, PBM_SETPOS,
                       (WPARAM)(brake * 1000.0f), 0);
    snprintf(buttons, sizeof buttons, "Buttons:");
    for (i = 0; i < 32; i++)
        if (s_probe_state.buttons & (1u << i)) {
            char one[12];
            snprintf(one, sizeof one, " %u", i + 1);
            strncat(buttons, one, sizeof buttons - strlen(buttons) - 1);
            shown++;
        }
    if (!shown)
        strncat(buttons, " none", sizeof buttons - strlen(buttons) - 1);
    if (s_probe_state.pov >= 0) {
        char pov[32];
        snprintf(pov, sizeof pov, "\r\nPOV: %d degrees",
                 s_probe_state.pov / 100);
        strncat(buttons, pov, sizeof buttons - strlen(buttons) - 1);
    }
    SetDlgItemTextA(s_page, IDC_LIVE_BUTTONS, buttons);
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

static void update_binding_display(unsigned action, int pad)
{
    HWND control;
    char text[32];

    if (s_page_index != PAGE_BINDINGS || !s_page || action >= ACTION_COUNT)
        return;
    control = GetDlgItem(s_page, (pad ? IDC_PAD_TEXT_BASE :
                                 IDC_KEY_TEXT_BASE) + (int)action);
    if (!control)
        return;
    if (!pad) {
        SetWindowTextA(control, s_key_binding[s_binding_title][action]);
        return;
    }
    {
        unsigned button = (unsigned)strtoul(
            s_pad_binding[s_binding_title][action], NULL, 10);
        snprintf(text, sizeof text, button ? "Button %u" : "Unbound", button);
    }
    SetWindowTextA(control, text);
}

static void report_duplicate_binding(int pad)
{
    unsigned i, j;
    for (i = 0; i < ACTION_COUNT; i++) {
        int have_i = pad ? k_actions[i].has_pad :
            (s_binding_title ? k_actions[i].offroad_key :
                               k_actions[i].hydro_key) != NULL;
        if (action_visible(i) && have_i)
            for (j = i + 1; j < ACTION_COUNT; j++)
                if (action_visible(j) && (pad ? k_actions[j].has_pad :
                    (s_binding_title ? k_actions[j].offroad_key :
                                       k_actions[j].hydro_key) != NULL) &&
                    !_stricmp(pad ? s_pad_binding[s_binding_title][i] :
                                   s_key_binding[s_binding_title][i],
                              pad ? s_pad_binding[s_binding_title][j] :
                                   s_key_binding[s_binding_title][j]) &&
                    (!pad || strcmp(s_pad_binding[s_binding_title][i], "0"))) {
                    set_status("Duplicate binding: one input is assigned to more than one action.");
                    return;
                }
    }
}

static void handle_key_capture(WPARAM key, LPARAM detail)
{
    char name[32];
    const char *mapped;

    if (key == VK_ESCAPE) {
        s_capture_kind = 0;
        set_status("Binding cancelled.");
        return;
    }
    if (key == VK_BACK) {
        snprintf(s_key_binding[s_binding_title][s_capture_action],
                 sizeof s_key_binding[0][0], "NONE");
    } else {
        mapped = key_name_from_message(key, detail, name, sizeof name);
        if (!mapped) {
            MessageBeep(MB_ICONWARNING);
            set_status("That key is not one of the MAME key names supported by VCThunder.");
            return;
        }
        snprintf(s_key_binding[s_binding_title][s_capture_action],
                 sizeof s_key_binding[0][0], "%s", mapped);
    }
    s_capture_kind = 0;
    mark_dirty();
    update_binding_display((unsigned)s_capture_action, 0);
    report_duplicate_binding(0);
}

static void begin_key_capture(unsigned action)
{
    s_capture_kind = 1;
    s_capture_action = (int)action;
    SetFocus(s_window);
    set_status("Press a key; Backspace clears the binding and Escape cancels.");
}

static void begin_pad_capture(unsigned action)
{
    if (s_probe_selected < 0) {
        set_status("Attach and select a test controller before binding a button.");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    s_capture_kind = 2;
    s_capture_action = (int)action;
    s_capture_armed = 0;
    set_status("Release all buttons, then press one to bind; Backspace clears and Escape cancels.");
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
        if (!validate_settings(why, sizeof why)) {
            MessageBoxA(s_window, why, "VCThunder settings",
                        MB_OK | MB_ICONWARNING);
            return 0;
        }
    }
    if (!game_path(title, path, sizeof path)) {
        MessageBoxA(s_window,
            "The selected title's executable is not present under data_dir. "
            "Choose the correct game-data folder on Files & logging.",
            "Game data not found", MB_OK | MB_ICONWARNING);
        return 0;
    }
    s_result = !_stricmp(title, "hydro") ? LAUNCHER_HYDRO : LAUNCHER_OFFROAD;
    DestroyWindow(s_window);
    return 1;
}

static void choose_initial_probe(void)
{
    const char *provider = model_value("analog", "provider");
    const char *name = model_value("analog", "dinput_name");
    const char *di_index = model_value("analog", "dinput_device");
    unsigned configured_index = !_stricmp(provider, "xinput") ?
        model_uint("analog", "xinput_device", 0) :
        !_stricmp(provider, "winmm") ? model_uint("analog", "winmm_device", 0) :
        model_uint("analog", "dinput_device", 0);
    int best = -1;
    unsigned i;

    if (!_stricmp(provider, "none"))
        return;
    for (i = 0; i < input_probe_count(); i++) {
        const input_probe_device_t *d = input_probe_device(i);
        int matches = !_stricmp(provider, "auto") ||
            (!_stricmp(provider, "dinput") && d->backend == INPUT_PROBE_DINPUT) ||
            (!_stricmp(provider, "xinput") && d->backend == INPUT_PROBE_XINPUT) ||
            (!_stricmp(provider, "winmm") && d->backend == INPUT_PROBE_WINMM);
        if (!matches)
            continue;
        if (d->backend == INPUT_PROBE_DINPUT && *name &&
            !contains_nocase(d->name, name))
            continue;
        if (_stricmp(provider, "auto")) {
            if (d->backend == INPUT_PROBE_DINPUT && !*name &&
                _stricmp(di_index, "auto") &&
                d->backend_index != configured_index)
                continue;
            if (d->backend != INPUT_PROBE_DINPUT &&
                d->backend_index != configured_index)
                continue;
        }
        if (best < 0)
            best = (int)i;
        if (!_stricmp(provider, "auto") && d->backend == INPUT_PROBE_DINPUT &&
            d->force_feedback) {
            best = (int)i;
            break;
        }
    }
    if (best < 0 && !_stricmp(provider, "auto") && input_probe_count())
        best = 0;
    if (best >= 0 && input_probe_select((unsigned)best))
        s_probe_selected = best;
}

static void refresh_input_probe(int rerender)
{
    s_capture_kind = 0;
    s_probe_selected = -1;
    input_probe_start(s_window);
    choose_initial_probe();
    set_status(input_probe_count() ? "Controller list refreshed." :
                                     "No controller detected.");
    if (rerender && (s_page_index == PAGE_DEVICES ||
                     s_page_index == PAGE_BINDINGS)) {
        render_page();
        layout_window();
    }
}

static void handle_command(WPARAM wparam, LPARAM lparam)
{
    int id = LOWORD(wparam), notify = HIWORD(wparam);

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
            s_capture_kind = 0;
            render_page();
            layout_window();
        }
        return;
    }
    if ((id == IDC_CONTROL_BINDINGS || id == IDC_CONTROL_DEVICES ||
         id == IDC_CONTROL_FFB) && notify == BN_CLICKED) {
        int page = id == IDC_CONTROL_BINDINGS ? PAGE_BINDINGS :
                   id == IDC_CONTROL_DEVICES ? PAGE_DEVICES : PAGE_FFB;
        pull_page();
        if (page != s_page_index) {
            s_page_index = page;
            s_capture_kind = 0;
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
        s_capture_kind = 0;
        render_page();
        layout_window();
        return;
    }
    if (id == IDC_PROBE_DEVICE && notify == CBN_SELCHANGE) {
        select_probe_from_combo((HWND)lparam, 1);
        return;
    }
    if (id == IDC_PROBE_REFRESH && notify == BN_CLICKED) {
        refresh_input_probe(1);
        return;
    }
    if (id == IDC_FFB_TEST && notify == BN_CLICKED) {
        if (input_probe_test_force())
            set_status("Force-feedback test running: brrr...");
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
        begin_axis_capture((unsigned)(id - IDC_DETECT_BASE));
        return;
    }
    if (id >= IDC_KEY_BIND_BASE && id < IDC_KEY_BIND_BASE + (int)ACTION_COUNT &&
        notify == BN_CLICKED) {
        begin_key_capture((unsigned)(id - IDC_KEY_BIND_BASE));
        return;
    }
    if (id >= IDC_PAD_BIND_BASE && id < IDC_PAD_BIND_BASE + (int)ACTION_COUNT &&
        notify == BN_CLICKED) {
        begin_pad_capture((unsigned)(id - IDC_PAD_BIND_BASE));
        return;
    }
    if (id >= IDC_SETTING_BASE && id < IDC_SETTING_BASE + (int)SETTING_COUNT &&
        (notify == EN_CHANGE || notify == CBN_SELCHANGE || notify == BN_CLICKED)) {
        pull_page();
        mark_dirty();
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
        LRESULT colour = control_colour(h, message, wparam, lparam);
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
        return 0;
    case WM_APP + 2:
        /* Rebuild responsive children only after the top-level WM_SIZE has
         * returned.  During a live mode switch Windows can still expose the
         * previous child client width from inside WM_SIZE, which placed
         * compact controls at their former wide coordinates. */
        s_relayout_pending = 0;
        render_page();
        layout_window();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (s_capture_kind == 1) {
            handle_key_capture(wparam, lparam);
            return 0;
        }
        if (s_capture_kind == 2 && wparam == VK_BACK) {
            snprintf(s_pad_binding[s_binding_title][s_capture_action],
                     sizeof s_pad_binding[0][0], "0");
            s_capture_kind = 0;
            mark_dirty();
            update_binding_display((unsigned)s_capture_action, 1);
            set_status("Controller binding cleared.");
            return 0;
        }
        if (wparam == VK_ESCAPE && s_capture_kind) {
            s_capture_kind = 0;
            set_status("Input capture cancelled.");
            return 0;
        }
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

static HFONT create_message_font(void)
{
    NONCLIENTMETRICSA metrics;
    memset(&metrics, 0, sizeof metrics);
    metrics.cbSize = sizeof metrics;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof metrics,
                              &metrics, 0)) {
        if (s_system_dpi != s_dpi) {
            metrics.lfMessageFont.lfHeight = MulDiv(
                metrics.lfMessageFont.lfHeight, (int)s_dpi,
                (int)s_system_dpi);
            metrics.lfMessageFont.lfWidth = MulDiv(
                metrics.lfMessageFont.lfWidth, (int)s_dpi,
                (int)s_system_dpi);
        }
        return CreateFontIndirectA(&metrics.lfMessageFont);
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* The message font, bold, at numerator/denominator of its height: the Play
 * button's face, and the two the Launch page's title lines use. One factory,
 * because the third caller was going to copy the second one. */
static HFONT create_bold_font(int numerator, int denominator)
{
    LOGFONTA font;

    if (!s_font || GetObjectA(s_font, sizeof font, &font) != sizeof font)
        return NULL;
    font.lfHeight = font.lfHeight * numerator / denominator;
    font.lfWidth = 0;
    font.lfWeight = FW_BOLD;
    return CreateFontIndirectA(&font);
}

static void enable_dpi_awareness(void)
{
    typedef BOOL (WINAPI *set_dpi_context_fn)(HANDLE);
    set_dpi_context_fn fn = (set_dpi_context_fn)(uintptr_t)
        GetProcAddress(GetModuleHandleA("user32.dll"),
                       "SetProcessDpiAwarenessContext");
    if (fn)
        fn((HANDLE)-4);             /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
}

static UINT system_dpi(void)
{
    typedef UINT (WINAPI *get_dpi_fn)(void);
    get_dpi_fn fn = (get_dpi_fn)(uintptr_t)
        GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForSystem");
    HDC screen;
    UINT dpi;

    if (fn)
        return fn();
    screen = GetDC(NULL);
    dpi = screen ? (UINT)GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen)
        ReleaseDC(NULL, screen);
    return dpi ? dpi : 96;
}

launcher_result_t launcher_run(const char *ini_path)
{
    WNDCLASSA wc;
    INITCOMMONCONTROLSEX controls;
    MSG message;
    HICON icon;
    MONITORINFO monitor;
    POINT cursor;
    HMONITOR display;
    RECT work;
    int width, height, x, y;

    s_result = LAUNCHER_CANCEL;
    s_dirty = s_loading = 0;
    s_page_index = PAGE_HOME;
    s_binding_title = 0;
    s_probe_selected = -1;
    s_capture_kind = 0;
    if (!ini_doc_load(&s_doc, ini_path)) {
        MessageBoxA(NULL, "vcthunder.ini could not be read.",
                    "VCThunder", MB_OK | MB_ICONERROR);
        return LAUNCHER_CANCEL;
    }
    load_settings();
    detect_external_overrides();

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)&launcher_run, &s_instance);
    enable_dpi_awareness();
    s_system_dpi = system_dpi();
    GetCursorPos(&cursor);
    display = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    memset(&monitor, 0, sizeof monitor);
    monitor.cbSize = sizeof monitor;
    if (GetMonitorInfoA(display, &monitor))
        work = monitor.rcWork;
    else
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    s_dpi = s_system_dpi;
    s_dpi = min(s_dpi, (UINT)MulDiv(work.right - work.left, 96, 960));
    s_dpi = min(s_dpi, (UINT)MulDiv(work.bottom - work.top, 96, 680));
    s_dpi = max(s_dpi, 96u);
    initialise_palette();
    memset(&controls, 0, sizeof controls);
    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);
    s_font = create_message_font();
    s_play_font = create_bold_font(2, 1);
    s_title_font = create_bold_font(3, 2);
    /* A 320x240 desktop has no room for a larger face; it still gets bold. */
    s_title_small_font = create_bold_font(1, 1);
    icon = LoadIconA(s_instance, MAKEINTRESOURCEA(IDI_VCTHUNDER));

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = s_instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = s_page_brush;
    wc.lpszClassName = "VCThunderLauncherPage";
    if (!RegisterClassA(&wc))
        goto done;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = card_proc;
    wc.hInstance = s_instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = s_page_brush;
    wc.lpszClassName = "VCThunderLauncherCard";
    if (!RegisterClassA(&wc))
        goto unregister_page;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = launcher_proc;
    wc.hInstance = s_instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = icon;
    wc.hbrBackground = s_main_brush;
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
        x, y, width, height, NULL, NULL, s_instance, NULL);
    if (!s_window)
        goto unregister_main;
    enable_dark_title_bar(s_window);
    SendMessage(s_window, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(s_window, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    ShowWindow(s_window, SW_SHOW);
    UpdateWindow(s_window);
    PostMessage(s_window, WM_APP + 1, 0, 0);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        int capture_key = s_capture_kind == 1 &&
            (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN);
        if (capture_key || !s_window || !IsDialogMessageA(s_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }

unregister_main:
    UnregisterClassA("VCThunderLauncher", s_instance);
unregister_card:
    UnregisterClassA("VCThunderLauncherCard", s_instance);
unregister_page:
    UnregisterClassA("VCThunderLauncherPage", s_instance);
done:
    input_probe_stop();
    if (s_font && s_font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(s_font);
    if (s_play_font)
        DeleteObject(s_play_font);
    if (s_title_font)
        DeleteObject(s_title_font);
    if (s_title_small_font)
        DeleteObject(s_title_small_font);
    s_font = NULL;
    s_play_font = NULL;
    s_title_font = NULL;
    s_title_small_font = NULL;
    release_palette();
    ini_doc_free(&s_doc);
    return s_result;
}
