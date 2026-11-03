/* launcher_page.h -- what a page in a file of its own needs from the window.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The window is src/launcher.c. The Controls page (launcher_controls.c) and the
 * CHD import (launcher_import.c) build children on its page, so they share its
 * control ids and its layout, and they read its state through the calls below,
 * never through its statics. The first half is the window's; the other two
 * are what each file gives back.
 */
#ifndef VCT_LAUNCHER_PAGE_H
#define VCT_LAUNCHER_PAGE_H

#include <stddef.h>

#include "vcthunder.h"
#include "ini.h"
#include "launcher_theme.h"

/* ---- the window's control ids and metrics ------------------------------- */

enum {
    IDC_NAV = 100,
    IDC_STATUS,
    IDC_APPLY,
    IDC_CLOSE,
    IDC_PAGE,
    IDC_FOCUS_RING,
    IDC_PLAY_HYDRO = 200,
    IDC_PLAY_OFFROAD,
    IDC_PLAY_ARCTIC,
    IDC_PLAY_UARCTIC,
    /* The Controls page's two tabs: Controls and Force Feedback. */
    IDC_CONTROL_MAIN = 210,
    IDC_CONTROL_FFB,
    /* THE GAME ROW at the top of the Controls tab: which cabinet's buttons,
     * and while each game has its own controller, whose controller. */
    IDC_TITLE_HYDRO = 220,
    IDC_TITLE_OFFROAD,
    /* One controller for both games, or one each. */
    IDC_SEPARATE_CONTROLLER,
    IDC_TITLE_ARCTIC = 225,
    IDC_PROBE_REFRESH = 230,
    IDC_FFB_TEST,
    /* The three disclosures. Every one of them is a checkbox whose state is
     * this window's and is never written to the file: what a user chose to
     * LOOK at is not configuration. */
    IDC_SHOW_RAW,
    IDC_SHOW_ADVANCED,
    IDC_SHOW_LEGACY,
    IDC_SETUP_AXES,
    IDC_LIVE_STEER = 240,
    IDC_LIVE_THROTTLE,
    IDC_LIVE_BRAKE,
    /* The line under the three bars: the hint, the missing-axis sentence, or
     * the step Set up is asking for: directly under the thing it is about,
     * not on the status bar, where it goes unread. */
    IDC_LIVE_NOTE,
    /* The raw disclosure's read-out: which buttons are down, and the hat. */
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
    /* The CHD import: the button on Files & Logging, and the block the Launch
     * page shows while one runs. */
    IDC_IMPORT = 390,
    IDC_IMPORT_CANCEL,
    IDC_IMPORT_BAR,
    IDC_IMPORT_TEXT,
    IDC_RESTORE_DEFAULTS,
    IDC_ABOUT_LINK,
    IDC_SHOW_LOGS,
    /* The Graphics page's Bezel box, one of each per row (MODEL_BEZELS),
     * and Ultimate's way back to Arctic's art. */
    IDC_BEZEL_ENABLE = 400,
    IDC_BEZEL_FILE = 404,
    IDC_BEZEL_CHOOSE = 408,
    IDC_BEZEL_NOTE = 412,
    IDC_BEZEL_INHERIT = 416,
    IDC_SETTING_BASE = 1000,
    IDC_BROWSE_BASE = 2000,
    IDC_DETECT_BASE = 3000,
    /* TWO SLOTS PER ACTION, EACH ONE INPUT OF ANY KIND: a key, or a button on
     * any controller. Clicking a slot replaces that input and nothing else,
     * so a wrong press costs one press, and two buttons on one controller are
     * two slots. Slot `s` of action `a` is BASE + a * BINDING_SLOTS + s. */
    IDC_BIND_SLOT_BASE = 4000,
    /* Every row of the game shown back to its default, greyed while they all
     * already are. One for the card, not one per row. */
    IDC_BIND_RESET = 4100,
    /* A settings row's label, so a greyed row can dim it. */
    IDC_LABEL_BASE = 4500,
    /* The per-setting help caption. Its own range so control_colour() can mute
     * it without asking what a static's text says. */
    IDC_HELP_BASE = 5000,
    /* A slider's value, to its right, and the balance row's left end. */
    IDC_VALUE_BASE = 5500,
    IDC_FLANK_BASE = 6000,
    IDC_SUB_BASE = 6500,
    /* The sizes beside the Graphics page's two scale rows, SCALE_LINES
     * each, and the outline round each row's lines. */
    IDC_SCALE_LINE_BASE = 7000,
    IDC_SCALE_FRAME_BASE = 7100,
    /* The project's mark, under the page tiles. */
    IDC_LOGO = 7200
};

/* TOUCH-SIZED, FOR EVERYONE. Every control a person presses is CTRL_H tall,
 * whatever presses it: about 6 mm on a Steam Deck, where a finger wants 7.
 * Text beside a control is centred on it by LABEL_DY; on a compact page the
 * control sits STACK_DY under its label. */
#define CTRL_H   THEME_CONTROL_H
#define LABEL_DY ((CTRL_H - 20) / 2)
#define STACK_DY 25

/* ---- what the window owes its pages --------------------------------------
 *
 * Defined in src/launcher.c. */

/* The scrolling page every child is built on; NULL before the first. */
HWND launcher_page(void);

/* The disclosures the user ticked, which are never written to the file. */
int  launcher_show_raw(void);           /* every axis the controller sends */
int  launcher_show_legacy(void);        /* the WinMM rows too              */
/* Whether controller `index` moved in the last moment. */
int  launcher_device_moving(unsigned index);

/* Read the page into the model and save it if anything changed: 0 when the
 * save was refused or failed, and whatever asked should not go on. */
int  launcher_save_if_dirty(void);
void render_page(void);
void show_page(int page);

/* Layout, in logical units. */
int  page_width(void);
int  page_header_bottom(void);
int  text_block_height(HFONT font, const char *text, int width);
int  frame_topic(int top, int bottom);
int  group_header(int y, const char *text, int button_id,
                  const char *button_text, int button_w, int enabled);
int  tab_rows_width(void);
int  add_tab_row_at(int x, int y, int height, int total,
                    const char *const *labels, const int *ids,
                    unsigned count);
void quiet_radio(HWND radio);
void update_reset_button(void);

/* What is attached. */
unsigned probe_axis_rows(void);
const char *probe_provider(const input_probe_device_t *device);
int  probe_id_of(const input_probe_device_t *d, ini_device_id_t *ids,
                 unsigned *count);

/* ---- the Controls page: src/launcher_controls.c ------------------------- */

/* The hand-built panel above each page's settings rows; returns where the
 * rows start. */
int  render_controls_panel(void);
int  render_ffb_panel(void);

/* A controller row's second line, also rewritten as the device moves. */
void device_what(unsigned i, char *out, size_t out_size);
/* Set up's button text, which follows what pressing it would do. */
const char *setup_button_text(void);
/* Why the game would never drive this controller's motor, or NULL. */
const char *ffb_reaches_device(const input_probe_device_t *d);
/* The line under the live bars when nothing more urgent holds it. */
extern const char k_driving_hint[];

/* ---- importing a game from a CHD: src/launcher_import.c ----------------- */

#define IMPORT_TIMER 2

int  import_running(void);
void import_pick(void);                 /* the system's file dialog         */
int  import_beside_exe(void);           /* 1 when it started importing      */
/* As the window opens: the CHDs dropped on the exe, or those beside it. */
void import_open(void);
void import_cancel(void);
void import_tick(void);                 /* IMPORT_TIMER                     */
/* 0 when the user chose not to stop an import and close. */
int  import_stop_for_close(void);
/* The Launch page's import block; returns where the cards start. */
int  render_import_block(int y, int x, int w);

#endif /* VCT_LAUNCHER_PAGE_H */
