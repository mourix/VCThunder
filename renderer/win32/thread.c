/* thread.c -- Win32 raster worker pool.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Workers split one triangle by scanline rows, spin on short jobs, and sleep
 * after idle periods. Interlocked generation and completion counters publish
 * job state with required ordering.
 */
#include <windows.h>
#include <stdlib.h>

#include "../core/vgl.h"
#include "../core/vcglide.h"

#define SPIN_YIELDS   4000      /* ~20-40 us of PAUSE before yielding */
#define YIELD_ROUNDS  2000      /* ~a frame of SwitchToThread before sleeping */

typedef struct {
    HANDLE          thread;
    HANDLE          wake;               /* only used once a worker has slept */
    volatile LONG   sleeping;
    int             index;
    int             y0, y1;
} worker_t;

static worker_t       s_worker[VGL_PAR_MAX];
static int            s_workers;        /* including the submitting thread */
static volatile LONG  s_gen;            /* bumped once per job */
static volatile LONG  s_done;           /* workers finished with this job */
static volatile LONG  s_quit;
static vgl_par_fn     s_fn;
static void          *s_ctx;

int vgl_par_workers(void)
{
    return s_workers < 1 ? 1 : s_workers;
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    worker_t *w = (worker_t *)arg;
    LONG seen = 0;

    for (;;) {
        long spins = 0, yields = 0;

        while (s_gen == seen) {
            if (s_quit)
                return 0;
            if (++spins < SPIN_YIELDS) {
                YieldProcessor();
            } else if (++yields < YIELD_ROUNDS) {
                SwitchToThread();
            } else {
                /* Nothing has been drawn for long enough that burning a core
                 * is the wrong trade. Sleeping costs the next job one
                 * SetEvent, and only the next one. */
                InterlockedExchange(&w->sleeping, 1);
                if (s_gen == seen)
                    WaitForSingleObject(w->wake, 50);
                InterlockedExchange(&w->sleeping, 0);
                yields = 0;
                spins = 0;
            }
        }
        seen = s_gen;
        if (s_quit)
            return 0;
        if (w->y1 > w->y0)
            s_fn(s_ctx, w->index, w->y0, w->y1);
        InterlockedIncrement(&s_done);
    }
}

/* Size the pool by physical cores. If topology discovery fails, conservatively
 * use half the logical processor count. */
static int physical_cores(const SYSTEM_INFO *si)
{
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf;
    DWORD bytes = 0, i;
    int cores = 0;

    GetLogicalProcessorInformation(NULL, &bytes);
    if (!bytes)
        return (int)si->dwNumberOfProcessors;
    buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(bytes);
    if (!buf)
        return (int)si->dwNumberOfProcessors;
    if (GetLogicalProcessorInformation(buf, &bytes)) {
        for (i = 0; i < bytes / sizeof *buf; i++)
            if (buf[i].Relationship == RelationProcessorCore)
                cores++;
    }
    free(buf);
    if (cores < 1)
        cores = (int)si->dwNumberOfProcessors;
    return cores;
}

int vgl_par_open(int want)
{
    SYSTEM_INFO si;
    int i, cores;

    if (s_workers)
        return s_workers;

    /* A pool is a new generation namespace.  vgl_par_close wakes the old
     * workers by incrementing s_gen; leaving that value in place makes each
     * replacement worker (whose local `seen` starts at zero) immediately run
     * s_fn/s_ctx from the last triangle before the surface reopen.  Those
     * pointers name the submitting thread's expired stack frame. */
    InterlockedExchange(&s_gen, 0);
    InterlockedExchange(&s_done, 0);
    InterlockedExchange(&s_quit, 0);
    s_fn = NULL;
    s_ctx = NULL;

    GetSystemInfo(&si);
    cores = physical_cores(&si);
    if (want <= 0)
        want = cores;
    if (want > VGL_PAR_MAX) want = VGL_PAR_MAX;
    if (want < 1) want = 1;
    s_workers = want;
    if (want == 1) {
        vgl_log(0, "raster: single-threaded (%d physical cores, %lu logical)",
                cores, si.dwNumberOfProcessors);
        return 1;
    }

    /* Worker 0 is the thread that submits the job: it rasterises its own
     * slice rather than waiting, which is both one more core and one less
     * handshake. */
    for (i = 1; i < s_workers; i++) {
        s_worker[i].index = i;
        s_worker[i].wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_worker[i].thread = CreateThread(NULL, 0, worker_main, &s_worker[i],
                                          0, NULL);
        if (!s_worker[i].thread) {
            vgl_log(0, "raster: only %d of %d workers started; continuing "
                       "with what came up", i, s_workers);
            s_workers = i;
            break;
        }
    }
    vgl_log(0, "raster: %d workers (%d physical cores, %lu logical), "
               "row-split, threshold %d rows", s_workers, cores,
            si.dwNumberOfProcessors, VGL_PAR_MIN_ROWS);
    return s_workers;
}

void vgl_par_close(void)
{
    int i;

    if (s_workers <= 1) {
        s_workers = 0;
        InterlockedExchange(&s_gen, 0);
        InterlockedExchange(&s_done, 0);
        InterlockedExchange(&s_quit, 0);
        s_fn = NULL;
        s_ctx = NULL;
        return;
    }
    InterlockedExchange(&s_quit, 1);
    InterlockedIncrement(&s_gen);
    for (i = 1; i < s_workers; i++) {
        if (s_worker[i].wake)
            SetEvent(s_worker[i].wake);
        if (s_worker[i].thread) {
            WaitForSingleObject(s_worker[i].thread, 1000);
            CloseHandle(s_worker[i].thread);
        }
        if (s_worker[i].wake)
            CloseHandle(s_worker[i].wake);
        s_worker[i].thread = NULL;
        s_worker[i].wake = NULL;
    }
    s_workers = 0;
    InterlockedExchange(&s_gen, 0);
    InterlockedExchange(&s_done, 0);
    InterlockedExchange(&s_quit, 0);
    s_fn = NULL;
    s_ctx = NULL;
}

void vgl_par_rows(vgl_par_fn fn, void *ctx, int y0, int y1)
{
    int rows = y1 - y0, n = s_workers, i, at, share, rem;
    LONG spins = 0;

    if (n <= 1 || rows <= 0) {
        fn(ctx, 0, y0, y1);
        return;
    }

    /* Rows are handed out in contiguous blocks rather than interleaved. Both
     * are correct; contiguous keeps each worker on its own cache lines of the
     * colour and depth buffers, and interleaved would have every worker
     * touching every line. */
    share = rows / n;
    rem   = rows % n;
    at    = y0;
    for (i = 0; i < n; i++) {
        int take = share + (i < rem ? 1 : 0);
        if (i > 0) {
            s_worker[i].y0 = at;
            s_worker[i].y1 = at + take;
        } else {
            y0 = at; y1 = at + take;
        }
        at += take;
    }

    s_fn  = fn;
    s_ctx = ctx;
    InterlockedExchange(&s_done, 0);
    InterlockedIncrement(&s_gen);       /* publishes everything above */
    for (i = 1; i < n; i++)
        if (s_worker[i].sleeping)
            SetEvent(s_worker[i].wake);

    if (y1 > y0)
        fn(ctx, 0, y0, y1);             /* the submitter's own share */

    /* Wait without timeout because returning early would expose a partially
     * drawn triangle; worker faults terminate the process. */
    while (s_done < (LONG)(n - 1)) {
        if (++spins < SPIN_YIELDS)
            YieldProcessor();
        else
            SwitchToThread();
    }
}
