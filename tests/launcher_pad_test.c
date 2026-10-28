/* launcher_pad_test.c -- the launcher's controller navigation, with no window.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * What a press means and where a move lands, on a pinned clock. The live half
 * needs a controller and a person; this pins the rules it is built on: nothing
 * acts before a controller has been seen at rest, Accept acts on release, a
 * gate swallows the press that ends it, and a move goes to the next row.
 */
#include <stdio.h>
#include <string.h>

#include "../src/launcher_pad.c"

static int s_failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        s_failures++;
    }
}

typedef struct {
    uint32_t buttons[IO_BUTTON_WORDS];
    launcher_pad_device_t d;
} fake_t;

static void fake_init(fake_t *f, int xinput)
{
    memset(f, 0, sizeof *f);
    f->d.present = 1;
    f->d.xinput = xinput;
    f->d.pov = -1;
    f->d.buttons = f->buttons;
}

static void press(fake_t *f, unsigned ordinal, int down)
{
    unsigned i = ordinal - 1;

    if (down)
        f->buttons[i >> 5] |= 1u << (i & 31);
    else
        f->buttons[i >> 5] &= ~(1u << (i & 31));
}

/* One tick of up to two controllers; the single event, PAD_NONE for none, or
 * -1 for more than one. */
static int tick2(launcher_pad_t *p, int gated, fake_t *a, fake_t *b,
                 uint32_t now)
{
    launcher_pad_device_t d[2];
    pad_event_t out[8];
    unsigned n = 0, got;

    d[n++] = a->d;
    if (b)
        d[n++] = b->d;
    got = launcher_pad_tick(p, gated, d, n, now, out, 8);
    return got == 0 ? PAD_NONE : got == 1 ? (int)out[0] : -1;
}

static int tick(launcher_pad_t *p, int gated, fake_t *a, uint32_t now)
{
    return tick2(p, gated, a, NULL, now);
}

static void test_direction(void)
{
    check(launcher_pad_direction(-1) == PAD_NONE, "centred is no move");
    check(launcher_pad_direction(65535) == PAD_NONE, "a 0xFFFF centre is "
          "no move, not left");
    check(launcher_pad_direction(0) == PAD_UP, "0 is up");
    check(launcher_pad_direction(9000) == PAD_RIGHT, "9000 is right");
    check(launcher_pad_direction(18000) == PAD_DOWN, "18000 is down");
    check(launcher_pad_direction(27000) == PAD_LEFT, "27000 is left");
    check(launcher_pad_direction(4500) == PAD_UP, "a tied diagonal goes "
          "to the side before it");
    check(launcher_pad_direction(31500) == PAD_LEFT, "up-left ties left");
    check(launcher_pad_direction(35999) == PAD_UP, "just short of 360 is up");
    check(launcher_pad_direction(8000) == PAD_RIGHT, "nearer right is right");
}

static void test_held(void)
{
    fake_t f;

    fake_init(&f, 1);
    press(&f, 1, 1); press(&f, 2, 1); press(&f, 5, 1); press(&f, 6, 1);
    press(&f, 8, 1);
    check(launcher_pad_held(1, f.buttons) ==
          (PAD_HOLD_ACCEPT | PAD_HOLD_BACK | PAD_HOLD_PREV | PAD_HOLD_NEXT |
           PAD_HOLD_START), "XInput: A B LB RB Start");
    check(launcher_pad_held(0, f.buttons) ==
          (PAD_HOLD_ACCEPT | PAD_HOLD_BACK),
          "any other controller: buttons 1 and 2 only");
    fake_init(&f, 1);
    press(&f, 3, 1); press(&f, 4, 1); press(&f, 7, 1);
    check(launcher_pad_held(1, f.buttons) == 0, "X, Y and Back do nothing");
}

static void test_arming(void)
{
    launcher_pad_t p;
    fake_t f;

    /* A HAT THAT READS UP BEFORE THE DEVICE HAS REPORTED never moves. */
    launcher_pad_reset(&p);
    fake_init(&f, 0);
    f.d.pov = 0;
    check(tick(&p, 0, &f, 0) == PAD_NONE, "a hat held at open: nothing");
    check(tick(&p, 0, &f, 1000) == PAD_NONE, "...and no repeat either");
    f.d.pov = -1;
    check(tick(&p, 0, &f, 1033) == PAD_NONE, "rest arms it");
    f.d.pov = 0;
    check(tick(&p, 0, &f, 1066) == PAD_UP, "then a press moves");

    /* A BUTTON DOWN AT OPEN never acts, on press or release. */
    launcher_pad_reset(&p);
    fake_init(&f, 1);
    press(&f, 1, 1);
    check(tick(&p, 0, &f, 0) == PAD_NONE, "A held at open: nothing");
    press(&f, 1, 0);
    check(tick(&p, 0, &f, 33) == PAD_NONE, "...and its release: nothing");
}

static void test_repeat(void)
{
    launcher_pad_t p;
    fake_t f;
    uint32_t t = 0;
    int moves = 0;

    launcher_pad_reset(&p);
    fake_init(&f, 1);
    tick(&p, 0, &f, t);
    f.d.pov = 18000;
    check(tick(&p, 0, &f, t += 33) == PAD_DOWN, "first move at once");
    check(tick(&p, 0, &f, t += 33) == PAD_NONE, "no repeat at once");
    /* 400 ms, then every 80: 1 + floor((1000 - 400) / 80) + 1 = 9 in a
     * second sampled every millisecond. */
    launcher_pad_reset(&p);
    f.d.pov = -1;
    tick(&p, 0, &f, 0);
    f.d.pov = 18000;
    for (t = 1; t <= 1000; t++)
        moves += tick(&p, 0, &f, t) == PAD_DOWN;
    check(moves == 9, "400 ms, then every 80 ms");
    /* Changing direction is a new first move. */
    f.d.pov = 27000;
    check(tick(&p, 0, &f, 1001) == PAD_LEFT, "a turn moves at once");
}

static void test_accept_on_release(void)
{
    launcher_pad_t p;
    fake_t f;

    launcher_pad_reset(&p);
    fake_init(&f, 1);
    tick(&p, 0, &f, 0);
    press(&f, 1, 1);
    check(tick(&p, 0, &f, 33) == PAD_NONE, "A acts on release, not press");
    press(&f, 1, 0);
    check(tick(&p, 0, &f, 66) == PAD_ACCEPT, "A released: accept");
    press(&f, 8, 1);
    check(tick(&p, 0, &f, 99) == PAD_NONE, "Start acts on release");
    press(&f, 8, 0);
    check(tick(&p, 0, &f, 132) == PAD_START, "Start released: start");
    press(&f, 2, 1);
    check(tick(&p, 0, &f, 165) == PAD_BACK, "B acts on press");
    press(&f, 2, 0);
    check(tick(&p, 0, &f, 198) == PAD_NONE, "...and not again on release");
    press(&f, 6, 1);
    check(tick(&p, 0, &f, 231) == PAD_NEXT_PAGE, "RB: next page");
    press(&f, 6, 0);
    press(&f, 5, 1);
    check(tick(&p, 0, &f, 264) == PAD_PREV_PAGE, "LB: previous page");
}

static void test_gate(void)
{
    launcher_pad_t p;
    fake_t f;

    /* A captures the slot: A goes down, the capture starts on its release
     * (gated from then), B is pressed to bind it, the capture ends with B
     * still down. Neither B nor its release may act. */
    launcher_pad_reset(&p);
    fake_init(&f, 1);
    tick(&p, 0, &f, 0);
    press(&f, 2, 1);
    check(tick(&p, 1, &f, 33) == PAD_NONE, "gated: B is the capture's");
    check(tick(&p, 0, &f, 66) == PAD_NONE, "gate open, B still down: "
          "nothing");
    press(&f, 2, 0);
    check(tick(&p, 0, &f, 99) == PAD_NONE, "B released: still nothing");
    press(&f, 2, 1);
    check(tick(&p, 0, &f, 132) == PAD_BACK, "the next B is back");

    /* A held when a gate closes is not released by the user. */
    launcher_pad_reset(&p);
    fake_init(&f, 1);
    tick(&p, 0, &f, 0);
    press(&f, 1, 1);
    tick(&p, 0, &f, 33);
    check(tick(&p, 1, &f, 66) == PAD_NONE, "A held as a gate closes: "
          "nothing");

    /* ...nor is one on a controller that went away. */
    launcher_pad_reset(&p);
    fake_init(&f, 1);
    tick(&p, 0, &f, 0);
    press(&f, 1, 1);
    tick(&p, 0, &f, 33);
    f.d.present = 0;
    check(tick(&p, 0, &f, 66) == PAD_NONE, "A held as its pad goes: "
          "nothing");
}

static void test_two_controllers(void)
{
    launcher_pad_t p;
    fake_t pad, wheel;

    /* An idle wheel reading its hat as up must not block the pad. */
    launcher_pad_reset(&p);
    fake_init(&pad, 1);
    fake_init(&wheel, 0);
    wheel.d.pov = 0;
    tick2(&p, 0, &pad, &wheel, 0);
    pad.d.pov = 9000;
    check(tick2(&p, 0, &pad, &wheel, 33) == PAD_RIGHT,
          "the pad moves beside an unarmed wheel");
    pad.d.pov = -1;
    tick2(&p, 0, &pad, &wheel, 66);
    /* The wheel's button 1 accepts, on release. */
    wheel.d.pov = -1;
    tick2(&p, 0, &pad, &wheel, 99);
    press(&wheel, 1, 1);
    tick2(&p, 0, &pad, &wheel, 132);
    press(&wheel, 1, 0);
    check(tick2(&p, 0, &pad, &wheel, 165) == PAD_ACCEPT,
          "a wheel's button 1 accepts");
}

static pad_rect_t r(int l, int t, int w, int h)
{
    pad_rect_t x;

    x.left = l; x.top = t; x.right = l + w; x.bottom = t + h;
    return x;
}

static void test_pick(void)
{
    /* A settings page: a full-width checkbox, then two rows of a label (not a
     * candidate) and a control, then a binding row of two slots, then a
     * button on the left. */
    pad_rect_t c[7];
    pad_rect_t from;

    c[0] = r(20, 10, 600, 24);          /* checkbox                  */
    c[1] = r(240, 50, 300, 24);         /* combo                     */
    c[2] = r(240, 90, 200, 24);         /* slider                    */
    c[3] = r(240, 130, 140, 24);        /* slot 1                    */
    c[4] = r(390, 130, 140, 24);        /* slot 2                    */
    c[5] = r(20, 170, 200, 26);         /* Reset to defaults         */
    c[6] = r(400, 200, 140, 24);        /* a row further, lined up    */

    check(launcher_pad_pick(&c[0], c, 7, PAD_DOWN) == 1, "checkbox down: "
          "the combo");
    check(launcher_pad_pick(&c[1], c, 7, PAD_DOWN) == 2, "combo down: the "
          "slider");
    check(launcher_pad_pick(&c[2], c, 7, PAD_DOWN) == 3, "slider down: the "
          "slot more in line");
    check(launcher_pad_pick(&c[4], c, 7, PAD_DOWN) == 5, "slot 2 down: the "
          "next row, not the better-aligned one after it");
    check(launcher_pad_pick(&c[4], c, 7, PAD_UP) == 2, "slot 2 up: the slider");
    check(launcher_pad_pick(&c[3], c, 7, PAD_RIGHT) == 4, "slot 1 right: "
          "slot 2");
    check(launcher_pad_pick(&c[4], c, 7, PAD_LEFT) == 3, "slot 2 left: "
          "slot 1");
    check(launcher_pad_pick(&c[3], c, 7, PAD_LEFT) == -1, "slot 1 left: "
          "nothing in this row");
    check(launcher_pad_pick(&c[0], c, 7, PAD_UP) == -1, "top: nothing up");
    check(launcher_pad_pick(&c[6], c, 7, PAD_DOWN) == -1, "bottom: nothing "
          "down");
    check(launcher_pad_pick(&c[1], c, 7, PAD_START) == -1, "not a move");

    /* A NEIGHBOUR A PIXEL LOWER IS IN THE SAME ROW: an edit and its Browse
     * button. */
    c[0] = r(240, 10, 200, 22);         /* edit                      */
    c[1] = r(450, 9, 80, 26);           /* Browse...                 */
    c[2] = r(240, 50, 200, 22);         /* next row                  */
    from = c[0];
    check(launcher_pad_pick(&from, c, 3, PAD_DOWN) == 2, "edit down: the "
          "next row, not its button");
    check(launcher_pad_pick(&from, c, 3, PAD_RIGHT) == 1, "edit right: its "
          "button");
}

int main(void)
{
    test_direction();
    test_held();
    test_arming();
    test_repeat();
    test_accept_on_release();
    test_gate();
    test_two_controllers();
    test_pick();
    if (s_failures) {
        printf("launcher pad: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("launcher pad: all checks passed\n");
    return 0;
}
