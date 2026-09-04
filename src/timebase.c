/* timebase.c -- virtualise the cabinet CPU clock with QueryPerformanceCounter.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game sees a stable 333 MHz counter and a matching seconds-per-tick scale,
 * independent of host frequency changes. speed_percent scales the virtual clock
 * without altering the reported hardware rate.
 */
#include "vcthunder.h"

#include <stdio.h>

/* The arcade cabinet's Celeron Mendocino. cpu_hz identifies the virtual CPU;
 * speed_percent controls the independent game-time/wall-time ratio. */
#define DEFAULT_TARGET_HZ 333000000u

static uint32_t       s_target_hz = DEFAULT_TARGET_HZ;
static uint64_t       s_clock_hz = DEFAULT_TARGET_HZ;
static unsigned       s_speed_percent = 100u;
static LARGE_INTEGER  s_qpf, s_qpc0;

/* Freeze virtual time during modal host operations. Advancing the epoch by the
 * paused duration preserves a monotonic clock with no artificial long frame. */
static volatile int   s_paused;
static LARGE_INTEGER  s_pause_qpc;
static unsigned       s_pause_depth;

typedef int (*hwcaps_fn)(void *);
static hwcaps_fn s_orig_hwcaps;

/* Elapsed QPC ticks converted to target-clock ticks without overflow and
 * without floating point: split into whole seconds and remainder so neither
 * product can exceed 64 bits (rem < qpf ~ 1e7, hz ~ 3.3e8 -> ~3.3e15). */
static uint64_t synth_ticks(void)
{
    LARGE_INTEGER now;
    uint64_t d, qpf, secs, rem;

    if (s_paused)
        now = s_pause_qpc;
    else
        QueryPerformanceCounter(&now);
    d   = (uint64_t)(now.QuadPart - s_qpc0.QuadPart);
    qpf = (uint64_t)s_qpf.QuadPart;
    if (!qpf)
        return 0;
    secs = d / qpf;
    rem  = d % qpf;
    return secs * s_clock_hz + (rem * s_clock_hz) / qpf;
}

/* Replaces _cpuTimeStamp @ 0x13ede8. Signature recovered from its own code:
 *     void __cdecl _cpuTimeStamp(uint32_t *hi, uint32_t *lo);
 * note the order; arg1 receives EDX (high), arg2 receives EAX (low). */
static void tb_cpuTimeStamp(uint32_t *hi, uint32_t *lo)
{
    uint64_t t = synth_ticks();

    if (hi) *hi = (uint32_t)(t >> 32);
    if (lo) *lo = (uint32_t)t;
}

/* Return raw virtual ticks and their reported rate. Callers difference ticks
 * before conversion to preserve interval precision under the game's x87 mode. */
uint64_t timebase_ticks(void) { return synth_ticks(); }
uint32_t timebase_hz(void)    { return s_target_hz; }

/* Hold the game's clock across a stall the game did not ask for. Nested because
 * a menu loop can be entered from inside a size/move loop, and only the
 * outermost pair may move the epoch. */
void timebase_pause(int on)
{
    LARGE_INTEGER now;

    if (on) {
        if (s_pause_depth++)
            return;
        QueryPerformanceCounter(&s_pause_qpc);
        s_paused = 1;
        return;
    }
    if (!s_pause_depth || --s_pause_depth)
        return;
    QueryPerformanceCounter(&now);
    s_qpc0.QuadPart += now.QuadPart - s_pause_qpc.QuadPart;
    s_paused = 0;
    LOGI("timebase: held the game clock for %.2f s across a modal window loop",
         s_qpf.QuadPart ? (double)(now.QuadPart - s_pause_qpc.QuadPart) /
                          (double)s_qpf.QuadPart : 0.0);
}

int timebase_paused(void) { return s_paused; }

/* Call-through hook on _hardware_caps_driver_QueryHardwareCaps. */
static int tb_QueryHardwareCaps(void *caps)
{
    int rc = s_orig_hwcaps(caps);
    unsigned char *p = (unsigned char *)caps;
    float scale = 1.0f / (float)s_target_hz;

    LOGI("hwcaps: game measured cpu_hz=%lu scale=%g -> forcing %u Hz, scale %g",
         (unsigned long)*(uint32_t *)(p + 0x0c), (double)*(float *)(p + 0x10),
         s_target_hz, (double)scale);

    *(uint32_t *)(p + 0x0c) = s_target_hz;   /* cpu_hz            */
    *(float    *)(p + 0x10) = scale;         /* seconds per tick  */
    LOGI("timebase: effective game clock %llu Hz (%u%% real time)",
         (unsigned long long)s_clock_hz, s_speed_percent);
    return rc;
}

void timebase_init(uint32_t target_hz, unsigned speed_percent)
{
    if (target_hz)
        s_target_hz = target_hz;
    if (speed_percent < 25u || speed_percent > 400u) {
        LOGW("timebase: speed_percent %u outside 25..400; using 100", speed_percent);
        speed_percent = 100u;
    }
    s_speed_percent = speed_percent;
    s_clock_hz = ((uint64_t)s_target_hz * s_speed_percent) / 100u;
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_qpc0);
    LOGI("timebase: virtual CPU %u Hz, speed %u%%, QPC frequency %llu Hz",
         s_target_hz, s_speed_percent, (unsigned long long)s_qpf.QuadPart);
}

int timebase_install(void)
{
    void *trampoline;

    /* The prologue is per-title: Hydro's is 6 relocatable bytes
     * (push ebp / mov ebp,esp / sub esp,0x2c) while Offroad's reaches five only
     * by taking in a `call rel32`, which the trampoline has to rebase. Both
     * facts come out of the binaries in gen-gamedefs.py's d_hooks(). */
    if (!game_require("timebase", "QueryHardwareCaps", G->queryhwcaps) ||
        !game_require("timebase", "relocatable QueryHardwareCaps prologue",
                      G->hook_queryhwcaps) ||
        !game_require("timebase", "_cpuTimeStamp", G->cputimestamp))
        return 0;
    trampoline = hook_prepare_call_through(G->queryhwcaps, G->hook_queryhwcaps);
    if (!trampoline) {
        LOGE("timebase: could not hook QueryHardwareCaps");
        return 0;
    }
    s_orig_hwcaps = (hwcaps_fn)trampoline;
    patch_jmp(G->queryhwcaps, (void *)tb_QueryHardwareCaps);
    patch_jmp(G->cputimestamp, (void *)tb_cpuTimeStamp);

    LOGI("timebase: _cpuTimeStamp -> synthetic, QueryHardwareCaps hooked "
         "(trampoline %p)", (void *)s_orig_hwcaps);
    return 1;
}
