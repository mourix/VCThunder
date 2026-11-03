/* splash_hold.c -- hold the full-screen splashes for a cabinet load's time.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The title splash after START and the screen before a race have no timer of
 * their own. The game puts the picture on screen, starts its sound and runs a
 * load, then fades the sound and moves on when the load returns, so each lasts
 * exactly as long as its load. On the cabinet that was seconds; here it is
 * milliseconds, and both the picture and the voice vanish.
 *
 * Each load's call is redirected here and held BEFORE the load runs: the
 * loads start the next screen's sounds themselves (Hydro's _wpr_select_Init
 * triggers the select music first thing), so a hold after one would play that
 * music under the splash. Offroad's title load takes no argument; the one
 * forwarded is the caller's stack slot, which a cdecl callee may read freely.
 * The game clock is left running, as it ran through the cabinet's loads; the
 * game resets its timers right after both. Audio mixes on its own threads, so
 * the splash's sound plays out during the hold.
 */
#include "vcthunder.h"

#define TITLE_HOLD_MS 3000u
#define RACE_HOLD_MS  6000u
/* How often a hold answers the window: a queue left for 5 s is Not Responding,
 * and Escape and the close button wait on it. */
#define HOLD_STEP_MS  15u

typedef int (__cdecl *load_fn)(int);

static load_fn s_title_load, s_race_load;

static int hold_then_load(load_fn load, int arg, DWORD hold_ms, const char *what)
{
    LARGE_INTEGER f, t0, t1;
    DWORD start = GetTickCount();
    int rc;

    watchdog_hold(1);
    while (GetTickCount() - start < hold_ms) {
        glide_answer_window();
        Sleep(HOLD_STEP_MS);
    }
    watchdog_hold(0);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    rc = load(arg);
    QueryPerformanceCounter(&t1);
    LOGI("splash hold: %s held %lu ms, then its load took %.0f ms", what,
         (unsigned long)hold_ms,
         (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
    return rc;
}

static int __cdecl title_load(int state)
{
    return hold_then_load(s_title_load, state, TITLE_HOLD_MS, "title splash");
}

static int __cdecl race_load(int arg)
{
    return hold_then_load(s_race_load, arg, RACE_HOLD_MS, "loading screen");
}

static load_fn call_target(uint32_t site)
{
    int32_t rel;

    memcpy(&rel, (const void *)(uintptr_t)(site + 1), 4);
    return (load_fn)(uintptr_t)(site + 5 + (uint32_t)rel);
}

static int is_call(uint32_t site)
{
    return *(const unsigned char *)(uintptr_t)site == 0xE8;
}

/* The title routes, then the race site last. */
#define ROUTES VCT_MAX_SPLASH_ROUTES

void splash_hold_install(void)
{
    uint32_t site[ROUTES + 1];
    unsigned i, routes = 0;

    memcpy(site, G->splash_title_site, sizeof G->splash_title_site);
    site[ROUTES] = G->splash_race_site;
    if (!site[0] || !site[ROUTES]) {
        LOGI("splash hold: not applicable to %s", G->id);
        return;
    }
    /* All or none: every site must be a call, and every title route a call to
     * the same load, before the first one is changed. */
    s_title_load = call_target(site[0]);
    for (i = 0; i < ROUTES; i++)
        if (site[i] && (!is_call(site[i]) || call_target(site[i]) != s_title_load)) {
            LOGE("splash hold: title route 0x%08X is not a call to the title "
                 "load; splashes run at load speed", site[i]);
            return;
        }
    if (!is_call(site[ROUTES])) {
        LOGE("splash hold: the race site is not a call; splashes run at load speed");
        return;
    }
    s_race_load = call_target(site[ROUTES]);
    for (i = 0; i <= ROUTES; i++) {
        if (!site[i])
            continue;
        if (!patch_call(site[i], i < ROUTES ? (void *)title_load
                                            : (void *)race_load))
            break;
        routes += i < ROUTES;
    }
    if (i <= ROUTES) {
        /* A refusal after the checks: the sites already changed go back. */
        LOGE("splash hold: 0x%08X refused its patch; splashes run at load speed",
             site[i]);
        while (i--)
            if (site[i])
                patch_call(site[i], i < ROUTES ? (void *)s_title_load
                                               : (void *)s_race_load);
        return;
    }
    LOGI("splash hold: title splash held %u ms on %u route(s), loading screen %u ms",
         TITLE_HOLD_MS, routes, RACE_HOLD_MS);
}
