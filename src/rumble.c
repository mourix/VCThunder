/* rumble.c -- the game's feedback events, described to a gamepad's motors.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A pad has no force axis, so the cabinet's DAC torque can only be DESCRIBED.
 * This is the one place in the project that invents an effect, and it is
 * fenced: it needs `[ffb] rumble` (on by default) and `[ffb] enabled`, and a
 * DirectInput wheel vetoes it outright. The source is ffb_events.c's four
 * channels rather than io_dac_out, which is a SUM no filter un-mixes, and every
 * number is per-title through `[<title>.ffb]`.
 */
#include "vcthunder.h"
#include "io.h"
#include "ini.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Time constants, in ms of the force signal; ours, describing the motors and
 * not the game's physics. SLOW is the fallback's "held" vs "happening" line;
 * MID splits the remainder between the two motors. */
#define RUMBLE_SLOW_MS      700.0f
#define RUMBLE_MID_MS       120.0f

/* The heavy mass is slower to start and slower to stop than the light one, and
 * its band oscillates around zero: without a release longer than a half-cycle
 * it would gate itself off at every zero crossing and stutter at ~1 Hz. */
#define RUMBLE_LOW_ATTACK_MS     40.0f
#define RUMBLE_LOW_RELEASE_MS   250.0f
#define RUMBLE_HIGH_ATTACK_MS    20.0f
#define RUMBLE_HIGH_RELEASE_MS  120.0f

/* Longer than the bed: a mass has to spin UP before anything is felt. */
#define IMPACT_RELEASE_MS       280.0f

/* What a mass will reliably START at: a level below this is absent, not
 * quieter. The bed is kicked here long enough to break away, then follows its
 * own signal down to BED_SUSTAIN. A fact about the MASS, so it is spent in the
 * motor's own units, motor_min_spin(). */
#define MOTOR_MIN_SPIN           0.08f
/* A mass has TWO levels: MOTOR_MIN_SPIN to start, and a lower one it holds
 * once turning. Pinning the bed at the start level is as constant as chopping
 * it was noisy, so the bed is kicked to the start level for BED_KICK_MS and
 * then allowed down to this sustain floor. */
#define BED_SUSTAIN              0.30f   /* of MOTOR_MIN_SPIN: what it HOLDS */
#define BED_GATE_CLOSE           0.18f   /* hysteresis under that            */
/* Two frames. A mass that has not broken away inside that will not. */
#define BED_KICK_MS             66.0f

/* Below this an eccentric mass draws current without turning. Hysteresis
 * because a quiet stretch sits ON the threshold and chatters. */
#define RUMBLE_FLOOR        0.02f
#define RUMBLE_FLOOR_CLOSE  0.012f

/* Full scale, and the smallest change worth a driver round trip: this runs on
 * the frame-boundary thread, which owns the game's code while it does. */
#define RUMBLE_FULL         65535.0f
#define RUMBLE_STEP         512u

/* The two paths are not scaled alike, events go through the per-title bed
 * trim, the fallback renders the raw byte. A ceiling, not a scale, so a gentle
 * menu keeps its own level. */
#define FALLBACK_CEIL    0.075f

/* The longest gap that is still a GAP rather than a pause. */
#define RUMBLE_DT_MAX_MS    100u

/* The normaliser: the peak decays with this half-life, so a quiet stretch
 * after a loud one recovers over seconds.
 *
 * IT DOES NOT COMPOSE WITH THE PER-TITLE BED TRIM, deliberately. The peak is
 * measured AFTER `rumble_texture_percent`, so turning the boost on undoes the
 * one key the two titles must not share. Off by default; the ini says so. */
#define AGC_HALFLIFE_MS  4000.0f
#define AGC_TARGET       0.85f
#define AGC_FLOOR        0.02f

/* BOOST. The game says when it fires (ffb_events.c's `boosting`, from the
 * title's own fuel or flag; pressing is not boosting, both titles burn fuel the
 * player collects). When it starts, a spark through the impact path, both
 * masses. While it runs, a buzz on the light motor sized from the force: both
 * titles synthesise boost as a fast alternation (Hydro's booster flips sign
 * every tick, Offroad's nitro every two), and half the tick-to-tick change
 * measures it at its own size, where the engine channel's high-pass reads a
 * tick-rate flip at a third of it and the bed trim then caps it. Measured over
 * the level just before boost fired, so the engine and the surface are not
 * counted twice. A profile with no boost chain falls back to the button. */
#define BOOST_ALT_ATTACK_MS     20.0f
#define BOOST_ALT_RELEASE_MS   150.0f
#define BOOST_BASE_MS          500.0f   /* the baseline, while not held       */
#define BOOST_FULL               0.50f  /* alternation over baseline = full   */
/* The button alone is not evidence of boosting (an empty tank), so without
 * the game's chain the spark also waits for the buzz to reach this. */
#define BOOST_BUTTON_SPARK       0.40f  /* of full scale                      */

/* Withdrawn and not to be reintroduced: `BED_PWM_HZ` (duty-carried bed level)
 * and `rumble_pulse_hz` (chopped heavy motor for road grain). Both are square
 * waves on a ~30 Hz transport with a 15.6 ms clock; slower reads as knocking,
 * faster aliases, and there is no setting in between. */

static int      s_enabled;              /* both ini switches, resolved once */
static unsigned s_strength_percent;
static unsigned s_sustain_percent;
static unsigned s_impact_percent;
static unsigned s_spring_percent;       /* held level added back on purpose */
static unsigned s_texture_percent;      /* the continuous bed, under the hits */
static unsigned s_texture_curve;        /* range compression, 100 == linear  */
static unsigned s_boost_percent;        /* the boost path, outside the bed trim */
static unsigned s_agc_percent;          /* ceiling on the boost; 100 == off */
static int      s_use_events;           /* [ffb] rumble_events, and anchors  */

static int      s_have_state;           /* the filters carry a real sample   */
static int      s_ticked;               /* ...and the CLOCK has been read    */
static double   s_last_tick;            /* the last sample's stamp, ms       */
/* What the last step consumed and commanded, for [diagnostics] ffb_record. */
static ffb_event_state_t s_rec_ev;
static int      s_rec_events;
static unsigned s_rec_lo, s_rec_hi;
static float    s_slow, s_mid;          /* the two low-passes                */
static float    s_env_low, s_env_high;  /* the two band envelopes            */
static float    s_env_hit;              /* impacts, on their own release     */
static int      s_bed_open;             /* the bed gate's hysteresis       */
static float    s_bed_kick_ms;          /* ...and the break-away it owes   */
static float    s_ac_peak;              /* the normaliser's decaying peak    */
static int      s_boost_held;           /* the BOOST button: the fallback    */
static float    s_boost_prev_f;         /* the last force, for the alternation */
static float    s_boost_alt;            /* its envelope                       */
static float    s_boost_base;           /* ...and its level before the press  */
static int      s_boost_sparked;        /* the spark's hysteresis            */
static unsigned s_low, s_high;          /* what the motors were last told    */
static int      s_pushed;               /* ...and whether they were told it  */
static int      s_low_open, s_high_open; /* the floor gate's hysteresis      */

/* The census window, read and reset by rumble_census(). */
static unsigned s_peak;
static unsigned s_win_n, s_win_pinned;
/* The bed's own DUTY over the window: the only figure that separates a bed
 * answering the engine from one that never lets go. Loudness cannot. */
static unsigned s_win_high_on;
static double   s_win_high;
static double   s_win_force, s_win_ac;
static float    s_win_gain_max;
/* The only two figures about the TRANSPORT rather than the model: everything
 * above is what was asked for, these are what reached a device. */
static unsigned s_win_sent, s_win_dropped;

/* One-pole coefficient over a measured interval; dt is whatever the frame
 * took. */
static float pole(float dt_ms, float tau_ms)
{
    return 1.0f - (float)exp(-(double)dt_ms / (double)tau_ms);
}

static float absf(float v)
{
    return v < 0.0f ? -v : v;
}

/* Fast towards a rising signal, slow away from a falling one: what the
 * mass does. */
static void follow(float *env, float target, float dt_ms,
                   float attack_ms, float release_ms)
{
    *env += (target - *env) *
            pole(dt_ms, target > *env ? attack_ms : release_ms);
}

static unsigned scale_motor(float unit, unsigned percent, int *open)
{
    float v = unit * (float)percent * (float)s_strength_percent / 10000.0f;

    if (v < (*open ? RUMBLE_FLOOR_CLOSE : RUMBLE_FLOOR)) {
        *open = 0;
        return 0;
    }
    *open = 1;
    if (v > 1.0f)
        v = 1.0f;
    return (unsigned)(v * RUMBLE_FULL);
}

/* MOTOR_MIN_SPIN in the units the MOTOR is commanded in, not the model's:
 * scale_motor() has both trims still to apply, so the threshold has to be
 * divided by them here. The only constant in the file on that side. */
static float motor_min_spin(unsigned percent)
{
    float scale = (float)percent * (float)s_strength_percent / 10000.0f;
    float unit;

    if (scale <= 0.0f)
        return 0.0f;                    /* a trim of zero is silence */
    unit = MOTOR_MIN_SPIN / scale;
    /* A trim whose FULL scale is under the mass's start level has asked for
     * the impossible: give it the loudest it allows and let RUMBLE_FLOOR
     * decide whether that turns at all. */
    return unit > 1.0f ? 1.0f : unit;
}

static void push(unsigned low, unsigned high)
{
    if (s_pushed) {
        unsigned dl = low  > s_low  ? low  - s_low  : s_low  - low;
        unsigned dh = high > s_high ? high - s_high : s_high - high;

        if (!dl && !dh)
            return;
        /* Skip a round trip for a change no hand can feel, but never one to
         * rest: a motor left spinning under the threshold is the one failure
         * a user cannot work around. */
        if ((low || high) && dl < RUMBLE_STEP && dh < RUMBLE_STEP)
            return;
    }
    /* The sink is XInput and only XInput, so a WinMM pad runs this whole
     * model with nothing to buzz; count the refusals for the census. */
    if (!io_host_rumble(low, high)) {
        s_win_dropped++;
        return;
    }
    s_win_sent++;
    s_low = low;
    s_high = high;
    s_pushed = 1;
}

static void silence(void)
{
    s_have_state = 0;
    s_slow = s_mid = s_env_low = s_env_high = s_env_hit = 0.0f;
    /* Hysteresis is history too: an OPEN gate carried across a pause lets the
     * resume start at the lower close threshold. */
    s_low_open = s_high_open = 0;
    s_bed_open = 0;
    s_bed_kick_ms = 0.0f;
    if (s_pushed && (s_low || s_high))
        push(0, 0);
}

static float agc_gain(float ac, unsigned dt_ms)
{
    float a = absf(ac);
    float ceiling = (float)s_agc_percent / 100.0f;
    float gain;

    s_ac_peak *= (float)pow(0.5, (double)dt_ms / (double)AGC_HALFLIFE_MS);
    if (a > s_ac_peak)
        s_ac_peak = a;
    if (ceiling <= 1.0f)
        return 1.0f;
    gain = AGC_TARGET / (s_ac_peak > AGC_FLOOR ? s_ac_peak : AGC_FLOOR);
    /* Boost only: attenuating a loud passage would take the size off the
     * events worth feeling. */
    if (gain < 1.0f) gain = 1.0f;
    if (gain > ceiling) gain = ceiling;
    return gain;
}

/* --- the ini ------------------------------------------------------------- */

static unsigned rumble_percent(const char *path, const char *key,
                               unsigned fallback, unsigned lo, unsigned hi)
{
    char text[32], def[32];
    unsigned n;

    snprintf(def, sizeof def, "%u", fallback);
    ini_read_scoped(path, "ffb", key, def, text, sizeof text);
    n = ini_uint_or(text, 0, UINT_MAX, fallback);
    if (n < lo) n = lo;
    return n > hi ? hi : n;
}

void rumble_preload(const char *ini)
{
    char path[MAX_PATH];
    char text[32];
    int enabled, wanted;

    s_enabled = 0;
    s_have_state = 0;
    s_slow = s_mid = s_env_low = s_env_high = 0.0f;
    s_env_hit = 0.0f;
    s_bed_open = 0;
    s_bed_kick_ms = 0.0f;
    s_ac_peak = 0.0f;
    s_boost_held = 0;
    s_boost_prev_f = s_boost_alt = s_boost_base = 0.0f;
    s_boost_sparked = 0;
    s_low = s_high = 0;
    s_pushed = 0;
    s_ticked = 0;
    s_peak = s_win_n = s_win_pinned = s_win_high_on = 0;
    s_win_sent = s_win_dropped = 0;
    s_win_high = 0.0;
    s_win_force = s_win_ac = 0.0;
    s_win_gain_max = 0.0f;
    s_low_open = s_high_open = 0;

    /* ABSOLUTE FIRST: a bare name resolves against the WINDOWS directory, so
     * every key below would silently take its default (ini.h). */
    ini_resolve_path(ini, path, sizeof path);
    ini_read_scoped(path, "ffb", "enabled", "true", text, sizeof text);
    enabled = ini_bool_or(text, 1);
    ini_read_scoped(path, "ffb", "rumble", "true", text, sizeof text);
    wanted = ini_bool_or(text, 1);

    s_strength_percent = rumble_percent(path, "rumble_strength_percent", 100, 0, 200);
    s_sustain_percent  = rumble_percent(path, "rumble_sustain_percent", 100, 0, 200);
    s_impact_percent   = rumble_percent(path, "rumble_impact_percent", 100, 0, 200);
    s_spring_percent   = rumble_percent(path, "rumble_spring_percent", 0, 0, 100);
    /* 0..400: Hydro's engine-only bed saturated the old 200 ceiling, so the
     * ini silently disagreed with the log. */
    s_texture_percent  = rumble_percent(path, "rumble_texture_percent", 50, 0, 400);
    s_texture_curve    = rumble_percent(path, "rumble_texture_curve_percent", 35, 2, 100);
    s_boost_percent    = rumble_percent(path, "rumble_boost_percent", 100, 0, 200);
    s_agc_percent      = rumble_percent(path, "rumble_agc_percent", 100, 100, 800);
    ini_read_scoped(path, "ffb", "rumble_events", "true", text, sizeof text);
    s_use_events = ini_bool_or(text, 1);

    if (!wanted)
        return;
    if (!enabled) {
        LOGI("io: [ffb] rumble is set but [ffb] enabled is false, so the pad "
             "stays still: one switch turns force feedback off, both kinds");
        return;
    }
    /* THE DEVICE THAT ANSWERED, NOT THE ONE THAT WAS NAMED: the shipped
     * provider is `auto`, so testing the configured name left this guard off
     * for every default install. Runs after di_preload() for this. */
    if (di_present()) {
        LOGI("io: [ffb] rumble is set but %s answered and takes the game's "
             "REAL force through the wheel; no derived effect is added to it",
             di_device_name());
        return;
    }
    if (io_config()->provider == PROVIDER_WINMM ||
        io_config()->provider == PROVIDER_DINPUT) {
        LOGI("io: [ffb] rumble is set but [analog] provider = %s, which is not "
             "a pad; no rumble",
             io_config()->provider == PROVIDER_WINMM ? "winmm" : "dinput");
        return;
    }
    s_enabled = 1;
    LOGI("io: pad rumble on, DERIVED from the game's force command: "
         "swell %u%% (low motor), impacts %u%% (high motor), strength %u%%",
         s_sustain_percent, s_impact_percent, s_strength_percent);
    /* Two decimals on the boost: %u/100 reported 150%% as "1x". The spring is
     * named as the FALLBACK'S, the game's units carry none, so on the events
     * path that key moves nothing. */
    LOGI("io: pad rumble shaping: no DC path; a held force falls silent in "
         "about a second, %u%% of it added back on purpose where the byte is "
         "the source (the game's own units carry no spring); quiet stretches "
         "boosted up to %u.%02ux; bed %u%% at curve %u%% under the hits",
         s_spring_percent, s_agc_percent / 100, s_agc_percent % 100,
         s_texture_percent, s_texture_curve);
}

/* At INSTALL, not preload: the profile's anchors do not exist yet there. */
void rumble_report_source(void)
{
    ffb_event_state_t ev = { 0 };

    if (!s_enabled)
        return;
    /* `anchored`, not the return value: that answers whether the path is
     * running RIGHT NOW, and at install no title is in a race. io.h defines
     * the two fields for exactly this difference. */
    ffb_events_take(&ev);
    if (s_use_events && ev.anchored)
        LOGI("io: pad rumble takes the GAME'S OWN units: impacts, texture and "
             "body come from %s's own force-feedback cells, not from the DAC "
             "byte they are summed into", G->id);
    else if (s_use_events)
        LOGI("io: [ffb] rumble_events is on but %s carries no force-feedback "
             "anchors; the pad falls back to filtering the DAC", G->id);
    else
        LOGI("io: [ffb] rumble_events = false; the pad filters the DAC byte "
             "even where %s's own units are available", G->id);
}

int rumble_active(void)
{
    return s_enabled;
}

/* --- the motors ---------------------------------------------------------- */

/* One force sample; the interval is an ARGUMENT and there is no clock here, so
 * tests/rumble_model_test.c can drive the whole model on a pinned timeline. */
static void rumble_step(float f, unsigned dt_ms)
{
    float ac, gain, low, high;
    /* ZEROED, NOT MERELY DECLARED: the short circuit below leaves
     * ffb_events_take() uncalled when rumble_events is false, and the fallback
     * still reads ev.anchored to decide FALLBACK_CEIL. Zero is also the right
     * answer there. No warning sees this; the call is extern. */
    ffb_event_state_t ev = { 0 };
    int events, firing;

    if (f < -1.0f) f = -1.0f;
    if (f >  1.0f) f =  1.0f;

    if (!s_have_state) {
        /* Seed, do not integrate: a first sample has no interval behind it.
         * Seeding both low-passes to f is what makes it carry no AC. */
        s_have_state = 1;
        s_slow = s_mid = f;
        s_env_low = s_env_high = 0.0f;
        s_boost_prev_f = f;
        return;
    }
    if (!dt_ms)
        return;

    /* AFTER the two returns above, never before: ffb_events_take() CONSUMES
     * the impact latch, so taking it on a sample about to be discarded throws
     * the collision away. */
    events = s_use_events && ffb_events_take(&ev);

    if (dt_ms > RUMBLE_DT_MAX_MS)
        dt_ms = RUMBLE_DT_MAX_MS;

    /* BOTH low-passes track the byte on BOTH paths, though only the fallback
     * reads them: a path that stops advancing them hands the fallback a filter
     * seeded whenever it last ran, and a held byte then reads as a step from
     * nothing at every events->fallback switch. The spring is removed where it
     * is USED, below. */
    s_slow += (f - s_slow) * pole((float)dt_ms, RUMBLE_SLOW_MS);
    s_mid  += (f - s_mid)  * pole((float)dt_ms, RUMBLE_MID_MS);

    /* The alternation runs on both paths, so a boost that starts in a menu
     * transition does not find a cold envelope. The baseline only moves while
     * boost is not firing: firing, it is the level boost is measured against. */
    firing = ev.boost_known ? ev.boosting : s_boost_held;
    {
        float alt = absf(f - s_boost_prev_f) * 0.5f;

        s_boost_prev_f = f;
        follow(&s_boost_alt, alt, (float)dt_ms,
               BOOST_ALT_ATTACK_MS, BOOST_ALT_RELEASE_MS);
        if (!firing) {
            s_boost_base += (s_boost_alt - s_boost_base) *
                            pole((float)dt_ms, BOOST_BASE_MS);
            s_boost_sparked = 0;
        }
    }

    if (events) {
        /* The game's own units are the only source here; the DAC filter below
         * does not run. Combining the two put the steering spring back,
         * because the byte still contains it. */
        float hit = absf(ev.impact);
        float bed = ev.texture;
        float boost = 0.0f;

        if (firing && s_boost_alt > s_boost_base) {
            boost = (s_boost_alt - s_boost_base) / BOOST_FULL;
            if (boost > 1.0f)
                boost = 1.0f;
        }
        /* Once per firing, on the tick it starts. Through the impact path,
         * both masses: the moment the boost hits is felt on the wheel. */
        if (firing && !s_boost_sparked &&
            (ev.boost_known || boost >= BOOST_BUTTON_SPARK)) {
            float spark = (float)s_boost_percent / 100.0f;

            s_boost_sparked = 1;
            if (spark > 1.0f)
                spark = 1.0f;
            if (spark > hit)
                hit = spark;
        }
        boost = boost * (float)s_boost_percent / 100.0f;

        /* The curve goes on whichever channel wins, not the surface alone: a
         * smooth road produces no tyre jolts, so tarmac's only source is the
         * engine channel and it needs the same compression. */
        if (ev.engine > bed)
            bed = ev.engine;
        if (bed > 0.0f)
            bed = (float)pow((double)bed, (double)s_texture_curve / 100.0);
        bed = bed * (float)s_texture_percent / 100.0f;
        /* After the trim and the curve, not through them: Offroad's bed is
         * trimmed to 8%, which is what kept its nitro off the pad. */
        if (boost > bed)
            bed = boost;
        if (bed > 1.0f)
            bed = 1.0f;

        /* Terrain is one thing driving both masses, so one trim governs it;
         * shaping only the bed left the suspension at full scale. */
        follow(&s_env_low, ev.body * (float)s_texture_percent / 100.0f,
               (float)dt_ms, RUMBLE_LOW_ATTACK_MS, RUMBLE_LOW_RELEASE_MS);
        follow(&s_env_high, bed, (float)dt_ms,
               RUMBLE_HIGH_ATTACK_MS, RUMBLE_HIGH_RELEASE_MS);
        /* Whole and immediate, on its OWN envelope so it outlives the bed's
         * short release; loudest wins, as the game's impulse arming does. */
        if (hit > s_env_hit)
            s_env_hit = hit;
        if (hit * 0.6f > s_env_low)
            s_env_low = hit * 0.6f;     /* a hit has weight as well as edge */
        ac = s_env_high > s_env_low ? s_env_high : s_env_low;
        if (s_env_hit > ac)
            ac = s_env_hit;
    } else {
        /* The high-pass is the point: `f - s_slow`, never `f`. Everything this
         * path can do is bounded by the byte being a sum. */
        ac = f - s_slow;
        follow(&s_env_low, absf(s_mid - s_slow), (float)dt_ms,
               RUMBLE_LOW_ATTACK_MS, RUMBLE_LOW_RELEASE_MS);
        follow(&s_env_high, absf(f - s_mid), (float)dt_ms,
               RUMBLE_HIGH_ATTACK_MS, RUMBLE_HIGH_RELEASE_MS);
        /* Only when the title HAS channels and they are momentarily down (the
         * menu case). With no anchors at all this is the only path there is. */
        if (ev.anchored) {
            if (s_env_low > FALLBACK_CEIL)  s_env_low = FALLBACK_CEIL;
            if (s_env_high > FALLBACK_CEIL) s_env_high = FALLBACK_CEIL;
        }
    }

    /* Outside the branch: consulted by both paths, so decaying it in one
     * freezes a stale crash while a title sits on the other. */
    s_env_hit -= s_env_hit * pole((float)dt_ms, IMPACT_RELEASE_MS);

    gain = agc_gain(ac, dt_ms);

    low = s_env_low * gain;
    high = s_env_high * gain;

    /* Not normalised (a deliberate constant, not a signal) and the
     * FALLBACK's only: the game's units carry no spring to add back. */
    if (!events)
        low += absf(s_slow) * (float)s_spring_percent / 100.0f;

    /* THE BED, KICKED ONTO THE RANGE THE MASS HAS. After the envelope, not
     * before: the release is longer than anything down here, so a shape
     * imposed on the input comes back out as a level. Events path only, the
     * fallback is a menu detent under FALLBACK_CEIL. */
    {
        float min_spin = motor_min_spin(s_impact_percent);

        if (events && min_spin > 0.0f) {
            float sustain = min_spin * BED_SUSTAIN;
            float gate = s_bed_open ? min_spin * BED_GATE_CLOSE : sustain;

            if (high < gate) {
                s_bed_open = 0;
                s_bed_kick_ms = 0.0f;
                high = 0.0f;
            } else {
                if (!s_bed_open) {
                    s_bed_open = 1;
                    s_bed_kick_ms = BED_KICK_MS;
                }
                /* Spent once per opening, never renewed while the bed runs,
                 * or it is a chop again with a longer period. */
                if (s_bed_kick_ms > 0.0f) {
                    s_bed_kick_ms -= (float)dt_ms;
                    if (high < min_spin)
                        high = min_spin;
                } else if (high < sustain) {
                    high = sustain;
                }
            }
        }
    }
    /* The impact rides OVER the bed, never through it. */
    if (s_env_hit * gain > high)
        high = s_env_hit * gain;

    s_win_n++;
    s_win_force += absf(f);
    s_win_ac += absf(ac);
    if (gain > s_win_gain_max)
        s_win_gain_max = gain;

    {
        unsigned lo = scale_motor(low, s_sustain_percent, &s_low_open);
        unsigned hi = scale_motor(high, s_impact_percent, &s_high_open);

        /* Every sample, whether or not push() sent it: the census is about
         * the model's output, not USB traffic. */
        if (hi) {
            s_win_high_on++;
            s_win_high += (double)hi;
        }

        /* Here and not in push(): raised after io_host_rumble() it measured
         * the SINK while the rest of the line measured the model, so one
         * census said both things at once. */
        if (lo > s_peak) s_peak = lo;
        if (hi > s_peak) s_peak = hi;

        /* A peak alone cannot tell "reached full scale once" from "sat
         * there", and the second is a defect. */
        if (lo >= (unsigned)(0.99f * RUMBLE_FULL) ||
            hi >= (unsigned)(0.99f * RUMBLE_FULL))
            s_win_pinned++;
        s_rec_ev = ev;
        s_rec_events = events;
        s_rec_lo = lo;
        s_rec_hi = hi;
        push(lo, hi);
    }
}

void rumble_set_force(float f, double when_ms, int boost_held)
{
    unsigned dt = 0;

    if (!s_enabled)
        return;
    s_boost_held = boost_held;
    /* The interval comes from the tick's stamp, not a clock read here: ticks
     * one pass owes arrive back to back. A stamp behind the last one is no
     * interval. It is taken whether or not the sample is used: the peak
     * decays against the WALL and the filters do not. s_ticked is deliberately
     * not cleared by silence(); s_have_state says the FILTERS are cold. */
    if (!s_ticked || when_ms > s_last_tick) {
        if (s_ticked)
            dt = (unsigned)(when_ms - s_last_tick + 0.5);
        s_ticked = 1;
        s_last_tick = when_ms;
    }
    /* Not focused is nobody holding the pad; drop the history, including the
     * peak the boost is measured against, so a resume is not a transient
     * against a reading minutes old. */
    if (!io_host_active()) {
        s_ac_peak *= (float)pow(0.5, (double)dt / (double)AGC_HALFLIFE_MS);
        silence();
        return;
    }
    rumble_step(f, dt);
}

int rumble_record_state(ffb_event_state_t *ev, unsigned *lo, unsigned *hi)
{
    *ev = s_rec_ev;
    *lo = s_rec_lo;
    *hi = s_rec_hi;
    return s_rec_events;
}

void rumble_census(char *out, size_t size)
{
    unsigned n = s_win_n ? s_win_n : 1;

    /* The transport's clause, only when it has something to say. `sent` alone
     * is not the tell; push()'s dead band holds most samples on purpose,
     * but one DROPPED command is a refusal, and a refusal is a defect. */
    char sink[72];

    sink[0] = '\0';
    if (s_win_dropped)
        snprintf(sink, sizeof sink,
                 ", %u of %u commands REFUSED by the sink",
                 s_win_dropped, s_win_sent + s_win_dropped);

    /* Mean |force| against mean |AC| says whether there is anything in the
     * signal but the held level; both in percent, to read against the ini. */
    snprintf(out, size,
             "peak motor %u of 65535, pinned %u%% of samples, "
             "bed spinning %u%% of samples at mean %.1f%% while it ran, "
             "mean |force| %.1f%% of which AC %.1f%%, boost up to %.1fx%s",
             s_peak, 100u * s_win_pinned / n,
             100u * s_win_high_on / n,
             s_win_high_on ?
                 100.0 * s_win_high / s_win_high_on / RUMBLE_FULL : 0.0,
             100.0 * s_win_force / n,
             100.0 * s_win_ac / n,
             (double)s_win_gain_max, sink);
    s_peak = s_win_n = s_win_pinned = s_win_high_on = 0;
    s_win_sent = s_win_dropped = 0;
    s_win_force = s_win_ac = s_win_high = 0.0;
    s_win_gain_max = 0.0f;
}

void rumble_close(void)
{
    if (s_enabled)
        silence();
    s_enabled = 0;
}
