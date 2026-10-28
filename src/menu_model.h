/* menu_model.h -- the select screens' virtual wheel, without a game.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Every select screen in all three cabinets reads the wheel as a POSITION, so
 * a key or a stick that springs back to centre pulls the selection back with
 * it. This is the model that fixes that: a
 * virtual wheel position v that a digital press moves one item at a time and
 * that stays where it was left, closed on the game's own index, and turned
 * into the board's steering byte closed on the field the screen reads.
 *
 * It is pure on purpose: no windows.h, no G, no clock of its own. The caller
 * reads the game into a menu_screen_t and the host into a menu_input_t each
 * tick, and tests/menu_model_test.c drives it against modelled screens with
 * no game and no device. menu_wheel.c is the half that reads the game.
 *
 * Units: v, the bands and the screen's field are the game's own steering
 * units, -1..+1, and the field is assumed to rise with the byte. The wheel in
 * menu_input_t is the host's normalised reading, -1..+1.
 */
#ifndef VCT_MENU_MODEL_H
#define VCT_MENU_MODEL_H

#include <stdint.h>

/* Hydro's initials are the most items any screen has: 41. */
#define MENU_ITEMS_MAX 64

typedef enum {
    MENU_NONE = 0,      /* no select screen: everything passes through      */
    MENU_POSITION,      /* the item is where the field is                   */
    MENU_RATE           /* the field turns a wheel of items (Offroad's      */
                        /* initials); centre snaps to the nearest           */
} menu_kind_t;

/* One select screen as the game has it this tick. */
typedef struct {
    menu_kind_t kind;
    /* Changes whenever the game moves to another screen, so the model starts
     * that screen afresh. Any value; only a change means anything. */
    unsigned screen;
    /* Position screens: the game keeps item k while the field is inside
     * [lo[k], hi[k]]. The bands rise with k and may overlap; that overlap is
     * the screen's hysteresis. v rests at a band's middle, and a step aims
     * at the middle of the part of the next band outside the current one.
     * Every item listed is one the game will select: a descriptor leaves
     * out what the game skips. */
    int   n;
    float lo[MENU_ITEMS_MAX], hi[MENU_ITEMS_MAX];
    /* Rate screens: +1 when steering right raises the index, -1 when it
     * lowers it; the deflection a step pushes with, past the snap zone and
     * inside the slowest speed; and first..last, the index's ends. */
    int   rate_sign;
    float push;
    int   first, last;
    /* The game's current item, and the field this screen reads. An index
     * outside 0..n-1 (position) or below 0 (rate) is "not yet": the screen
     * has not made its first pass. */
    int   index;
    float field;
} menu_screen_t;

/* The host this tick. */
typedef struct {
    /* Presses in the wheel's direction: keys, the hat, the D-pad, a stick
     * (menu_stick()). Left and right together are no press. */
    int left, right;
    /* Presses in the item's direction: the menu_prev/menu_next bindings. */
    int prev, next;
    /* A wheel that holds its position, and where it is. */
    int   wheel_present;
    float wheel;
    /* The byte the board would send with this model out of the way. */
    unsigned host_byte;
} menu_input_t;

typedef struct {
    unsigned byte;      /* the steering byte to send                         */
    int digital;        /* 1: the byte is the model's; 0: it is host_byte    */
    int mute_force;     /* the keys own the screen: send the wheel no force  */
} menu_output_t;

typedef enum { MENU_ANALOG = 0, MENU_DIGITAL } menu_owner_t;

typedef struct {
    float takeover;     /* [analog] menu_takeover, of half the wheel's range */

    int active;         /* a screen was up last tick                         */
    unsigned screen;
    menu_owner_t owner;
    float v;            /* the virtual wheel                                 */
    int at;             /* the item v rests on, -1 for none yet              */
    int settling;       /* entered with the keys; centre until it settles    */
    int settle_index;
    uint32_t settle_t;
    float a0;           /* the real wheel when the keys last took over       */
    float trim;         /* byte offset the loop has learned (§2.3)           */
    float still_at;     /* the field has stayed near this, and v at this ... */
    float still_v;
    uint32_t still_since; /* ... since this                                  */
    uint32_t last_ms;

    int dir;            /* the press held now: -1, 0, +1 (wheel direction)   */
    int queued;         /* taps not yet stepped, signed                      */
    int blocked;        /* a wall was hit; no repeat until released          */
    uint32_t repeat_at; /* when a held press may step again                  */

    int stepping;       /* a step is in flight                               */
    int step_dir, step_from;
    uint32_t step_t0;
    int arrived;        /* the field has reached v during this step          */
    uint32_t arrived_t;

    /* What happened, for a trace: steps begun, landed, given up at a wall.
     * They count for the model's life, across screens. */
    unsigned started, landed, walls;
} menu_model_t;

/* A model at rest, inactive. `takeover` is clamped to the setting's range. */
void menu_model_init(menu_model_t *m, float takeover);

/* One tick. `now_ms` is any millisecond clock; it may wrap. With no screen
 * up (MENU_NONE) the output is host_byte and the model forgets everything. */
menu_output_t menu_model_tick(menu_model_t *m, const menu_screen_t *s,
                              const menu_input_t *in, uint32_t now_ms);

/* A counted list's bands (Offroad's select screens): the game's index is
 * ftol(count * x), clamped to ftol(count) - 1, where x settles on
 * (field - off) * scale; and where `enabled` is given, the index is then
 * moved to the nearest enabled item, the lower on a tie. Fills s->n, lo and
 * hi with the enabled items' bands in field units, and `order[k]` with each
 * one's index in the game's list, so the game's index can be found in it.
 * Returns s->n, 0 when nothing is enabled. */
int menu_bands_counted(menu_screen_t *s, float count, float off, float scale,
                       const unsigned char *enabled, int *order);

/* A stick that springs back, as a digital press: past MENU_STICK_PRESS it is
 * down, below MENU_STICK_RELEASE it is up again. `latch` holds the direction
 * between calls (start it at 0). Returns -1, 0 or +1. */
#define MENU_STICK_PRESS   0.5f
#define MENU_STICK_RELEASE 0.35f
int menu_stick(int *latch, float x);

/* The timing every screen shares (§2.2). A press steps at once; held, it
 * steps again after MENU_REPEAT_FIRST_MS, then every MENU_REPEAT_MS, each
 * step only once the last has landed. A step whose index has not changed
 * MENU_GIVE_UP_MS after the field reached v, or MENU_GIVE_UP_CAP_MS after
 * it began, has hit a wall and settles back. */
#define MENU_REPEAT_FIRST_MS 350u
#define MENU_REPEAT_MS       120u
#define MENU_GIVE_UP_MS      400u
#define MENU_GIVE_UP_CAP_MS  1500u

/* How long a field must stay put before it is taken as settled: the
 * longest delay measured between a byte and the game reading it (~95 ms,
 * Hydro's initials) and three frames more. The byte trim waits for it, and
 * so does a screen entered with the keys. */
#define MENU_SETTLE_MS 200u

/* The board's centre byte: what a wheel at rest, and no key, sends. */
#define MENU_CENTRE 128u

#endif
