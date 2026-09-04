/* audio_offroad.c -- Offroad software Audiolib backend.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Replace the immutable hardware dispatch table while retaining native cookie,
 * priority, sample, movie, and menu bookkeeping. Finite voices clear their
 * playing state at completion.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>

#define OR_VOICES       32u
#define OR_JUMP_SLOTS   11u
#define OR_GAIN_MAX  0xFFFFu
#define OR_STALL_MS     250u

/* Exact layout consumed by Offroad's _dsound_* layer. A 32-bit address is used
 * explicitly rather than a C pointer so this remains a proved 40-byte ABI even
 * if a host compiler is accidentally pointed at the wrong architecture. */
typedef struct {
    uint32_t rate_hz;           /* +00 source frames/second                 */
    uint32_t gain_l;            /* +04 front-speaker-bus native gain code   */
    uint32_t gain_r;            /* +08 seat-woofer-bus native gain code     */
    uint32_t pcm_addr;          /* +0c signed 16-bit mono game-memory PCM   */
    uint32_t frames;            /* +10 source frames                        */
    int32_t  loops;             /* +14 <=0 infinite; >0 total traversals   */
    uint32_t loops_done;        /* +18 completed end crossings              */
    uint32_t loop_point;        /* +1c first frame of traversals after #1   */
    uint32_t position;          /* +20 current source frame                 */
    uint32_t playing;           /* +24 nonzero while active                 */
} or_attr_t;

_Static_assert(sizeof(or_attr_t) == 40, "Offroad Audiolib attribute ABI changed");

typedef struct {
    or_attr_t attr;
    uint64_t  cursor;           /* 32.32 source-frame cursor                */
    int       allocated;
} or_voice_t;

static or_voice_t s_voice[OR_VOICES];
static CRITICAL_SECTION s_cs;
static int s_cs_ready;
static volatile LONG s_installed;

static uint32_t s_master_l = 0x40u;
static uint32_t s_master_r = 0x40u;
static uint32_t s_output_rate = 48000u;
static DWORD    s_last_advance_ms;
static unsigned s_allocated;
static unsigned s_n_init, s_n_uninit, s_n_alloc, s_n_free;
static unsigned s_n_play, s_n_stop, s_n_complete, s_n_wrap;
static unsigned s_n_clock_catchup;
static int      s_minimum_reported;

static uint32_t gain16(uint32_t v)
{
    return v > OR_GAIN_MAX ? OR_GAIN_MAX : v;
}

static uint32_t loop_point(const or_voice_t *v)
{
    return v->attr.loop_point < v->attr.frames ? v->attr.loop_point : 0u;
}

static int playable(const or_voice_t *v)
{
    return v->allocated && v->attr.pcm_addr && v->attr.frames &&
           v->attr.rate_hz;
}

/* Complete one traversal. cursor may carry more than one traversal's
 * overshoot; callers deliberately loop until it is inside the next one. */
static int finish_pass(or_voice_t *v)
{
    uint64_t end = (uint64_t)v->attr.frames << 32;
    uint64_t overshoot = v->cursor >= end ? v->cursor - end : 0;
    uint32_t lp;

    if (v->attr.loops_done != UINT32_MAX)
        v->attr.loops_done++;

    if (v->attr.loops > 0 &&
        v->attr.loops_done >= (uint32_t)v->attr.loops) {
        v->cursor = end;
        v->attr.position = v->attr.frames;
        v->attr.playing = 0;
        s_n_complete++;
        return 0;
    }

    lp = loop_point(v);         /* invalid/zero-length loop normalises to 0 */
    v->cursor = ((uint64_t)lp << 32) + overshoot;
    s_n_wrap++;
    return 1;
}

static int normalize_cursor(or_voice_t *v)
{
    uint64_t end;

    if (!v->attr.playing)
        return 0;
    if (!playable(v)) {
        v->attr.playing = 0;
        s_n_complete++;
        return 0;
    }

    end = (uint64_t)v->attr.frames << 32;
    while (v->attr.playing && v->cursor >= end)
        if (!finish_pass(v))
            break;

    if (v->attr.playing)
        v->attr.position = (uint32_t)(v->cursor >> 32);
    return v->attr.playing != 0;
}

/* Advance without producing samples. This is used only if the WASAPI render
 * callback has stopped progressing for OR_STALL_MS: dsound's IsCookiePlaying
 * poll must still be able to retire a finite voice after a device loss. */
static void advance_delta(or_voice_t *v, uint64_t delta)
{
    uint64_t end, left;

    if (!normalize_cursor(v))
        return;
    end = (uint64_t)v->attr.frames << 32;

    while (delta && v->attr.playing) {
        left = end - v->cursor;
        if (delta < left) {
            v->cursor += delta;
            delta = 0;
        } else {
            delta -= left;
            v->cursor = end;
            if (!finish_pass(v))
                break;
            /* finish_pass can preserve a large overshoot only for the render
             * path; here delta owns it, so the new cursor is exactly the loop
             * point and end remains unchanged. */
        }
    }
    if (v->attr.playing)
        v->attr.position = (uint32_t)(v->cursor >> 32);
}

static void catch_up_clock_locked(void)
{
    DWORD now = GetTickCount();
    DWORD elapsed = now - s_last_advance_ms;
    uint64_t output_frames;
    uint32_t whole, tail, i;

    if (!s_last_advance_ms || elapsed < OR_STALL_MS || !s_output_rate)
        return;

    /* One-second chunks keep step*frames within uint64_t even for the full
     * uint32 source-rate domain. Sixty seconds is enough to retire every normal
     * one-shot after a long device stall without spending unbounded time in a
     * game-thread status query. */
    if (elapsed > 60000u)
        elapsed = 60000u;
    output_frames = (uint64_t)elapsed * s_output_rate / 1000u;
    whole = (uint32_t)(output_frames / s_output_rate);
    tail = (uint32_t)(output_frames % s_output_rate);

    for (i = 0; i < OR_VOICES; i++) {
        uint32_t sec;
        uint64_t step;
        or_voice_t *v = &s_voice[i];

        if (!v->attr.playing || !v->attr.rate_hz)
            continue;
        step = ((uint64_t)v->attr.rate_hz << 32) / s_output_rate;
        for (sec = 0; sec < whole && v->attr.playing; sec++)
            advance_delta(v, step * s_output_rate);
        if (tail && v->attr.playing)
            advance_delta(v, step * tail);
    }
    s_last_advance_ms = now;
    s_n_clock_catchup++;
}

static int valid_slot(uint32_t slot)
{
    return slot < OR_VOICES;
}

/* ---- the eleven cdecl Audiolib driver entries ------------------------- */

static int __cdecl or_Init(void)
{
    EnterCriticalSection(&s_cs);
    memset(s_voice, 0, sizeof s_voice);
    s_allocated = 0;
    /* This is the original backend's reset value. Native dsound immediately
     * sets 0xffff/0xffff and menusound sets 0x4000/0x4000. */
    s_master_l = s_master_r = 0x40u;
    s_last_advance_ms = GetTickCount();
    s_n_init++;
    LeaveCriticalSection(&s_cs);
    LOGT("offroad audio: Init (32 voices, master reset to 0x40/0x40)");
    return 0;
}

static int __cdecl or_UnInit(void)
{
    EnterCriticalSection(&s_cs);
    memset(s_voice, 0, sizeof s_voice);
    s_allocated = 0;
    s_n_uninit++;
    LeaveCriticalSection(&s_cs);
    LOGT("offroad audio: UnInit");
    return 0;
}

static int __cdecl or_GetAllocatedCount(void)
{
    int n;
    EnterCriticalSection(&s_cs);
    n = (int)s_allocated;
    LeaveCriticalSection(&s_cs);
    return n;
}

static int __cdecl or_GetFreeCount(void)
{
    int n;
    EnterCriticalSection(&s_cs);
    n = (int)(OR_VOICES - s_allocated);
    LeaveCriticalSection(&s_cs);
    return n;
}

static int __cdecl or_Allocate(const or_attr_t *initial)
{
    uint32_t i;
    or_attr_t logged;

    if (!initial)
        return -1;

    EnterCriticalSection(&s_cs);
    for (i = 0; i < OR_VOICES; i++)
        if (!s_voice[i].allocated)
            break;
    if (i == OR_VOICES) {
        LeaveCriticalSection(&s_cs);
        LOGW("offroad audio: all 32 Audiolib voices are allocated");
        return -1;
    }

    memset(&s_voice[i], 0, sizeof s_voice[i]);
    memcpy(&s_voice[i].attr, initial, sizeof s_voice[i].attr);
    s_voice[i].attr.gain_l = gain16(s_voice[i].attr.gain_l);
    s_voice[i].attr.gain_r = gain16(s_voice[i].attr.gain_r);
    s_voice[i].attr.loops_done = 0;
    s_voice[i].attr.position = 0;
    s_voice[i].attr.playing = 0;       /* allocation never starts playback */
    s_voice[i].cursor = 0;
    s_voice[i].allocated = 1;
    s_allocated++;
    s_n_alloc++;
    logged = s_voice[i].attr;
    LeaveCriticalSection(&s_cs);

    LOGT("offroad audio: allocate v=%u pcm=0x%08X frames=%u rate=%u "
         "bus=%04X/%04X loop=%ld@%u", i, logged.pcm_addr, logged.frames,
         logged.rate_hz, logged.gain_l, logged.gain_r, (long)logged.loops,
         logged.loop_point);
    return (int)i;
}

static int __cdecl or_Deallocate(uint32_t slot)
{
    if (!valid_slot(slot)) {
        LOGW("offroad audio: deallocate slot %u out of range", slot);
        return 1;
    }

    /* Synchronising with the mixer before clearing pcm_addr is load-bearing:
     * the game can free the PCM immediately after this function returns. */
    EnterCriticalSection(&s_cs);
    if (!s_voice[slot].allocated) {
        LeaveCriticalSection(&s_cs);
        LOGW("offroad audio: deallocate of free slot %u", slot);
        return 1;
    }
    memset(&s_voice[slot], 0, sizeof s_voice[slot]);
    s_allocated--;
    s_n_free++;
    LeaveCriticalSection(&s_cs);
    LOGT("offroad audio: deallocate v=%u", slot);
    return 0;
}

static int __cdecl or_GetAttr(uint32_t slot, or_attr_t *out)
{
    if (!valid_slot(slot) || !out)
        return 1;

    EnterCriticalSection(&s_cs);
    catch_up_clock_locked();
    if (!s_voice[slot].allocated) {
        LeaveCriticalSection(&s_cs);
        return 1;
    }
    memcpy(out, &s_voice[slot].attr, sizeof *out);
    LeaveCriticalSection(&s_cs);
    return 0;
}

static int __cdecl or_SetAttr(uint32_t slot, const or_attr_t *in, uint32_t mask)
{
    or_voice_t *v;
    int was_playing, now_playing;

    if (!valid_slot(slot) || !in)
        return 1;

    EnterCriticalSection(&s_cs);
    catch_up_clock_locked();
    v = &s_voice[slot];
    if (!v->allocated) {
        LeaveCriticalSection(&s_cs);
        return 1;
    }
    was_playing = v->attr.playing != 0;

    if (mask & 0x01u)
        v->attr.rate_hz = in->rate_hz;
    if (mask & 0x04u) {
        v->attr.gain_l = gain16(in->gain_l);
        v->attr.gain_r = gain16(in->gain_r);
    }
    if (mask & 0x08u)
        v->attr.loops = in->loops;
    if (mask & 0x10u)
        v->attr.loop_point = in->loop_point;
    if (mask & 0x40u) {
        v->attr.position = in->position > v->attr.frames
                         ? v->attr.frames : in->position;
        v->cursor = (uint64_t)v->attr.position << 32;
    }
    if (mask & 0x20u) {
        if (in->playing) {
            if (!was_playing)
                v->attr.loops_done = 0;
            v->attr.playing = playable(v) ? 1u : 0u;
            normalize_cursor(v);
        } else {
            v->attr.playing = 0;
        }
    }

    now_playing = v->attr.playing != 0;
    if (!was_playing && now_playing)
        s_n_play++;
    else if (was_playing && !now_playing)
        s_n_stop++;
    LeaveCriticalSection(&s_cs);

    if (was_playing != now_playing)
        LOGT("offroad audio: %s v=%u mask=0x%02X", now_playing ? "play" : "stop",
             slot, mask & 0xFFu);
    return 0;
}

static int __cdecl or_GetMaster(uint32_t *left, uint32_t *right)
{
    EnterCriticalSection(&s_cs);
    /* The cabinet driver treats the outputs independently: either pointer may
     * be NULL, including both.  Some callers only query one side. */
    if (left)
        *left = s_master_l;
    if (right)
        *right = s_master_r;
    LeaveCriticalSection(&s_cs);
    return 0;
}

static int __cdecl or_SetMaster(uint32_t left, uint32_t right)
{
    int report_minimum = 0;

    EnterCriticalSection(&s_cs);
    s_master_l = gain16(left);
    s_master_r = gain16(right);
    if (s_master_l <= 0x00FFu && s_master_r <= 0x00FFu &&
        !s_minimum_reported) {
        s_minimum_reported = 1;
        report_minimum = 1;
    }
    LeaveCriticalSection(&s_cs);
    LOGT("offroad audio: master=%04X/%04X", s_master_l, s_master_r);
    if (report_minimum)
        LOGW("offroad audio: cabinet operator volume is at its minimum "
             "(about -42 dB); use the configured volume_up control if the "
             "game is too quiet");
    return 0;
}

static int __cdecl or_Noop(void)
{
    return 0;
}

static const void *const s_jump[OR_JUMP_SLOTS] = {
    (const void *)or_Init,
    (const void *)or_UnInit,
    (const void *)or_GetAllocatedCount,
    (const void *)or_GetFreeCount,
    (const void *)or_Allocate,
    (const void *)or_Deallocate,
    (const void *)or_GetAttr,
    (const void *)or_SetAttr,
    (const void *)or_GetMaster,
    (const void *)or_SetMaster,
    (const void *)or_Noop,
};

/* ---- shared WASAPI transport adapter ---------------------------------- */

static void mix_voice(or_voice_t *v, float *out, uint32_t frames,
                      uint32_t output_rate, float user_gain)
{
    const int16_t *pcm;
    uint64_t step;
    uint32_t combined_l, combined_r;
    float front, seat, gl, gr;
    uint32_t i;

    if (!normalize_cursor(v))
        return;

    pcm = (const int16_t *)(uintptr_t)v->attr.pcm_addr;
    step = ((uint64_t)v->attr.rate_hz << 32) / output_rate;
    /* Match _audiolib_ymf_command's integer gain law, including its two
     * dropped low bits: ((voice >> 1) * (master >> 1)) >> 14.  It writes that
     * result << 15 to a YMFPCI playback-bank gain whose unity is 0x40000000,
     * hence combined/32768 here.  In particular, the game's 0x00ff minimum
     * master is about -42 dB, not another spelling of unity. */
    combined_l = ((v->attr.gain_l >> 1) * (s_master_l >> 1)) >> 14;
    combined_r = ((v->attr.gain_r >> 1) * (s_master_r >> 1)) >> 14;
    front = (float)combined_l * (1.0f / 32768.0f)
          * user_gain * (1.0f / 32768.0f);
    seat  = (float)combined_r * (1.0f / 32768.0f)
          * user_gain * (1.0f / 32768.0f);
    audio_route_buses(front, seat, &gl, &gr);

    for (i = 0; i < frames && v->attr.playing; i++) {
        uint32_t idx, next;
        float frac, sample;

        if (!normalize_cursor(v))
            break;
        idx = (uint32_t)(v->cursor >> 32);
        next = idx + 1u;
        if (next >= v->attr.frames) {
            int another = v->attr.loops <= 0 ||
                v->attr.loops_done + 1u < (uint32_t)v->attr.loops;
            next = another ? loop_point(v) : idx;
        }

        frac = (float)(uint32_t)v->cursor * (1.0f / 4294967296.0f);
        sample = (float)pcm[idx] + ((float)pcm[next] - (float)pcm[idx]) * frac;
        out[2u * i]     += sample * gl;
        out[2u * i + 1] += sample * gr;

        v->cursor += step;
        normalize_cursor(v);   /* publishes completion within this host block */
    }
}

int audio_offroad_install(void)
{
    uint32_t i;

    if (!game_require("offroad audio", "_Audiolib_ymf_JumpTable",
                      G->audiolib_ymf_jump_table) ||
        !game_require("offroad audio", "derived Audiolib jump count",
                      G->audiolib_jump_count))
        return 0;
    if (G->audiolib_jump_count != OR_JUMP_SLOTS) {
        LOGE("offroad audio: %s has %u Audiolib slots; this backend implements "
             "%u: refusing a partial table", G->id, G->audiolib_jump_count,
             OR_JUMP_SLOTS);
        return 0;
    }

    InitializeCriticalSection(&s_cs);
    s_cs_ready = 1;
    s_last_advance_ms = GetTickCount();
    for (i = 0; i < G->audiolib_jump_count; i++)
        patch_slot(G->audiolib_ymf_jump_table + i * sizeof(uint32_t), s_jump[i]);

    /* The render thread started during audio_preload and may already be calling
     * audio_offroad_mix(). Publish only after the lock, state and full table are
     * ready. InterlockedExchange is the release barrier. */
    InterlockedExchange(&s_installed, 1);
    LOGI("offroad audio: 11-entry Audiolib YMF backend replaced at 0x%08X; "
         "32 signed-16 mono voices, front/seat cabinet buses, native dsound cookies",
         G->audiolib_ymf_jump_table);
    return 1;
}

int audio_offroad_active(void)
{
    return InterlockedCompareExchange(&s_installed, 0, 0) != 0;
}

void audio_offroad_mix(float *stereo, uint32_t frames, uint32_t output_rate,
                        float user_gain)
{
    uint32_t i;

    if (!audio_offroad_active() || !stereo || !frames || !output_rate)
        return;

    EnterCriticalSection(&s_cs);
    s_output_rate = output_rate;
    for (i = 0; i < OR_VOICES; i++)
        if (s_voice[i].attr.playing)
            mix_voice(&s_voice[i], stereo, frames, output_rate, user_gain);
    s_last_advance_ms = GetTickCount();
    LeaveCriticalSection(&s_cs);
}

void audio_offroad_census(unsigned starved)
{
    char line[640];
    unsigned i, active = 0;
    int n = 0;

    if (!audio_offroad_active())
        return;
    line[0] = '\0';
    EnterCriticalSection(&s_cs);
    for (i = 0; i < OR_VOICES; i++) {
        const or_attr_t *a = &s_voice[i].attr;
        if (!a->playing)
            continue;
        active++;
        if (active <= 6)
            n += snprintf(line + n, sizeof line - (size_t)n,
                          " [v%u %uHz bus=%04X/%04X pass=%u/%ld pos=%u/%u]",
                          i, a->rate_hz, a->gain_l, a->gain_r, a->loops_done,
                          (long)a->loops, a->position, a->frames);
    }
    LOGI("offroad audio: %u alloc/%u free, %u play/%u stop/%u natural, "
         "%u wrap, %u silent catch-up, %u starved; master=%04X/%04X, "
         "%u voice(s) active%s", s_n_alloc, s_n_free, s_n_play, s_n_stop,
         s_n_complete, s_n_wrap, s_n_clock_catchup, starved, s_master_l,
         s_master_r, active, line);
    LeaveCriticalSection(&s_cs);
}

void audio_offroad_shutdown(void)
{
    if (!s_cs_ready)
        return;

    /* audio.c joins the render thread before calling us, so clearing the flag
     * and deleting the lock cannot race a mix callback. */
    InterlockedExchange(&s_installed, 0);
    EnterCriticalSection(&s_cs);
    LOGI("offroad audio: shutdown; init %u/uninit %u, alloc %u/free %u, "
         "play %u/stop %u/natural %u, wraps %u, silent catch-up %u",
         s_n_init, s_n_uninit, s_n_alloc, s_n_free, s_n_play, s_n_stop,
         s_n_complete, s_n_wrap, s_n_clock_catchup);
    memset(s_voice, 0, sizeof s_voice);
    LeaveCriticalSection(&s_cs);
    DeleteCriticalSection(&s_cs);
    s_cs_ready = 0;
}

int audio_offroad_install_silent_completion(void)
{
    if (!game_require("offroad audio silent fallback",
                      "_dsound_IsCookiePLaying", G->dsound_is_playing))
        return 0;

    /* Pass-through's native branch returns one forever. Returning zero lets
     * __WorkChannels immediately perform the normal Stop/Release/Sample_Disuse
     * chain, so silence cannot strand a sample reference and hang bank unload. */
    patch_ret_imm(G->dsound_is_playing, 0);
    LOGW("offroad audio: silent fallback corrects _dsound_IsCookiePLaying @ "
         "0x%08X to completed; sounds retire immediately instead of forever",
         G->dsound_is_playing);
    return 1;
}
