/* launcher_theme.h -- the launcher's palette, fonts, DPI and owner-draw.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * THE ONLY PART OF THE WINDOW THAT IS ABOUT HOW SOMETHING LOOKS rather than
 * what it says: sixteen statics that every page reads and nothing but this file
 * writes. It knows about colour and NOT about pages, because a palette that
 * decided for itself which button is a tab would have to be edited when a page
 * is added; those questions are asked back through the two functions at the
 * bottom.
 */
#ifndef VCT_LAUNCHER_THEME_H
#define VCT_LAUNCHER_THEME_H

#include "vcthunder.h"

/* ---- opening and closing ------------------------------------------------- */

/* Per-monitor DPI awareness, for the whole process. Before the work area is
 * measured, or it comes back in the scaled coordinates this is about to stop
 * using. */
void theme_enable_dpi_awareness(void);

/* The palette and the four fonts, at a DPI that fits `work`. */
void theme_open(HINSTANCE instance, const RECT *work);
void theme_close(void);

HINSTANCE theme_instance(void);
UINT theme_dpi(void);
int  theme_dark(void);

/* A geometry constant, into pixels, and back. */
/* A GEOMETRY CONSTANT GOES THROUGH THIS. The DPI is chosen once, when the
 * window opens, against the work area, because a scale that does not fit the
 * desktop is a window with its buttons off the bottom of it. It never changes
 * after, so a cached measurement taken at one DPI stays valid. */
int  scale_px(int value);
int  unscale_px(int value);

/* ---- what the pages draw with -------------------------------------------- */

HFONT  theme_font(void);                /* the message font                 */
HFONT  theme_play_font(void);           /* the two Play buttons' face       */
HFONT  theme_title_font(void);          /* the Launch page's title          */
HFONT  theme_title_small_font(void);    /* ...on a desktop with no room     */
HFONT  theme_note_font(void);           /* the lines under the live bars    */
HFONT  theme_heading_font(void);        /* every box's title, in capitals   */
HBRUSH theme_main_brush(void);          /* the frame                        */
HBRUSH theme_page_brush(void);          /* the scrolling page and its cards */
HBRUSH theme_border_brush(void);        /* a card's frame                   */

void enable_dark_title_bar(HWND h);
void theme_control(HWND h);
void set_font(HWND h);

/* EVERY CONTROL THIS WINDOW MAKES, in logical units and already themed. A
 * push button becomes owner-draw in dark mode, which is what draw_dark_button()
 * below then paints. */
HWND make_control(DWORD ex, const char *class_name, const char *text,
                  DWORD style, int x, int y, int w, int h,
                  HWND parent, int id);

/* 1 when it painted. WM_DRAWITEM, from both window procedures. */
int  draw_dark_button(const DRAWITEMSTRUCT *draw);

/* The WM_CTLCOLOR* family. `on_page` is whether the parent is the scrolling
 * page rather than the frame, which is the only thing about the caller this
 * needs and is something the caller already knows. 0 when the system's own
 * colours should stand, which is every High Contrast theme. */
LRESULT control_colour(int on_page, UINT message, WPARAM wparam, LPARAM lparam);

/* ---- ...and what the window owes it --------------------------------------
 *
 * Defined in src/launcher.c. Two questions, both about identity rather than
 * about colour: WHICH button this is, and whether this control is subordinate
 * to the one above it. */

typedef enum {
    THEME_BUTTON_PLAIN = 0,
    THEME_BUTTON_TAB,           /* one of a row, and not the one you are on */
    THEME_BUTTON_TAB_LIT,       /* ...and the one you are                   */
    THEME_BUTTON_HYDRO,         /* the two Play buttons, in the cabinets'   */
    THEME_BUTTON_OFFROAD,       /* own colours                              */
    THEME_BUTTON_LIT,           /* a binding slot held, or being captured   */
    THEME_BUTTON_EMPTY          /* a binding slot with nothing in it        */
} theme_button_t;

theme_button_t launcher_button_kind(int id);
int launcher_control_is_caption(int id);

/* A static's text colour; the window decides what each tone means. High
 * Contrast keeps the system's colours. */
typedef enum {
    THEME_TONE_NONE = 0,
    THEME_TONE_RED,             /* hold still                               */
    THEME_TONE_GREEN,           /* do it now                                */
    THEME_TONE_AMBER,           /* going on, or going again                 */
    THEME_TONE_BLUE,            /* done, finish it                          */
    THEME_TONE_DIM              /* a greyed settings row's label            */
} theme_tone_t;

theme_tone_t launcher_control_tone(int id);

#endif /* VCT_LAUNCHER_THEME_H */
