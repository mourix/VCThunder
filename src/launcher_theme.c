/* launcher_theme.c -- the launcher's palette, fonts, DPI and owner-draw.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See src/launcher_theme.h for the line this sits behind. Everything here was
 * src/launcher.c's and is unchanged except where it asked what a button was:
 * draw_dark_button() and control_colour() ask the window now, because the
 * answer is about a page and this file is not.
 */
#include "launcher_theme.h"

#include <commctrl.h>
#include <limits.h>
#include <string.h>

static HINSTANCE s_instance;
static HFONT s_font, s_play_font, s_title_font, s_title_small_font, s_note_font;
static HFONT s_heading_font;
static HBRUSH s_main_brush, s_page_brush, s_control_brush, s_border_brush;
static COLORREF s_main_color, s_page_color, s_control_color, s_text_color;
static UINT s_dpi = 96;
static UINT s_system_dpi = 96;
static int s_dark_mode;

HINSTANCE theme_instance(void) { return s_instance; }
UINT   theme_dpi(void)               { return s_dpi; }
int    theme_dark(void)              { return s_dark_mode; }
HFONT  theme_font(void)              { return s_font; }
HFONT  theme_play_font(void)         { return s_play_font; }
HFONT  theme_title_font(void)        { return s_title_font; }
HFONT  theme_title_small_font(void)  { return s_title_small_font; }
HFONT  theme_note_font(void)         { return s_note_font ? s_note_font : s_font; }
HFONT  theme_heading_font(void)      { return s_heading_font ? s_heading_font : s_font; }
HBRUSH theme_main_brush(void)        { return s_main_brush; }
HBRUSH theme_page_brush(void)        { return s_page_brush; }
HBRUSH theme_border_brush(void)      { return s_border_brush; }


int scale_px(int value)
{
    return MulDiv(value, (int)s_dpi, 96);
}

int unscale_px(int value)
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

void enable_dark_title_bar(HWND h)
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

void theme_control(HWND h)
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

int draw_dark_button(const DRAWITEMSTRUCT *draw)
{
    RECT rc;
    char text[128];
    COLORREF face, border, text_colour;
    HBRUSH brush;
    HPEN pen, old_pen;
    HGDIOBJ old_brush, old_font = NULL;
    theme_button_t kind;
    int tab;

    if (!s_dark_mode || !draw || draw->CtlType != ODT_BUTTON)
        return 0;
    rc = draw->rcItem;
    kind = launcher_button_kind((int)draw->CtlID);
    /* 1 lit, 0 unlit, -1 not a tab: the shape the rest of this was written
     * against, and the one thing about a tab that changes what is drawn. */
    tab = kind == THEME_BUTTON_TAB_LIT ? 1 : kind == THEME_BUTTON_TAB ? 0 : -1;
    if (tab >= 0) {
        /* A TAB ROW HAS TO READ AS A ROW: unselected tabs are the page
         * itself carrying a label, so the one lit box is the whole answer to
         * "where am I". ODS_SELECTED is the mouse holding an unselected tab
         * down, not the subpage. */
        face = tab ? RGB(48, 48, 52) :
               draw->itemState & ODS_SELECTED ? RGB(38, 38, 40) : s_page_color;
    } else if (!(draw->itemState & ODS_DISABLED) &&
        kind == THEME_BUTTON_HYDRO) {
        face = draw->itemState & ODS_SELECTED ? RGB(0, 107, 132) :
                                                RGB(0, 134, 165);
    } else if (kind == THEME_BUTTON_EMPTY) {
        /* AN OUTLINE, NOT A FACE: a filled box reads as a button that does
         * something, and an empty slot is a place to put something. */
        face = draw->itemState & ODS_SELECTED ? RGB(38, 38, 40) :
                                                s_page_color;
    } else if (!(draw->itemState & ODS_DISABLED) &&
               kind == THEME_BUTTON_LIT) {
        face = draw->itemState & ODS_SELECTED ? RGB(0, 84, 150) :
                                                RGB(0, 100, 180);
    } else if (!(draw->itemState & ODS_DISABLED) &&
               kind == THEME_BUTTON_OFFROAD) {
        face = draw->itemState & ODS_SELECTED ? RGB(151, 10, 13) :
                                                RGB(189, 12, 16);
    } else {
        face = draw->itemState & ODS_DISABLED ? RGB(38, 38, 40) :
               draw->itemState & ODS_SELECTED ? RGB(58, 58, 62) :
                                                RGB(48, 48, 52);
    }
    /* An unselected tab keeps a thin edge of its own: a bare label on the page
     * background reads as a caption nobody can click, and this row is the one
     * thing on the page that is not a setting. */
    border = draw->itemState & ODS_FOCUS ? RGB(0, 120, 215) :
             tab == 0 ? RGB(72, 72, 76) : RGB(100, 100, 104);
    text_colour = tab >= 0 ? (tab ? s_text_color : RGB(198, 198, 202)) :
                  draw->itemState & ODS_DISABLED ? RGB(125, 125, 128) :
                  kind == THEME_BUTTON_EMPTY ? RGB(135, 135, 140) :
                                               s_text_color;
    brush = CreateSolidBrush(face);
    pen = CreatePen(kind == THEME_BUTTON_EMPTY &&
                    !(draw->itemState & ODS_FOCUS) ? PS_DOT : PS_SOLID,
                    1, border);
    old_brush = SelectObject(draw->hDC, brush);
    old_pen = SelectObject(draw->hDC, pen);
    /* A dotted pen paints its gaps in the background colour. */
    SetBkColor(draw->hDC, face);
    Rectangle(draw->hDC, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(draw->hDC, old_pen);
    SelectObject(draw->hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    if (tab == 1) {
        /* THE BAR IS WHAT SAYS "YOU ARE HERE"; the lit fill alone is one
         * shade off the unlit one at a glance. After the fill, so it replaces
         * the bottom border rather than sitting above it. */
        RECT bar = rc;

        bar.left += 1;
        bar.right -= 1;
        bar.bottom -= 1;
        bar.top = bar.bottom - scale_px(3);
        brush = CreateSolidBrush(RGB(0, 120, 215));
        FillRect(draw->hDC, &bar, brush);
        DeleteObject(brush);
        rc.bottom -= scale_px(3);
    }
    GetWindowTextA(draw->hwndItem, text, sizeof text);
    {
        HFONT font = (HFONT)SendMessage(draw->hwndItem, WM_GETFONT, 0, 0);
        if (font)
            old_font = SelectObject(draw->hDC, font);
    }
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, text_colour);
    if ((draw->itemState & ODS_SELECTED) && tab < 0)
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

LRESULT control_colour(int on_page, UINT message, WPARAM wparam,
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
    } else if (on_page) {
        background = s_page_color;
        brush = s_page_brush;
    } else {
        background = s_main_color;
        brush = s_main_brush;
    }
    SetBkColor(dc, background);
    /* A caption is subordinate to the label above it. Only here: High Contrast
     * returned above, having handed its colours back to the system, which is
     * the one theme where inventing a second grey would be wrong. The same
     * goes for the tones. */
    switch (launcher_control_tone(GetDlgCtrlID((HWND)lparam))) {
    case THEME_TONE_RED:   SetTextColor(dc, RGB(255, 110, 110)); break;
    case THEME_TONE_GREEN: SetTextColor(dc, RGB(110, 215, 125)); break;
    case THEME_TONE_AMBER: SetTextColor(dc, RGB(240, 185, 70));  break;
    case THEME_TONE_BLUE:  SetTextColor(dc, RGB(115, 175, 255)); break;
    /* Dimmer than a caption, so a greyed label and its help stay apart. */
    case THEME_TONE_DIM:   SetTextColor(dc, RGB(120, 120, 124)); break;
    default:
        SetTextColor(dc,
                     launcher_control_is_caption(GetDlgCtrlID((HWND)lparam)) ?
                     RGB(176, 176, 180) : s_text_color);
        break;
    }
    return (LRESULT)brush;
}

void set_font(HWND h)
{
    if (h && s_font)
        SendMessage(h, WM_SETFONT, (WPARAM)s_font, TRUE);
}

HWND make_control(DWORD ex, const char *class_name, const char *text,
                         DWORD style, int x, int y, int w, int h,
                         HWND parent, int id)
{
    if (s_dark_mode && !_stricmp(class_name, "BUTTON") &&
        ((style & BS_TYPEMASK) == BS_PUSHBUTTON ||
         (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON))
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    /* A STATIC draws `&` as an underline under the next letter, and nothing
     * here is a mnemonic: a page name ("Files & Logging"), a device name or a
     * path would lose a character. text_block_height() measures DT_NOPREFIX,
     * so this is also what makes the measure and the drawing agree. */
    if (!_stricmp(class_name, "STATIC"))
        style |= SS_NOPREFIX;
    HWND control = CreateWindowExA(ex, class_name, text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
        scale_px(x), scale_px(y),
        scale_px(w), scale_px(h), parent, (HMENU)(INT_PTR)id,
        s_instance, NULL);
    set_font(control);
    theme_control(control);
    return control;
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

/* The message font at numerator/denominator of its height, at `weight`: the
 * Play button's face, the Launch page's two titles and the live-bar notes. */
static HFONT create_weighted_font(int numerator, int denominator, int weight)
{
    LOGFONTA font;

    if (!s_font || GetObjectA(s_font, sizeof font, &font) != sizeof font)
        return NULL;
    font.lfHeight = font.lfHeight * numerator / denominator;
    font.lfWidth = 0;
    font.lfWeight = weight;
    return CreateFontIndirectA(&font);
}

static HFONT create_bold_font(int numerator, int denominator)
{
    return create_weighted_font(numerator, denominator, FW_BOLD);
}

void theme_enable_dpi_awareness(void)
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

/* THE SCALE IS CHOSEN ONCE, AGAINST THE DESKTOP IT HAS TO FIT. The two-column
 * form begins at a 960x680 window, so a system DPI that would not leave room
 * for one is capped rather than obeyed: a faithful 200% on a 1366x768 laptop
 * is a window whose Apply button is off the bottom of the screen. */
void theme_open(HINSTANCE instance, const RECT *work)
{
    s_instance = instance;
    s_system_dpi = system_dpi();
    s_dpi = s_system_dpi;
    s_dpi = min(s_dpi, (UINT)MulDiv(work->right - work->left, 96, 960));
    s_dpi = min(s_dpi, (UINT)MulDiv(work->bottom - work->top, 96, 680));
    s_dpi = max(s_dpi, 96u);
    initialise_palette();
    s_font = create_message_font();
    s_play_font = create_bold_font(2, 1);
    s_title_font = create_bold_font(3, 2);
    /* A 320x240 desktop has no room for a larger face; it still gets bold. */
    s_title_small_font = create_bold_font(1, 1);
    /* Heavier than the prose around it, lighter than a heading. */
    s_note_font = create_weighted_font(1, 1, FW_SEMIBOLD);
    s_heading_font = create_bold_font(1, 1);
}

void theme_close(void)
{
    if (s_font && s_font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(s_font);
    if (s_play_font)
        DeleteObject(s_play_font);
    if (s_title_font)
        DeleteObject(s_title_font);
    if (s_title_small_font)
        DeleteObject(s_title_small_font);
    if (s_note_font)
        DeleteObject(s_note_font);
    if (s_heading_font)
        DeleteObject(s_heading_font);
    s_heading_font = NULL;
    s_font = NULL;
    s_play_font = NULL;
    s_title_font = NULL;
    s_title_small_font = NULL;
    s_note_font = NULL;
    release_palette();
}
