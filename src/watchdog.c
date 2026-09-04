/* watchdog.c -- the hang watchdog.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A separate thread watching the frame heartbeat. On a timeout it samples the
 * render thread and walks its validated EBP chain, which is how a spin, a
 * block and a modal loop inside DefWindowProc are told apart: they look
 * identical from the outside and need completely different fixes.
 *
 * Self-contained: it owns its thread and reads one timestamp. See
 * src/glide_int.h for the seam.
 */
#include "vcthunder.h"
#include "glide_int.h"

#include <stdio.h>
#include <string.h>

/* Its own copy. QueryPerformanceFrequency is fixed for the life of the
 * process, so sharing one with the frame reports would be an accessor across a
 * file boundary for a constant. */
static LARGE_INTEGER s_perf_freq;

/* Watch the frame heartbeat from a separate thread. On timeout, sample the
 * render thread and walk its validated EBP chain to distinguish spins, blocks,
 * and modal loops. */
#define WATCHDOG_STALL_MS   5000u
/* Before the first frame the game may legitimately be loading hundreds of MB. */
#define WATCHDOG_FIRST_FRAME_MS 20000u
#define WATCHDOG_REPORTS    6u
#define WATCHDOG_FRAMES     16u

static HANDLE  s_render_thread;
static HANDLE  s_watchdog_thread;
static volatile LONG64 s_last_swap_qpc_atomic;
static LONG64 s_watchdog_armed_qpc;
static volatile LONG   s_watchdog_stop;

static void watchdog_dump(unsigned report, double stalled_s)
{
    CONTEXT ctx;
    char line[512];
    int n = 0;
    unsigned depth;

    if (SuspendThread(s_render_thread) == (DWORD)-1)
        return;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(s_render_thread, &ctx)) {
        char mod[MAX_PATH] = "?";
        HMODULE h = NULL;

        /* Naming the module is the whole point when EIP is outside the game:
         * "blocked in ntdll" and "blocked in the Glide backend" call for
         * completely different fixes, and the first freeze report could not tell
         * them apart because the chain walk gave up as soon as it saw a non-game
         * return address. */
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)ctx.Eip, &h) && h)
            GetModuleFileNameA(h, mod, sizeof mod);

        log_printf(InterlockedCompareExchange64(&s_last_swap_qpc_atomic, 0, 0)
                       ? LOG_ERR : LOG_WARN,
             "watchdog: %s for %.1f s (report %u/%u); render thread "
             "EIP=0x%08lX ESP=0x%08lX EBP=0x%08lX %s [%s]",
             InterlockedCompareExchange64(&s_last_swap_qpc_atomic, 0, 0)
                 ? "no frame" : "NO FIRST FRAME",
             stalled_s, report, WATCHDOG_REPORTS,
             ctx.Eip, ctx.Esp, ctx.Ebp,
             diag_in_game((uint32_t)ctx.Eip) ? "(in game code)" : "(outside the game image)",
             mod);

        /* Print the render thread ID beside lock ownership to make the
         * comparison explicit. */
        LOGE("watchdog: render thread id=%lu (compare with the DCS lock owner "
             "below: equal means it OWNS the lock and is blocked elsewhere, "
             "different means it is WAITING on it)",
             (unsigned long)GetThreadId(s_render_thread));

        /* The render thread's block names the OBJECT and never the HOLDER. Ask
         * the audio side directly, from here, while everything is still parked. */
        audio_report_dcs_state();

        /* Walk the raw stack rather than the EBP chain when blocked outside the
         * game: system call frames do not keep frame pointers, so the chain
         * terminates immediately and tells us nothing. Scanning for plausible
         * return addresses is crude, but it recovers who called in. */
        if (!diag_in_game((uint32_t)ctx.Eip)) {
            uint32_t sp, v;
            int found = 0;

            /* Classify stack values by executable-page membership before
             * presenting them as return addresses. */
            for (sp = (uint32_t)ctx.Esp; sp < (uint32_t)ctx.Esp + 512 && found < 10;
                 sp += 4) {
                MEMORY_BASIC_INFORMATION mbi;
                char mn[MAX_PATH];
                const char *base = "";
                HMODULE hm = NULL;
                int exec = 0;

                if (!diag_peek32(sp, &v))
                    break;
                if (!diag_in_game(v) && !(v >= 0x60000000u && v < 0x80000000u))
                    continue;
                if (VirtualQuery((LPCVOID)(uintptr_t)v, &mbi, sizeof mbi) &&
                    mbi.State == MEM_COMMIT)
                    exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                           PAGE_EXECUTE_READWRITE |
                                           PAGE_EXECUTE_WRITECOPY)) != 0;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(uintptr_t)v, &hm) && hm &&
                    GetModuleFileNameA(hm, mn, sizeof mn)) {
                    const char *slash = strrchr(mn, '\\');
                    base = slash ? slash + 1 : mn;
                }
                n += snprintf(line + n, sizeof line - (size_t)n,
                              " [0x%08X %s %s+0x%X]", v,
                              exec ? "code" : "DATA",
                              *base ? base : (diag_in_game(v) ? "game" : "?"),
                              hm ? (unsigned)(v - (uint32_t)(uintptr_t)hm)
                                 : (unsigned)(uintptr_t)(v -
                                       (uint32_t)(uintptr_t)mbi.AllocationBase));
                if (n <= 0 || (size_t)n >= sizeof line - 48)
                    break;
                found++;
            }
            if (found)
                LOGE("watchdog: stack scan (code = a plausible return address, "
                     "DATA = an object; a DATA address repeated around an ntdll "
                     "wait is what is being waited ON):%s", line);
            ResumeThread(s_render_thread);
            return;
        }

        {
            uint32_t chain[WATCHDOG_FRAMES];
            unsigned got = diag_return_chain((uint32_t)ctx.Ebp, chain,
                                             WATCHDOG_FRAMES);

            /* A first link outside the game is not a frame-pointer prologue,
             * so the walk says nothing and printing it would invite belief. */
            if (got && !diag_in_game(chain[0]))
                got = 0;
            for (depth = 0; depth < got; depth++) {
                n += snprintf(line + n, sizeof line - (size_t)n, " 0x%08X",
                              chain[depth]);
                if (n <= 0 || (size_t)n >= sizeof line - 16)
                    break;
            }
        }
        if (n > 0)
            LOGE("watchdog: return chain:%s  (resolve with "
                 "scripts/p1-symbols.py --lookup)", line);
    }
    ResumeThread(s_render_thread);
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    unsigned reported = 0;
    LARGE_INTEGER now;

    (void)arg;
    while (!InterlockedCompareExchange(&s_watchdog_stop, 0, 0)) {
        LONG64 last;
        double stalled, threshold = (double)WATCHDOG_STALL_MS;

        Sleep(1000);
        if (!s_perf_freq.QuadPart)
            continue;
        /* A modal window loop is a deliberate stop, not a hang: window.c holds
         * the game's clock across it. Reporting the render thread parked in
         * user32 six times is how a useful instrument gets ignored. */
        if (timebase_paused())
            continue;
        last = InterlockedCompareExchange64(&s_last_swap_qpc_atomic, 0, 0);
        /* No swap has EVER happened: measure from when the watchdog armed.
         *
         * A `continue` here would leave the one case the instrument cannot
         * diagnose as "the game never reached its first frame": precisely the
         * case that needs it. Offroad can hang silently between __chdir and
         * its first Glide call, with the watchdog waiting for a frame to stop
         * arriving. */
        if (!last)
            last = s_watchdog_armed_qpc;
        if (!last)
            continue;
        QueryPerformanceCounter(&now);
        stalled = (double)(now.QuadPart - last) / (double)s_perf_freq.QuadPart;

        /* Loading is not stalling. Hydro spends 6-7 s opening a 179 MB archive
         * before its first swap, and reporting that five times as an error is
         * how a useful instrument gets ignored. Before the first frame the
         * threshold is much longer and the report is a warning; after it, a
         * missing frame really is a fault. */
        if (!InterlockedCompareExchange64(&s_last_swap_qpc_atomic, 0, 0))
            threshold = (double)WATCHDOG_FIRST_FRAME_MS;

        if (stalled * 1000.0 < threshold) {
            reported = 0;                   /* frames resumed; re-arm */
            continue;
        }
        if (reported >= WATCHDOG_REPORTS)
            continue;
        watchdog_dump(++reported, stalled);
        if (reported == WATCHDOG_REPORTS)
            LOGE("watchdog: %u reports taken; staying quiet unless frames "
                 "resume. An EIP that moved between them is a spin; one that "
                 "did not is a block.", WATCHDOG_REPORTS);
    }
    return 0;
}

static void watchdog_start(void);

/* Arm from the game thread before it is handed to the game. The first-swap call
 * site is kept as a fallback for any path that reaches rendering without going
 * through main.c, and starting twice is a no-op. */
void glide_watchdog_arm(void)
{
    watchdog_start();
}

static void watchdog_start(void)
{
    if (s_watchdog_thread)
        return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_render_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0)) {
        LOGW("watchdog: cannot duplicate render-thread handle (%lu); a freeze "
             "will not be diagnosable", GetLastError());
        return;
    }
    {
        LARGE_INTEGER now;
        /* The QPC frequency is otherwise only sampled by the per-frame perf
         * report, so before the first frame it is still 0, and the watchdog
         * loop treats 0 as "cannot measure" and sleeps forever. Arming has to
         * establish its own preconditions rather than inherit the frame path's. */
        if (!s_perf_freq.QuadPart)
            QueryPerformanceFrequency(&s_perf_freq);
        QueryPerformanceCounter(&now);
        s_watchdog_armed_qpc = now.QuadPart;
    }
    s_watchdog_thread = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (!s_watchdog_thread)
        LOGW("watchdog: CreateThread failed (%lu)", GetLastError());
    else
        LOGI("watchdog: armed; reports the render thread's position if frames "
             "stop for %u ms", WATCHDOG_STALL_MS);
}
/* ---- the seam (src/glide_int.h) ----------------------------------------- */

/* Both jobs of the swap in one call: record the heartbeat, and on the first
 * swap identify the thread that produced it. They were two statements at the
 * top of the swap hook and there is no reason for the caller to know that. */
void wd_note_swap(uint64_t swap_qpc)
{
    InterlockedExchange64(&s_last_swap_qpc_atomic, (LONG64)swap_qpc);
    watchdog_start();
}
