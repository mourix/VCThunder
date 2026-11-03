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
#include <stdio.h>
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

static COLORREF game_face(theme_button_t kind);

COLORREF theme_game_colour(theme_button_t kind)
{
    return game_face(kind);
}

/* A game's own colour: its Play button's face. */
static COLORREF game_face(theme_button_t kind)
{
    switch (kind) {
    case THEME_BUTTON_HYDRO:   return RGB(0, 134, 165);
    case THEME_BUTTON_OFFROAD: return RGB(189, 12, 16);
    case THEME_BUTTON_ARCTIC:  return RGB(214, 51, 132);
    case THEME_BUTTON_UARCTIC: return RGB(96, 64, 176);
    default:                   return s_control_color;
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
                                                game_face(kind);
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
                                                game_face(kind);
    } else if (!(draw->itemState & ODS_DISABLED) &&
               kind == THEME_BUTTON_ARCTIC) {
        face = draw->itemState & ODS_SELECTED ? RGB(171, 41, 106) :
                                                game_face(kind);
    } else if (!(draw->itemState & ODS_DISABLED) &&
               kind == THEME_BUTTON_UARCTIC) {
        face = draw->itemState & ODS_SELECTED ? RGB(77, 51, 141) :
                                                game_face(kind);
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
    /* A Play button's edge is THEME_EDGE thick, inside its rectangle; a
     * dotted pen is one pixel or it is not dotted. */
    if (kind == THEME_BUTTON_EMPTY && !(draw->itemState & ODS_FOCUS))
        pen = CreatePen(PS_DOT, 1, border);
    else
        pen = CreatePen(PS_INSIDEFRAME,
                        game_face(kind) != s_control_color ?
                            max(1, scale_px(THEME_EDGE)) : 1, border);
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

/* ONE LINE OF TEXT IN AN OWNER-DRAWN STATIC, on the page's background, in
 * wide characters: a size's multiplication sign is not in every ANSI code
 * page. A caption's grey, as control_colour() gives one, and amber for a
 * choice that did not hold; High Contrast keeps the system's colours. */
void theme_draw_line(const DRAWITEMSTRUCT *draw, const wchar_t *first,
                     const wchar_t *second, int second_x, int warn)
{
    COLORREF ink = s_dark_mode ? (warn ? RGB(240, 185, 70) : RGB(176, 176, 180)) :
                                 GetSysColor(COLOR_WINDOWTEXT);
    UINT format = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX |
                  DT_END_ELLIPSIS;
    HGDIOBJ old_font = SelectObject(draw->hDC, s_font);
    RECT rc = draw->rcItem;

    FillRect(draw->hDC, &rc, s_dark_mode ? s_page_brush :
                                           GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ink);
    DrawTextW(draw->hDC, first, -1, &rc, format);
    rc.left += second_x;
    DrawTextW(draw->hDC, second, -1, &rc, format);
    SelectObject(draw->hDC, old_font);
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

/* ---- the parts Windows would draw at the monitor's scale -----------------
 *
 * These controls are Common Controls 5 (the DLL carries no manifest), which
 * has no custom draw for a button and draws a dropdown's arrow and a
 * checkbox's box at the system metrics of the monitor. So both are painted
 * whole here, from the control's own state, and repainted after every message
 * that lets the control draw on its own. */
static WNDPROC s_mark_proc, s_combo_proc;

static COLORREF accent(void)
{
    return s_dark_mode ? RGB(0, 120, 215) : GetSysColor(COLOR_HIGHLIGHT);
}

static void fill(HDC dc, const RECT *r, COLORREF colour)
{
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(dc, r, brush);
    DeleteObject(brush);
}

static int mark_type(HWND h)
{
    LONG_PTR style = GetWindowLongPtrA(h, GWL_STYLE);
    return (int)(style & BS_TYPEMASK);
}

static int is_radio(HWND h)
{
    int type = mark_type(h);
    return type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
}

static void paint_mark(HWND h, HDC dc)
{
    LONG_PTR style = GetWindowLongPtrA(h, GWL_STYLE);
    int enabled = IsWindowEnabled(h), radio = is_radio(h);
    int on = SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int mark = scale_px(THEME_CHECK_MARK), text_x = scale_px(THEME_CHECK_TEXT_X);
    int line, block, top, stroke = max(1, scale_px(2));
    UINT format = DT_NOPREFIX | DT_LEFT |
                  ((style & BS_MULTILINE) ? DT_WORDBREAK :
                                            DT_SINGLELINE | DT_END_ELLIPSIS);
    HBRUSH back = (HBRUSH)SendMessage(GetParent(h), WM_CTLCOLORSTATIC,
                                      (WPARAM)dc, (LPARAM)h);
    COLORREF ink = GetTextColor(dc), edge, face;
    HFONT font = (HFONT)SendMessage(h, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = SelectObject(dc, font ? font : s_font);
    RECT rc, text_rc, box;
    TEXTMETRICA metrics;
    char text[512];

    GetClientRect(h, &rc);
    FillRect(dc, &rc, back ? back : GetSysColorBrush(COLOR_BTNFACE));
    GetWindowTextA(h, text, sizeof text);
    GetTextMetricsA(dc, &metrics);
    line = metrics.tmHeight;
    SetRect(&text_rc, text_x, 0, rc.right, rc.bottom);
    block = line;
    if (text[0]) {
        RECT measure = text_rc;
        DrawTextA(dc, text, -1, &measure, format | DT_CALCRECT);
        block = max(line, (int)(measure.bottom - measure.top));
    }
    /* The text block centred, and the mark on its first line. */
    top = max(0, ((int)rc.bottom - block) / 2);
    text_rc.top = top;
    SetRect(&box, 0, top + (line - mark) / 2, mark, top + (line - mark) / 2 + mark);
    if (!enabled)
        ink = s_dark_mode ? RGB(125, 125, 128) : GetSysColor(COLOR_GRAYTEXT);
    edge = !enabled ? ink : on ? accent() :
           s_dark_mode ? RGB(160, 160, 164) : GetSysColor(COLOR_WINDOWTEXT);
    face = s_dark_mode ? s_control_color : GetSysColor(COLOR_WINDOW);
    {
        HBRUSH brush = CreateSolidBrush(on && !radio ? edge : face);
        HPEN pen = CreatePen(PS_SOLID, max(1, scale_px(1)), edge);
        HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);

        if (radio)
            Ellipse(dc, box.left, box.top, box.right, box.bottom);
        else
            Rectangle(dc, box.left, box.top, box.right, box.bottom);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }
    if (on && radio) {
        RECT dot = box;
        HBRUSH brush = CreateSolidBrush(edge);
        HGDIOBJ old_brush = SelectObject(dc, brush),
                old_pen = SelectObject(dc, GetStockObject(NULL_PEN));

        InflateRect(&dot, -mark / 4, -mark / 4);
        Ellipse(dc, dot.left, dot.top, dot.right + 1, dot.bottom + 1);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(brush);
    } else if (on) {
        POINT tick[3];
        HPEN pen = CreatePen(PS_SOLID, stroke, s_dark_mode ? RGB(255, 255, 255) :
                             GetSysColor(COLOR_HIGHLIGHTTEXT));
        HGDIOBJ old_pen = SelectObject(dc, pen);

        tick[0].x = box.left + mark * 22 / 100; tick[0].y = box.top + mark * 52 / 100;
        tick[1].x = box.left + mark * 42 / 100; tick[1].y = box.top + mark * 72 / 100;
        tick[2].x = box.left + mark * 78 / 100; tick[2].y = box.top + mark * 30 / 100;
        Polyline(dc, tick, 3);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
    if (text[0]) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        DrawTextA(dc, text, -1, &text_rc, format);
        if (GetFocus() == h &&
            !(SendMessage(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RECT focus = text_rc;
            DrawTextA(dc, text, -1, &focus, format | DT_CALCRECT);
            InflateRect(&focus, scale_px(2), scale_px(1));
            DrawFocusRect(dc, &focus);
        }
    }
    SelectObject(dc, old_font);
}

/* After each of these the control may have drawn itself; paint it over. */
static int repaint_after(UINT message)
{
    switch (message) {
    case BM_SETCHECK: case BM_SETSTATE: case BM_CLICK:
    case CB_SETCURSEL: case CB_SELECTSTRING: case CB_SHOWDROPDOWN:
    case CB_ADDSTRING: case CB_DELETESTRING: case CB_RESETCONTENT:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_CHAR:
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_SETTEXT:
    case WM_SETFONT: case WM_UPDATEUISTATE: case WM_CAPTURECHANGED:
    case WM_MOUSEWHEEL: case WM_COMMAND:
        return 1;
    default:
        return 0;
    }
}

static LRESULT painted(HWND h, UINT message, WPARAM wparam, LPARAM lparam,
                       WNDPROC next, void (*paint)(HWND, HDC))
{
    LRESULT result;

    if (message == WM_ERASEBKGND)
        return TRUE;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps;
        HDC dc = message == WM_PAINT ? BeginPaint(h, &ps) : (HDC)wparam;

        paint(h, dc);
        if (message == WM_PAINT)
            EndPaint(h, &ps);
        return 0;
    }
    result = CallWindowProcA(next, h, message, wparam, lparam);
    if (repaint_after(message) ||
        (message == WM_MOUSEMOVE && GetCapture() == h))
        InvalidateRect(h, NULL, FALSE);
    return result;
}

static LRESULT CALLBACK mark_proc(HWND h, UINT message, WPARAM wparam,
                                  LPARAM lparam)
{
    return painted(h, message, wparam, lparam, s_mark_proc, paint_mark);
}

static void paint_combo(HWND h, HDC dc)
{
    int enabled = IsWindowEnabled(h), focus = GetFocus() == h;
    COLORREF face = s_dark_mode ? (enabled ? s_control_color : RGB(38, 38, 40)) :
                    GetSysColor(enabled ? COLOR_WINDOW : COLOR_BTNFACE);
    COLORREF edge = focus ? accent() :
                    s_dark_mode ? RGB(100, 100, 104) : GetSysColor(COLOR_WINDOWFRAME);
    COLORREF ink = !enabled ?
        (s_dark_mode ? RGB(125, 125, 128) : GetSysColor(COLOR_GRAYTEXT)) :
        (s_dark_mode ? s_text_color : GetSysColor(COLOR_WINDOWTEXT));
    HFONT font = (HFONT)SendMessage(h, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = SelectObject(dc, font ? font : s_font);
    LRESULT sel = SendMessage(h, CB_GETCURSEL, 0, 0);
    int side, chevron = scale_px(10), cx, cy;
    char item[512] = "";
    RECT rc, text_rc;
    POINT arrow[3];

    GetClientRect(h, &rc);
    side = rc.bottom;
    fill(dc, &rc, face);
    {
        HBRUSH brush = CreateSolidBrush(edge);
        RECT frame = rc;
        int i;

        for (i = 0; i < max(1, scale_px(1)); i++) {
            FrameRect(dc, &frame, brush);
            InflateRect(&frame, -1, -1);
        }
        DeleteObject(brush);
    }
    if (sel != CB_ERR &&
        SendMessage(h, CB_GETLBTEXTLEN, (WPARAM)sel, 0) < (LRESULT)sizeof item)
        SendMessageA(h, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
    SetRect(&text_rc, scale_px(8), 0, rc.right - side, rc.bottom);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    DrawTextA(dc, item, -1, &text_rc,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    cx = rc.right - side / 2;
    cy = rc.bottom / 2;
    arrow[0].x = cx - chevron / 2; arrow[0].y = cy - chevron / 4;
    arrow[1].x = cx + chevron / 2; arrow[1].y = cy - chevron / 4;
    arrow[2].x = cx;               arrow[2].y = cy + chevron / 4;
    {
        HBRUSH brush = CreateSolidBrush(ink);
        HGDIOBJ old_brush = SelectObject(dc, brush),
                old_pen = SelectObject(dc, GetStockObject(NULL_PEN));

        Polygon(dc, arrow, 3);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(brush);
    }
    SelectObject(dc, old_font);
}

/* A CLOSED DROPDOWN UNDER THE POINTER TAKES THE WHEEL AND STEPS ITS CHOICE, so
 * scrolling a page over one would change a setting nobody touched. The wheel
 * goes to the page, as a slider's does; an open list keeps its own. */
static LRESULT CALLBACK combo_proc(HWND h, UINT message, WPARAM wparam,
                                   LPARAM lparam)
{
    if (message == WM_MOUSEWHEEL && !SendMessage(h, CB_GETDROPPEDSTATE, 0, 0))
        return SendMessage(GetParent(h), message, wparam, lparam);
    return painted(h, message, wparam, lparam, s_combo_proc, paint_combo);
}

int theme_draw_combo_item(const DRAWITEMSTRUCT *draw)
{
    char item[512] = "";
    RECT text_rc;
    int lit;

    if (!draw || draw->CtlType != ODT_COMBOBOX)
        return 0;
    /* The closed face is paint_combo()'s, whole. */
    if (draw->itemState & ODS_COMBOBOXEDIT) {
        InvalidateRect(draw->hwndItem, NULL, FALSE);
        return 1;
    }
    lit = (draw->itemState & ODS_SELECTED) != 0;
    fill(draw->hDC, &draw->rcItem,
         lit ? accent() : s_dark_mode ? s_control_color : GetSysColor(COLOR_WINDOW));
    if (draw->itemID == (UINT)-1)
        return 1;
    if (SendMessage(draw->hwndItem, CB_GETLBTEXTLEN, draw->itemID, 0) <
        (LRESULT)sizeof item)
        SendMessageA(draw->hwndItem, CB_GETLBTEXT, draw->itemID, (LPARAM)item);
    text_rc = draw->rcItem;
    text_rc.left += scale_px(8);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, lit ? (s_dark_mode ? RGB(255, 255, 255) :
                                   GetSysColor(COLOR_HIGHLIGHTTEXT)) :
                 s_dark_mode ? s_text_color : GetSysColor(COLOR_WINDOWTEXT));
    DrawTextA(draw->hDC, item, -1, &text_rc,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    return 1;
}

HWND make_control(DWORD ex, const char *class_name, const char *text,
                         DWORD style, int x, int y, int w, int h,
                         HWND parent, int id)
{
    int combo = !_stricmp(class_name, "COMBOBOX");
    int trackbar = !_stricmp(class_name, TRACKBAR_CLASSA);
    int mark = !_stricmp(class_name, "BUTTON") && !(style & BS_PUSHLIKE) &&
               ((style & BS_TYPEMASK) == BS_CHECKBOX ||
                (style & BS_TYPEMASK) == BS_AUTOCHECKBOX ||
                (style & BS_TYPEMASK) == BS_RADIOBUTTON ||
                (style & BS_TYPEMASK) == BS_AUTORADIOBUTTON);

    if (s_dark_mode && !_stricmp(class_name, "BUTTON") &&
        ((style & BS_TYPEMASK) == BS_PUSHBUTTON ||
         (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON))
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    /* Owner-drawn items, so a dropped list's rows are as tall as the control
     * that opened it. */
    if (combo)
        style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    if (trackbar)
        style |= TBS_FIXEDLENGTH;
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
    if (control && combo) {
        SendMessage(control, CB_SETITEMHEIGHT, (WPARAM)-1,
                    scale_px(THEME_CONTROL_H) - 6);
        SendMessage(control, CB_SETITEMHEIGHT, 0,
                    scale_px(THEME_CONTROL_H - 4));
        if (!s_combo_proc)
            s_combo_proc = (WNDPROC)GetWindowLongPtrA(control, GWLP_WNDPROC);
        SetWindowLongPtrA(control, GWLP_WNDPROC, (LONG_PTR)combo_proc);
    }
    if (control && mark) {
        if (!s_mark_proc)
            s_mark_proc = (WNDPROC)GetWindowLongPtrA(control, GWLP_WNDPROC);
        SetWindowLongPtrA(control, GWLP_WNDPROC, (LONG_PTR)mark_proc);
    }
    if (control && trackbar)
        SendMessage(control, TBM_SETTHUMBLENGTH,
                    (WPARAM)scale_px(THEME_CONTROL_H * 6 / 10), 0);
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

/* A DPI THAT DOES NOT LEAVE ROOM FOR THE TWO-COLUMN FORM IS CAPPED rather than
 * obeyed: that form begins at a 960x680 window, and a faithful 200% on a
 * 1366x768 laptop is a window whose Apply button is off the bottom of the
 * screen. */
static UINT fit_work(UINT dpi, const RECT *work)
{
    dpi = min(dpi, (UINT)MulDiv(work->right - work->left, 96, 960));
    dpi = min(dpi, (UINT)MulDiv(work->bottom - work->top, 96, 680));
    return max(dpi, 96u);
}

/* Every face is the message font at a size and weight, so all six are made
 * together and replaced together. */
#define THEME_FONTS 6

static void fonts_get(HFONT out[THEME_FONTS])
{
    out[0] = s_font;
    out[1] = s_play_font;
    out[2] = s_title_font;
    out[3] = s_title_small_font;
    out[4] = s_note_font;
    out[5] = s_heading_font;
}

static void fonts_make(void)
{
    s_font = create_message_font();
    s_play_font = create_bold_font(2, 1);
    s_title_font = create_bold_font(3, 2);
    /* A 320x240 desktop has no room for a larger face; it still gets bold. */
    s_title_small_font = create_bold_font(1, 1);
    /* Heavier than the prose around it, lighter than a heading. */
    s_note_font = create_weighted_font(1, 1, FW_SEMIBOLD);
    s_heading_font = create_bold_font(1, 1);
}

static void fonts_delete(HFONT font[THEME_FONTS])
{
    int i;

    for (i = 0; i < THEME_FONTS; i++)
        if (font[i] && font[i] != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(font[i]);
}

void theme_open(HINSTANCE instance, const RECT *work)
{
    s_instance = instance;
    s_system_dpi = system_dpi();
    s_dpi = fit_work(s_system_dpi, work);
    initialise_palette();
    fonts_make();
}

UINT theme_floor_dpi(HWND h)
{
    typedef UINT (WINAPI *window_dpi_fn)(HWND);
    window_dpi_fn fn = (window_dpi_fn)(uintptr_t)
        GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    UINT dpi = fn ? fn(h) : 0;
    MONITORINFO monitor;

    if (!dpi)
        dpi = s_system_dpi;
    memset(&monitor, 0, sizeof monitor);
    monitor.cbSize = sizeof monitor;
    if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST),
                         &monitor))
        return max(dpi, 96u);
    return fit_work(dpi, &monitor.rcWork);
}

struct font_swap {
    HFONT from[THEME_FONTS], to[THEME_FONTS];
};

static BOOL CALLBACK swap_font(HWND h, LPARAM data)
{
    const struct font_swap *swap = (const struct font_swap *)data;
    HFONT font = (HFONT)SendMessage(h, WM_GETFONT, 0, 0);
    int i;

    for (i = 0; i < THEME_FONTS; i++)
        if (font && font == swap->from[i]) {
            SendMessage(h, WM_SETFONT, (WPARAM)swap->to[i], FALSE);
            break;
        }
    return TRUE;
}

void theme_set_dpi(UINT dpi, HWND top)
{
    struct font_swap swap;

    if (!dpi || dpi == s_dpi)
        return;
    fonts_get(swap.from);
    s_dpi = dpi;
    fonts_make();
    fonts_get(swap.to);
    if (top)
        EnumChildWindows(top, swap_font, (LPARAM)&swap);
    fonts_delete(swap.from);
}

void theme_close(void)
{
    HFONT font[THEME_FONTS];

    fonts_get(font);
    fonts_delete(font);
    s_heading_font = NULL;
    s_font = NULL;
    s_play_font = NULL;
    s_title_font = NULL;
    s_title_small_font = NULL;
    s_note_font = NULL;
    release_palette();
}
