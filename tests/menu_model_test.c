/* menu_model_test.c -- the select screens' virtual wheel, against modelled
 * screens.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * No game and no device. Each screen below is a model of one kind the census
 * found, written from the game's own code
 * where it has been read (Hydro and Offroad) and from what Phase 0 measured
 * where it has not (Arctic): Hydro's overlapping bands stepped on odd frames,
 * with the items it refuses; Hydro's 41 initials slots and their formula;
 * Offroad's filtered index over a count, where a disabled item hands its
 * band to the nearest enabled one; Arctic's equal notches; Offroad's letter
 * wheel, which turns faster the further it is steered and stops at its ends.
 * Between the byte and the field each one puts the delay and lag measured
 * for it, and some a miscalibration and a dead zone, because the model is
 * closed on the field and has to see through them.
 *
 * These are models of the screens, not the screens: what this proves is that
 * the core does what the plan says against anything shaped like them. Whether
 * the games ARE shaped like them is each title's phase, run live.
 *
 * Every test that claims a defect is fixed runs the defect first: the same
 * screen, with the model out of the way, has to show it.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/menu_model.h"

static int s_failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        s_failures++;
    }
}

/* ---- the modelled screens ------------------------------------------------ */

enum { HYDRO_BANDS, HYDRO_INITIALS, OFFROAD_SELECT, ARCTIC_NOTCH,
       OFFROAD_INITIALS };

#define TICK_MS  16     /* the board's poll, [controls] io_ms              */
#define FRAME_MS 33     /* the game's frame                                */
#define LETTERS  47

typedef struct {
    int kind;
    unsigned screen;
    /* The byte as the game reads it: gain, offset, a dead zone, a lag. */
    float gain, offset, dead, lag_ms;
    /* The game reads the byte once a frame, as it was delay_ms ago, and then
     * smooths it over lag_ms. Measured live on Hydro (Phase 2): its select
     * screens see a byte ~46 ms late, its initials ~95 ms late and then at
     * once. A pure delay winds up an integrator in a way a lag of the same
     * length does not. */
    unsigned delay_ms;
    unsigned char sent[64];         /* the byte each tick, newest last */
    unsigned nsent;
    float field;
    int index, n, enabled;
    /* Hydro's bands: item k is centre k +- half. */
    float c0, pitch, half;
    int refused[MENU_ITEMS_MAX];
    int frame_no;
    float filtered;     /* Offroad: 0..1                                    */
    int mirror;         /* Offroad's initials, steered the other way round  */
    float wheel;        /* Offroad's initials: the letter wheel, 0..1       */
    /* Offroad: items the game moves past to the nearest enabled one. */
    unsigned char skip[MENU_ITEMS_MAX];
    unsigned clock, frame_acc;
} game_t;

/* The field over which the game keeps item k, as the screen's own code
 * decides it. */
static void band_of(const game_t *g, int k, float *lo, float *hi)
{
    float n = (float)g->n, a = 0, b = 0;

    switch (g->kind) {
    case HYDRO_BANDS:
        a = g->c0 + g->pitch * (float)k - g->half;
        b = g->c0 + g->pitch * (float)k + g->half;
        break;
    case HYDRO_INITIALS:            /* y = 42 (f+1)/2 in [k, k+2) */
        a = (float)k / 21.0f - 1.0f;
        b = (float)(k + 2) / 21.0f - 1.0f;
        break;
    case OFFROAD_SELECT:            /* the floor of count x position */
        a = (float)k / n * 2.0f - 1.0f;
        b = (float)(k + 1) / n * 2.0f - 1.0f;
        break;
    case ARCTIC_NOTCH:              /* kept within 0.75 of a notch's width */
        a = ((float)k + 0.5f - 0.75f) / n * 2.0f - 1.0f;
        b = ((float)k + 0.5f + 0.75f) / n * 2.0f - 1.0f;
        break;
    }
    *lo = a < -1 ? -1 : a;
    *hi = b > 1 ? 1 : b;
}

static float centre_of(const game_t *g, int k)
{
    float lo, hi;

    band_of(g, k, &lo, &hi);
    return (lo + hi) * 0.5f;
}

static void base(game_t *g, int kind, int n)
{
    memset(g, 0, sizeof *g);
    g->kind = kind;
    g->n = g->enabled = n;
    g->gain = 1.0f;
    g->lag_ms = 50;
    g->screen = 1;
}

static void hydro_tracks(game_t *g)
{
    base(g, HYDRO_BANDS, 10);
    g->delay_ms = 46;
    g->c0 = -0.9f, g->pitch = 0.2f, g->half = 0.15f;
    g->refused[0] = g->refused[9] = 1;
    g->index = 4;
}

static void hydro_boats(game_t *g)
{
    int k;

    base(g, HYDRO_BANDS, 9);
    g->delay_ms = 46;
    g->c0 = -0.888f, g->pitch = 0.222f, g->half = 0.161f;
    for (k = 6; k < 9; k++)
        g->refused[k] = 1;
    g->index = 4;
}

static void hydro_initials(game_t *g)
{
    base(g, HYDRO_INITIALS, 41);
    g->delay_ms = 95;
    g->lag_ms = 10;
    g->index = 20;
}

/* __UpdateSelectionControls and the screen after it. `skip` is one item the
 * list has disabled, or -1. */
static void offroad_select(game_t *g, int count, int skip)
{
    base(g, OFFROAD_SELECT, count);
    g->lag_ms = 10;
    g->delay_ms = 33;
    if (skip >= 0)
        g->skip[skip] = 1;
    g->index = count / 2;
    if (g->skip[g->index])
        g->index--;
    g->filtered = ((float)g->index + 0.5f) / (float)count;
}

static void arctic(game_t *g, int n)
{
    base(g, ARCTIC_NOTCH, n);
    g->lag_ms = 0;
    g->index = n / 2;
}

static void offroad_initials(game_t *g)
{
    base(g, OFFROAD_INITIALS, LETTERS);
    g->lag_ms = 10;
    g->delay_ms = 33;
    g->index = 10;
    g->wheel = (float)(LETTERS - 1 - 10) / (float)(LETTERS - 1);
}

/* The field the game would settle on for this byte. */
static float shape(const game_t *g, unsigned byte)
{
    float x = ((float)byte - 127.5f) / 127.5f * g->gain + g->offset;

    if (g->dead > 0) {
        if (fabsf(x) <= g->dead)
            x = 0;
        else
            x = (x - (x > 0 ? g->dead : -g->dead)) / (1.0f - g->dead);
    }
    return x > 1 ? 1 : x < -1 ? -1 : x;
}

/* _wpr_hiscore_Work, while letters are being entered: the slot from the
 * steering, with the previous slot as its hysteresis. */
static int hydro_initials_slot(float f, int old)
{
    float x = (f + 1.0f) * 0.5f;
    int s = x < (float)(old + 1) / 42.0f ? (int)(x * 42.0f)
                                         : (int)(x * 42.0f - 1.0f);
    return s < 0 ? 0 : s > 40 ? 40 : s;
}

static void game_frame(game_t *g)
{
    float f = g->field;

    g->frame_no++;
    switch (g->kind) {
    case HYDRO_BANDS:
        /* One item per odd frame while the field is outside the band. */
        if (g->frame_no & 1) {
            float lo, hi;
            int t;

            band_of(g, g->index, &lo, &hi);
            t = f > hi ? g->index + 1 : f < lo ? g->index - 1 : g->index;

            if (t >= 0 && t < g->n && !g->refused[t])
                g->index = t;
        }
        break;
    case HYDRO_INITIALS:
        if (g->frame_no & 1)
            g->index = hydro_initials_slot(f, g->index);
        break;
    case OFFROAD_SELECT: {
        /* Within 0.1 of the target the position snaps to it; past that it
         * moves 0.1 and a tenth of what is left. The index is its floor
         * over the count, and the screen then moves it to the nearest
         * enabled item, the lower on a tie. */
        float t = (f + 1.0f) * 0.5f, d = t - g->filtered;
        int k, j, best = -1, bd = 0;

        if (fabsf(d) < 0.1f) {
            g->filtered = t;
        } else {
            float x = g->filtered + (d > 0 ? 0.1f : -0.1f);
            g->filtered = x + 0.1f * (t - x);
        }
        k = (int)(g->filtered * (float)g->n);
        if (k >= g->n)
            k = g->n - 1;
        for (j = 0; j < g->n; j++) {
            int dist = j > k ? j - k : k - j;
            if (!g->skip[j] && (best < 0 || dist < bd))
                best = j, bd = dist;
        }
        g->index = best;
        break;
    }
    case ARCTIC_NOTCH: {
        /* The feedback thread: the slot under the position, unless the
         * current notch is within 0.75 of a notch's width of it. */
        float w = 2.0f / (float)g->n, slot = (f + 1.0f) / w;
        int k = slot < 0 ? 0 : slot > g->n - 1 ? g->n - 1 : (int)slot;

        if (g->index >= 0 && g->index < g->n && k != g->index &&
            fabsf(((float)g->index + 0.5f) * w - (f + 1.0f)) < 0.75f * w)
            k = g->index;
        g->index = k;
        break;
    }
    case OFFROAD_INITIALS: {
        /* __initials_WorkSelectionWheel, about 60 calls a second: past 0.3
         * the wheel turns at one of three speeds, inside it snaps a tenth
         * of the way to the nearest letter. Steering right turns the wheel
         * down and the letter up; the wheel stops at its ends. */
        int call;

        for (call = 0; call < 2; call++) {
            float s = g->mirror ? f : -f, a = fabsf(s);
            float n = (float)(LETTERS - 1);

            if (a > 0.3f) {
                float k = a < 0.5f ? 0.3f : a < 0.7f ? 0.5f : 0.7f;
                g->wheel += 0.015f * (s < 0 ? -k : k);
            } else {
                float to = floorf(g->wheel * n + 0.5f) / n, d = to - g->wheel;
                g->wheel = fabsf(d) < 0.2f / n ? to : g->wheel + 0.1f * d;
            }
            g->wheel = g->wheel < 0 ? 0 : g->wheel > 1 ? 1 : g->wheel;
        }
        g->index = LETTERS - 1 - (int)floorf(g->wheel * (LETTERS - 1) + 0.5f);
        break;
    }
    }
}

/* The game between two host ticks: the field chases the byte, and the
 * screen runs its frames. */
/* The byte sent `ago` ms before now. */
static unsigned sent_before(const game_t *g, unsigned ago)
{
    unsigned back = ago / TICK_MS;

    if (!g->nsent)
        return 128;
    if (back >= g->nsent)
        back = g->nsent - 1;
    return g->sent[(g->nsent - 1 - back) % 64];
}

static void game_run(game_t *g, unsigned byte, unsigned ms)
{
    if (g->nsent >= 64) {
        memmove(g->sent, g->sent + 1, 63);
        g->nsent = 63;
    }
    g->sent[g->nsent++] = (unsigned char)byte;
    g->clock += ms;
    g->frame_acc += ms;
    while (g->frame_acc >= FRAME_MS) {
        float want = shape(g, sent_before(g, g->delay_ms));

        g->frame_acc -= FRAME_MS;
        g->field = g->lag_ms > 0
            ? g->field + (want - g->field) *
                  (1.0f - expf(-(float)FRAME_MS / g->lag_ms))
            : want;
        game_frame(g);
    }
}

static void describe(const game_t *g, menu_screen_t *s)
{
    int k;

    memset(s, 0, sizeof *s);
    s->screen = g->screen;
    s->index = g->index;
    s->field = g->field;
    s->n = g->n;
    if (g->kind == OFFROAD_INITIALS) {
        s->kind = MENU_RATE;
        s->rate_sign = g->mirror ? -1 : 1;
        s->push = 0.4f;
        s->first = 0;
        s->last = LETTERS - 1;
        return;
    }
    if (g->kind == OFFROAD_SELECT) {
        unsigned char en[MENU_ITEMS_MAX];
        int order[MENU_ITEMS_MAX];

        for (k = 0; k < g->n; k++)
            en[k] = !g->skip[k];
        s->kind = MENU_POSITION;
        menu_bands_counted(s, (float)g->n, -1.0f, 0.5f, en, order);
        s->index = -1;
        for (k = 0; k < s->n; k++)
            if (order[k] == g->index)
                s->index = k;
        return;
    }
    s->kind = MENU_POSITION;
    for (k = 0; k < g->n; k++)
        band_of(g, k, &s->lo[k], &s->hi[k]);
}

/* ---- the run ------------------------------------------------------------- */

typedef struct {
    game_t g;
    menu_model_t m;
    menu_input_t in;
    menu_output_t out;
    int model;          /* 0: the model out of the way, today's behaviour   */
    int changes;        /* index changes seen                               */
    int max_jump;       /* the largest single change                        */
    float max_field;    /* the largest |field| the game read                */
    int steps;          /* steps the model began                            */
    unsigned last_t0;   /* ... counted by their start: a give-up and the    */
                        /* next step can share a tick                       */
} rig_t;

static void rig(rig_t *r, const game_t *g, int wheel_present, float wheel)
{
    memset(r, 0, sizeof *r);
    r->g = *g;
    r->model = 1;
    menu_model_init(&r->m, 0.08f);
    r->in.wheel_present = wheel_present;
    r->in.wheel = wheel;
    /* Today's byte: the wheel where it is, or keys at full lock. */
    r->in.host_byte = 128;
    r->g.field = shape(&r->g, 128);
}

static unsigned host_byte(const rig_t *r)
{
    if (r->in.wheel_present && !r->in.left && !r->in.right) {
        float b = 127.5f + r->in.wheel * 127.5f;
        return b < 0 ? 0 : b > 255 ? 255 : (unsigned)(b + 0.5f);
    }
    return r->in.right && !r->in.left ? 255
         : r->in.left && !r->in.right ? 0 : 128;
}

static int letter_delta(int from, int to)
{
    return to - from;
}

static void run(rig_t *r, unsigned ms)
{
    unsigned t;

    for (t = 0; t < ms; t += TICK_MS) {
        menu_screen_t s;
        int before = r->g.index, d;

        r->in.host_byte = host_byte(r);
        describe(&r->g, &s);
        if (r->model)
            r->out = menu_model_tick(&r->m, &s, &r->in, r->g.clock);
        else {
            r->out.byte = r->in.host_byte;
            r->out.digital = r->out.mute_force = 0;
        }
        if (r->m.stepping && r->m.step_t0 != r->last_t0) {
            r->steps++;
            r->last_t0 = r->m.step_t0;
        }
        game_run(&r->g, r->out.byte, TICK_MS);
        if (fabsf(r->g.field) > r->max_field)
            r->max_field = fabsf(r->g.field);
        d = r->g.kind == OFFROAD_INITIALS ? letter_delta(before, r->g.index)
                                          : r->g.index - before;
        if (d) {
            r->changes++;
            if (abs(d) > r->max_jump)
                r->max_jump = abs(d);
        }
    }
}

static void tap(rig_t *r, int dir)
{
    if (dir > 0)
        r->in.right = 1;
    else
        r->in.left = 1;
    run(r, 80);
    r->in.right = r->in.left = 0;
    run(r, 1500);
}

static const char *fmt(const char *f, int a, int b)
{
    static char buf[200];
    snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

/* ---- the tests ----------------------------------------------------------- */

typedef struct {
    const char *name;
    game_t g;
} screen_case_t;

static int build_cases(screen_case_t *c)
{
    int n = 0, k;
    /* Full lock always reaches both ends, as Phase 0 measured on keys in
     * every title: a gain below 1 would put the end items out of any
     * wheel's reach, and the cabinet's own calibration does not. */
    static const struct { float gain, offset, dead; } cal[3] = {
        { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.03f, 0.06f }, { 1.1f, -0.04f, 0.0f }
    };
    static const char *calname[3] = { "", " (miscalibrated)", " (hot)" };
    static char names[64][80];

    for (k = 0; k < 3; k++) {
        int base_n = n;

        hydro_tracks(&c[n].g), c[n++].name = "Hydro tracks";
        hydro_boats(&c[n].g), c[n++].name = "Hydro boats";
        hydro_initials(&c[n].g), c[n++].name = "Hydro initials";
        offroad_select(&c[n].g, 3, -1), c[n++].name = "Offroad game mode";
        offroad_select(&c[n].g, 8, 7), c[n++].name = "Offroad track";
        offroad_select(&c[n].g, 8, 5),
            c[n++].name = "Offroad machine, one disabled";
        offroad_select(&c[n].g, 2, -1), c[n++].name = "Offroad transmission";
        arctic(&c[n].g, 6), c[n++].name = "Arctic";
        arctic(&c[n].g, 8), c[n++].name = "Ultimate";
        for (; base_n < n; base_n++) {
            c[base_n].g.gain = cal[k].gain;
            c[base_n].g.offset = cal[k].offset;
            c[base_n].g.dead = cal[k].dead;
            snprintf(names[base_n], sizeof names[base_n], "%s%s",
                     c[base_n].name, calname[k]);
            c[base_n].name = names[base_n];
        }
    }
    return n;
}

static int lowest(const game_t *g)
{
    int k = 0;
    while (k < g->n && (g->refused[k] || g->skip[k]))
        k++;
    return k;
}

static int highest(const game_t *g)
{
    int k = g->enabled - 1;
    while (k > 0 && (g->refused[k] || g->skip[k]))
        k--;
    return k;
}

/* Where one tap from `from` should land: the next item the game shows, past
 * any it skips (Offroad's disabled items). */
static int next_item(const game_t *g, int from, int dir)
{
    int t = from + dir;

    while (t >= 0 && t < g->n && g->skip[t])
        t += dir;
    return t;
}

static void walk(rig_t *r, const char *name, int dir, int to, int end)
{
    char what[200];
    int guard = 0;

    while (r->g.index != to && guard++ < MENU_ITEMS_MAX) {
        int from = r->g.index, want = next_item(&r->g, from, dir);

        r->changes = 0;
        tap(r, dir);
        snprintf(what, sizeof what, "%s: tap %s from %d lands on %d (has %d,"
                 " %d changes)", name, dir > 0 ? "right" : "left", from,
                 want, r->g.index, r->changes);
        check(r->g.index == want && r->changes == 1, what);
        if (r->g.index != want)
            return;
        run(r, 1000);
        snprintf(what, sizeof what, "%s: released on %d, holds (has %d)",
                 name, want, r->g.index);
        check(r->g.index == want, what);
    }
    if (!end)
        return;
    r->changes = r->steps = 0;
    tap(r, dir);
    /* Past the list's own end nothing is even tried; past an item the game
     * refuses (a wall) a step is tried and given up. */
    if (to == (dir > 0 ? r->g.enabled - 1 : 0)) {
        snprintf(what, sizeof what, "%s: no step past the list's end at %d"
                 " (%d tried)", name, to, r->steps);
        check(r->steps == 0, what);
    }
    snprintf(what, sizeof what, "%s: a tap %s past %d does nothing (has %d,"
             " %d changes)", name, dir > 0 ? "right" : "left", to,
             r->g.index, r->changes);
    check(r->g.index == to && r->changes == 0, what);
}

/* One tap: exactly one item, held on release, nothing shown in between.
 * Walked from where the game sits with the keys centred, to each end and
 * back, the trim learning as it goes. */
static void test_tap_is_one_item(void)
{
    screen_case_t c[64];
    int n = build_cases(c), i;

    for (i = 0; i < n; i++) {
        rig_t r;
        int home;

        rig(&r, &c[i].g, 0, 0);
        r.model = 0;
        run(&r, 1500);
        r.model = 1;
        run(&r, 100);   /* the screen entered before the first press */
        home = r.g.index;
        walk(&r, c[i].name, 1, highest(&r.g), 1);
        walk(&r, c[i].name, -1, lowest(&r.g), 1);
        walk(&r, c[i].name, 1, home, 0);
    }
}

/* The defect, so the walk is known to be able to see it. */
static void test_control_shows_the_defect(void)
{
    game_t g;
    rig_t r;

    hydro_tracks(&g);
    rig(&r, &g, 0, 0);
    r.model = 0;
    run(&r, 1500);
    tap(&r, 1);
    check(r.g.index == 5 && r.changes > 2,
          "control: a key tap on Hydro's tracks runs and comes back");
    hydro_initials(&g);
    rig(&r, &g, 0, 0);
    r.model = 0;
    run(&r, 1500);
    tap(&r, 1);
    check(r.max_jump > 1, "control: a key tap on Hydro's initials jumps");
}

/* Entering with the keys opens where a wheel at rest would: the game's
 * own index can still be on the last screen's position (Offroad's filter
 * carries over), and the board sends centre until the game settles, then
 * holds that item. Offroad's game mode opens on index 0 for a moment and
 * settles on RALLY, the middle (Phase 3). */
static void test_entry_settles(void)
{
    game_t g;
    rig_t r;

    offroad_select(&g, 3, -1);
    g.index = 0;
    g.filtered = 0.14f;
    rig(&r, &g, 0, 0);
    r.model = 0;
    run(&r, 2000);
    check(r.g.index == 1, "control: centred keys settle game mode on 1");
    rig(&r, &g, 0, 0);
    run(&r, 2000);
    check(r.g.index == 1 && r.m.at == 1 && !r.m.settling,
          fmt("entry: the keys settle on 1 too, and hold it (has %d)",
              r.g.index, 0));
    tap(&r, -1);
    check(r.g.index == 0, "entry: then a tap steps from there");
}

/* Held: one item at once, the next at 350 ms, then one per 120 ms once
 * each has landed. Hydro's 41 slots end to end in about five seconds. */
static void test_repeat(void)
{
    game_t g;
    rig_t r;
    unsigned t;
    int at300 = -1, at500 = -1;

    hydro_initials(&g);
    rig(&r, &g, 0, 0);
    run(&r, 600);
    r.in.left = 1;                  /* to slot 0 first, untimed */
    run(&r, 6000);
    r.in.left = 0;
    run(&r, 1000);
    check(r.g.index == 0, fmt("repeat: a hold reaches slot 0 (has %d)",
                              r.g.index, 0));
    r.changes = r.max_jump = 0;
    r.in.right = 1;
    for (t = 0; t < 10000 && r.g.index < 40; t += TICK_MS) {
        run(&r, TICK_MS);
        if (t + TICK_MS >= 300 && at300 < 0)
            at300 = r.g.index;
        if (t + TICK_MS >= 500 && at500 < 0)
            at500 = r.g.index;
    }
    check(at300 == 1, fmt("repeat: one slot until 350 ms (has %d at 300 ms)",
                          at300, 0));
    check(at500 >= 2 && at500 <= 3,
          fmt("repeat: stepping by 500 ms (has %d)", at500, 0));
    check(r.g.index == 40 && t >= 4000 && t <= 7000,
          fmt("repeat: 0 to 40 in about five seconds (%d ms, slot %d)",
              (int)t, r.g.index));
    check(r.max_jump == 1, fmt("repeat: never more than one slot at once"
                               " (%d)", r.max_jump, 0));
    r.in.right = 0;
    run(&r, 2000);
    check(r.g.index == 40, "repeat: the end holds after the release");
}

/* Three quick taps are three items, the last two waiting their turn
 * while the first is still on its way. */
static void test_double_tap(void)
{
    game_t g;
    rig_t r;
    int i;

    hydro_boats(&g);
    rig(&r, &g, 0, 0);
    run(&r, 600);
    check(r.g.index == 4, "triple tap: boat select opens on boat 4");
    for (i = 0; i < 3; i++) {
        r.in.left = 1, run(&r, 16);
        r.in.left = 0, run(&r, 16);
    }
    check(r.g.index >= 3 && r.m.queued <= -1,
          fmt("triple tap: pressed faster than the steps land (on %d, %d"
              " waiting)", r.g.index, r.m.queued));
    run(&r, 1500);
    check(r.g.index == 1, fmt("triple tap: 4 -> 1 (has %d)", r.g.index, 0));
}

/* A wall inside the list (Hydro's refused boats): a hold stops at it and
 * settles, and does not keep pushing. */
static void test_wall(void)
{
    game_t g;
    rig_t r;
    unsigned ms;

    hydro_boats(&g);
    g.index = 3;
    rig(&r, &g, 0, 0);
    r.g.field = centre_of(&r.g, 3);
    run(&r, 400);
    r.in.right = 1;
    run(&r, 3000);
    check(r.g.index == 5, fmt("wall: a hold stops at boat 5 (has %d)",
                              r.g.index, 0));
    /* Still held: one try at boat 6, given up, and no more. */
    r.steps = 0;
    run(&r, 2000);
    check(r.steps == 0 && !r.m.stepping && fabsf(r.m.v - centre_of(&r.g, 5)) < 1e-5f,
          fmt("wall: a held press stops pushing (%d more tries)", r.steps,
              0));
    r.in.right = 0;
    run(&r, 1000);
    check(r.g.index == 5, "wall: boat 5 holds after the release");
    check(fabsf(r.g.field - centre_of(&r.g, 5)) < 0.03f,
          "wall: settled back on boat 5's centre");
    /* Into the wall and straight back: the way out waits only for the
     * game to have read the push, not for the cap. */
    r.in.right = 1, run(&r, 48);
    r.in.right = 0, run(&r, 48);
    r.in.left = 1, run(&r, 48);
    r.in.left = 0;
    for (ms = 0; ms < 2000 && r.g.index != 4; ms += TICK_MS)
        run(&r, TICK_MS);
    check(r.g.index == 4 && ms < 1000,
          fmt("wall: back out of it in %d ms (on %d)", (int)ms, r.g.index));
    tap(&r, 1);
    check(r.g.index == 5, "wall: and back to boat 5");
    /* Three taps into it: one try. The other two would hit it again. */
    r.steps = 0;
    for (ms = 0; ms < 3; ms++) {
        r.in.right = 1, run(&r, 16);
        r.in.right = 0, run(&r, 16);
    }
    run(&r, 2500);
    check(r.steps == 1 && r.g.index == 5,
          fmt("wall: three taps into it are one try (%d)", r.steps, 0));
    tap(&r, 1);
    check(r.g.index == 5 && fabsf(r.g.field - centre_of(&r.g, 5)) < 0.03f,
          "wall: a tap into it gives up and settles back");
    /* Unlocked (a view button, which passes through untouched). */
    r.g.refused[6] = 0;
    tap(&r, 1);
    check(r.g.index == 6, "wall: once unlocked the next tap takes boat 6");
}

/* A field that never reaches v (a screen that stopped reading the wheel,
 * or a dead zone wider than the trim can cross) still ends the step, at
 * MENU_GIVE_UP_CAP_MS, and settles where the game is. */
static void test_field_never_arrives(void)
{
    game_t g;
    rig_t r;

    hydro_boats(&g);
    g.index = 4;
    g.gain = 0.0f;                  /* the byte reaches nothing */
    rig(&r, &g, 0, 0);
    r.g.field = 0;
    run(&r, 400);
    r.in.right = 1, run(&r, 48);
    r.in.right = 0, run(&r, (MENU_GIVE_UP_CAP_MS - 200) / TICK_MS * TICK_MS);
    check(r.m.stepping, "never arrives: still trying before the cap");
    run(&r, 400);
    check(!r.m.stepping && fabsf(r.m.v - centre_of(&r.g, 4)) < 1e-5f,
          "never arrives: given up at the cap, settled on the game's item");
}

/* A wheel that never touches a key is byte-for-byte today. */
static void test_wheel_alone_is_today(void)
{
    game_t g;
    rig_t r;
    unsigned t;
    int same = 1;

    hydro_tracks(&g);
    rig(&r, &g, 1, 0.0f);
    for (t = 0; t < 4000; t += TICK_MS) {
        r.in.wheel = sinf((float)t / 600.0f) * 0.9f;
        run(&r, TICK_MS);
        same &= r.out.byte == r.in.host_byte && !r.out.digital &&
                !r.out.mute_force;
    }
    check(same, "wheel alone: every byte is the host's, no mute");
}

/* Keys take over from a wheel; the wheel takes back past menu_takeover from
 * where it was at the press, not below it, and not by sitting in the wrong
 * band. */
static void test_takeover(void)
{
    game_t g;
    rig_t r;

    hydro_tracks(&g);
    rig(&r, &g, 1, 0.1f);           /* on item 5, as the wheel left it */
    run(&r, 600);
    check(r.g.index == 5 && !r.out.digital, "takeover: wheel owns item 5");
    tap(&r, 1);
    check(r.g.index == 6 && r.out.digital && r.out.mute_force,
          "takeover: a key steps to 6 and owns the screen, wheel muted");
    run(&r, 3000);
    check(r.g.index == 6 && r.out.digital,
          "takeover: a still wheel in item 5's band does not take back");
    r.in.wheel = 0.1f - 0.07f;
    run(&r, 500);
    check(r.out.digital, "takeover: 0.07 from the press is not a takeover");
    r.in.wheel = 0.1f - 0.09f;     /* in item 5's band, not item 6's */
    run(&r, 32);
    check(!r.out.digital && !r.out.mute_force &&
          r.out.byte == r.in.host_byte,
          "takeover: 0.09 from the press is, and the byte is the wheel's");
    run(&r, 1500);
    check(r.g.index == 5, fmt("takeover: the selection follows the wheel"
                              " again (has %d)", r.g.index, 0));
    /* A later press latches a0 afresh. */
    r.in.wheel = 0.5f;
    run(&r, 1500);
    tap(&r, -1);
    check(r.out.digital, "takeover: a second press owns it again");
}

/* No wheel: the keys own every screen from the moment it is entered. */
static void test_entry_owner(void)
{
    game_t g;
    rig_t r;

    arctic(&g, 6);
    rig(&r, &g, 0, 0);
    run(&r, 600);
    check(r.out.digital && r.g.index == 3 && r.m.at == 3,
          "entry: no wheel, the keys own the screen and hold notch 3");
    rig(&r, &g, 1, -0.6f);
    run(&r, 300);
    check(!r.out.digital, "entry: a wheel owns the screen it enters");
}

/* The rate screen: a tap is one letter, a hold one letter per repeat, both
 * ways, every change is one letter, and the wheel's ends stop it. */
static void test_rate(void)
{
    game_t g;
    rig_t r;
    int start, i;

    offroad_initials(&g);
    rig(&r, &g, 0, 0);
    run(&r, 600);
    check(r.g.index == 10, "rate: entering keeps letter 10");
    tap(&r, 1);
    check(r.g.index == 11 && r.changes == 1,
          fmt("rate: a right tap is the next letter (has %d, %d changes)",
              r.g.index, r.changes));
    tap(&r, -1);
    check(r.g.index == 10, fmt("rate: a left tap is back (has %d)",
                               r.g.index, 0));
    /* Gently: Offroad's letter wheel turns at its slowest between 0.3 and
     * 0.5, about 11 letters a second already, and full lock is the
     * fastest. */
    check(r.max_field > 0.3f && r.max_field < 0.5f,
          fmt("rate: a tap deflects inside the slowest speed (peak %d/100)",
              (int)(r.max_field * 100), 0));
    r.in.next = 1;
    run(&r, 80);
    r.in.next = 0;
    run(&r, 1500);
    check(r.g.index == 11, fmt("rate: menu_next is the next letter (has %d)",
                               r.g.index, 0));
    for (i = 0; i < 3; i++) {
        start = r.g.index;
        r.changes = r.max_jump = 0;
        r.in.right = 1;
        run(&r, 1350);
        r.in.right = 0;
        run(&r, 1500);
        check(r.max_jump == 1,
              fmt("rate: a hold never jumps more than a letter (%d)",
                  r.max_jump, 0));
        check(r.changes >= 6 && r.changes <= 10 &&
              letter_delta(start, r.g.index) == r.changes,
              fmt("rate: a 1.35 s hold is one letter per repeat (%d letters,"
                  " %d net)", r.changes, letter_delta(start, r.g.index)));
    }
    /* The ends: the wheel stops there, and a tap past one is no step. */
    r.in.left = 1;
    run(&r, 9000);
    r.in.left = 0;
    run(&r, 1500);
    check(r.g.index == 0, fmt("rate: a hold reaches letter 0 (has %d)",
                              r.g.index, 0));
    r.steps = 0;
    tap(&r, -1);
    check(r.g.index == 0 && r.steps == 0, "rate: a tap past letter 0 is"
          " not even tried");
    /* A wheel steered the other way round: the keys follow the wheel, and
     * menu_next still means the next letter. */
    offroad_initials(&g);
    g.mirror = 1;
    rig(&r, &g, 0, 0);
    run(&r, 600);
    tap(&r, 1);
    check(r.g.index == 9, fmt("rate, mirrored: a right tap turns the wheel"
                              " the other way (has %d)", r.g.index, 0));
    r.in.next = 1;
    run(&r, 80);
    r.in.next = 0;
    run(&r, 1500);
    check(r.g.index == 10, fmt("rate, mirrored: menu_next is still the next"
                               " letter (has %d)", r.g.index, 0));
}

/* A press held into a new screen belongs to the last one. */
static void test_held_across_screens(void)
{
    game_t g;
    rig_t r;
    unsigned started;

    hydro_tracks(&g);
    rig(&r, &g, 0, 0);
    run(&r, 400);
    r.in.right = 1;
    run(&r, 100);
    hydro_boats(&r.g);              /* the track is chosen, boats are up */
    r.g.screen = 2;
    r.g.index = 2;
    r.g.field = r.m.v;
    started = r.m.started;
    run(&r, 1500);
    check(r.g.index == 4, fmt("held across screens: boats open on 4 and the"
                              " held press steps nothing (has %d)",
                              r.g.index, 0));
    check(r.m.started == started && started > 0,
          "held across screens: the step counts survive the new screen");
    r.in.right = 0;
    run(&r, 200);
    tap(&r, 1);
    check(r.g.index == 5, "held across screens: a fresh press steps");
}

/* Offroad's index outside its count on a screen's first pass is "not yet". */
static void test_not_yet(void)
{
    game_t g;
    rig_t r;
    menu_screen_t s;
    menu_output_t o;
    uint32_t t;

    offroad_select(&g, 2, -1);
    rig(&r, &g, 0, 0);
    describe(&r.g, &s);
    s.index = 2;
    r.in.right = 1;
    o = menu_model_tick(&r.m, &s, &r.in, 0);
    check(o.digital && r.m.queued == 0 && r.m.blocked,
          "not yet: a press held on entry does not queue");
    r.in.right = 0;
    o = menu_model_tick(&r.m, &s, &r.in, 16);
    r.in.right = 1;
    o = menu_model_tick(&r.m, &s, &r.in, 32);
    check(!r.m.stepping && r.m.queued == 1,
          "not yet: a tap waits for the first pass");
    s.index = 0;
    r.in.right = 0;
    for (t = 48; t <= 48 + MENU_SETTLE_MS + 16 && !r.m.stepping; t += 16)
        o = menu_model_tick(&r.m, &s, &r.in, t);
    check(r.m.stepping && r.m.v > 0.4f,
          "not yet: then, once the index has settled, it steps");
    (void)o;
}

/* No screen: the host's byte, and the model forgets. */
static void test_none(void)
{
    menu_model_t m;
    menu_screen_t s;
    menu_input_t in;
    menu_output_t o;
    uint32_t t;

    memset(&s, 0, sizeof s);
    memset(&in, 0, sizeof in);
    menu_model_init(&m, 0.08f);
    in.host_byte = 200;
    in.right = 1;
    o = menu_model_tick(&m, &s, &in, 0);
    check(o.byte == 200 && !o.digital && !o.mute_force && !m.active,
          "none: pass-through");
    o = menu_model_tick(&m, NULL, &in, 16);
    check(o.byte == 200 && !o.digital, "none: NULL screen passes through");
    /* Up, a press held, then gone: everything is forgotten, and the same
     * screen coming back is entered afresh. */
    s.kind = MENU_RATE;
    s.screen = 7;
    s.index = 5, s.first = 0, s.last = 46, s.push = 0.4f, s.rate_sign = 1;
    in.right = 0;
    for (t = 32; t <= 32 + MENU_SETTLE_MS + 16; t += 16)
        menu_model_tick(&m, &s, &in, t);
    in.right = 1;
    menu_model_tick(&m, &s, &in, t);
    check(m.active && m.stepping, "none: a rate screen up and stepping");
    s.kind = MENU_NONE;
    o = menu_model_tick(&m, &s, &in, t + 16);
    check(!m.active && !m.stepping && !m.queued && m.dir == 0 &&
          m.takeover == 0.08f && m.started == 1,
          "none: forgets the screen, keeps takeover and the step counts");
    s.kind = MENU_RATE;
    o = menu_model_tick(&m, &s, &in, t + 32);
    check(m.active && !m.stepping && m.blocked && m.v == 0,
          "none: the same screen back is entered, the held press blocked");
    menu_model_init(&m, 0.0f);
    check(m.takeover == 0.02f, "init: takeover clamps to 0.02");
    menu_model_init(&m, 3.0f);
    check(m.takeover == 0.5f, "init: takeover clamps to 0.5");
}

static void test_stick(void)
{
    int l = 0;

    check(menu_stick(&l, 0.45f) == 0, "stick: 0.45 is not a press");
    check(menu_stick(&l, 0.55f) == 1, "stick: 0.55 is");
    check(menu_stick(&l, 0.40f) == 1, "stick: 0.40 still is");
    check(menu_stick(&l, 0.30f) == 0, "stick: 0.30 releases");
    check(menu_stick(&l, -0.6f) == -1, "stick: -0.6 is left");
    check(menu_stick(&l, 0.6f) == 1, "stick: a flick across is right");
}

int main(void)
{
    test_tap_is_one_item();
    test_control_shows_the_defect();
    test_entry_settles();
    test_repeat();
    test_double_tap();
    test_wall();
    test_field_never_arrives();
    test_wheel_alone_is_today();
    test_takeover();
    test_entry_owner();
    test_rate();
    test_held_across_screens();
    test_not_yet();
    test_none();
    test_stick();
    if (s_failures) {
        printf("menu model: %d FAILED\n", s_failures);
        return 1;
    }
    printf("menu model: every screen, one item a press, held and handed back"
           "\n");
    return 0;
}
