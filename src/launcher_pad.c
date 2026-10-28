/* launcher_pad.c -- driving the launcher from a controller, with no window in it.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#include <string.h>

#include "launcher_pad.h"
#include "io.h"

/* xi_axes.h's order: A, B, X, Y, LB, RB, Back, Start, as ordinals. */
enum { XI_A = 1, XI_B = 2, XI_LB = 5, XI_RB = 6, XI_START = 8 };

unsigned launcher_pad_held(int xinput, const uint32_t *buttons)
{
    unsigned held = 0;

    if (!buttons)
        return 0;
    if (io_button_down(buttons, XI_A)) held |= PAD_HOLD_ACCEPT;
    if (io_button_down(buttons, XI_B)) held |= PAD_HOLD_BACK;
    if (!xinput)
        return held;
    if (io_button_down(buttons, XI_LB)) held |= PAD_HOLD_PREV;
    if (io_button_down(buttons, XI_RB)) held |= PAD_HOLD_NEXT;
    if (io_button_down(buttons, XI_START)) held |= PAD_HOLD_START;
    return held;
}

pad_event_t launcher_pad_direction(int pov)
{
    static const pad_event_t side[4] = { PAD_UP, PAD_RIGHT, PAD_DOWN,
                                         PAD_LEFT };

    if (pov < 0 || pov >= 36000)
        return PAD_NONE;
    return side[((unsigned)pov + 4499u) / 9000u % 4u];
}

void launcher_pad_reset(launcher_pad_t *p)
{
    memset(p, 0, sizeof *p);
}

static unsigned emit(pad_event_t *out, unsigned cap, unsigned n,
                     pad_event_t e)
{
    if (n < cap)
        out[n++] = e;
    return n;
}

unsigned launcher_pad_tick(launcher_pad_t *p, int gated,
                           const launcher_pad_device_t *devices, unsigned n,
                           uint32_t now_ms, pad_event_t *out, unsigned cap)
{
    pad_event_t dir = PAD_NONE;
    unsigned held = 0, pressed, released, count = 0, i;
    int lost = gated;

    if (n > INPUT_PROBE_MAX_DEVICES)
        n = INPUT_PROBE_MAX_DEVICES;
    for (i = 0; i < INPUT_PROBE_MAX_DEVICES; i++) {
        const launcher_pad_device_t *d = i < n ? &devices[i] : NULL;
        pad_event_t way;
        unsigned bits;

        if (gated || !d || !d->present) {
            lost |= p->armed[i];
            p->armed[i] = 0;
            continue;
        }
        way = launcher_pad_direction(d->pov);
        bits = launcher_pad_held(d->xinput, d->buttons);
        if (!p->armed[i]) {
            p->armed[i] = way == PAD_NONE && !bits;
            continue;
        }
        if (dir == PAD_NONE)
            dir = way;
        held |= bits;
    }

    if (dir != p->dir) {
        if (dir != PAD_NONE) {
            count = emit(out, cap, count, dir);
            p->repeat_at = now_ms + LAUNCHER_PAD_FIRST_REPEAT_MS;
        }
    } else if (dir != PAD_NONE && (int32_t)(now_ms - p->repeat_at) >= 0) {
        count = emit(out, cap, count, dir);
        p->repeat_at = now_ms + LAUNCHER_PAD_REPEAT_MS;
    }
    p->dir = dir;

    pressed = held & ~p->held;
    released = p->held & ~held;
    p->held = held;
    if (pressed & PAD_HOLD_BACK)
        count = emit(out, cap, count, PAD_BACK);
    if (pressed & PAD_HOLD_PREV)
        count = emit(out, cap, count, PAD_PREV_PAGE);
    if (pressed & PAD_HOLD_NEXT)
        count = emit(out, cap, count, PAD_NEXT_PAGE);
    /* A button that stopped counting because a gate closed or its controller
     * went away was never let go by the user. */
    if (!lost && (released & PAD_HOLD_ACCEPT))
        count = emit(out, cap, count, PAD_ACCEPT);
    if (!lost && (released & PAD_HOLD_START))
        count = emit(out, cap, count, PAD_START);
    return count;
}

static int gap(int a_lo, int a_hi, int b_lo, int b_hi)
{
    if (b_lo >= a_hi)
        return b_lo - a_hi;
    if (a_lo >= b_hi)
        return a_lo - b_hi;
    return 0;
}

static int absolute(int v)
{
    return v < 0 ? -v : v;
}

int launcher_pad_pick(const pad_rect_t *from, const pad_rect_t *candidates,
                      unsigned n, pad_event_t dir)
{
    int vertical = dir == PAD_UP || dir == PAD_DOWN;
    int fx = (from->left + from->right) / 2, fy = (from->top + from->bottom) / 2;
    int best = -1, best_score = 0, best_off = 0;
    unsigned i;

    if (!vertical && dir != PAD_LEFT && dir != PAD_RIGHT)
        return -1;
    for (i = 0; i < n; i++) {
        const pad_rect_t *c = &candidates[i];
        int cx = (c->left + c->right) / 2, cy = (c->top + c->bottom) / 2;
        int score, off;

        /* PAST THE EDGE, not just past the centre: a neighbour in the same
         * row a pixel lower is not "below". */
        switch (dir) {
        case PAD_DOWN:  if (cy < from->bottom) continue; break;
        case PAD_UP:    if (cy > from->top) continue; break;
        case PAD_RIGHT: if (cx < from->right) continue; break;
        default:        if (cx > from->left) continue; break;
        }
        if (vertical) {
            /* THE NEAREST ROW FIRST: a row further down that lines up
             * better loses to the next one, or a move skips rows. */
            score = 8 * gap(from->top, from->bottom, c->top, c->bottom) +
                    gap(from->left, from->right, c->left, c->right);
            off = absolute(cx - fx);
        } else {
            if (c->top >= from->bottom || c->bottom <= from->top)
                continue;
            score = gap(from->left, from->right, c->left, c->right);
            off = absolute(cy - fy);
        }
        if (best < 0 || score < best_score ||
            (score == best_score && off < best_off)) {
            best = (int)i;
            best_score = score;
            best_off = off;
        }
    }
    return best;
}
