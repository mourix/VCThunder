/* ffb_events.c -- the game's OWN force-feedback units, read rather than summed.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * io_dac_out is one signed byte and it is the SUM; a wheel wants that, a pad
 * cannot use it. Both titles compose the force from named cells, so this reads
 * the decomposition and rumble.c consumes four channels from it. IT WRITES
 * NOTHING AND HOOKS ONE THING: cells are read on the frame-boundary pass
 * through diag_peek32(), and the hook is Offroad's FFB tick, which CONSUMES its
 * collision cell on the game's own 60 Hz thread.
 */
#include "vcthunder.h"
#include "io.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* A window of the census, matched to the ffb_trace window so the two read
 * against each other in the log. */
#define FFB_WINDOW_MS   5000u
/* Event lines per window: a log that scrolls its own evidence away is not
 * evidence. */
#define FFB_EVENT_CAP   24u

static int      s_on;
static int      s_reported;
/* Whether this profile carries anything the PAD can use. Separate from s_on on
 * purpose: the trace is a diagnostic switch, the channels are not, and they
 * must never be the same flag. */
static int      s_have_channels;

/* Hydro. */
static float    s_imp_a, s_imp_b;
static unsigned s_n_collide, s_n_imp_a, s_n_imp_b;
static float    s_collide_min, s_collide_max;
static float    s_imp_a_max, s_imp_b_max;
/* Offroad, per slot. A statistic one dead cell can dominate is not a statistic,
 * max|value| over tireJolts was won by a constant in 70 of 77 windows, so
 * each slot is correlated against the force commanded on the same pass. */
#define FFB_SLOTS_MAX   40

typedef struct {
    double sx, sxx, sxy;
    float  lo, hi;
} ffb_slot_t;

typedef struct {
    const char *name;
    unsigned    count;                  /* slots actually sampled           */
    unsigned    n;                      /* samples this window              */
    double      sy, syy;                /* the commanded force, shared      */
    float       last;                   /* max |value|, for the mark line   */
    unsigned    peak_index;
    ffb_slot_t  slot[FFB_SLOTS_MAX];
} ffb_array_t;

static ffb_array_t s_tire  = { "tireJolts",  0, 0, 0, 0, 0, 0, {{0,0,0,0,0}} };
static ffb_array_t s_shock = { "shocksHist", 0, 0, 0, 0, 0, 0, {{0,0,0,0,0}} };
static ffb_array_t s_last  = { "lastShock",  0, 0, 0, 0, 0, 0, {{0,0,0,0,0}} };

static int      s_mode = -1;
static unsigned s_n_mode;

static int      s_force_type = -1;
static float    s_hit;
static unsigned s_n_hit, s_n_type;
static float    s_hit_max;
static unsigned s_type_seen;            /* bitmask of the types observed */

/* Marker keys: a driver cannot read a clock mid-race, so F5..F8 stamp the log
 * with the ini's name for them and a snapshot of every term. F5..F8 because
 * neither game binds them; ESCAPE is not a candidate at any price. */
static const int k_mark_vk[4] = { VK_F5, VK_F6, VK_F7, VK_F8 };
static int      s_mark_down[4];

/* When each channel last FIRED: by the time a hand reaches a key the value has
 * decayed, and a small number reads the same as a late one. */
static DWORD    s_fired_imp_a, s_fired_imp_b, s_fired_collide, s_fired_hit;

/* --- Offroad's hit channel, taken at the source ------------------------
 *
 * _ffbnew_FFBFunc reads _ffbnew_CurHitVal and zeroes it on the game's 60 Hz
 * thread, so a poll sees a collision essentially never. The hook reads and
 * changes nothing; its signature is the call site's. */
static float peek_float(uint32_t va);          /* defined below */
static void  offer_impact(float v);            /* defined below */

typedef int (__cdecl *ffb_hit_fn_t)(int, int);
static ffb_hit_fn_t s_real_ffbfunc;
static volatile LONG s_hook_hits;
static float    s_hook_peak;
static float    s_hook_last;

static int __cdecl ffb_hit_hook(int a, int b)
{
    if ((s_on || s_have_channels) && G->ffb_hit_val) {
        float v = peek_float(G->ffb_hit_val);
        float m = v < 0.0f ? -v : v;

        if (m > 0.0f) {
            s_hook_last = v;
            if (m > s_hook_peak)
                s_hook_peak = m;
            InterlockedIncrement(&s_hook_hits);
            s_fired_hit = GetTickCount();
            offer_impact(v);
        }
    }
    return s_real_ffbfunc(a, b);
}

static void age(char *out, size_t size, DWORD when)
{
    if (!when)
        snprintf(out, size, "never");
    else
        snprintf(out, size, "%lums ago",
                 (unsigned long)(GetTickCount() - when));
}

/* --- what the pad consumes ---------------------------------------------
 *
 * Three channels plus the engine: what two motors can express. The impact is
 * LATCHED and consumed by reading, so a burst cannot fire twice.
 *
 * THE LATCH IS CROSS-THREAD AND IS EXCHANGED, NOT ASSIGNED: written by the
 * game's 60 Hz FFB tick through the hook, read-and-cleared by whichever thread
 * is polling. A read-then-store loses a hit landing in between, on the one
 * channel that is hooked precisely because a poll cannot see it. Held as the
 * float's BITS, which is what Interlocked* operates on. */
static volatile LONG s_ev_impact_bits;  /* latched float, -1..+1            */

static float impact_bits_to_float(LONG bits)
{
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}
static float    s_ev_texture;           /* 0..1, already normalised by game */
static float    s_ev_body;              /* 0..1, normalised HERE, see below */
/* The body cells are in the GAME'S OWN units (lastShockVals ranged -15..+19
 * over a race) and this host has no scale for them, so a slowly decaying
 * running peak normalises: it adapts per title and per vehicle with nobody
 * writing a divisor down. Such a channel is a DEVIATION, never a magnitude,
 * the cells rest near -10 or +5 depending on the corner, so an absolute
 * reading pins the motor while parked, hence the baseline. */
typedef struct {
    float base;
    float peak;
    int   seeded;
} ffb_chan_t;

/* Only the body needs one: the surface channel is a magnitude the game already
 * normalised, where zero means smooth, and baselining it deleted every steady
 * surface. */
static ffb_chan_t s_ch_body;

/* --- the engine channel ------------------------------------------------
 *
 * Hydro's MOTOR, BOOSTER, FWDCAV and REVCAV units are computed inline and
 * stored in no cell, so no anchor reaches them. They are FAST by construction;
 * the steering spring is SLOW, because it follows a hand. A 20 ms pole cuts at
 * ~8 Hz, above anything an arm does and below the rate these units run at. */
static float    s_engine_lp;
static int      s_engine_seeded;
static float    s_ev_engine;
/* Boost FIRING, from the game's chain; see boost_poll(). */
static int      s_ev_boosting;
static float    s_fuel_last;
static double   s_fuel_fell_ms;
static int      s_fuel_seen;
static unsigned s_n_boost_on;
#define BOOST_FUEL_HOLD_MS  120.0
static int      s_ev_boost;          /* the RAW state, not a flag */
static unsigned s_boost_seen;        /* which values a run actually shows */
static unsigned s_boost_n[8];        /* and how many SAMPLES each one held */
static int      s_boost_last = -1;
#define ENGINE_POLE_MS   20.0f

/* An envelope, not an instantaneous magnitude: |high-pass| of a noisy signal
 * wobbles at a few hertz and is felt as knocking. The release must exceed the
 * period of the game's own oscillator, 300 ms reproduces the knock at 27%
 * ripple, 600 ms holds 15%, and it is also what a settle FEELS like, because
 * an envelope only rises on a sample exceeding it, so following a throttle
 * down to cruise runs at the release and not at the attack. */
#define ENGINE_ATTACK_MS     30.0f
#define ENGINE_RELEASE_MS   600.0f

#define CHAN_BASE_MS            800.0f  /* one pole on the running baseline */
#define CHAN_PEAK_HALFLIFE_MS  8000.0f
#define CHAN_PEAK_FLOOR     0.25f       /* never divide by silence          */

/* MILLISECONDS, NOT PER-SAMPLE COEFFICIENTS: update_force_feedback() runs once
 * per control tick, and the tick is each title's own period (33 ms Hydro,
 * 16 ms Offroad), so a fraction-per-call means two different time constants.
 *
 * NO CEILING ON dt here, where rumble.c clamps: its clamp stops a long gap
 * integrating into a jolt at a motor, but these are baselines, and after a
 * stall the right answer IS to snap to the current reading. */
static float pole(float dt_ms, float tau_ms)
{
    return 1.0f - (float)exp(-(double)dt_ms / (double)tau_ms);
}

/* `normalise` is for channels in the GAME'S OWN units, which this host has no
 * scale for; one the game already normalised passes through. */
static float chan_update(ffb_chan_t *c, float v, int normalise,
                         unsigned dt_ms)
{
    float dev;

    if (!c->seeded) {
        /* Seed, do not report: the first sample has no baseline behind it, and
         * treating it as a deviation would fire every channel at load. */
        c->seeded = 1;
        c->base = v;
        return 0.0f;
    }
    c->base += (v - c->base) * pole((float)dt_ms, CHAN_BASE_MS);
    dev = v - c->base;
    if (dev < 0.0f)
        dev = -dev;
    if (!normalise)
        return dev > 1.0f ? 1.0f : dev;
    c->peak *= (float)pow(0.5, (double)dt_ms / CHAN_PEAK_HALFLIFE_MS);
    if (dev > c->peak)
        c->peak = dev;
    if (c->peak <= CHAN_PEAK_FLOOR)
        return 0.0f;
    dev /= c->peak;
    return dev > 1.0f ? 1.0f : dev;
}

/* The surface channel's own census: three rounds went on guessing what tarmac
 * and dirt read, from feel alone. */
static unsigned s_tex_n;
static double   s_tex_sum;
static float    s_tex_min, s_tex_max;

static unsigned s_accum_n;
static double   s_accum_sum;
static float    s_accum_max;

static DWORD    s_window;
static unsigned s_events, s_suppressed;

/* diag_peek32() answers 0 for an address that would fault, which is right
 * here: an unreadable cell has no event in it, and this must never be the
 * thing that takes a race down. */
static float peek_float(uint32_t va)
{
    uint32_t raw = 0;
    float f;

    if (!va || !diag_peek32(va, &raw))
        return 0.0f;
    memcpy(&f, &raw, sizeof f);
    /* A NaN in a cell we do not own is a reading, not a crash, but it must
     * not reach the arithmetic below. */
    if (!(f > -1.0e6f && f < 1.0e6f))
        return 0.0f;
    return f;
}

/* THE SWITCH GATES THE TRACE. s_have_channels opens ffb_events_poll() to both
 * shipped titles, so every logging path below has to ask s_on again or the
 * census runs for a diagnostic nobody turned on. */
static void event(const char *fmt, ...)
{
    va_list ap;
    char line[256];

    if (!s_on)
        return;
    if (s_events >= FFB_EVENT_CAP) {
        s_suppressed++;
        return;
    }
    s_events++;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    LOGI("ffb: %s", line);
}

/* Poll one array term against the force commanded on the same pass. `count` is
 * the profile's, from the next symbol along: this host does not know how long
 * the game's arrays are and must not pretend to. */
static void poll_array(ffb_array_t *a, uint32_t base, unsigned count,
                       float force)
{
    float top = 0.0f;
    unsigned i, at = 0;

    if (!base || !count)
        return;
    if (count > FFB_SLOTS_MAX)
        count = FFB_SLOTS_MAX;
    a->count = count;
    for (i = 0; i < count; i++) {
        float v = peek_float(base + i * 4u);
        float m = v < 0.0f ? -v : v;
        ffb_slot_t *sl = &a->slot[i];

        if (!a->n || v < sl->lo) sl->lo = v;
        if (!a->n || v > sl->hi) sl->hi = v;
        sl->sx += v;
        sl->sxx += (double)v * v;
        sl->sxy += (double)v * force;
        if (m > top) {
            top = m;
            at = i;
        }
    }
    a->sy += force;
    a->syy += (double)force * force;
    a->n++;
    a->last = top;
    a->peak_index = at;
}

/* Pearson's r between one slot and the commanded force. 0 when either side
 * never moved: a dead cell has no correlation, not a perfect one. */
static double slot_r(const ffb_array_t *a, unsigned i)
{
    const ffb_slot_t *sl = &a->slot[i];
    double n = (double)a->n;
    double vx = n * sl->sxx - sl->sx * sl->sx;
    double vy = n * a->syy - a->sy * a->sy;
    double den;

    if (n < 2.0 || vx <= 0.0 || vy <= 0.0)
        return 0.0;
    den = sqrt(vx) * sqrt(vy);
    return den > 0.0 ? (n * sl->sxy - sl->sx * a->sy) / den : 0.0;
}

static void report_array(ffb_array_t *a)
{
    unsigned i, k, dead = 0, best[3] = { 0, 0, 0 };
    double br[3] = { 0.0, 0.0, 0.0 };
    char line[192];
    int at = 0;

    if (!a->n || !a->count)
        return;
    for (i = 0; i < a->count; i++) {
        double r = slot_r(a, i);
        double m = r < 0.0 ? -r : r;

        if (a->slot[i].hi == a->slot[i].lo) {
            dead++;
            continue;
        }
        for (k = 0; k < 3; k++) {
            double cur = br[k] < 0.0 ? -br[k] : br[k];
            if (m > cur) {
                unsigned j;
                for (j = 2; j > k; j--) {
                    br[j] = br[j - 1];
                    best[j] = best[j - 1];
                }
                br[k] = r;
                best[k] = i;
                break;
            }
        }
    }
    line[0] = '\0';
    for (k = 0; k < 3; k++) {
        if (br[k] == 0.0)
            continue;
        at += snprintf(line + at, sizeof line - (size_t)at,
                       "%s[%u] r=%+.2f range %.2f..%.2f",
                       at ? ", " : "", best[k], br[k],
                       (double)a->slot[best[k]].lo,
                       (double)a->slot[best[k]].hi);
    }
    /* The dead count says whether an array is mostly padding or constants,
     * which a maximum alone cannot show. */
    LOGI("ffb: window %-10s %u/%u slots moved; strongest vs commanded force: %s",
         a->name, a->count - dead, a->count, at ? line : "none");

    memset(a->slot, 0, sizeof a->slot);
    a->n = 0;
    a->sy = a->syy = 0.0;
}

/* Offer an impact to the pad; bigger wins, as the game's impulse arming does. */
static void offer_impact(float v)
{
    float m = v < 0.0f ? -v : v;
    LONG want;

    memcpy(&want, &v, sizeof want);
    /* Compare and store as ONE operation, loser retries: this runs on the
     * game's FFB thread while the poll thread may be taking the latch. */
    for (;;) {
        LONG seen = s_ev_impact_bits;
        float have = impact_bits_to_float(seen);

        if (have < 0.0f)
            have = -have;
        if (m <= have)
            return;
        if (InterlockedCompareExchange(&s_ev_impact_bits, want, seen) == seen)
            return;
    }
}

/* Is the title's own path RUNNING? Having anchors is not the same thing:
 * Offroad gates on _ffbnew_curMode == 1 and its menus sit at 0, where the cells
 * go still by design while the byte still carries a force. */
static int ffb_path_live(void)
{
    uint32_t raw = 0;

    if (!G->ffb_mode)
        return 1;                       /* no mode cell: always live */
    if (!diag_peek32(G->ffb_mode, &raw))
        return 1;
    return raw == 1u;
}

/* The chain game.h describes: pick the local player's entry, deref the root,
 * take the hops, read the field. 0 on any unreadable link or an index out of
 * range, which is what "not boosting" also reads as. */
static int boost_cell(uint32_t *out)
{
    uint32_t slot = 0, p = 0;

    if (G->boost_index &&
        (!diag_peek32(G->boost_index, &slot) || slot >= G->boost_index_n))
        return 0;
    if (!diag_peek32(G->boost_root + slot * 4u, &p) || !p)
        return 0;
    if (G->boost_hop_n >= 1 && (!diag_peek32(p + G->boost_hop1, &p) || !p))
        return 0;
    if (G->boost_hop_n >= 2 && (!diag_peek32(p + G->boost_hop2, &p) || !p))
        return 0;
    return diag_peek32(p + G->boost_field, out);
}

/* Firing or not, per tick. A flag is read as it stands. Fuel is firing while
 * it falls, held BOOST_FUEL_HOLD_MS so the second tick of a frame, which sees
 * no physics step and so no fall, does not read as a stop. A rise is a pickup. */
static void boost_poll(double when_ms)
{
    uint32_t raw = 0;
    int on;

    if (!G->boost_root || !G->boost_kind)
        return;
    if (!boost_cell(&raw)) {
        on = 0;
        s_fuel_seen = 0;
    } else if (G->boost_kind == 2) {
        on = raw != 0;
    } else {
        float fuel;

        memcpy(&fuel, &raw, sizeof fuel);
        if (!(fuel > -1.0e6f && fuel < 1.0e6f))
            fuel = 0.0f;
        if (s_fuel_seen && fuel < s_fuel_last - 1.0e-4f)
            s_fuel_fell_ms = when_ms;
        on = s_fuel_seen && s_fuel_fell_ms > 0.0 &&
             when_ms - s_fuel_fell_ms <= BOOST_FUEL_HOLD_MS;
        s_fuel_last = fuel;
        s_fuel_seen = 1;
    }
    if (on && !s_ev_boosting) {
        s_n_boost_on++;
        event("boost firing");
    } else if (!on && s_ev_boosting) {
        event("boost stopped");
    }
    s_ev_boosting = on;
}

int ffb_events_take(ffb_event_state_t *out)
{
    if (!out)
        return 0;
    out->anchored = s_have_channels;
    out->have = s_have_channels && ffb_path_live();
    /* One-shot, consumed by reading and consumed ATOMICALLY, see the latch. */
    out->impact = impact_bits_to_float(
        InterlockedExchange(&s_ev_impact_bits, 0));
    out->texture = s_ev_texture;
    out->body = s_ev_body;
    out->engine = s_ev_engine;
    out->boost = s_ev_boost;
    out->boost_known = G->boost_root && G->boost_kind;
    out->boosting = s_ev_boosting;
    return out->have;
}

int ffb_events_active(void)
{
    return s_on;
}

void ffb_events_install(void)
{
    s_on = io_config_ffb_events();
    /* Not gated on the trace switch: turning the log off must not change what
     * the player feels. */
    s_have_channels = (G->ffb_collide || G->ffb_impulse_a || G->ffb_impulse_b ||
                       G->ffb_hit_fn || G->ffb_tire_jolts) != 0;
    if (!s_on && !s_have_channels)
        return;
    /* Say which cells this title has, once, so a silent trace is never read as
     * a quiet game. Under s_on: with the switch off these announced a trace
     * that was not going to run. */
    if (s_on) {
        if (G->ffb_accum || G->ffb_collide)
            LOGI("ffb: tracing %s's own units: accum @ 0x%08X, collide @ "
                 "0x%08X, impulse A @ 0x%08X, impulse B @ 0x%08X",
                 G->id, G->ffb_accum, G->ffb_collide,
                 G->ffb_impulse_a, G->ffb_impulse_b);
        else if (G->ffb_force_type || G->ffb_hit_val)
            LOGI("ffb: tracing %s's own units: mode @ 0x%08X, forceType @ "
                 "0x%08X, CurHitVal @ 0x%08X, tireJolts @ 0x%08X x%u, "
                 "shocksHist @ 0x%08X x%u, lastShockVals @ 0x%08X x%u",
                 G->id, G->ffb_mode, G->ffb_force_type, G->ffb_hit_val,
                 G->ffb_tire_jolts, G->ffb_tire_jolts_n,
                 G->ffb_shocks_hist, G->ffb_shocks_hist_n,
                 G->ffb_shock_vals, G->ffb_shock_vals_n);
        else {
            /* Inside this branch, so it cannot fire with the switch off and
             * take the hit hook below down with it; that hook is the PAD's
             * impact channel, not the trace's. */
            LOGW("ffb: [diagnostics] ffb_events is set but this profile "
                 "carries no force-feedback anchors, so there is nothing to "
                 "trace");
            s_on = 0;
        }
    }
    /* Outside it: the hook feeds the PAD, so it runs whatever the trace does. */
    if (G->ffb_hit_fn) {
        /* An absent prologue is 0: refuse rather than patch five bytes that
         * cannot be relocated. */
        if (!game_require("ffb", "the force-feedback tick's prologue",
                          G->hook_ffb_hit)) {
            LOGW("ffb: %s's FFB tick has no relocatable prologue; the pad's "
                 "impact channel stays at 0 and the trace cannot see a "
                 "collision either", G->id);
        } else {
            s_real_ffbfunc = (ffb_hit_fn_t)hook_call_through(
                G->ffb_hit_fn, (void *)ffb_hit_hook, G->hook_ffb_hit);
            if (s_real_ffbfunc)
                LOGI("ffb: hit channel hooked at _ffbnew_FFBFunc 0x%08X, the "
                     "cell is read on the way in, before the game spends it",
                     G->ffb_hit_fn);
            else
                LOGE("ffb: the FFB tick refused its hook; the pad's impacts "
                     "stay at 0 and the polled count will read 0");
        }
    }
    /* The marker keys annotate the trace and nothing else. */
    if (s_on) {
        char keys[128];
        unsigned i;
        int n = 0;

        keys[0] = '\0';
        for (i = 0; i < 4; i++) {
            const char *label = io_config_mark_label(i);
            if (label)
                n += snprintf(keys + n, sizeof keys - (size_t)n, "%sF%u=%s",
                              n ? ", " : "", 5 + i, label);
        }
        if (n)
            LOGI("ffb: marker keys: %s, press one while driving and the log "
                 "stamps it with every term's value", keys);
        else
            LOGI("ffb: no marker keys bound; set [diagnostics] mark1..mark4 to "
                 "name what F5..F8 mean and the trace will annotate itself");
    }
}

static void report(void)
{
    if (G->boost_root && G->boost_kind) {
        LOGI("ffb: window boost fired %u times, firing now %d", s_n_boost_on,
             s_ev_boosting);
        s_n_boost_on = 0;
    }
    if (G->ffb_collide || G->ffb_impulse_b) {
        if (G->ffb_boost_field) {
            LOGI("ffb: window boost state now %d, seen 0x%X, samples "
                 "0:%u 1:%u 2:%u 3:%u",
                 s_ev_boost, s_boost_seen, s_boost_n[0], s_boost_n[1],
                 s_boost_n[2], s_boost_n[3]);
            memset(s_boost_n, 0, sizeof s_boost_n);
        }
        LOGI("ffb: window collide %u (%.2f..%.2f), impulse A %u (peak %.2f), "
             "impulse B %u (peak %.2f)",
             s_n_collide, (double)s_collide_min, (double)s_collide_max,
             s_n_imp_a, (double)s_imp_a_max,
             s_n_imp_b, (double)s_imp_b_max);
    }
    if (G->ffb_hit_val || G->ffb_force_type) {
        /* Two counts on purpose: the polled one is what a 30 Hz sample of a
         * cell spent at 60 Hz can see, the hooked one is what was there. */
        LONG hooked = InterlockedExchange(&s_hook_hits, 0);

        LOGI("ffb: window hits %ld at the source (peak %.2f, last %+.3f) vs "
             "%u polled, force-type changes %u, types seen 0x%X, mode changes "
             "%u (now %d)",
             hooked, (double)s_hook_peak, (double)s_hook_last, s_n_hit,
             s_n_type, s_type_seen, s_n_mode, s_mode);
        s_hook_peak = 0.0f;
    }
    if (s_tex_n) {
        /* What the surface channel hands the pad, before any curve or trim,
         * with the ENGINE channel beside it: a smooth road has no tyre jolts,
         * so engine is the only thing carrying tarmac and reading one without
         * the other is how tarmac's silence stayed a mystery. */
        LOGI("ffb: window surface %.3f..%.3f mean %.3f, engine now %.3f, "
             "over %u samples",
             (double)s_tex_min, (double)s_tex_max, s_tex_sum / s_tex_n,
             (double)s_ev_engine, s_tex_n);
        s_tex_n = 0;
        s_tex_sum = 0.0;
        s_tex_min = s_tex_max = 0.0f;
    }
    report_array(&s_tire);
    report_array(&s_shock);
    report_array(&s_last);
    if (s_suppressed)
        LOGI("ffb: %u further event line(s) suppressed this window "
             "(cap %u)", s_suppressed, FFB_EVENT_CAP);

    s_n_collide = s_n_imp_a = s_n_imp_b = s_n_hit = s_n_type = s_n_mode = 0;
    s_collide_min = s_collide_max = 0.0f;
    s_imp_a_max = s_imp_b_max = s_hit_max = 0.0f;
    s_type_seen = 0;
    s_accum_n = 0;
    s_accum_sum = 0.0;
    s_accum_max = 0.0f;
    s_events = s_suppressed = 0;
}

/* Intervals from the tick stamps, as rumble_set_force() takes them: ticks one
 * pass owes arrive back to back, so a clock read here would see none between
 * them. A stamp behind the last one is no interval. The first call gets 0,
 * which every pole() turns into "do not move" while the seeding branches still
 * run. Nothing ELSE here is gated on dt, a collision on a sample the clock
 * has not moved under must still reach the latch. */
static double s_last_tick;
static int    s_ticked;

void ffb_events_poll(float force, double when_ms)
{
    unsigned dt = 0;

    if (!s_on && !s_have_channels)
        return;

    if (!s_ticked || when_ms > s_last_tick) {
        if (s_ticked)
            dt = (unsigned)(when_ms - s_last_tick + 0.5);
        s_ticked = 1;
        s_last_tick = when_ms;
    }
    boost_poll(when_ms);

    if (G->ffb_accum && s_on) {
        float a = peek_float(G->ffb_accum);
        float m = a < 0.0f ? -a : a;

        s_accum_n++;
        s_accum_sum += m;
        if (m > s_accum_max)
            s_accum_max = m;
    }
    if (G->ffb_collide) {
        /* Spent and zeroed by the game every frame, so a nonzero reading is a
         * collision we happened to be in step with. If this stays 0 through a
         * race full of walls, polling is the wrong instrument for the cell. */
        float c = peek_float(G->ffb_collide);

        if (c != 0.0f) {
            /* SEED OFF THE COUNTER, as s_tex_min/max below and io_board.c's
             * s_ffb_min/max do: comparing against a pair report() zeroed gives
             * a range that always contains 0. */
            if (!s_n_collide || c < s_collide_min) s_collide_min = c;
            if (!s_n_collide || c > s_collide_max) s_collide_max = c;
            s_n_collide++;
            s_fired_collide = GetTickCount();
            event("collide %+.3f", (double)c);
            offer_impact(c);
        }
    }
    if (G->ffb_impulse_a) {
        float v = peek_float(G->ffb_impulse_a);
        float m = v < 0.0f ? -v : v;

        /* A RISE is the event; the fall is the game's own decay. */
        if (m > s_imp_a + 0.02f) {
            s_n_imp_a++;
            s_fired_imp_a = GetTickCount();
            if (m > s_imp_a_max) s_imp_a_max = m;
            event("impulse A fired %+.3f (was %+.3f)", (double)v, (double)s_imp_a);
            offer_impact(v);            /* Hydro: the boat landing on water */
        }
        s_imp_a = m;
    }
    if (G->ffb_impulse_b) {
        float v = peek_float(G->ffb_impulse_b);
        float m = v < 0.0f ? -v : v;

        if (m > s_imp_b + 0.02f) {
            s_n_imp_b++;
            s_fired_imp_b = GetTickCount();
            if (m > s_imp_b_max) s_imp_b_max = m;
            event("impulse B fired %+.3f (was %+.3f)", (double)v, (double)s_imp_b);
            offer_impact(v);            /* Hydro: hitting a thing */
        }
        s_imp_b = m;
    }
    if (G->ffb_attached && G->ffb_boost_field) {
        /* Hydro's MOTOR term is SUBTRACTED from the accumulator BOOSTER adds
         * to, so boosting can LOWER the byte and no filter recovers a term
         * cancelled upstream; the boat is asked instead. The state is an ENUM,
         * not a flag, and means booster AVAILABILITY rather than activity,
         * reported, used by nothing. */
        uint32_t obj = 0, state = 0;

        if (diag_peek32(G->ffb_attached, &obj) && obj &&
            diag_peek32(obj + G->ffb_boost_field, &state)) {
            s_ev_boost = (int)state;
            if (state < 32)
                s_boost_seen |= 1u << state;
            if (state < 8)
                s_boost_n[state]++;
            /* A two-second state is invisible to a 5 s window printing only
             * its last reading, so transitions are events. */
            if ((int)state != s_boost_last) {
                event("boost state %d -> %u", s_boost_last, state);
                s_boost_last = (int)state;
            }
        } else {
            s_ev_boost = 0;
        }
    }
    if (G->ffb_force_type) {
        uint32_t raw = 0;
        int type = diag_peek32(G->ffb_force_type, &raw) ? (int)raw : -1;

        if (type != s_force_type) {
            if (type >= 0 && type < 32)
                s_type_seen |= 1u << type;
            s_n_type++;
            event("force type %d -> %d", s_force_type, type);
            s_force_type = type;
        }
    }
    if (G->ffb_mode) {
        uint32_t raw = 0;
        int mode = diag_peek32(G->ffb_mode, &raw) ? (int)raw : -1;

        /* _ffbnew_DoFFB gates the whole module on this: read it first when a
         * race traces nothing at all. */
        if (mode != s_mode) {
            s_n_mode++;
            event("mode %d -> %d", s_mode, mode);
            s_mode = mode;
        }
    }
    /* THE TRACE'S OWN WORK and the most expensive thing here: 47 peeks a
     * frame on Offroad, none of which reaches a motor. The pad's surface and
     * body channels are filled further down, from named slots. */
    if (s_on) {
        poll_array(&s_tire,  G->ffb_tire_jolts,  G->ffb_tire_jolts_n,  force);
        poll_array(&s_shock, G->ffb_shocks_hist, G->ffb_shocks_hist_n, force);
        poll_array(&s_last,  G->ffb_shock_vals,  G->ffb_shock_vals_n,  force);
    }
    {
        /* Everything synthesised as fast oscillation, engine, boost,
         * cavitation, nitro, read off the byte, the only place they exist. */
        if (!s_engine_seeded) {
            s_engine_seeded = 1;
            s_engine_lp = force;
        } else {
            s_engine_lp += (force - s_engine_lp) *
                           pole((float)dt, ENGINE_POLE_MS);
        }
        {
            float hp = force - s_engine_lp;

            if (hp < 0.0f)
                hp = -hp;
            if (hp > 1.0f)
                hp = 1.0f;
            s_ev_engine += (hp - s_ev_engine) *
                           pole((float)dt, hp > s_ev_engine ? ENGINE_ATTACK_MS
                                                            : ENGINE_RELEASE_MS);
        }
    }
    if (G->ffb_tire_jolts) {
        /* TEXTURE: field +2 of each four five-float tire records. These are
         * the slots _ffbnew_FFBFunc reads (+0x44, +0x58, +0x6c, +0x80) AND the
         * ones a race found at r = -0.42..-0.67 against the commanded force;
         * neither alone would do. Already normalised by the game. */
        static const unsigned k_tire_slot[4] = { 17, 22, 27, 32 };
        float top = 0.0f;
        unsigned i;

        for (i = 0; i < 4; i++) {
            float v, m;
            if (k_tire_slot[i] >= G->ffb_tire_jolts_n)
                continue;
            v = peek_float(G->ffb_tire_jolts + k_tire_slot[i] * 4u);
            m = v < 0.0f ? -v : v;
            if (m > top)
                top = m;
        }
        /* NOT baselined: `top` is a magnitude, so a baseline converges on its
         * own mean and deletes every STEADY surface, tarmac came out silent
         * while dirt survived. Zero already means smooth. */
        s_ev_texture = top > 1.0f ? 1.0f : top;
        if (!s_tex_n || s_ev_texture < s_tex_min) s_tex_min = s_ev_texture;
        if (!s_tex_n || s_ev_texture > s_tex_max) s_tex_max = s_ev_texture;
        s_tex_sum += s_ev_texture;
        s_tex_n++;
    }
    if (G->ffb_shock_vals) {
        /* BODY: the suspension, alive but WEAK (r 0.02..0.26 against the
         * force, vs the tire jolts' 0.67), so a slow bed and not the signal.
         * The SUM, not the maximum, the corners rest at different offsets, so
         * a maximum reports which corner is furthest from zero. In the game's
         * own units, hence the running peak. */
        float top = 0.0f;
        unsigned i;

        for (i = 0; i < G->ffb_shock_vals_n && i < 16; i++)
            top += peek_float(G->ffb_shock_vals + i * 4u);
        s_ev_body = chan_update(&s_ch_body, top, 1, dt);
    }
    if (G->ffb_hit_val) {
        float v = peek_float(G->ffb_hit_val);
        float m = v < 0.0f ? -v : v;

        if (m > s_hit + 0.02f) {
            s_n_hit++;
            s_fired_hit = GetTickCount();
            if (m > s_hit_max) s_hit_max = m;
            event("hit %+.3f (was %+.3f)", (double)v, (double)s_hit);
        }
        s_hit = m;
    }

    /* The marker keys are the trace's too. */
    if (s_on) {
        unsigned i;
        for (i = 0; i < 4; i++) {
            const char *label = io_config_mark_label(i);
            int down = label && io_host_key_down(k_mark_vk[i]);

            if (down && !s_mark_down[i]) {
                /* Not rate-limited by event(): a deliberate mark must never be
                 * the line the cap drops. */
                char a1[24], a2[24], a3[24];

                if (G->ffb_collide || G->ffb_impulse_b) {
                    age(a1, sizeof a1, s_fired_imp_a);
                    age(a2, sizeof a2, s_fired_imp_b);
                    age(a3, sizeof a3, s_fired_collide);
                    LOGI("ffb: MARK %s | boost state %d | impulse A fired %s "
                         "(now %.3f), impulse B fired %s (now %.3f), "
                         "collide %s, accum %.3f",
                         label, s_ev_boost, a1, (double)s_imp_a, a2,
                         (double)s_imp_b, a3,
                         (double)peek_float(G->ffb_accum));
                } else {
                    age(a1, sizeof a1, s_fired_hit);
                    LOGI("ffb: MARK %s | tireJolts %.3f[%u] shocksHist %.3f "
                         "lastShock %.3f, hit fired %s (now %.3f), mode %d",
                         label, (double)s_tire.last, s_tire.peak_index,
                         (double)s_shock.last, (double)s_last.last, a1,
                         (double)peek_float(G->ffb_hit_val), s_mode);
                }
            }
            s_mark_down[i] = down;
        }
    }

    /* Gated here rather than inside report(), so the window clock is not kept
     * running for a report that is never made. */
    if (!s_on)
        return;
    {
        DWORD now = GetTickCount();

        if (!s_reported) {
            s_reported = 1;
            s_window = now;
            return;
        }
        if ((DWORD)(now - s_window) < FFB_WINDOW_MS)
            return;
        s_window = now;
    }
    report();
}
