/* launcher_pad.h -- driving the launcher from a controller, with no window in it.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * WHAT A CONTROLLER MEANS TO THE LAUNCHER: the D-pad moves, and a few buttons
 * act. This half turns polled controller states into those events and picks
 * the control a move lands on; src/launcher.c owns the windows they act on.
 * Nothing here creates a window or reads a device, so
 * tests/launcher_pad_test.c links it without a GUI.
 */
#ifndef VCT_LAUNCHER_PAD_H
#define VCT_LAUNCHER_PAD_H

#include <stdint.h>

#include "vcthunder.h"

typedef enum {
    PAD_NONE = 0,
    PAD_UP,
    PAD_DOWN,
    PAD_LEFT,
    PAD_RIGHT,
    PAD_ACCEPT,                 /* A, or button 1 on any other controller   */
    PAD_BACK,                   /* B, or button 2                           */
    PAD_PREV_PAGE,              /* LB                                       */
    PAD_NEXT_PAGE,              /* RB                                       */
    PAD_START                   /* Start                                    */
} pad_event_t;

/* The buttons that act, as bits of one mask. */
enum {
    PAD_HOLD_ACCEPT = 1u << 0,
    PAD_HOLD_BACK   = 1u << 1,
    PAD_HOLD_PREV   = 1u << 2,
    PAD_HOLD_NEXT   = 1u << 3,
    PAD_HOLD_START  = 1u << 4
};

/* WHICH BUTTONS ACT, BY BACKEND. XInput fixes its order (xi_axes.h), so A, B,
 * LB, RB and Start are known. Any other controller is a wheel or a pad of
 * unknown layout: its first two buttons accept and go back, and it has no page
 * or Start button. */
unsigned launcher_pad_held(int xinput, const uint32_t *buttons);

/* A hat in hundredths of degrees, -1 centred, to PAD_UP..PAD_RIGHT or
 * PAD_NONE. A diagonal goes to the nearer side, ties to the one clockwise
 * before it. */
pad_event_t launcher_pad_direction(int pov);

#define LAUNCHER_PAD_FIRST_REPEAT_MS 400u
#define LAUNCHER_PAD_REPEAT_MS        80u

typedef struct {
    unsigned char armed[INPUT_PROBE_MAX_DEVICES];
    pad_event_t dir;
    unsigned held;
    uint32_t repeat_at;
} launcher_pad_t;

/* One controller this tick. `present` 0 for one that did not answer. */
typedef struct {
    int present;
    int xinput;
    int pov;
    const uint32_t *buttons;
} launcher_pad_device_t;

void launcher_pad_reset(launcher_pad_t *p);

/* ONE TICK. Returns how many events were written to `out`.
 *
 * A CONTROLLER COUNTS ONLY ONCE IT HAS BEEN SEEN AT REST, and every one is
 * set back to waiting while `gated` (a capture or Set up has the controllers)
 * and when it stops answering. So the press that ends a capture, a hat that
 * reads up before the device has reported, or a button already down when the
 * window opened never moves anything.
 *
 * A move repeats while held. Accept and Start act on RELEASE, so the game a
 * Play starts never sees the button still down; the other buttons act on
 * press. */
unsigned launcher_pad_tick(launcher_pad_t *p, int gated,
                           const launcher_pad_device_t *devices, unsigned n,
                           uint32_t now_ms, pad_event_t *out, unsigned cap);

typedef struct {
    int left, top, right, bottom;
} pad_rect_t;

/* WHERE A MOVE LANDS: the index into `candidates` of the control `dir` goes
 * to from `from`, or -1 when nothing lies that way. Up and down take the
 * nearest row, and in it the control most in line; left and right stay in the
 * same row. All rectangles in one coordinate space. */
int launcher_pad_pick(const pad_rect_t *from, const pad_rect_t *candidates,
                      unsigned n, pad_event_t dir);

#endif /* VCT_LAUNCHER_PAD_H */
