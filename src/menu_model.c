/* menu_model.c -- the select screens' virtual wheel, without a game.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * menu_model.h is the contract. A
 * press moves v to the neighbouring item and holds it there until the game's
 * own index changes, then settles v on whatever item the game took. So a
 * step is one item whatever the width, the calibration or the game's
 * smoothing, and a release changes nothing. A wheel the player turns past
 * `takeover` since the last press owns the screen again.
 */
#include "menu_model.h"

#include <string.h>

/* The field has reached v: about 2.5 byte counts. */
#define MENU_ARRIVE 0.02f

/* The loop's byte trim (§2.3): the error is integrated over this time
 * constant, never past MENU_TRIM_MAX counts, and only while v has not changed
 * and the field has stayed within MENU_STILL of one value for
 * MENU_SETTLE_MS. An error the
 * field is still on its way out of is the game's delay, not its
 * calibration, and integrating it winds up: a held key on Hydro's initials
 * ran three slots a step. The game reads the byte up to ~95 ms late there
 * and then all at once, so a field that has not moved yet can still be on
 * its way; MENU_SETTLE_MS is that delay and three frames more. A field
 * stuck in a dead zone stays still and is walked out of. */
#define MENU_TRIM_TAU_MS 250.0f
#define MENU_TRIM_MAX    64.0f
#define MENU_STILL       0.01f

/* A tick longer than this is a stall, not a measure of anything; the trim
 * takes it as this long rather than jumping by its whole length. */
#define MENU_DT_MAX_MS   100u

/* Taps waiting for a step, either way. */
#define MENU_QUEUE_MAX   8

static float absf(float x) { return x < 0 ? -x : x; }

static int index_valid(const menu_screen_t *s)
{
    if (s->kind == MENU_RATE)
        return s->index >= 0;
    return s->index >= 0 && s->index < s->n && s->n <= MENU_ITEMS_MAX;
}

/* Where v rests on item k: its band's middle, or the rate screen's snap
 * zone. */
static float rest(const menu_screen_t *s, int k)
{
    return s->kind == MENU_RATE ? 0.0f : (s->lo[k] + s->hi[k]) * 0.5f;
}

/* Where a step from `from` to its neighbour `to` aims: the middle of the
 * part of to's band that is outside from's. Inside the overlap the game
 * keeps `from`, so a band's own middle can sit exactly on the line the step
 * has to cross (Hydro's initials). */
static float aim(const menu_screen_t *s, int from, int to)
{
    if (to > from) {
        float a = s->hi[from] > s->lo[to] ? s->hi[from] : s->lo[to];
        return (a + s->hi[to]) * 0.5f;
    } else {
        float b = s->lo[from] < s->hi[to] ? s->lo[from] : s->hi[to];
        return (s->lo[to] + b) * 0.5f;
    }
}

/* +1 when the item index rises with the wheel turning right: always, on a
 * position screen, whose bands rise with the index. */
static int index_sign(const menu_screen_t *s)
{
    if (s->kind == MENU_RATE)
        return s->rate_sign < 0 ? -1 : 1;
    return 1;
}

/* The item next to k on the wheel's `dir` side, or -1 at an end. */
static int neighbour(const menu_screen_t *s, int k, int dir)
{
    int t = k + dir;

    if (t < 0 || t >= s->n)
        return -1;
    return t;
}

static void enter(menu_model_t *m, const menu_screen_t *s,
                  const menu_input_t *in, int dir, uint32_t now)
{
    float takeover = m->takeover;
    unsigned started = m->started, landed = m->landed, walls = m->walls;

    memset(m, 0, sizeof *m);
    m->takeover = takeover;
    m->started = started, m->landed = landed, m->walls = walls;
    m->active = 1;
    m->screen = s->screen;
    m->owner = in->wheel_present ? MENU_ANALOG : MENU_DIGITAL;
    m->at = index_valid(s) ? s->index : -1;
    m->v = m->at >= 0 ? rest(s, m->at) : 0.0f;
    /* With the keys, the screen opens where a wheel at rest would put it. */
    m->settling = m->owner == MENU_DIGITAL;
    m->settle_index = -2;
    m->settle_t = now;
    m->a0 = in->wheel;
    m->last_ms = now;
    m->still_at = s->field;
    m->still_v = m->v;
    m->still_since = now;
    /* A press held across the change of screen belongs to the last one. */
    m->dir = dir;
    m->blocked = dir != 0;
}

/* Begin one step from the game's current item. 0: nothing to step to (an
 * end), or the screen has not made its first pass. */
static int start(menu_model_t *m, const menu_screen_t *s, int dir,
                 uint32_t now)
{
    if (!index_valid(s))
        return 0;
    if (s->kind == MENU_RATE) {
        int t = s->index + dir * index_sign(s);

        /* The wheel is clamped at its ends: a push past one turns nothing. */
        if (t < s->first || t > s->last)
            return 0;
        m->v = (float)dir * s->push;
    } else {
        int t = neighbour(s, s->index, dir);

        if (t < 0)
            return 0;
        m->v = aim(s, s->index, t);
    }
    m->stepping = 1;
    m->started++;
    m->step_dir = dir;
    m->step_from = s->index;
    m->step_t0 = now;
    m->arrived = 0;
    return 1;
}

static void progress(menu_model_t *m, const menu_screen_t *s, uint32_t now)
{
    /* Landed, on whatever item the game took: a field that lags passes
     * the items in order, so the first change is the neighbour. */
    if (index_valid(s) && s->index != m->step_from) {
        m->stepping = 0;
        m->landed++;
        m->at = s->index;
        m->v = rest(s, s->index);
        return;
    }
    if (!m->arrived && absf(s->field - m->v) < MENU_ARRIVE) {
        m->arrived = 1;
        m->arrived_t = now;
    }
    if ((m->arrived && now - m->arrived_t >= MENU_GIVE_UP_MS) ||
        now - m->step_t0 >= MENU_GIVE_UP_CAP_MS) {
        /* A wall: the game would not take the item. Settle on the one it
         * has. Taps waiting to go the same way would only hit it again;
         * taps the other way are the way out. */
        m->stepping = 0;
        m->walls++;
        if (m->queued * m->step_dir > 0)
            m->queued = 0;
        m->blocked = m->dir != 0;
        if (index_valid(s)) {
            m->at = s->index;
            m->v = rest(s, s->index);
        }
    }
}

static unsigned to_byte(menu_model_t *m, const menu_screen_t *s, uint32_t dt,
                        uint32_t now)
{
    float b;

    /* A new v is a new transient as surely as a moving field is. */
    if (absf(s->field - m->still_at) > MENU_STILL || m->v != m->still_v) {
        m->still_at = s->field;
        m->still_v = m->v;
        m->still_since = now;
    }
    if (dt && now - m->still_since >= MENU_SETTLE_MS) {
        m->trim += (m->v - s->field) * 127.5f * (float)dt / MENU_TRIM_TAU_MS;
        if (m->trim > MENU_TRIM_MAX)
            m->trim = MENU_TRIM_MAX;
        if (m->trim < -MENU_TRIM_MAX)
            m->trim = -MENU_TRIM_MAX;
    }
    b = 127.5f + m->v * 127.5f + m->trim;
    if (b <= 0)
        return 0;
    if (b >= 255)
        return 255;
    return (unsigned)(b + 0.5f);
}

void menu_model_init(menu_model_t *m, float takeover)
{
    memset(m, 0, sizeof *m);
    if (!(takeover >= 0.02f))
        takeover = 0.02f;
    if (takeover > 0.5f)
        takeover = 0.5f;
    m->takeover = takeover;
}

menu_output_t menu_model_tick(menu_model_t *m, const menu_screen_t *s,
                              const menu_input_t *in, uint32_t now)
{
    menu_output_t out;
    int dir, item;
    uint32_t dt;

    out.byte = in->host_byte;
    out.digital = 0;
    out.mute_force = 0;

    if (!s || s->kind == MENU_NONE) {
        if (m->active) {
            unsigned started = m->started, landed = m->landed,
                     walls = m->walls;

            menu_model_init(m, m->takeover);
            m->started = started, m->landed = landed, m->walls = walls;
        }
        return out;
    }
    /* One direction on the wheel; two presses that disagree are none. */
    dir = (in->right ? 1 : 0) - (in->left ? 1 : 0);
    item = ((in->next ? 1 : 0) - (in->prev ? 1 : 0)) * index_sign(s);
    if (dir && item && dir != item)
        dir = 0;
    else if (!dir)
        dir = item;

    if (!m->active || s->screen != m->screen)
        enter(m, s, in, dir, now);

    dt = now - m->last_ms;
    if (dt > MENU_DT_MAX_MS)
        dt = MENU_DT_MAX_MS;
    m->last_ms = now;

    if (dir != m->dir) {
        m->dir = dir;
        m->blocked = 0;
        if (dir) {
            if (m->owner == MENU_ANALOG) {
                m->owner = MENU_DIGITAL;
                m->stepping = 0;
                m->queued = 0;
                m->trim = 0;
                m->at = index_valid(s) ? s->index : -1;
                if (m->at >= 0)
                    m->v = rest(s, m->at);
            }
            m->a0 = in->wheel;
            if (m->queued * dir < MENU_QUEUE_MAX)
                m->queued += dir;
            m->repeat_at = now + MENU_REPEAT_FIRST_MS;
        }
    }

    if (m->owner == MENU_DIGITAL && in->wheel_present &&
        absf(in->wheel - m->a0) > m->takeover) {
        m->owner = MENU_ANALOG;
        m->stepping = 0;
        m->queued = 0;
    }

    if (m->owner == MENU_DIGITAL && m->settling) {
        /* Centre until the game has kept one item for MENU_SETTLE_MS. A
         * screen can open with its index still on the last screen's
         * position (Offroad's filter), and the item a cabinet's wheel at
         * rest gives is where the game then settles. Presses wait. */
        if (!index_valid(s) || s->index != m->settle_index) {
            m->settle_index = index_valid(s) ? s->index : -2;
            m->settle_t = now;
        } else if (now - m->settle_t >= MENU_SETTLE_MS) {
            m->settling = 0;
            m->at = s->index;
            m->v = rest(s, m->at);
        }
        if (m->settling) {
            out.byte = MENU_CENTRE;
            out.digital = 1;
            out.mute_force = 1;
            return out;
        }
    }

    if (m->owner == MENU_DIGITAL) {
        if (m->stepping)
            progress(m, s, now);
        while (!m->stepping && m->queued) {
            int d = m->queued > 0 ? 1 : -1;

            if (!index_valid(s))
                break;
            m->queued -= d;
            start(m, s, d, now);
        }
        if (!m->stepping && !m->queued && m->dir && !m->blocked &&
            (int32_t)(now - m->repeat_at) >= 0 &&
            start(m, s, m->dir, now))
            m->repeat_at = now + MENU_REPEAT_MS;
        /* The keys took over from a wheel on a screen without an item yet.
         * After that v holds, and the game is never followed: v only rests
         * in the band of the item the game has, and the game keeps an item
         * while the field is in its band, so an index that differs is the
         * field still on its way. */
        if (!m->stepping && m->at < 0 && index_valid(s)) {
            m->at = s->index;
            m->v = rest(s, m->at);
        }

        out.byte = to_byte(m, s, dt, now);
        out.digital = 1;
        out.mute_force = 1;
    }
    return out;
}

int menu_bands_counted(menu_screen_t *s, float count, float off, float scale,
                       const unsigned char *enabled, int *order)
{
    int raw = (int)count, k, n = 0;

    s->n = 0;
    if (raw < 1 || raw > MENU_ITEMS_MAX || !(scale > 0))
        return 0;
    for (k = 0; k < raw; k++) {
        float lo = k ? ((float)k / count) / scale + off : -1.0f;
        float hi = k < raw - 1 ? ((float)(k + 1) / count) / scale + off : 1.0f;
        int e = k, d;

        /* The nearest enabled item, scanning up from 0 so a tie goes low. */
        if (enabled) {
            int best = -1, bd = 0;

            for (d = 0; d < raw; d++) {
                int dist = d > k ? d - k : k - d;

                if (enabled[d] && (best < 0 || dist < bd))
                    best = d, bd = dist;
            }
            if (best < 0)
                return 0;
            e = best;
        }
        if (n && order[n - 1] == e) {
            s->hi[n - 1] = hi;
            continue;
        }
        order[n] = e;
        s->lo[n] = lo;
        s->hi[n] = hi;
        n++;
    }
    s->n = n;
    return n;
}

int menu_stick(int *latch, float x)
{
    if (*latch > 0 && x < MENU_STICK_RELEASE)
        *latch = 0;
    else if (*latch < 0 && x > -MENU_STICK_RELEASE)
        *latch = 0;
    if (!*latch) {
        if (x > MENU_STICK_PRESS)
            *latch = 1;
        else if (x < -MENU_STICK_PRESS)
            *latch = -1;
    }
    return *latch;
}
