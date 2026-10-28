/* arctic_ffb.c -- Arctic Thunder: the cabinet's feel, described to a pad.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The cabinet has two things that move: the handlebars, a position servo the
 * game shakes and jolts, and a seat transducer playing engine patterns. A pad
 * gets both through rumble.c, the model Hydro and Offroad use, fed from the
 * game's own cells instead of a DAC byte (there is none):
 *
 *   impact   the handlebar JOLT: the game's crash, collision and landing sites
 *            set it 0..1 and its effects callback decays it 0.04 per 32 ms,
 *            so a rise is a new event
 *   texture  the handlebar SHAKE the game asks for every frame, |-1..1|
 *   body     the seat shaker. Its byte is an 8-slot on/off pattern, not a
 *            level: every BOOST byte has four bits set, FULL two or three,
 *            IDLE one or two, and the ramp's are 3, 7, 0xF..0xFF. So the
 *            level is the bits set over 8.
 *   boosting the shaker playing the pattern the game itself labels BOOST
 *
 * The cells are read, never written (generated/arctic_ffb.c names them).
 * Everything runs on the shell's
 * I/O thread: poll() every pass, so a jolt is seen within a millisecond of the
 * game setting it, and tick() at the feedback thread's 16 ms.
 */
#include "vcthunder.h"
#include "arctic.h"
#include "io.h"

#include <stdio.h>
#include <string.h>

#define WINDOW_MS   5000u       /* the census, as the other titles' ffb_trace */
#define EVENT_CAP   24u         /* event lines per window                      */
#define JOLT_RISE   0.001f      /* under the decay step, over float noise      */
/* The callback takes 0.04 off the jolt every 32 ms, and contact held against
 * a wall re-arms it at its level each time: a rise of one step is that, a
 * larger one a new hit. Both are impacts; only the log tells them apart. */
#define JOLT_HELD   0.045f

static volatile float    *s_jolt, *s_shake;
static volatile uint32_t *s_shaker_on, *s_pattern;
static int      s_armed, s_on;

/* What poll() has seen since the last take(). */
static float    s_jolt_last;
static float    s_impact;               /* the largest new jolt, consumed */
static float    s_texture, s_body;
static int      s_have, s_boosting;

/* The census window. */
static DWORD    s_win_start;
static unsigned s_win_hits, s_win_held, s_win_lines, s_win_polls, s_win_racing;
static float    s_win_jolt_max, s_win_shake_max;
static double   s_win_shake, s_win_body;
static unsigned s_win_boost;

/* [diagnostics] ffb_record, the other titles' columns: one row per tick. */
static FILE    *s_rec;
static int      s_rec_failed;
static unsigned s_rec_rows;

int arctic_ffb_arm(HMODULE snow, uint32_t preferred)
{
    uint8_t *img = (uint8_t *)snow;
    unsigned k;

    (void)preferred;
    s_armed = 0;
    /* No cells derived for this title: the pad takes nothing, exactly as a
     * cell that does not check leaves it. */
    if (!AT->ffb) {
        LOGW("arctic: %s has no feedback cells; the pad takes nothing from"
             " the game", AT->ids->id);
        return 0;
    }
    for (k = 0; k < AFFB_CELLS; k++) {
        uint32_t named;

        memcpy(&named, img + AT->ffb->site[k], 4);
        if (named != (uint32_t)(uintptr_t)(img + AT->ffb->cell[k])) {
            LOGW("arctic: ffb cell %u: the instruction at +0x%06X names 0x%08X,"
                 " not 0x%08X; the pad takes nothing from the game", k,
                 AT->ffb->site[k], named,
                 (uint32_t)(uintptr_t)(img + AT->ffb->cell[k]));
            return 0;
        }
    }
    s_jolt = (volatile float *)(img + AT->ffb->cell[AFFB_JOLT]);
    s_shake = (volatile float *)(img + AT->ffb->cell[AFFB_SHAKE]);
    s_shaker_on = (volatile uint32_t *)(img + AT->ffb->cell[AFFB_SHAKER_ON]);
    s_pattern = (volatile uint32_t *)(img + AT->ffb->cell[AFFB_PATTERN]);
    s_armed = 1;
    return 1;
}

static float absf(float v)
{
    return v < 0.0f ? -v : v;
}

static unsigned bits8(BYTE b)
{
    unsigned n = 0;

    for (; b; b &= (BYTE)(b - 1))
        n++;
    return n;
}

static int take(ffb_event_state_t *out)
{
    memset(out, 0, sizeof *out);
    out->anchored = 1;
    out->have = s_have;
    out->impact = s_impact;
    out->texture = s_texture;
    out->body = s_body;
    out->boost_known = 1;
    out->boosting = s_boosting;
    s_impact = 0.0f;
    return out->have;
}

void arctic_ffb_start(const char *ini)
{
    s_on = 0;
    rumble_preload(ini);
    if (!rumble_active())
        return;
    if (!s_armed) {
        LOGW("arctic: pad rumble is on but the game's feedback cells did not"
             " check; the pad stays still");
        rumble_close();
        return;
    }
    ffb_events_set_source(take);
    s_on = 1;
    s_win_start = GetTickCount();
    LOGI("arctic: pad rumble takes the cabinet's own feedback: handlebar jolts"
         " (impacts) and shake (light motor), the seat shaker's pattern (heavy"
         " motor), its BOOST pattern (boost)");
}

static void census(void)
{
    char pad[320];
    unsigned n = s_win_polls ? s_win_polls : 1;
    unsigned r = s_win_racing ? s_win_racing : 1;

    if (io_config_ffb_trace()) {
        rumble_census(pad, sizeof pad);
        LOGI("arctic: ffb window: racing %u%% of the window, %u jolt%s up to"
             " %.2f (and %u re-armed by contact held), shake mean %.2f peak"
             " %.2f, seat shaker mean %.0f%%, BOOST %u%%",
             100u * s_win_racing / n, s_win_hits, s_win_hits == 1 ? "" : "s",
             (double)s_win_jolt_max, s_win_held,
             s_win_shake / r, (double)s_win_shake_max, 100.0 * s_win_body / r,
             100u * s_win_boost / r);
        LOGI("arctic: ffb window pad: %s", pad);
    }
    s_win_hits = s_win_held = s_win_lines = s_win_polls = s_win_racing = 0;
    s_win_boost = 0;
    s_win_jolt_max = s_win_shake_max = 0.0f;
    s_win_shake = s_win_body = 0.0;
}

void arctic_ffb_poll(BYTE shaker)
{
    float jolt, shake;

    if (!s_on)
        return;
    jolt = *s_jolt;
    shake = absf(*s_shake);
    s_have = *s_shaker_on != 0;
    s_boosting = s_have && *s_pattern == AT->ffb->boost_pattern;
    s_texture = shake > 1.0f ? 1.0f : shake;
    s_body = s_have ? (float)bits8(shaker) / 8.0f : 0.0f;
    if (jolt > s_jolt_last + JOLT_RISE) {
        int hit = jolt > s_jolt_last + JOLT_HELD;

        if (jolt > s_impact)
            s_impact = jolt;
        if (jolt > s_win_jolt_max)
            s_win_jolt_max = jolt;
        if (!hit) {
            s_win_held++;
        } else {
            s_win_hits++;
            log_printf(io_config_ffb_trace() && s_win_lines++ < EVENT_CAP ?
                       LOG_INFO : LOG_TRACE,
                       "arctic: ffb jolt %.2f (from %.2f)%s", (double)jolt,
                       (double)s_jolt_last, s_have ? "" : ", not racing");
        }
    }
    s_jolt_last = jolt;

    s_win_polls++;
    if (s_have) {
        s_win_racing++;
        s_win_shake += s_texture;
        s_win_body += s_body;
        if (s_texture > s_win_shake_max)
            s_win_shake_max = s_texture;
        if (s_boosting)
            s_win_boost++;
    }
}

static void record(double when_ms, int attack)
{
    const char *path = io_config_ffb_record();
    ffb_event_state_t ev;
    unsigned lo, hi;
    int events;

    if (!path || s_rec_failed)
        return;
    if (!s_rec) {
        if (!(s_rec = fopen(path, "w"))) {
            s_rec_failed = 1;
            LOGW("arctic: ffb_record: cannot open '%s'", path);
            return;
        }
        fprintf(s_rec, "t_ms,force,events,have,impact,texture,body,engine,"
                       "lo,hi,boost_btn,boost_known,boosting\n");
        LOGI("arctic: ffb_record: writing every tick to '%s'", path);
    }
    events = rumble_record_state(&ev, &lo, &hi);
    fprintf(s_rec, "%.2f,0,%d,%d,%.4f,%.4f,%.4f,%.4f,%u,%u,%d,%d,%d\n",
            when_ms, events, ev.have, (double)ev.impact, (double)ev.texture,
            (double)ev.body, (double)ev.engine, lo, hi, attack, ev.boost_known,
            ev.boosting);
    if (++s_rec_rows % 64u == 0)
        fflush(s_rec);
}

void arctic_ffb_tick(int attack)
{
    double now;

    if (!s_on)
        return;
    /* No force: the cabinet has no torque command for the model to filter,
     * only the units above, so the byte path stays silent. */
    now = io_clock_ms();
    rumble_set_force(0.0f, now, attack);
    record(now, attack);
    if (GetTickCount() - s_win_start >= WINDOW_MS) {
        s_win_start = GetTickCount();
        census();
    }
}

void arctic_ffb_stop(void)
{
    if (!s_on)
        return;
    s_on = 0;
    if (s_rec) {
        fclose(s_rec);
        s_rec = NULL;
    }
    rumble_close();
    ffb_events_set_source(NULL);
}
