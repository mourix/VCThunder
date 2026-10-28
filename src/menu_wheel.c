/* menu_wheel.c -- the select screens, as the game sees them.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Every select screen in all three cabinets reads the wheel as a POSITION and
 * maps it to an item. This file is the game's
 * half of the fix: it reads which screen is up, which item it has and the band
 * each item holds over, into a menu_screen_t for menu_model.c, and hands the
 * model's byte back to the board. It also traces each change, behind
 * [diagnostics] input_trace. It reads game memory and writes none.
 *
 * Every cell is per-title data: G (generated/<title>_profile.c) for the ETS
 * titles, AT->menu (generated/<title>_menu.c) for the win32 ones. A screen
 * whose cell is 0 is reported INAPPLICABLE once and never read. Offsets and
 * masks (the *_field, *_states_*, band and index members) mean something only
 * beside their screen's cell, and 0 is a real offset.
 */
#include "vcthunder.h"
#include "game.h"
#include "arctic.h"
#include "io.h"

#include <stdio.h>
#include <string.h>

/* Unaligned and size-free, unlike diag_peek32(): an item's index is a word at
 * an odd offset. A failed read is 0 and says so through the return value. */
static int peek(uint32_t addr, void *out, size_t n)
{
    SIZE_T got = 0;

    memset(out, 0, n);
    if (addr < 0x10000u)
        return 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr,
                             out, n, &got) && got == n;
}

static uint8_t  peek_u8(uint32_t a)  { uint8_t v;  peek(a, &v, 1); return v; }
static uint32_t peek_u32(uint32_t a) { uint32_t v; peek(a, &v, 4); return v; }
static float    peek_f(uint32_t a)   { float v;    peek(a, &v, 4); return v; }
static int16_t  peek_s16(uint32_t a) { int16_t v;  peek(a, &v, 2); return v; }

/* One line per change: the same text twice in a row is not a change. */
static void report(const char *line)
{
    static char last[256];

    if (!strcmp(line, last))
        return;
    snprintf(last, sizeof last, "%s", line);
    LOGI("menu: %s", line);
}

/* The screen census, once, so an absent cell reads INAPPLICABLE in the run's
 * own output rather than as a screen that never came up. */
static void census_once(int sel, int ini)
{
    static int done;

    if (done)
        return;
    done = 1;
    LOGI("menu: %s select screens %s, initials %s", G->id,
         sel ? "mapped" : "INAPPLICABLE (no cells)",
         ini ? "mapped" : "INAPPLICABLE (no cells)");
}

static int initials_up(void)
{
    uint32_t st = peek_u32(G->menu_ini_state);
    int on = G->menu_ini_state_kind == 1 ? st == G->menu_ini_state_want
           : G->menu_ini_state_kind == 2 ? st != G->menu_ini_state_want : 0;

    return on && peek_u32(G->menu_ini_gate) == G->menu_ini_gate_want;
}

/* ---- the screens, for the model ------------------------------------------ */

static float clamp_unit(float x) { return x < -1 ? -1 : x > 1 ? 1 : x; }

/* Hydro's track and boat lists: up, stepping (menu_sel_enable; a list that
 * has been chosen ignores the wheel), every item's band from the list itself,
 * and only the items the game will take. An unavailable track is skipped by
 * the game, so the model's list leaves it out and one press is still one
 * item. */
static int hydro_select(menu_screen_t *s)
{
    uint32_t sub, bit, list, stride, count, cur, lo_off, hi_off, k;
    int a, avail, gi;

    if (!G->menu_list_a || !G->menu_list_b || !G->menu_sel_enable ||
        !peek_u32(G->menu_sel_active))
        return 0;
    sub = peek_u32(G->menu_sel_sub);
    bit = sub < 32 ? 1u << sub : 0;
    if (!(bit & (G->menu_sel_states_a | G->menu_sel_states_b)) ||
        !peek_u32(G->menu_sel_enable))
        return 0;
    a = (bit & G->menu_sel_states_a) != 0;
    list = a ? G->menu_list_a : G->menu_list_b;
    stride = a ? G->menu_stride_a : G->menu_stride_b;
    count = a ? G->menu_count_a : G->menu_count_b;
    lo_off = a ? G->menu_band_a_lo : G->menu_band_b_lo;
    hi_off = a ? G->menu_band_a_hi : G->menu_band_b_hi;
    avail = (G->menu_avail_lists & (a ? 1u : 2u)) != 0;
    cur = peek_u32(a ? G->menu_sel_cur_a : G->menu_sel_cur_b);
    if (!stride || count > MENU_ITEMS_MAX)
        return 0;
    gi = cur >= list && (cur - list) % stride == 0 &&
         (cur - list) / stride < count ? (int)((cur - list) / stride) : -1;

    s->kind = MENU_POSITION;
    s->screen = a ? 1 : 2;
    s->n = 0;
    s->index = -1;
    for (k = 0; k < count; k++) {
        uint32_t item = list + k * stride;

        if (avail && !peek_u8(item + G->menu_avail_a))
            continue;
        if ((int)k == gi)
            s->index = s->n;
        s->lo[s->n] = peek_f(item + lo_off);
        s->hi[s->n] = peek_f(item + hi_off);
        s->n++;
    }
    s->field = peek_f(G->menu_steer + G->menu_sel_field);
    return s->n > 0;
}

/* Hydro's initials. While letters are being entered the slot is
 * y = (field - off) * scale * bins, and slot k is kept for y in
 * [k, k + 1 + hyst): the next slot up is taken past that, the one below
 * under k. Once every letter is in, the screen offers two slots, split at
 * one field value. The constants are the game's own, read where its code
 * reads them; a formula whose one-slot fraction is not 1/bins is not the one
 * this computes, and is refused. */
static int hydro_initials(menu_screen_t *s)
{
    static int refused;
    float off, scale, inv, bins, hyst, split;
    uint32_t count, slot, k;

    if (!G->menu_ini_count || !G->menu_ini_k_bins)
        return 0;
    off = peek_f(G->menu_ini_k_off);
    scale = peek_f(G->menu_ini_k_scale);
    inv = peek_f(G->menu_ini_k_inv);
    bins = peek_f(G->menu_ini_k_bins);
    hyst = peek_f(G->menu_ini_k_hyst);
    split = peek_f(G->menu_ini_k_split);
    if (!(scale > 0 && bins > 0) || inv * bins < 0.999f ||
        inv * bins > 1.001f || G->menu_ini_slot_max >= MENU_ITEMS_MAX) {
        if (!refused++)
            LOGW("menu: hydro initials REFUSED: the slot formula reads"
                 " off %g scale %g inv %g bins %g, not one this computes",
                 (double)off, (double)scale, (double)inv, (double)bins);
        return 0;
    }
    count = peek_u32(G->menu_ini_count);
    slot = peek_u32(G->menu_ini_slot);
    s->kind = MENU_POSITION;
    /* One screen for every letter: the game keeps the slot from one letter
     * to the next, and a new screen would settle it back to the middle.
     * The end mode is a different list. */
    s->screen = count < G->menu_ini_letters ? 3 : 4;
    s->field = peek_f(G->menu_steer + G->menu_ini_field);
    if (count < G->menu_ini_letters) {
        s->n = (int)G->menu_ini_slot_max + 1;
        for (k = 0; k < (uint32_t)s->n; k++) {
            s->lo[k] = clamp_unit((float)k / bins / scale + off);
            s->hi[k] = clamp_unit(((float)k + 1.0f + hyst) / bins / scale +
                                  off);
        }
        s->lo[0] = -1.0f;
        s->hi[s->n - 1] = 1.0f;
        s->index = slot <= G->menu_ini_slot_max ? (int)slot : -1;
    } else {
        s->n = 2;
        s->lo[0] = -1.0f, s->hi[0] = split;
        s->lo[1] = split, s->hi[1] = 1.0f;
        s->index = slot == G->menu_ini_end_lo ? 0
                 : slot == G->menu_ini_end_hi ? 1 : -1;
    }
    return 1;
}

/* Offroad's select screens (__UpdateSelectionControls, then the screen):
 * the index is the floor of the count times a position that settles on
 * (field - off) * scale, and on the track and truck screens a disabled item
 * hands its place to the nearest enabled one. menu_bands_counted() turns
 * that into the enabled items' bands. */
static int offroad_select(menu_screen_t *s)
{
    unsigned char en[MENU_ITEMS_MAX];
    int order[MENU_ITEMS_MAX], k, n;
    uint32_t sub, bit, list = 0, idx;
    float count, scale;

    if (!G->menu_sel_index || !G->menu_sel_k_scale ||
        !peek_u32(G->menu_sel_active))
        return 0;
    sub = peek_u32(G->menu_sel_sub);
    bit = sub < 32 ? 1u << sub : 0;
    if (!(bit & G->menu_sel_states_a))
        return 0;
    count = peek_f(G->menu_sel_count);
    scale = peek_f(G->menu_sel_k_scale);
    n = (int)count;
    if (n < 1 || n > MENU_ITEMS_MAX)
        return 0;
    if (G->menu_remap_a && (bit & G->menu_remap_a_states))
        list = G->menu_remap_a;
    else if (G->menu_remap_b && (bit & G->menu_remap_b_states))
        list = G->menu_remap_b;
    for (k = 0; k < n; k++) {
        uint32_t item = list + (uint32_t)k * G->menu_en_stride;

        en[k] = !list ||
                ((peek_u8(item + G->menu_en_flag_off) & G->menu_en_flag_mask) &&
                 !peek_u32(item + G->menu_en_lock_off));
    }
    s->kind = MENU_POSITION;
    s->screen = 10 + sub;
    if (!menu_bands_counted(s, count, peek_f(G->menu_sel_k_off), scale,
                            list ? en : NULL, order))
        return 0;
    idx = peek_u32(G->menu_sel_index);
    s->index = -1;
    for (k = 0; k < s->n; k++)
        if ((uint32_t)order[k] == idx)
            s->index = k;
    s->field = peek_f(G->menu_steer + G->menu_sel_field);
    return 1;
}

/* Offroad's initials: a rate screen. Past the snap zone the letter wheel
 * turns, slowest up to k_slow, so a step pushes halfway between the two; the
 * wheel stops at its ends, letters 0 and top. */
static int offroad_initials(menu_screen_t *s)
{
    float snap, slow;

    if (!G->menu_ini_wheel || !G->menu_ini_k_snap)
        return 0;
    snap = peek_f(G->menu_ini_k_snap);
    slow = peek_f(G->menu_ini_k_slow);
    if (!(snap > 0 && slow > snap && slow < 1))
        return 0;
    s->kind = MENU_RATE;
    s->screen = 20;
    s->rate_sign = (int32_t)G->menu_ini_rate_sign < 0 ? -1 : 1;
    s->push = (snap + slow) * 0.5f;
    s->first = 0;
    s->last = (int)G->menu_ini_top;
    s->index = (int)peek_u32(G->menu_ini_slot);
    if (s->index > s->last)
        s->index = -1;
    s->field = peek_f(G->menu_steer + G->menu_ini_field);
    return 1;
}

static int arctic_screen(menu_screen_t *s);

/* The screen up now, or MENU_NONE. Initials first: the select flag can stay
 * up beside them. An Arctic title has no G and its own reader. */
static void screen_now(menu_screen_t *s)
{
    memset(s, 0, sizeof *s);
    if (!G) {
        if (!arctic_screen(s))
            memset(s, 0, sizeof *s);
        return;
    }
    if (!G->menu_steer)
        return;
    if (G->menu_ini_state && G->menu_ini_gate && initials_up() &&
        (hydro_initials(s) || offroad_initials(s)))
        return;
    memset(s, 0, sizeof *s);
    if (hydro_select(s) || offroad_select(s))
        return;
    memset(s, 0, sizeof *s);
}

static menu_model_t s_model;
static int s_model_ready;

/* A select screen was up at the last step (menu_wheel_screen_up()). */
static int s_screen_up;

static unsigned menu_wheel_step(const menu_input_t *in, int *mute)
{
    static int last_owner = -1;
    static unsigned last_screen;
    menu_screen_t s;
    menu_output_t out;

    if (!s_model_ready) {
        menu_model_init(&s_model,
                        (float)io_config()->menu_takeover_percent / 100.0f);
        s_model_ready = 1;
    }
    screen_now(&s);
    s_screen_up = s.kind != MENU_NONE;
    out = menu_model_tick(&s_model, &s, in, (uint32_t)io_clock_ms());
    *mute = out.mute_force;
    if (io_config_input_trace()) {
        static unsigned started, landed, walls;
        int owner = s.kind == MENU_NONE ? -1 : out.digital;
        uint32_t ms = (uint32_t)io_clock_ms() - s_model.step_t0;

        if ((owner != last_owner || s.screen != last_screen) && owner >= 0)
            LOGI("menu: screen %u: %s", s.screen,
                 out.digital ? "the keys own it" : "the wheel owns it");
        last_owner = owner;
        last_screen = s.screen;
        if (s_model.started != started)
            LOGI("menu: step %s from %d, aiming at %+.3f", s_model.step_dir > 0
                 ? "right" : "left", s_model.step_from, (double)s_model.v);
        if (s_model.landed != landed)
            LOGI("menu: landed on %d after %u ms", s.index, ms);
        if (s_model.walls != walls)
            LOGI("menu: a wall past %d, given up after %u ms", s_model.step_from,
                 ms);
        started = s_model.started;
        landed = s_model.landed;
        walls = s_model.walls;
    }
    return out.byte;
}

int menu_wheel_screen_up(void) { return s_screen_up; }

unsigned menu_wheel_steer(const host_state_t *host, unsigned byte, int *mute)
{
    static int stick;
    const controls_config_t *cfg = io_config();
    menu_wheel_mode_t mode = io_config_menu_wheel();
    menu_input_t in;
    int st = 0;

    *mute = 0;
    s_screen_up = 0;
    if (mode == MENU_WHEEL_OFF)
        return byte;
    memset(&in, 0, sizeof in);
    if (mode == MENU_WHEEL_PAD && host->present)
        st = menu_stick(&stick, host->steer);
    in.left = io_key_down(&cfg->steer_left) || host->left || st < 0;
    in.right = io_key_down(&cfg->steer_right) || host->right || st > 0;
    in.prev = io_pad_down(host->buttons, &cfg->pad_menu_prev);
    in.next = io_pad_down(host->buttons, &cfg->pad_menu_next);
    in.wheel_present = mode == MENU_WHEEL_WHEEL && host->present;
    in.wheel = host->steer;
    in.host_byte = byte;
    return menu_wheel_step(&in, mute);
}

void menu_wheel_trace(unsigned steer_byte)
{
    char line[256];
    int sel = G->menu_steer && G->menu_sel_active && G->menu_sel_sub;
    int ini = G->menu_steer && G->menu_ini_state && G->menu_ini_gate &&
              G->menu_ini_slot;

    if (!io_config_input_trace())
        return;
    census_once(sel, ini);

    /* Initials first: Offroad's select flag stays up through the race and
     * the results, on a sub-screen that does not read the wheel. */
    if (ini && initials_up()) {
        float steer = peek_f(G->menu_steer + G->menu_ini_field);

        if (G->menu_ini_wheel)
            snprintf(line, sizeof line,
                     "initials letter %u wheel %.3f steer %+.3f byte %u",
                     peek_u32(G->menu_ini_slot),
                     (double)peek_f(G->menu_ini_wheel), (double)steer,
                     steer_byte);
        else
            snprintf(line, sizeof line, "initials slot %u steer %+.3f byte %u",
                     peek_u32(G->menu_ini_slot), (double)steer, steer_byte);
        report(line);
        return;
    }
    if (sel && peek_u32(G->menu_sel_active)) {
        uint32_t sub = peek_u32(G->menu_sel_sub);
        float steer = peek_f(G->menu_steer + G->menu_sel_field);
        uint32_t bit = sub < 32 ? 1u << sub : 0;

        if (G->menu_sel_cur_a && (bit & (G->menu_sel_states_a |
                                         G->menu_sel_states_b))) {
            int a = (bit & G->menu_sel_states_a) != 0;
            uint32_t item = peek_u32(a ? G->menu_sel_cur_a : G->menu_sel_cur_b);
            float lo = peek_f(item + (a ? G->menu_band_a_lo : G->menu_band_b_lo));
            float hi = peek_f(item + (a ? G->menu_band_a_hi : G->menu_band_b_hi));

            snprintf(line, sizeof line,
                     "select %s (sub %u) item %d band [%+.3f,%+.3f] steer %+.3f"
                     " byte %u", a ? "tracks" : "boats", sub,
                     peek_s16(item + G->menu_item_index), (double)lo,
                     (double)hi, (double)steer, steer_byte);
            report(line);
            return;
        }
        if (G->menu_sel_index && (bit & G->menu_sel_states_a)) {
            snprintf(line, sizeof line,
                     "select state %u index %u of %.0f (enabled %u) target %.3f"
                     " filtered %.3f notch %.3f steer %+.3f byte %u", sub,
                     peek_u32(G->menu_sel_index),
                     (double)peek_f(G->menu_sel_count),
                     peek_u32(G->menu_sel_enabled),
                     (double)peek_f(G->menu_sel_target),
                     (double)peek_f(G->menu_sel_filtered),
                     (double)peek_f(G->menu_notch), (double)steer, steer_byte);
            report(line);
            return;
        }
        /* Up, but a sub-screen that does not read the wheel. */
        snprintf(line, sizeof line, "select (sub %u, not a wheel screen)", sub);
        report(line);
        return;
    }
    report("none");
}

/* ---- the win32 titles ----------------------------------------------------- */

static volatile int32_t *s_count, *s_index;
static volatile float   *s_pos, *s_hyst;
/* The servo: where the game tells the bars to be, and how hard (arctic_io.c
 * outputs(), on the shell thread that also samples), for the trace. It
 * follows the notch, on a scale of its own, and never names a default. */
static unsigned s_servo_target, s_servo_strength;

void menu_wheel_arctic_servo(unsigned target, unsigned strength)
{
    s_servo_target = target;
    s_servo_strength = strength;
}

int menu_wheel_arctic_arm(HMODULE snow)
{
    uint8_t *img = (uint8_t *)snow;
    unsigned k;

    s_count = s_index = 0;
    s_pos = s_hyst = 0;
    if (!AT->menu) {
        LOGI("menu: %s select screens INAPPLICABLE (no cells)", AT->ids->id);
        return 0;
    }
    for (k = 0; k < AMENU_CELLS; k++) {
        uint32_t named;

        memcpy(&named, img + AT->menu->site[k], 4);
        if (named != (uint32_t)(uintptr_t)(img + AT->menu->cell[k])) {
            LOGW("menu: cell %u: the operand at +0x%06X names 0x%08X, not"
                 " +0x%06X; the select screens are not traced", k,
                 AT->menu->site[k], named, AT->menu->cell[k]);
            return 0;
        }
    }
    s_count = (volatile int32_t *)(img + AT->menu->cell[AMENU_COUNT]);
    s_index = (volatile int32_t *)(img + AT->menu->cell[AMENU_INDEX]);
    s_pos = (volatile float *)(img + AT->menu->cell[AMENU_POS]);
    s_hyst = (volatile float *)(img + AT->menu->cell[AMENU_HYST]);
    return 1;
}

/* Arctic's select screens: the bars' position into `count` equal notches,
 * the current one kept while the position is within HYST notch widths of
 * its centre (the feedback thread, snow.exe; HYST is the game's own
 * constant). Each feedback_set_notching() call resets the notch to -1, and
 * that is how a new screen shows itself, as does the count dropping between
 * screens. */
static int arctic_screen(menu_screen_t *s)
{
    static unsigned screen;
    static int last_index = -1, last_count;
    int32_t n, idx;
    float w, hyst;
    int k;

    if (!s_count)
        return 0;
    n = *s_count;
    idx = *s_index;
    hyst = *s_hyst;
    if (n < 2 || n > MENU_ITEMS_MAX || !(hyst > 0 && hyst < 2)) {
        last_index = -1;
        return 0;
    }
    if ((idx < 0 && last_index >= 0) || n != last_count)
        screen++;
    last_index = idx;
    last_count = n;
    w = 2.0f / (float)n;
    s->kind = MENU_POSITION;
    s->screen = 100 + screen;
    s->n = n;
    for (k = 0; k < n; k++) {
        float c = -1.0f + ((float)k + 0.5f) * w;

        s->lo[k] = clamp_unit(c - hyst * w);
        s->hi[k] = clamp_unit(c + hyst * w);
    }
    s->index = idx >= 0 && idx < n ? idx : -1;
    s->field = *s_pos;
    return 1;
}

void menu_wheel_arctic_trace(unsigned steer_byte)
{
    char line[160];
    int32_t n;

    if (!s_count || !io_config_input_trace())
        return;
    n = *s_count;
    if (n < 2) {
        report("none");
        return;
    }
    snprintf(line, sizeof line, "select notch %d of %d pos %+.3f byte %u"
             " servo %u/%u", (int)*s_index, (int)n, (double)*s_pos,
             steer_byte, s_servo_target, s_servo_strength);
    report(line);
}
