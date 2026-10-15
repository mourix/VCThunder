/* audio.c -- Hydro audio backend.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game retains its YMF bookkeeping while the host replaces hardware command
 * submission with a 48 kHz WASAPI mixer. A voice loops when its buffer was
 * allocated as a loop or the play's loopDefault is 0; see av_Play. A render
 * thread mixes voices, and a separate DCS thread executes the game's interrupt
 * callback.
 */
#include "vcthunder.h"

#define COBJMACROS
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The command table the game's ISR would have drained: 32 slots, addressed as
 * (generation << 4) | cookieIndex with 16 cookies and a 1-bit generation. */
#define AV_VOICES        32
#define AV_BUFFERS     4096      /* _YmfSoundBuffers is 4096 x 12 bytes */
#define AV_OUT_RATE   48000u     /* what the DS-XG ran at; see header comment */

/* Any value a real 8259 IRQ line cannot be. _ymf_Init publishes it in
 * _YmfInfo+4 and every DCS critical section then routes through us. */
#define AV_IRQ_TOKEN  0x59464Du  /* 'YMF' */

typedef struct {
    const int16_t *pcm;
    uint32_t frames;             /* PCM length in samples, not bytes */
    uint32_t rate;               /* source rate as the game declared it */
    uint64_t pos;                /* 32.32 fixed-point read cursor */
    int      loop;
    int      active;
    int      vol;                /* 0..255, 256 would be unity gain */
    int      pan;                /* 0..255, 0 = hard left */
    int      pitch;              /* 0..255, 128 = unity */
    /* Whether a ChangePitch is pending for the drain this Play shares. The
     * cabinet kept per-voice DIRTY BITS and serviced them in a fixed order, so
     * a pitch commanded either side of a Play still won that drain; see
     * av_Play. */
    int      pitch_dirty;
} av_voice_t;

/* What _ymf_AllocateSoundBuffer was told, kept only for the loop flag. The
 * descriptor itself stays in the game's own _YmfSoundBuffers. */
typedef struct {
    const int16_t *pcm;
    uint32_t bytes;
    int      loop;
} av_buffer_t;

static av_voice_t   s_voice[AV_VOICES];
static av_buffer_t  s_buffer[AV_BUFFERS];

static CRITICAL_SECTION s_voice_cs;   /* voice array: mixer vs. game threads   */
static int s_cs_ready;

/* Model the audio IRQ mask as global role-owned state. Game threads share one
 * role; the host interrupt thread owns the other. */
typedef enum { AV_MASK_FREE = 0, AV_MASK_GAME, AV_MASK_TICK } av_mask_role_t;

#define AV_MASK_POLL_MS    5u         /* re-check cadence while waiting        */
/* Break the cycle in 20 ms, not 1000.
 *
 * The game takes a DCS lock and then SPINS on _cpuTimeStamp, which lands in
 * timebase.c's synth_ticks on every sample, so it waits for DCS progress only
 * the tick thread can make while the tick thread waits for this mask. A
 * circular wait; nothing but the steal ends it.
 *
 * On the cabinet the DCS is a SEPARATE PROCESSOR and masking the main CPU never
 * stopped it; modelling the board as a thread that needs the game's interrupt
 * mask is what couples them. Decoupling properly is a bigger change.
 *
 * What this fixes is the COST: a steal costs the audio clock the whole
 * threshold. Healthy holds measure 0.03 to 0.34 ms, so 20 ms is ~60x the worst
 * honest hold and cannot fire on one. */
#define AV_MASK_STEAL_MS  20u         /* give up and take it; see mask_acquire */
/* How long the tick waits out one of the game's inline `cli` regions before
 * running anyway: about a fifth of the 5.4 ms tick, slack for the OS
 * preempting the game inside one. */
#define AV_IRQ_WAIT_US    1000u

static CRITICAL_SECTION s_mask_cs;    /* guards the three fields below, briefly */
static HANDLE           s_mask_free;  /* auto-reset, pulsed on a full release   */
static av_mask_role_t   s_mask_owner;
static int              s_mask_nest;
static DWORD            s_dcs_tid;    /* our tick: the one thread that is the ISR */

static volatile LONG s_dcs_unbalanced;/* unmask with no matching mask          */
static volatile LONG s_dcs_leaked;    /* depth cb() took and did not give back */
static volatile LONG s_dcs_stolen;    /* masks taken after AV_MASK_STEAL_MS    */
/* Plays that arrived on a voice still carrying an earlier sound's pitch. The
 * count is the defect's rate; see av_Play for why it is a defect. */
static volatile LONG s_pitch_reverted;
/* HOW CLOSE THE PCM RAN TO THE CLIFF.
 *
 * s_n_starved counts GetBuffer FAILING, which is not what a dropout is: when
 * the render thread merely wakes late WASAPI plays whatever is still in the
 * endpoint buffer and fails nothing. So `0 starved` is not evidence of a clean
 * stream. Padding is the honest measure; how many frames the endpoint has
 * left when we wake to refill, and its MINIMUM over an interval is the margin
 * the stream actually ran on. */
static volatile LONG s_pad_min = -1;       /* frames; -1 = nothing sampled yet */
static volatile LONG s_n_drained;          /* wakes that found the endpoint empty */

/* HOW LONG THE AUDIO CLOCK STOPPED FOR.
 *
 * The tick RATE is the wrong number. _dcs_TimerCB ignores the interval we hand
 * it and drives the playlist from an ABSOLUTE tick count compared against
 * per-entry deadlines, so a slow tick does not play the music slow; it makes
 * the playlist COARSE, and what is audible is a GAP ended by every missed
 * deadline firing at once. One 200 ms hole in a 30 s interval costs 37 of
 * about 5,556 ticks, under 1% of the rate, and the user hears the music stop. */
#define AV_TICK_LATE_MULT 4u          /* later than 4x the period is a gap */
static LARGE_INTEGER s_tick_woke;     /* when the tick last came off its wait */
static volatile LONG s_tick_gap_max_us;
static volatile LONG s_tick_late;
/* WHO was holding it. s_mask_owner is a ROLE, and a role cannot be suspended
 * and sampled: without it a steal costs seconds of audio clock and nothing can
 * say which thread was stuck. */
static DWORD s_mask_tid;
/* When the CURRENT holder took it. The steal fires after the waiter has waited,
 * which says nothing about how long any one holder has held, a thread that
 * acquires and releases every few microseconds can starve the tick thread for a
 * second without ever holding it long. Those are opposite bugs: a stuck holder
 * wants unsticking, a starved waiter wants fairness. */
static LARGE_INTEGER s_mask_since, s_mask_qpf;
static volatile LONG s_mask_takes;    /* acquisitions, to size the churn */
static volatile LONG s_mask_held_max_us;  /* longest single hold this interval */
/* WHERE the masked region began. A holder sampled mid-flight names where the
 * thread had got to, never where it masked. patch_jmp puts our handler at the
 * game's own _EtsPicEnable entry, so our return address IS the game's call
 * site. */
static uint32_t s_mask_from;
static volatile LONG s_holder_reports;
#define AV_HOLDER_REPORTS 8L          /* then stay quiet */

static int      s_master  = 255;      /* _audio_Init sets 0xFF                 */
static float    s_atten_l = 1.0f;     /* cabinet's front-speaker bus trim      */
static float    s_atten_r = 1.0f;     /* cabinet's seat-woofer bus trim        */
static float    s_user_gain = 1.0f;   /* [audio] volume, host-side only        */

/* Route the cabinet's front-speaker and seat-woofer buses to the selected host
 * output while preserving each title's gain law. */
typedef enum { ROUTE_MIX = 0, ROUTE_CABINET } av_route_t;
static av_route_t s_route = ROUTE_MIX;

/* Apply an attenuate-only balance between front and seat buses. A value of 50
 * preserves unity gain for both buses.
 *
 * mix keeps the two bus gains at constant power: -3 dB at 50, where both are
 * at unity, rising to unity at either end, where one bus plays alone. So 0
 * plays the front bus alone on both speakers at its own level. */
static float s_bal_front = 1.0f;
static float s_bal_seat  = 1.0f;
static float s_mix_gain  = 0.70710678f;

static void balance_set(unsigned percent)
{
    if (percent > 100u)
        percent = 100u;
    s_bal_front = percent <= 50u ? 1.0f : (float)(100u - percent) / 50.0f;
    s_bal_seat  = percent >= 50u ? 1.0f : (float)percent / 50.0f;
    s_mix_gain  = 1.0f / sqrtf(s_bal_front * s_bal_front +
                               s_bal_seat * s_bal_seat);
}

void audio_route_buses(float front, float seat, float *left, float *right)
{
    front *= s_bal_front;
    seat  *= s_bal_seat;

    switch (s_route) {
    case ROUTE_CABINET:
        *left = front;
        *right = seat;
        break;
    default:
        /* Both buses to both speakers. At balance 50 the gain is -3 dB, so a
         * constant-power Hydro voice centred across both buses sums back to
         * unity. The same linear final routing applies to Offroad's
         * independently programmed bus gains. */
        *left = *right = (front + seat) * s_mix_gain;
        break;
    }
}

static const char *route_description(void)
{
    static char text[96];

    snprintf(text, sizeof text, "%s, balance front %.2f / seat %.2f, mix %.2f",
             s_route == ROUTE_CABINET
                 ? "cabinet (front bus left, seat woofer right)"
                 : "mix (both cabinet buses to both speakers)",
             s_bal_front, s_bal_seat, s_mix_gain);
    return text;
}

/* ---- WASAPI ------------------------------------------------------------- */

static IMMDeviceEnumerator *s_enum;
static IMMDevice           *s_dev;
static IAudioClient        *s_client;
static IAudioRenderClient  *s_render;
static WAVEFORMATEX        *s_fmt;
static HANDLE  s_wake;
static HANDLE  s_render_thread;
static UINT32  s_buf_frames;
static float  *s_scratch;             /* interleaved stereo mix, 1.0 = full    */
static int     s_fmt_float;           /* device wants float32, else int16      */
static int     s_channels;
static uint32_t s_out_rate = AV_OUT_RATE;
static volatile LONG s_stop;
static int     s_ready;
static int     s_driver_installed;

/* ---- DCS tick ----------------------------------------------------------- */

typedef void (__cdecl *av_dcs_cb_t)(uint32_t seconds_as_float_bits);

static av_dcs_cb_t s_dcs_cb;
static HANDLE      s_dcs_thread;
/* The cabinet's audio interrupt period, 5.4 ms. The game's fades (0x115808)
 * add a fixed step per tick sized for 0.0054 s and end on real time, so any
 * other rate breaks them (at 5 ms a fade to silence wrapped and came back
 * loud). Microseconds, because 5.4 is not whole; dcs_thread keeps it exact on
 * average with a deadline. */
static unsigned    s_dcs_period_us = 5400;

/* ---- counters (reported at shutdown, cheap enough to always keep) -------- */

static unsigned s_n_play, s_n_stop, s_n_starved, s_n_ticks;

/* ------------------------------------------------------------------------- */
/*  mixing                                                                    */
/* ------------------------------------------------------------------------- */

/* The game's own pan law, recomputed rather than read out of
 * _audio_dspcmd_sineMap: reading that table would mean the mixer thread
 * touching game memory _ymf_Init may be rewriting. Same curve to the last bit
 * that matters at 16-bit output. */
static float s_pan_table[256];

static void pan_table_init(void)
{
    int i;
    for (i = 0; i < 256; i++)
        s_pan_table[i] = (float)sin((double)i * (1.0 / 256.0) * 1.5707963267948966);
}

/* The DS-XG pitch law, exactly as the interrupt handler computed it. */
static uint32_t effective_rate(const av_voice_t *v)
{
    uint32_t r = v->rate;
    int p = v->pitch;

    if (p <= 0x80)
        return (uint32_t)(((uint64_t)(p + 0x80) * r) >> 8);
    return (uint32_t)(((uint64_t)p * r) >> 7);
}

static void mix_voice(av_voice_t *v, float *out, uint32_t frames)
{
    uint32_t rate = effective_rate(v);
    uint64_t step;
    float base, front, seat, gl, gr;
    uint32_t i;

    if (!v->pcm || !v->frames || !rate)
        return;

    step = ((uint64_t)rate << 32) / s_out_rate;

    /* The two cabinet buses, exactly as the interrupt handler computed the
     * hardware's left_gain / right_gain (see 0x1164c6). */
    base  = (float)v->vol * (1.0f / 256.0f)
          * (float)s_master * (1.0f / 255.0f) * s_user_gain * (1.0f / 32768.0f);
    front = base * s_pan_table[255 - v->pan] * s_atten_l;
    seat  = base * s_pan_table[v->pan]       * s_atten_r;

    audio_route_buses(front, seat, &gl, &gr);

    for (i = 0; i < frames; i++) {
        uint32_t idx = (uint32_t)(v->pos >> 32);
        uint32_t nxt;
        float frac, s;

        if (idx >= v->frames) {
            if (!v->loop) {
                v->active = 0;
                return;
            }
            v->pos -= (uint64_t)v->frames << 32;
            idx = (uint32_t)(v->pos >> 32);
            if (idx >= v->frames) {          /* pathological step; give up */
                v->active = 0;
                return;
            }
        }

        nxt = idx + 1;
        if (nxt >= v->frames)
            nxt = v->loop ? 0 : idx;

        frac = (float)(uint32_t)(v->pos & 0xFFFFFFFFu) * (1.0f / 4294967296.0f);
        s = (float)v->pcm[idx] + ((float)v->pcm[nxt] - (float)v->pcm[idx]) * frac;

        out[2 * i]     += s * gl;
        out[2 * i + 1] += s * gr;
        v->pos += step;
    }
}

static void mix_block(float *out, uint32_t frames)
{
    int i;

    memset(out, 0, (size_t)frames * 2 * sizeof(float));

    EnterCriticalSection(&s_voice_cs);
    for (i = 0; i < AV_VOICES; i++)
        if (s_voice[i].active)
            mix_voice(&s_voice[i], out, frames);
    LeaveCriticalSection(&s_voice_cs);

    /* Offroad owns a different voice ABI and lock, but shares this WASAPI
     * transport. Never nest the locks: its deallocator must be able to wait for
     * a mix before the game frees PCM. */
    audio_offroad_mix(out, frames, s_out_rate, s_user_gain);
}

/* Sum of up to 16 simultaneous voices at full scale will clip; the DS-XG's
 * accumulator saturated too. Hard-clamp rather than scale, so a loud moment does
 * not duck everything else. */
static void emit(const float *mix, void *dst, uint32_t frames)
{
    uint32_t i;
    int c;

    if (s_fmt_float) {
        float *d = (float *)dst;
        for (i = 0; i < frames; i++) {
            for (c = 0; c < s_channels; c++) {
                float v = (c < 2) ? mix[2 * i + c] : 0.0f;
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                d[(size_t)i * s_channels + c] = v;
            }
        }
    } else {
        int16_t *d = (int16_t *)dst;
        for (i = 0; i < frames; i++) {
            for (c = 0; c < s_channels; c++) {
                float v = (c < 2) ? mix[2 * i + c] : 0.0f;
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                d[(size_t)i * s_channels + c] = (int16_t)(v * 32767.0f);
            }
        }
    }
}

/* Periodic census, at info level. The counters are otherwise only reported by
 * audio_shutdown(), which a `taskkill` run never reaches, and "is anything
 * actually playing, and at what pan" is the first question every audio problem
 * asks. Same cadence as the frame-rate reporter in glide_bind.c. */
static void census(void)
{
    char line[512];
    int n = 0, active = 0, i;

    if (audio_offroad_active()) {
        audio_offroad_census(s_n_starved);
        return;
    }

    line[0] = '\0';
    EnterCriticalSection(&s_voice_cs);
    for (i = 0; i < AV_VOICES; i++) {
        if (!s_voice[i].active)
            continue;
        active++;
        if (n < 6)
            n += snprintf(line + n, sizeof line - (size_t)n,
                          " [v%d vol=%d pan=%d pitch=%d %uHz%s]",
                          i, s_voice[i].vol, s_voice[i].pan, s_voice[i].pitch,
                          (unsigned)s_voice[i].rate,
                          s_voice[i].loop ? " loop" : "");
    }
    LeaveCriticalSection(&s_voice_cs);

    LOGI("audio: %u plays, %u stops, %u DCS ticks, %u starved, master=%d, "
         "%d voice(s) active%s",
         s_n_play, s_n_stop, s_n_ticks, s_n_starved, s_master, active, line);

    /* Printed every interval even when healthy, unlike the fault counters
     * below: the number a dropout report needs is the MARGIN the stream ran
     * on, and a margin is only meaningful as a series. */
    {
        LONG pad = InterlockedExchange(&s_pad_min, -1);
        LONG dry = InterlockedExchange(&s_n_drained, 0);

        if (pad >= 0 && s_out_rate)
            LOGI("audio: PCM margin this interval: least padding %ld frames "
                 "(%.1f ms) of a %u-frame buffer (%.1f ms)",
                 (long)pad, 1000.0 * (double)pad / (double)s_out_rate,
                 (unsigned)s_buf_frames,
                 1000.0 * (double)s_buf_frames / (double)s_out_rate);
        {
            LONG gap = InterlockedExchange(&s_tick_gap_max_us, 0);
            LONG late = InterlockedExchange(&s_tick_late, 0);

            if (gap)
                LOGI("audio: DCS tick gap this interval: longest %.1f ms of a "
                     "%.1f ms period; %ld tick(s) later than %ux",
                     (double)gap / 1000.0, s_dcs_period_us / 1000.0, (long)late,
                     AV_TICK_LATE_MULT);
            /* Generous on purpose. Below this the playlist is merely coarse;
             * above it there is a hole the game started and stopped nothing in,
             * and the catch-up is what gets heard. */
            if (gap > 40000)
                LOGE("audio: the DCS tick did not run for %.0f ms. Hydro's "
                     "playlist starts and stops no sample in such a window and "
                     "then fires every missed deadline at once, which is heard "
                     "as the music cutting out", (double)gap / 1000.0);
        }
        if (dry)
            LOGE("audio: the endpoint RAN DRY on %ld refill(s) this interval: "
                 "the render thread woke to find nothing left to play. That is "
                 "an audible dropout, and it is a scheduling fault, not a mixer "
                 "one", (long)dry);
    }

    /* The IRQ mask's own shape. `longest hold` against `takes` separates a slow
     * holder from a starved waiter, and it is reported every interval whether or
     * not a steal happened, so no run is wasted waiting for one. */
    LOGI("audio: irq mask: %ld acquisitions, longest single hold %.2f ms",
         (long)InterlockedExchange(&s_mask_takes, 0),
         (double)InterlockedExchange(&s_mask_held_max_us, 0) / 1000.0);

    /* Only when nonzero: a line that prints "0" every 30 s trains the reader to
     * skip it. The first two are RATES, not errors: on a global IRQ mask an
     * unmatched unmask is an unmask of something already unmasked, and the
     * leaked levels are cb()'s, unwound. The third is a real fault. */
    {
        LONG unbal = InterlockedExchange(&s_dcs_unbalanced, 0);
        LONG leaked = InterlockedExchange(&s_dcs_leaked, 0);
        LONG stolen = InterlockedExchange(&s_dcs_stolen, 0);

        if (unbal)
            LOGI("audio: %ld DCS unmask(es) this interval had no matching mask "
                 "(normal for a global IRQ mask; counted, not acted on)",
                 (long)unbal);
        if (leaked)
            LOGI("audio: %ld DCS mask level(s) leaked by cb() and unwound this "
                 "interval", (long)leaked);
        {
            LONG rev = InterlockedExchange(&s_pitch_reverted, 0);

            if (rev)
                LOGI("audio: %ld play(s) this interval landed on a voice still "
                     "carrying an earlier sound's pitch and were reverted to "
                     "unity, as _audio_dspcmd_Play's own drain did", (long)rev);
        }
        if (stolen)
            LOGE("audio: %ld DCS mask(es) STOLEN after %u ms this interval; a "
                 "holder was stuck. The game kept running; find out why",
                 (long)stolen, AV_MASK_STEAL_MS);
    }
}

static DWORD WINAPI render_thread(LPVOID unused)
{
    DWORD next_census = GetTickCount() + 30000;
    DWORD mmcss_task = 0;
    HANDLE mmcss;
    /* IAudioClient::Start leaves the endpoint empty, so the FIRST wake always
     * finds zero padding, and reporting that as a dropout would print RAN DRY
     * on every launch, which is what makes s_n_starved useless. Nothing is
     * sampled until one buffer has been written. */
    int primed = 0;

    (void)unused;

    /* MMCSS gives the audio callback deadline-aware priority without letting a
     * raw TIME_CRITICAL thread starve the game and D3D12 submission threads. */
    mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss_task);
    if (!mmcss)
        LOGW("audio: MMCSS Audio registration failed %lu; continuing at "
             "normal thread priority", GetLastError());

    while (!s_stop) {
        UINT32 pad = 0, avail;
        BYTE *dst = NULL;

        if ((LONG)(GetTickCount() - next_census) >= 0) {
            next_census = GetTickCount() + 30000;
            census();
        }

        if (WaitForSingleObject(s_wake, 200) == WAIT_TIMEOUT)
            continue;
        if (s_stop)
            break;

        if (FAILED(IAudioClient_GetCurrentPadding(s_client, &pad)))
            continue;
        if (primed) {
            LONG seen = (LONG)pad, was;

            /* Monotone minimum, published without a lock: the census resets it
             * to -1 and races only itself, so the worst outcome is one
             * interval's minimum landing in the next one. */
            for (;;) {
                was = s_pad_min;
                if (was >= 0 && was <= seen)
                    break;
                if (InterlockedCompareExchange(&s_pad_min, seen, was) == was)
                    break;
            }
            if (!pad)
                InterlockedIncrement(&s_n_drained);
        }
        avail = s_buf_frames - pad;
        if (!avail)
            continue;

        if (FAILED(IAudioRenderClient_GetBuffer(s_render, avail, &dst))) {
            s_n_starved++;
            continue;
        }
        mix_block(s_scratch, avail);
        emit(s_scratch, dst, avail);
        IAudioRenderClient_ReleaseBuffer(s_render, avail, 0);
        primed = 1;
    }
    if (mmcss)
        AvRevertMmThreadCharacteristics(mmcss);
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  the DS-XG command interface, as the game calls it                         */
/* ------------------------------------------------------------------------- */

static int voice_ok(uint32_t v)
{
    if (v < AV_VOICES)
        return 1;
    LOGW("audio: voice index %u out of range", (unsigned)v);
    return 0;
}

/* A diagnostic that re-runs game code and then restores the game's memory
 * (the draw replay) must not let that code reach the speakers: a voice it
 * started would outlive the game's own record of it and loop for ever. Only
 * the thread doing the replay is held; the DCS thread's commands pass. Both
 * titles' voice entries ask audio_held(): Hydro's DSP commands here, Offroad's
 * Audiolib table in audio_offroad.c. */
static volatile DWORD s_hold_tid;

void audio_hold_this_thread(int on)
{
    s_hold_tid = on ? GetCurrentThreadId() : 0;
}

static volatile LONG s_held_cmds;

unsigned audio_held_commands(void) { return (unsigned)InterlockedExchange(&s_held_cmds, 0); }

int audio_held(void)
{
    if (s_hold_tid && s_hold_tid == GetCurrentThreadId()) {
        InterlockedIncrement(&s_held_cmds);
        return 1;
    }
    return 0;
}
#define AV_HELD() audio_held()

static void __cdecl av_Play(uint32_t voice, const int16_t *pcm, uint32_t rate,
                            uint32_t bytes, uint32_t loop_default)
{
    if (AV_HELD())
        return;
    av_voice_t *v;
    int loop = 0, i;

    if (!voice_ok(voice))
        return;

    /* loopDefault 0 loops, as the DS-1 does with a play bank left at 0: race
     * music is 0 and plays until the game stops it, one-shots are 1. The
     * engine and drone loops are 0 as well, and also flagged at
     * allocation. */
    loop = loop_default == 0;
    for (i = 0; i < AV_BUFFERS; i++)
        if (s_buffer[i].pcm == pcm) {
            loop |= s_buffer[i].loop;
            break;
        }

    EnterCriticalSection(&s_voice_cs);
    v = &s_voice[voice];
    v->pcm    = pcm;
    v->frames = bytes / 2u;                 /* 16-bit mono */
    v->rate   = rate;
    v->pos    = 0;
    v->loop   = loop;
    v->active = (pcm && v->frames) ? 1 : 0;
    /* Pitch is per sound, not per voice, as in the game: _audio_dspcmd_Play
     * sets dirty bit 0x1 without touching the pitch cell, and the drain
     * programs the raw rate unless ChangePitch's 0x4 is also pending. Keeping
     * the old pitch would play a one-shot on a wound-down engine voice an
     * octave low. A ChangePitch either side of a Play within one tick still
     * wins; the flag is set without the voice lock, so a lost update costs one
     * tick at unity. */
    if (v->pitch_dirty) {
        /* pending: the drain would have applied it, so leave it alone */
    } else if (v->pitch != 0x80) {
        InterlockedIncrement(&s_pitch_reverted);
        v->pitch = 0x80;
    }
    LeaveCriticalSection(&s_voice_cs);

    s_n_play++;
    LOGT("audio: play  voice=%2u pcm=%p frames=%u rate=%u loop=%d (ld=%u)",
         (unsigned)voice, (const void *)pcm, (unsigned)v->frames,
         (unsigned)rate, loop, (unsigned)loop_default);
}

static void __cdecl av_Stop(uint32_t voice)
{
    if (AV_HELD())
        return;
    if (!voice_ok(voice))
        return;
    EnterCriticalSection(&s_voice_cs);
    s_voice[voice].active = 0;
    LeaveCriticalSection(&s_voice_cs);
    s_n_stop++;
    LOGT("audio: stop  voice=%2u", (unsigned)voice);
}

static void __cdecl av_ChangePitch(uint32_t voice, int32_t pitch)
{
    if (AV_HELD())
        return;
    if (!voice_ok(voice))
        return;
    if (pitch < 0)   pitch = 0;
    if (pitch > 255) pitch = 255;
    s_voice[voice].pitch = pitch;
    s_voice[voice].pitch_dirty = 1;
}

static void __cdecl av_ChangeVolume(uint32_t voice, int32_t vol)
{
    if (AV_HELD())
        return;
    if (!voice_ok(voice))
        return;
    if (vol < 0)   vol = 0;
    if (vol > 255) vol = 255;
    /* Changes only: a fade would otherwise log every step at the same value. */
    if (s_voice[voice].vol != vol)
        LOGT("audio: vol   voice=%2u %d -> %d", (unsigned)voice,
             (int)s_voice[voice].vol, (int)vol);
    s_voice[voice].vol = vol;
}

/* The game's own ChangePan honours a cabinet speaker-mode cell: 0 forces every
 * voice to centre, 2 reverses left and right. Reproduced rather than assumed
 * stereo: an operator running a mono cabinet would otherwise get a stereo mix. */
static void __cdecl av_ChangePan(uint32_t voice, int32_t pan)
{
    if (AV_HELD())
        return;
    uint32_t mode = *(volatile uint32_t *)(uintptr_t)G->speaker_mode;

    if (!voice_ok(voice))
        return;
    if (pan < 0)   pan = 0;
    if (pan > 255) pan = 255;

    if (mode == 0)      pan = 0x7F;
    else if (mode == 2) pan = 255 - pan;

    s_voice[voice].pan = pan;
}

static void __cdecl av_SetMasterVolume(int32_t vol)
{
    if (AV_HELD())
        return;
    if (vol < 0)   vol = 0;
    if (vol > 255) vol = 255;
    s_master = vol;
    LOGT("audio: master volume %d", (int)vol);
}

/* Both arguments are float bit patterns: the game stores them raw and the
 * interrupt handler multiplies with `fmul dword ptr`. */
static void __cdecl av_SetLeftRightAttenuators(uint32_t left_bits,
                                               uint32_t right_bits)
{
    if (AV_HELD())
        return;
    float l, r;

    memcpy(&l, &left_bits,  sizeof l);
    memcpy(&r, &right_bits, sizeof r);

    if (!(l >= 0.0f) || l > 4.0f) l = 1.0f;      /* also catches NaN */
    if (!(r >= 0.0f) || r > 4.0f) r = 1.0f;

    s_atten_l = l;
    s_atten_r = r;
    LOGT("audio: attenuators L=%.3f R=%.3f", (double)l, (double)r);
}

/* ------------------------------------------------------------------------- */
/*  _ymf_Init / _ymf_Shutdown and the sound-buffer hooks                      */
/* ------------------------------------------------------------------------- */

typedef int  (__cdecl *alloc_fn_t)(const int16_t *, uint32_t, uint32_t,
                                   uint32_t, uint32_t, uint32_t);
typedef int  (__cdecl *free_fn_t)(uint32_t);
typedef void (__cdecl *void_fn_t)(void);

static alloc_fn_t s_orig_alloc;
static free_fn_t  s_orig_free;

static DWORD WINAPI dcs_thread(LPVOID unused);

static int __cdecl av_ymf_Init(av_dcs_cb_t cb)
{
    /* Publish the token DCS will hand back to _EtsPicEnable for every one of its
     * critical sections. On real hardware this is the board's PCI interrupt line. */
    *(volatile uint32_t *)(uintptr_t)(G->ymf_info + 4) = AV_IRQ_TOKEN;

    /* Still the game's own code: it fills _audio_dspcmd_sineMap, which
     * _ymf_ConstantPowerCookies reads to cross-fade the engine loops. Skipping it
     * leaves that table zeroed and the engine silent. Pure x87, no hardware. */
    ((void_fn_t)(uintptr_t)G->dspcmd_init)();

    s_dcs_cb = cb;

    if (!s_dcs_thread) {
        s_dcs_thread = CreateThread(NULL, 0, dcs_thread, NULL, 0, NULL);
        if (!s_dcs_thread)
            LOGE("audio: CreateThread(dcs) failed %lu", GetLastError());
    }

    LOGI("audio: _ymf_Init -> ok (callback %p, %.1f ms tick)",
         (void *)cb, s_dcs_period_us / 1000.0);
    return 0;                                   /* _dcs_Init tests for zero */
}

static int __cdecl av_ymf_Shutdown(void)
{
    int i;

    s_dcs_cb = NULL;
    EnterCriticalSection(&s_voice_cs);
    for (i = 0; i < AV_VOICES; i++)
        s_voice[i].active = 0;
    LeaveCriticalSection(&s_voice_cs);
    LOGI("audio: _ymf_Shutdown; all voices silenced");
    return 0;
}

/* Call-through: the original stores {pcm, bytes, rate} into the game's
 * _YmfSoundBuffers. We want argument 4, which it discards: 1 for exactly the
 * four engine and drone loops, 0 for every sample bank. Music in a bank loops
 * by loopDefault instead; see av_Play. */
static int __cdecl av_alloc_buffer(const int16_t *pcm, uint32_t bytes,
                                   uint32_t rate, uint32_t loop,
                                   uint32_t unused, uint32_t index)
{
    if (index < AV_BUFFERS) {
        s_buffer[index].pcm   = pcm;
        s_buffer[index].bytes = bytes;
        s_buffer[index].loop  = loop != 0;
    } else {
        LOGW("audio: sound buffer index %u out of range", (unsigned)index);
    }
    LOGT("audio: alloc buf=%-4u pcm=%p bytes=%u rate=%u loop=%u",
         (unsigned)index, (const void *)pcm, (unsigned)bytes,
         (unsigned)rate, (unsigned)loop);

    return s_orig_alloc(pcm, bytes, rate, loop, unused, index);
}

/* Call-through: the original hands the PCM back to the game's allocator. Stop
 * anything still reading it first: the game does release cookies before
 * unloading a bank, but the mixer runs on another thread and a freed pointer
 * would be read for up to one buffer period. */
static int __cdecl av_free_buffer(uint32_t index)
{
    if (index < AV_BUFFERS) {
        const int16_t *pcm = s_buffer[index].pcm;
        int i;

        if (pcm) {
            EnterCriticalSection(&s_voice_cs);
            for (i = 0; i < AV_VOICES; i++)
                if (s_voice[i].pcm == pcm) {
                    s_voice[i].active = 0;
                    s_voice[i].pcm    = NULL;
                }
            LeaveCriticalSection(&s_voice_cs);
        }
        s_buffer[index].pcm  = NULL;
        s_buffer[index].loop = 0;
    }
    return s_orig_free(index);
}

/* ------------------------------------------------------------------------- */
/*  _EtsPicEnable: IRQ masking becomes a real mutex                           */
/* ------------------------------------------------------------------------- */

/* Reimplement the inlined DCS critical sections through their common IRQ-mask
 * call. Ownership is per role rather than per thread, allowing cross-thread
 * game unmasking while preserving recursive entry by the interrupt callback. */
static av_mask_role_t mask_role(void)
{
    return GetCurrentThreadId() == s_dcs_tid ? AV_MASK_TICK : AV_MASK_GAME;
}

static int mask_nest_of(av_mask_role_t role)
{
    int n;
    EnterCriticalSection(&s_mask_cs);
    n = s_mask_owner == role ? s_mask_nest : 0;
    LeaveCriticalSection(&s_mask_cs);
    return n;
}

/* Name the thread that was holding the mask, and say where it was.
 *
 * Sampled into locals and the thread RESUMED before anything is logged:
 * log_printf takes the CRT's lock for stderr, and taking that while the sampled
 * thread might hold it is a deadlock. watchdog.c logs while its target is still
 * suspended; this deliberately does not. */
static void report_mask_holder(DWORD tid, av_mask_role_t held, unsigned waited)
{
    HANDLE h;
    CONTEXT ctx;
    uint32_t chain[8], eip = 0, ebp = 0;
    unsigned depth = 0, i;
    int got = 0, in_game;
    char line[512];
    int n;

    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                   THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) {
        LOGE("audio: the mask was held by thread %lu; cannot open it (%lu), so "
             "it cannot be sampled", (unsigned long)tid,
             (unsigned long)GetLastError());
        return;
    }
    if (SuspendThread(h) != (DWORD)-1) {
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &ctx)) {
            got = 1;
            eip = ctx.Eip;
            ebp = ctx.Ebp;
            depth = diag_return_chain(ebp, chain,
                                      (unsigned)(sizeof chain /
                                                 sizeof chain[0]));
        }
        ResumeThread(h);
    }
    CloseHandle(h);

    in_game = got && eip >= G->image_base && eip < G->span_end;
    n = snprintf(line, sizeof line,
                 "audio: the mask was held for %u ms by %s, thread %lu, EIP=",
                 waited, held == AV_MASK_TICK ? "the tick" : "the game",
                 (unsigned long)tid);
    if (n > 0 && (size_t)n < sizeof line)
        n += diag_describe_addr(line + n, sizeof line - (size_t)n, got ? eip : 0);
    if (n > 0 && (size_t)n < sizeof line)
        n += snprintf(line + n, sizeof line - (size_t)n, "; return chain:");
    for (i = 0; i < depth && n > 0 && (size_t)n < sizeof line; i++) {
        n += snprintf(line + n, sizeof line - (size_t)n, " ");
        if (n > 0 && (size_t)n < sizeof line)
            n += diag_describe_addr(line + n, sizeof line - (size_t)n, chain[i]);
    }
    LOGE("%s", line);
    if (in_game)
        LOGE("audio: name it with  python3 scripts/p1-symbols.py --lookup "
             "0x%08lX", (unsigned long)eip);
}

/* Acquire the IRQ mask with a bounded wait. On timeout, transfer ownership and
 * report the previous role to keep the cabinet responsive. */
static void mask_acquire(av_mask_role_t role, uint32_t from)
{
    unsigned waited = 0;

    for (;;) {
        EnterCriticalSection(&s_mask_cs);
        if (s_mask_owner == AV_MASK_FREE || s_mask_owner == role) {
            s_mask_owner = role;
            if (s_mask_nest == 0) {
                s_mask_tid = GetCurrentThreadId();
                if (!s_mask_qpf.QuadPart)
                    QueryPerformanceFrequency(&s_mask_qpf);
                QueryPerformanceCounter(&s_mask_since);
                s_mask_from = from;
            }
            s_mask_nest++;
            InterlockedIncrement(&s_mask_takes);
            LeaveCriticalSection(&s_mask_cs);
            return;
        }
        LeaveCriticalSection(&s_mask_cs);

        if (!s_mask_free) {             /* not started yet: nothing to wait on */
            Sleep(0);
            continue;
        }
        if (WaitForSingleObject(s_mask_free, AV_MASK_POLL_MS) == WAIT_OBJECT_0)
            continue;                   /* released; go and try to claim it  */

        waited += AV_MASK_POLL_MS;
        if (waited < AV_MASK_STEAL_MS)
            continue;

        {
            av_mask_role_t held;
            DWORD held_tid;
            LARGE_INTEGER held_since, qpc_now;
            uint32_t held_from;
            LONG takes;
            char site[160];
            double held_ms = -1.0;

            QueryPerformanceCounter(&qpc_now);
            EnterCriticalSection(&s_mask_cs);
            held = s_mask_owner;
            held_tid = s_mask_tid;
            held_since = s_mask_since;
            held_from = s_mask_from;
            takes = InterlockedExchange(&s_mask_takes, 0);
            if (InterlockedIncrement(&s_dcs_stolen) == 1)
                LOGE("audio: the DCS audio mask was held by %s for %u ms; taking "
                     "it for %s rather than blocking forever. Expected: the game "
                     "holds a DCS lock and spins on its clock waiting for DCS "
                     "progress that %s can only make once it has this mask. The "
                     "steal breaks that cycle; the count is the rate, not a new "
                     "fault",
                     held == AV_MASK_TICK ? "the tick" : "the game",
                     waited, role == AV_MASK_TICK ? "the tick" : "the game",
                     role == AV_MASK_TICK ? "the tick" : "the game");
            s_mask_owner = role;
            s_mask_nest = 1;
            s_mask_tid = GetCurrentThreadId();
            /* The stealer is the holder from NOW: otherwise the census's
             * "longest single hold" runs from the STOLEN holder's start through
             * the stealer's release, so every steal reports the wait as part of
             * a hold. The honest per-holder number is logged just below. */
            s_mask_since = qpc_now;
            LeaveCriticalSection(&s_mask_cs);

            if (s_mask_qpf.QuadPart && held_since.QuadPart)
                held_ms = 1000.0 * (double)(qpc_now.QuadPart - held_since.QuadPart) /
                          (double)s_mask_qpf.QuadPart;
            /* THE measurement: how long the holder we are taking it from has
             * actually held it, against the 1000 ms we waited. Milliseconds here
             * means the waiter was starved, not that anyone was stuck. */
            diag_describe_addr(site, sizeof site, held_from);
            LOGE("audio: the waiter waited %u ms; the holder had held it for "
                 "%.1f ms, over %ld acquisitions since the last report; it "
                 "MASKED AT %s", waited, held_ms, (long)takes, site);

            /* Outside the lock, and capped: an error path that logs per
             * occurrence is how 28.4 GB gets written in one run. */
            if (held_tid && held_tid != GetCurrentThreadId() &&
                InterlockedIncrement(&s_holder_reports) <= AV_HOLDER_REPORTS)
                report_mask_holder(held_tid, held, waited);
        }
        return;
    }
}

static void mask_release(av_mask_role_t role)
{
    EnterCriticalSection(&s_mask_cs);
    if (s_mask_owner != role || s_mask_nest <= 0) {
        /* Unmasking what is already unmasked, or what the other role holds.
         * Both are no-ops on real hardware. Counted, never acted on. */
        InterlockedIncrement(&s_dcs_unbalanced);
    } else if (--s_mask_nest == 0) {
        /* Every hold, not just the ones a steal happens to catch. Making the
         * measurement conditional on the exception meant a 6-race run with no
         * steal said nothing at all; the distribution is what answers whether a
         * holder is slow or a waiter is starved, and it is free to keep. */
        if (s_mask_qpf.QuadPart && s_mask_since.QuadPart) {
            LARGE_INTEGER now;
            LONG us;

            QueryPerformanceCounter(&now);
            us = (LONG)(1000000.0 * (double)(now.QuadPart - s_mask_since.QuadPart) /
                        (double)s_mask_qpf.QuadPart);
            if (us > s_mask_held_max_us)
                s_mask_held_max_us = us;
        }
        s_mask_owner = AV_MASK_FREE;
        s_mask_tid = 0;
        s_mask_since.QuadPart = 0;
        if (s_mask_free)
            SetEvent(s_mask_free);
    }
    LeaveCriticalSection(&s_mask_cs);
}

int audio_mask_depth(void)
{
    return s_cs_ready ? mask_nest_of(mask_role()) : 0;
}

void audio_mask_unwind(int depth)
{
    if (!s_cs_ready)
        return;
    while (mask_nest_of(mask_role()) > depth)
        mask_release(mask_role());
}

static int __cdecl av_EtsPicEnable(uint32_t irq, uint32_t enable)
{
    if (irq != AV_IRQ_TOKEN)
        return 0;
    /* audio_install() patches this in unconditionally, but audio_preload()
     * (which creates the mask) only runs when [audio] audio is true. With audio
     * off there is no tick thread and so no ISR to exclude: doing nothing is
     * correct, and the alternative is entering an uninitialised
     * CRITICAL_SECTION. */
    if (!s_cs_ready)
        return 0;
    if (enable)
        mask_release(mask_role());
    else
        mask_acquire(mask_role(),
                     (uint32_t)(uintptr_t)__builtin_return_address(0));
    return 0;
}

/* Report IRQ-mask ownership, nesting, tick progress, and thread identity for
 * watchdog diagnostics. */
void audio_report_dcs_state(void)
{
    static unsigned last_ticks;
    static int have_last;
    CONTEXT ctx;
    DWORD eip = 0;
    int got = 0, in_game, nest;
    av_mask_role_t owner;

    if (s_dcs_thread && SuspendThread(s_dcs_thread) != (DWORD)-1) {
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_dcs_thread, &ctx)) {
            eip = ctx.Eip;
            got = 1;
        }
        ResumeThread(s_dcs_thread);
    }
    in_game = got && (uint32_t)eip >= G->image_base &&
              (uint32_t)eip < G->span_end;

    LOGE("watchdog: our threads; caller(watchdog)=%lu dcs=%lu wasapi=%lu",
         (unsigned long)GetCurrentThreadId(),
         (unsigned long)(s_dcs_thread ? GetThreadId(s_dcs_thread) : 0),
         (unsigned long)(s_render_thread ? GetThreadId(s_render_thread) : 0));

    EnterCriticalSection(&s_mask_cs);
    owner = s_mask_owner;
    nest  = s_mask_nest;
    LeaveCriticalSection(&s_mask_cs);

    LOGE("watchdog: DCS mask owner=%s nest=%d stolen=%ld | ticks %u (%s) | "
         "dcs thread EIP=0x%08lX %s",
         owner == AV_MASK_TICK ? "TICK" : owner == AV_MASK_GAME ? "GAME" : "free",
         nest, (long)s_dcs_stolen, s_n_ticks,
         have_last ? (s_n_ticks != last_ticks ? "ADVANCING" : "STOPPED")
                   : "first sample",
         (unsigned long)eip,
         !got ? "(could not sample)"
              : in_game ? "(IN GAME CODE: cb() is stuck holding the mask)"
                        : "(outside the game image)");

    last_ticks = s_n_ticks;
    have_last = 1;
}

/* ------------------------------------------------------------------------- */
/*  the DCS tick: what the audio interrupt used to drive                    */
/* ------------------------------------------------------------------------- */

/* The real interrupt handed _dcs_TimerCB a constant 0.0054 s and _dcs_DrivePL
 * re-measured elapsed time with the game's own timer anyway, forwarding the float
 * to the audio driver's callback. Passing the measured interval instead of a
 * constant is strictly more correct and costs nothing. */
static DWORD WINAPI dcs_thread(LPVOID unused)
{
    LARGE_INTEGER freq, last, next;
    HANDLE timer;
    LARGE_INTEGER due;
    LONGLONG period;

    (void)unused;
    s_tick_woke.QuadPart = 0;
    /* Before anything can call _EtsPicEnable on this thread: mask_role() reads
     * this to tell the ISR from the game, and a tick that looked like the game
     * would wait on a mask it is itself holding. */
    s_dcs_tid = GetCurrentThreadId();
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    /* A one-shot timer re-armed against a deadline, because the periodic
     * timer takes whole milliseconds. High resolution where Windows has it,
     * because the plain timer and Sleep wait at the system timer's grain,
     * 15.6 ms unless something in the process raised it, and at that grain
     * most ticks are dropped and every fade ends early. Each wait rounds to
     * the timer's grain; the deadline advances by exactly one period, so the
     * average is exact. Logged, because the tick's census cannot tell which. */
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
    if (timer) {
        LOGI("audio: DCS tick timer: high resolution");
    } else {
        DWORD err = GetLastError();

        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        LOGW("audio: DCS tick timer: no high-resolution timer (error %lu); "
             "%s, at the system timer's grain", (unsigned long)err,
             timer ? "plain waitable timer" : "Sleep");
    }
    period = (LONGLONG)s_dcs_period_us * freq.QuadPart / 1000000;
    if (period <= 0)
        period = 1;
    next.QuadPart = last.QuadPart + period;

    while (!s_stop) {
        av_dcs_cb_t cb;
        LARGE_INTEGER now;
        double dt;
        float dtf;
        uint32_t bits, saved;
        volatile uint32_t *depth =
            (volatile uint32_t *)(uintptr_t)G->dcs_lock_depth;

        {
            LARGE_INTEGER t0;
            LONGLONG rem;

            QueryPerformanceCounter(&t0);
            rem = next.QuadPart - t0.QuadPart;
            if (rem > 0) {
                due.QuadPart = -(rem * 10000000 / freq.QuadPart);
                if (due.QuadPart == 0)
                    due.QuadPart = -1;
                if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(timer, 100);
                else
                    Sleep((DWORD)((rem * 1000 + freq.QuadPart - 1) / freq.QuadPart));
            }
            /* A whole missed period is dropped, not replayed, as a masked
             * interrupt on the cabinet was delivered once. The phase is kept,
             * so jitter never drifts the rate. */
            QueryPerformanceCounter(&t0);
            next.QuadPart += period;
            if (t0.QuadPart >= next.QuadPart)
                next.QuadPart += ((t0.QuadPart - next.QuadPart) / period + 1) * period;
        }
        if (s_stop)
            break;

        /* Measured across the WAIT ONLY; before the mask is taken and before
         * any game code runs, so this is the scheduler's answer and not the
         * callback's. `last` cannot serve: it is reset after the callback and
         * folds the tick's own duration in. */
        {
            LARGE_INTEGER woke;

            QueryPerformanceCounter(&woke);
            if (freq.QuadPart && s_tick_woke.QuadPart) {
                LONG gap_us = (LONG)(1000000LL *
                                     (woke.QuadPart - s_tick_woke.QuadPart) /
                                     freq.QuadPart);

                if (gap_us > s_tick_gap_max_us)
                    InterlockedExchange(&s_tick_gap_max_us, gap_us);
                if ((unsigned)gap_us > AV_TICK_LATE_MULT * s_dcs_period_us)
                    InterlockedIncrement(&s_tick_late);
            }
            s_tick_woke = woke;
        }

        cb = s_dcs_cb;
        if (!cb)
            continue;

        QueryPerformanceCounter(&now);
        dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;
        if (dt < 0.0 || dt > 1.0)
            dt = (double)s_dcs_period_us / 1000000.0;
        dtf = (float)dt;
        memcpy(&bits, &dtf, sizeof bits);

        /* The ISR runs with the IRQ masked, so take the mask for the whole tick.
         * A game thread wanting to mask now waits until cb() returns, which is
         * exactly what the cabinet's single CPU did to it. */
        game_side_thread_enter();       /* before the mask: the replay's order */
        mask_acquire(AV_MASK_TICK, 0);
        saved = *depth;
        *depth = 0;
        {
            /* Restore the interrupt thread's entry depth after callbacks that
             * leave nested mask levels active. */
            int before = mask_nest_of(AV_MASK_TICK);
            unsigned leaked = 0;

            /* The playlist callback executes game code as the cabinet audio
             * interrupt. Acquire the audio mask first, then honour CLI/STI
             * emulation before entering the callback. */
            game_thread_seen("the game's DCS playlist driver");
            game_irq_wait(AV_IRQ_WAIT_US);
            cb(bits);

            while (mask_nest_of(AV_MASK_TICK) > before) {
                mask_release(AV_MASK_TICK);
                leaked++;
            }
            if (leaked)
                InterlockedExchangeAdd(&s_dcs_leaked, (LONG)leaked);
        }
        *depth = saved;
        /* The drain ran once per interrupt, after the callback had queued that
         * tick's commands. This is that boundary: everything the callback just
         * asked for has been applied, so no pitch is pending any longer. */
        {
            int i;
            for (i = 0; i < AV_VOICES; i++)
                s_voice[i].pitch_dirty = 0;
        }
        mask_release(AV_MASK_TICK);
        game_side_thread_leave();
        s_n_ticks++;
    }

    if (timer) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  device setup                                                              */
/* ------------------------------------------------------------------------- */

/* mingw declares the KSDATAFORMAT_SUBTYPE_* GUIDs but ships them in no import
 * library the shim links, so spell the two we need out. They are the WAVE format
 * tags widened into GUIDs: {tag}-0000-0010-8000-00AA00389B71. */
static const GUID AV_SUBTYPE_PCM = {
    0x00000001, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID AV_SUBTYPE_IEEE_FLOAT = {
    0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static int format_understood(const WAVEFORMATEX *f)
{
    if (f->nChannels < 1)
        return 0;

    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32) {
        s_fmt_float = 1;
        return 1;
    }
    if (f->wFormatTag == WAVE_FORMAT_PCM && f->wBitsPerSample == 16) {
        s_fmt_float = 0;
        return 1;
    }
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;

        if (IsEqualGUID(&e->SubFormat, &AV_SUBTYPE_IEEE_FLOAT) &&
            f->wBitsPerSample == 32) {
            s_fmt_float = 1;
            return 1;
        }
        if (IsEqualGUID(&e->SubFormat, &AV_SUBTYPE_PCM) &&
            f->wBitsPerSample == 16) {
            s_fmt_float = 0;
            return 1;
        }
    }
    return 0;
}

/* MUST run before game_map(). WASAPI is COM and brings up MMDevAPI, AudioSes
 * and the endpoint's driver stack lazily, all of which need LoadLibrary, which
 * stops working once the game's CODE lands on this process's PE headers. Same
 * rule as win32_preload(), glide_preload() and diego_preload(). */
int audio_preload(unsigned dcs_us, unsigned volume_percent, const char *routing,
                  unsigned balance)
{
    HRESULT hr;
    REFERENCE_TIME dur = 20 * 10000;      /* 20 ms; shared mode may round up */
    WAVEFORMATEX *mix_fmt = NULL;
    WAVEFORMATEX *fixed_fmt = NULL;
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    if (routing && _stricmp(routing, "cabinet") == 0)
        s_route = ROUTE_CABINET;
    else
        s_route = ROUTE_MIX;
    balance_set(balance);

    if (dcs_us)
        s_dcs_period_us = dcs_us;
    s_user_gain = (float)volume_percent / 100.0f;
    if (s_user_gain < 0.0f) s_user_gain = 0.0f;
    if (s_user_gain > 4.0f) s_user_gain = 4.0f;

    pan_table_init();
    InitializeCriticalSection(&s_voice_cs);
    InitializeCriticalSection(&s_mask_cs);
    /* Auto-reset: a full release wakes one waiter, which re-checks and either
     * claims the mask or waits again. A release with nobody waiting leaves it
     * signalled, so the next waiter takes one spurious pass through the loop:
     * harmless, and cheaper than tracking waiters. */
    s_mask_free = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_cs_ready = 1;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&s_enum);
    if (FAILED(hr)) {
        LOGE("audio: MMDeviceEnumerator failed 0x%08lX", (unsigned long)hr);
        return 0;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(s_enum, eRender, eConsole,
                                                     &s_dev);
    if (FAILED(hr)) {
        LOGE("audio: no default render endpoint (0x%08lX)", (unsigned long)hr);
        return 0;
    }
    hr = IMMDevice_Activate(s_dev, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&s_client);
    if (FAILED(hr)) {
        LOGE("audio: IAudioClient activate failed 0x%08lX", (unsigned long)hr);
        return 0;
    }
    hr = IAudioClient_GetMixFormat(s_client, &mix_fmt);
    if (FAILED(hr) || !mix_fmt) {
        LOGE("audio: GetMixFormat failed 0x%08lX", (unsigned long)hr);
        return 0;
    }

    /* Mix at the cabinet's real 48 kHz, independent of the endpoint's
     * control-panel rate. Some USB DACs advertise 384 kHz as their shared mix
     * format; following it multiplied every voice's mixer work by eight.
     * Shared-mode WASAPI's AUTOCONVERTPCM converts this stream to the endpoint
     * format. */
    fixed_fmt = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof *fixed_fmt);
    if (!fixed_fmt) {
        CoTaskMemFree(mix_fmt);
        LOGE("audio: fixed-format allocation failed");
        return 0;
    }
    memset(fixed_fmt, 0, sizeof *fixed_fmt);
    fixed_fmt->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fixed_fmt->nChannels = 2;
    fixed_fmt->nSamplesPerSec = AV_OUT_RATE;
    fixed_fmt->wBitsPerSample = 32;
    fixed_fmt->nBlockAlign = fixed_fmt->nChannels *
                             fixed_fmt->wBitsPerSample / 8;
    fixed_fmt->nAvgBytesPerSec = fixed_fmt->nSamplesPerSec *
                                 fixed_fmt->nBlockAlign;
    s_fmt = fixed_fmt;
    s_fmt_float = 1;
    s_channels = 2;
    s_out_rate = AV_OUT_RATE;

    hr = IAudioClient_Initialize(s_client, AUDCLNT_SHAREMODE_SHARED,
                                 stream_flags, dur, 0, s_fmt, NULL);
    if (FAILED(hr)) {
        /* A small number of older drivers reject automatic conversion. Retry
         * on a fresh IAudioClient with the endpoint's own format so audio is
         * never lost merely because the optimization is unavailable. */
        LOGW("audio: fixed 48000 Hz shared stream rejected 0x%08lX; retrying "
             "the endpoint mix format", (unsigned long)hr);
        IAudioClient_Release(s_client);
        s_client = NULL;
        CoTaskMemFree(fixed_fmt);
        s_fmt = mix_fmt;
        mix_fmt = NULL;
        hr = IMMDevice_Activate(s_dev, &IID_IAudioClient, CLSCTX_ALL, NULL,
                                (void **)&s_client);
        if (FAILED(hr) || !s_client) {
            LOGE("audio: fallback IAudioClient activate failed 0x%08lX",
                 (unsigned long)hr);
            return 0;
        }
        if (!format_understood(s_fmt)) {
            LOGE("audio: unsupported shared mix format (tag=%u bits=%u ch=%u) "
                 "-- the shim mixes to float32 or 16-bit PCM only",
                 (unsigned)s_fmt->wFormatTag, (unsigned)s_fmt->wBitsPerSample,
                 (unsigned)s_fmt->nChannels);
            return 0;
        }
        s_channels = s_fmt->nChannels;
        s_out_rate = s_fmt->nSamplesPerSec;
        hr = IAudioClient_Initialize(s_client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     dur, 0, s_fmt, NULL);
        if (FAILED(hr)) {
            LOGE("audio: fallback IAudioClient::Initialize failed 0x%08lX",
                 (unsigned long)hr);
            return 0;
        }
    } else {
        LOGI("audio: endpoint mix %u Hz x%u; using fixed 48000 Hz float32 "
             "stereo client with shared-mode conversion",
             (unsigned)mix_fmt->nSamplesPerSec,
             (unsigned)mix_fmt->nChannels);
        CoTaskMemFree(mix_fmt);
    }

    s_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_wake || FAILED(IAudioClient_SetEventHandle(s_client, s_wake))) {
        LOGE("audio: SetEventHandle failed");
        return 0;
    }
    if (FAILED(IAudioClient_GetBufferSize(s_client, &s_buf_frames))) {
        LOGE("audio: GetBufferSize failed");
        return 0;
    }
    hr = IAudioClient_GetService(s_client, &IID_IAudioRenderClient,
                                 (void **)&s_render);
    if (FAILED(hr)) {
        LOGE("audio: IAudioRenderClient failed 0x%08lX", (unsigned long)hr);
        return 0;
    }

    s_scratch = (float *)HeapAlloc(GetProcessHeap(), 0,
                                   (size_t)s_buf_frames * 2 * sizeof(float));
    if (!s_scratch) {
        LOGE("audio: scratch buffer allocation failed");
        return 0;
    }

    if (FAILED(IAudioClient_Start(s_client))) {
        LOGE("audio: IAudioClient::Start failed");
        return 0;
    }
    s_render_thread = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
    if (!s_render_thread) {
        LOGE("audio: CreateThread(render) failed %lu", GetLastError());
        return 0;
    }

    s_ready = 1;
    if (GAME_HAS(audiolib_ymf_jump_table))
        LOGI("audio: %u Hz %s x%d, %u-frame buffer (%.1f ms), Offroad "
             "Audiolib front/seat cabinet buses, routing=%s",
             (unsigned)s_out_rate, s_fmt_float ? "float32" : "int16", s_channels,
             (unsigned)s_buf_frames,
             1000.0 * (double)s_buf_frames / (double)s_out_rate,
             route_description());
    else
        LOGI("audio: %u Hz %s x%d, %u-frame buffer (%.1f ms), DCS tick %.1f ms, "
             "routing=%s",
             (unsigned)s_out_rate, s_fmt_float ? "float32" : "int16", s_channels,
             (unsigned)s_buf_frames,
             1000.0 * (double)s_buf_frames / (double)s_out_rate,
             s_dcs_period_us / 1000.0,
             route_description());
    return 1;
}

int audio_ready(void)
{
    return s_ready;
}

/* Runs after stubs_install_m1(), so these overwrite the YMF/ETSHW stubs rather
 * than the other way round: same ordering rule as glide_bind(). */
int audio_install(void)
{
    int i;
    int ready = 1;

    if (!s_ready)
        return 0;

    /* Hydro cuts at _audio_dspcmd_*. Offroad's corresponding portable boundary
     * is its 11-entry Audiolib driver table; audio_offroad.c keeps Offroad's own
     * dsound/cookie layer and implements the table's distinct attribute ABI. */
    if (!GAME_HAS(dspcmd_play) || !GAME_HAS(ymf_init)) {
        if (audio_offroad_install()) {
            s_driver_installed = 1;
            return 1;
        }
        return 0;
    }

    ready &= game_require("audio", "_ymf_Init", G->ymf_init);
    ready &= game_require("audio", "_ymf_Shutdown", G->ymf_shutdown);
    ready &= game_require("audio", "_audio_dspcmd_Play", G->dspcmd_play);
    ready &= game_require("audio", "_audio_dspcmd_Stop", G->dspcmd_stop);
    ready &= game_require("audio", "_audio_dspcmd_ChangePitch", G->dspcmd_pitch);
    ready &= game_require("audio", "_audio_dspcmd_ChangeVolume", G->dspcmd_volume);
    ready &= game_require("audio", "_audio_dspcmd_ChangePan", G->dspcmd_pan);
    ready &= game_require("audio", "_audio_dspcmd_SetMasterVolume",
                          G->dspcmd_master_vol);
    ready &= game_require("audio", "_audio_dspcmd_SetLeftRightAttenuators",
                          G->dspcmd_lr_atten);
    ready &= game_require("audio", "_EtsPicEnable", G->ets_pic_enable);
    ready &= game_require("audio", "_ymf_AllocSoundBuffer", G->ymf_alloc_buffer);
    ready &= game_require("audio", "_ymf_FreeSoundBuffer", G->ymf_free_buffer);
    ready &= game_require("audio", "relocatable _ymf_AllocSoundBuffer prologue",
                          G->hook_ymf_alloc);
    ready &= game_require("audio", "relocatable _ymf_FreeSoundBuffer prologue",
                          G->hook_ymf_free);
    if (!ready) {
        s_ready = 0;
        return 0;
    }

    /* Build every fallible trampoline before changing any target entry. */
    s_orig_alloc = (alloc_fn_t)hook_prepare_call_through(G->ymf_alloc_buffer,
                                                         G->hook_ymf_alloc);
    s_orig_free = (free_fn_t)hook_prepare_call_through(G->ymf_free_buffer,
                                                       G->hook_ymf_free);
    if (!s_orig_alloc || !s_orig_free) {
        LOGE("audio: sound-buffer trampolines failed; audio disabled");
        s_ready = 0;
        return 0;
    }

    for (i = 0; i < AV_VOICES; i++) {
        /* The hardware's table starts at zero, which would mean hard left and
         * half speed. Pan really is per VOICE and survives a Play, so this is
         * the only thing that sets it before the game first does. Pitch is per
         * SOUND, av_Play reverts it, so this is belt and braces there. */
        s_voice[i].pan   = 0x7F;
        s_voice[i].pitch = 0x80;
    }

    patch_jmp(G->ymf_init,     (void *)av_ymf_Init);
    patch_jmp(G->ymf_shutdown, (void *)av_ymf_Shutdown);

    patch_jmp(G->dspcmd_play,        (void *)av_Play);
    patch_jmp(G->dspcmd_stop,        (void *)av_Stop);
    patch_jmp(G->dspcmd_pitch,       (void *)av_ChangePitch);
    patch_jmp(G->dspcmd_volume,      (void *)av_ChangeVolume);
    patch_jmp(G->dspcmd_pan,         (void *)av_ChangePan);
    patch_jmp(G->dspcmd_master_vol,  (void *)av_SetMasterVolume);
    patch_jmp(G->dspcmd_lr_atten,    (void *)av_SetLeftRightAttenuators);

    patch_jmp(G->ets_pic_enable, (void *)av_EtsPicEnable);
    patch_jmp(G->ymf_alloc_buffer, (void *)av_alloc_buffer);
    patch_jmp(G->ymf_free_buffer, (void *)av_free_buffer);

    LOGI("audio: 9 YMF/DSP entry points replaced, 2 hooked, _EtsPicEnable "
         "is now the DCS mutex");
    s_driver_installed = 1;
    return 1;
}

/* Enable Offroad's built-in silent mode after DATA unpacking. In this mode,
 * report immediate cookie completion so one-shot sample references are
 * released normally. */
int audio_install_passthrough(void)
{
    volatile uint32_t *flag;

    if (s_driver_installed || !G->dsound_passthrough)
        return 0;

    if (!audio_offroad_install_silent_completion()) {
        LOGE("audio: refusing unsafe %s pass-through: cookie completion could "
             "not be corrected", G->id);
        return 0;
    }

    flag = (volatile uint32_t *)(uintptr_t)G->dsound_passthrough;
    if (*flag) {
        LOGI("audio: _dsound_passThrough already %u; safe immediate completion "
             "is installed", *flag);
        return 1;
    }
    *flag = 1;
    LOGW("audio: no output backend for %s; _dsound_passThrough @ 0x%08X "
         "set to 1 with immediate cookie completion. The game is silent, finite "
         "sounds retire safely, and _dsound_InitDS can still start.",
         G->id, G->dsound_passthrough);
    return 1;
}

void audio_shutdown(void)
{
    int offroad = audio_offroad_active();

    InterlockedExchange(&s_stop, 1);

    if (s_dcs_thread) {
        WaitForSingleObject(s_dcs_thread, 500);
        CloseHandle(s_dcs_thread);
        s_dcs_thread = NULL;
    }
    if (s_wake)
        SetEvent(s_wake);
    if (s_render_thread) {
        WaitForSingleObject(s_render_thread, 500);
        CloseHandle(s_render_thread);
        s_render_thread = NULL;
    }
    if (s_client)
        IAudioClient_Stop(s_client);

    /* Offroad's lock remains live until the shared render thread is joined. */
    if (offroad)
        audio_offroad_shutdown();

    if (s_ready && !offroad)
        LOGI("audio: %u plays, %u stops, %u DCS ticks, %u starved buffers",
             s_n_play, s_n_stop, s_n_ticks, s_n_starved);

    if (s_render) { IAudioRenderClient_Release(s_render); s_render = NULL; }
    if (s_client) { IAudioClient_Release(s_client);       s_client = NULL; }
    if (s_dev)    { IMMDevice_Release(s_dev);             s_dev = NULL; }
    if (s_enum)   { IMMDeviceEnumerator_Release(s_enum);  s_enum = NULL; }
    if (s_fmt)    { CoTaskMemFree(s_fmt);                 s_fmt = NULL; }
    if (s_scratch){ HeapFree(GetProcessHeap(), 0, s_scratch); s_scratch = NULL; }

    if (s_cs_ready) {
        DeleteCriticalSection(&s_voice_cs);
        DeleteCriticalSection(&s_mask_cs);
        if (s_mask_free) {
            CloseHandle(s_mask_free);
            s_mask_free = NULL;
        }
        s_cs_ready = 0;
    }
    s_ready = 0;
}
