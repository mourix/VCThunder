/* launcher_capture.h -- the launcher's capture machine: slots, Detect, Set up.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * THREE FLOWS BEHIND ONE FLAG: the binding capture (a slot), the axis guard
 * (`Detect`) and Set up, which wraps the guard rather than being a second
 * instrument. Exactly one runs at a time, so every way out of any of them has to
 * be a way out of all three, and the fields that say where each one is sit
 * behind this boundary, not in the window. The note line comes with them: which
 * of two speakers outranks the other is only enforceable where both are.
 */
#ifndef VCT_LAUNCHER_CAPTURE_H
#define VCT_LAUNCHER_CAPTURE_H

#include <stddef.h>

#include "vcthunder.h"

/* ---- what the window calls ----------------------------------------------- */

/* No capture, wizard or axis detection in progress. */
void capture_reset(void);

/* IS ANYTHING RUNNING? The message loop asks, because a key pressed during a
 * capture must bypass IsDialogMessage() to reach WM_KEYDOWN at all. */
int  capture_active(void);

/* THE ONE DOOR OUT, from outside. Seven callers: a page change, either subpage
 * tab, Rescan, Detect, a slot and Reset to defaults. The flow's OWN transitions are
 * deliberately not routed through it; wizard_took(), wizard_failed() and
 * wizard_start_step() hand the capture on. */
void abandon_capture(void);

/* The whole WM_KEYDOWN branch: 1 when a capture consumed the key. Escape is in
 * here with the flag it is gated on, which is the half of the trap above that
 * a boundary alone would not have fixed. */
int  capture_key(WPARAM key, LPARAM detail);

/* One frame. `have_device` is whether the selected controller answered this
 * tick, because the axis guard must not measure a device that did not. */
void capture_tick_all(int have_device);

/* The four things the user can start. */
void begin_capture(unsigned action, unsigned slot); /* a binding row's slot */
void restore_all_bindings(void);           /* Reset to defaults, the card  */
void begin_axis_capture(unsigned setting);  /* Detect, on an axis row       */
void begin_wizard(void);                    /* Set up                       */
/* Set up's button while it runs: skip this step, leaving its row as it was.
 * 1 while Set up is running. */
void wizard_skip(void);
int  capture_wizard_running(void);

/* The axis guard's confirmation, posted out of the tick because a modal opened
 * from inside the preview's timer re-enters it (WM_APP + 3). */
void confirm_axis_detect(void);

/* THE SLOT WHILE IT IS BEING CAPTURED: 1, with the countdown, when it is the
 * slot this capture fills; 0 when it should show its input. */
int  capture_slot_text(unsigned action, unsigned slot, char *out,
                       size_t out_size);

/* The first line under the bars: Set up's step question while it runs, a held
 * result after, or NULL when the live preview's own hint should be drawn. */
const char *capture_note(void);
/* The second line while Set up runs ("" otherwise), and its state, which the
 * window turns into a colour. */
typedef enum {
    SAY_NONE = 0,
    SAY_WAIT,                   /* hands off while the controller settles   */
    SAY_GO,                     /* move it now                              */
    SAY_SEEN,                   /* a move has been seen, keep going         */
    SAY_DONE,                   /* measured: let it go                      */
    SAY_AGAIN                   /* that was the device, not you: again      */
} capture_say_t;
const char *capture_note_say(void);
capture_say_t capture_note_state(void);

/* One tick of the hold. 1 while the note line is still a capture's, so the
 * preview leaves it alone; always 1 while Set up runs. */
int  capture_note_tick(void);

/* ---- ...and what the window owes it --------------------------------------
 *
 * Defined in src/launcher.c. This list is the cost of the boundary and is
 * meant to be read as one: a capture says things, marks the file unsaved,
 * refreshes the control it just wrote, and asks what is attached. It does not
 * lay anything out, build a page or own a window. */

/* The status bar, the note line under the live bars, and the frame. */
void set_status(const char *text);
int  launcher_note_visible(void);           /* is the Controls page up?     */
void launcher_note_lines(const char *first, const char *second);
HWND launcher_window(void);

/* The model, and the controls showing it. */
void mark_dirty(void);
void request_relayout(void);
void update_binding_display(unsigned action);
void update_model_control(const char *section, const char *key);
void model_set(const char *section, const char *key, const char *value);
size_t pad_binding_render(const char *value, char *out, size_t out_size);
const char *key_display_name(const char *ini_name);
void pad_binding_text(const char *value, char *out, size_t out_size);
/* Two [gamepad] references to one button: a bare ordinal is the button on the
 * controller that drives the car, under whatever label that one has. */
int  pad_refs_same(const char *a, const char *b);
int  action_visible(unsigned action);
int  launcher_binding_title(void);          /* 0 hydro, 1 offroad           */

/* What is attached, and what it is doing right now. */
int  launcher_probe_selected(void);         /* -1 when none is ticked       */
int  launcher_probe_poll(void);             /* the selected one, into ...   */
const input_probe_state_t *launcher_probe_state(void);
/* Every device's buttons this tick, or NULL for one that did not answer. */
const input_probe_state_t *launcher_device_state(unsigned index);
int  ensure_device_label(const input_probe_device_t *d, char *out,
                         size_t out_size);
/* The controller's product name, as its row shows it without the backend. */
const char *probe_product_name(const input_probe_device_t *device);
/* The DirectInput controller the host drives with, or -1: an axis on it is
 * written bare, an axis on any other with its label. */
int  launcher_default_row(void);
int  probe_backend_matches_setting(const input_probe_device_t *device,
                                   const char *key);

#endif /* VCT_LAUNCHER_CAPTURE_H */
