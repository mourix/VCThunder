/* profile.c -- statistical sampler for game code.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A helper thread suspends the render thread, records EIP and x87 state, and
 * resumes it. Samples are reported by symbol and 64-byte bucket. Enabled only
 * by diagnostics.profile_ms.
 */
#include "vcthunder.h"


#include <stdio.h>
#include <string.h>

/* Attribute each address to the nearest preceding CODE symbol. Function totals
 * complement the finer 64-byte buckets. */
static uint32_t func_of(uint32_t eip)
{
    unsigned lo = 0, hi = G->func_count, mid;

    if (eip < G->func_va[0])
        return 0;
    while (hi - lo > 1) {
        mid = lo + (hi - lo) / 2;
        if (G->func_va[mid] <= eip)
            lo = mid;
        else
            hi = mid;
    }
    return G->func_va[lo];
}

#define BUCKET_SHIFT 6                     /* 64-byte buckets */
#define MAX_BUCKETS  8192

static struct { uint32_t addr; uint32_t hits; } s_bucket[MAX_BUCKETS];
static unsigned s_nbuckets;
static uint64_t s_samples, s_in_game, s_in_backend, s_in_water;

/* Sample and clear sticky x87 exception flags. Each positive sample represents
 * activity within one sampling interval rather than an inherited latch. */
static uint64_t s_fp_de, s_fp_ue, s_fp_pe, s_fp_ie, s_fp_seen;
static uint32_t s_fp_cw_last;
static HANDLE   s_target, s_thread;
static volatile LONG s_stop;
static unsigned s_period_ms;

/* A sticky-flag duty cycle measures presence, not event rate. Supplement it
 * with live-register magnitudes, EIP attribution, and a local normal-versus-
 * denormal cost probe. */
static uint64_t s_reg_live, s_reg_sub32, s_reg_sub64, s_reg_extden;

/* Unbiased binary exponent bands. -126 is the float32 normal floor and -149
 * its denormal floor, so the last two bands are values that a float32 store
 * cannot hold and a float32 load must assist on. */
static uint64_t s_exp_band[6];
static const char *const EXP_BAND_NAME[6] = {
    ">=2^0", "2^-1..-30", "2^-31..-60", "2^-61..-126",
    "2^-127..-149 (f32 denormal)", "<2^-149 (f32 flushes to 0)"
};

static struct { uint32_t addr; uint32_t hits; } s_func[MAX_BUCKETS];
static unsigned s_nfuncs;
static struct { uint32_t addr; uint32_t hits; } s_bucket_de[MAX_BUCKETS];
static unsigned s_nbuckets_de;
static uint64_t s_samples_de;

/* Takes the KEY, already reduced by the caller: the bucket tables key on
 * eip >> BUCKET_SHIFT, the function table keys on a whole function VA. Shifting
 * in here would have quietly truncated the latter. */
static void record_into(uint32_t key, void *table, unsigned *count)
{
    struct bucket { uint32_t addr, hits; } *b = table;
    unsigned i;

    for (i = 0; i < *count; i++) {
        if (b[i].addr == key) {
            b[i].hits++;
            return;
        }
    }
    if (*count < MAX_BUCKETS) {
        b[*count].addr = key;
        b[*count].hits = 1;
        (*count)++;
    }
}

static void record(uint32_t eip)
{
    record_into(eip >> BUCKET_SHIFT, s_bucket, &s_nbuckets);
    record_into(func_of(eip), s_func, &s_nfuncs);
}

/* The x87 register file as FSAVE lays it out: eight 10-byte extended values in
 * physical order, plus a 2-bit tag each (0 valid, 1 zero, 2 special, 3 empty).
 * Physical order is fine here; the question is what magnitudes are in flight,
 * not which is ST(0). */
static void sample_registers(const FLOATING_SAVE_AREA *fs)
{
    unsigned r;

    for (r = 0; r < 8; r++) {
        const unsigned char *p = (const unsigned char *)fs->RegisterArea + r * 10;
        unsigned tag = ((unsigned)fs->TagWord >> (2 * r)) & 3u;
        unsigned raw_exp;
        uint64_t sig;
        int exp;

        if (tag == 3u || tag == 1u)          /* empty or zero: no magnitude */
            continue;

        memcpy(&sig, p, sizeof sig);
        raw_exp = (unsigned)(p[8] | ((unsigned)p[9] << 8)) & 0x7FFFu;
        if (raw_exp == 0x7FFFu)              /* NaN / infinity */
            continue;
        if (raw_exp == 0u && sig == 0u)
            continue;

        s_reg_live++;
        if (raw_exp == 0u) {                 /* true extended denormal */
            s_reg_extden++;
            s_exp_band[5]++;
            continue;
        }

        exp = (int)raw_exp - 16383;
        if      (exp >= 0)    s_exp_band[0]++;
        else if (exp >= -30)  s_exp_band[1]++;
        else if (exp >= -60)  s_exp_band[2]++;
        else if (exp >= -126) s_exp_band[3]++;
        else if (exp >= -149) s_exp_band[4]++;
        else                  s_exp_band[5]++;

        if (exp < -126) s_reg_sub32++;       /* denormal if stored as float32 */
        if (exp < -1022) s_reg_sub64++;      /* denormal if stored as double  */
    }
}

/* Compare identical steady-state loops using normal and denormal memory
 * operands. RDTSC provides a stable ratio, not an absolute core-cycle count. */
static uint64_t time_fmul(float operand, unsigned iters)
{
    volatile float v = operand;
    volatile float acc = 1.0f;
    uint64_t t0, t1;
    unsigned i;

    t0 = diag_rdtsc();
    for (i = 0; i < iters; i++) {
        __asm__ volatile ("flds %1 ; fmuls %2 ; fstps %0"
                          : "=m" (acc) : "m" (acc), "m" (v) : "st");
    }
    t1 = diag_rdtsc();
    return t1 - t0;
}

static void report_denormal_cost(void)
{
    const unsigned ITERS = 200000u;
    union { uint32_t u; float f; } tiny;
    uint64_t normal, denorm;
    double a, b;

    tiny.u = 0x00000001u;                    /* 1.4e-45, the smallest float32 */

    time_fmul(1.0f, 1000u);                  /* warm the loop into the caches */
    normal = time_fmul(1.0f, ITERS);
    denorm = time_fmul(tiny.f, ITERS);

    a = (double)normal / ITERS;
    b = (double)denorm / ITERS;
    LOGI("profile: x87 fmul cost on this CPU - normal operand %.1f tsc/op, "
         "denormal operand %.1f tsc/op, ratio %.1fx (assist ~%.0f ticks)",
         a, b, a > 0.0 ? b / a : 0.0, b - a);
    LOGI("profile: for assists alone to explain the 15.7x frame inflation "
         "(~164M cycles/frame) there must be ~%.0f of them per submitted "
         "triangle at that cost", b > a ? 164e6 / (b - a) / 1559.0 : 0.0);
}

static DWORD WINAPI sampler(LPVOID arg)
{
    CONTEXT ctx;

    (void)arg;
    while (!InterlockedCompareExchange(&s_stop, 0, 0)) {
        Sleep(s_period_ms);
        if (SuspendThread(s_target) == (DWORD)-1)
            continue;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_FLOATING_POINT;
        if (GetThreadContext(s_target, &ctx)) {
            uint32_t sw = ctx.FloatSave.StatusWord;
            s_fp_seen++;
            s_fp_cw_last = ctx.FloatSave.ControlWord;
            if (sw & 0x0002u) s_fp_de++;   /* denormal operand */
            if (sw & 0x0010u) s_fp_ue++;   /* underflow        */
            if (sw & 0x0020u) s_fp_pe++;   /* precision (normal, expected) */
            if (sw & 0x0001u) s_fp_ie++;   /* invalid          */
            /* Clear the sticky bits so the next sample reports fresh news. */
            if (sw & 0x003Fu) {
                ctx.FloatSave.StatusWord = sw & ~0x003Fu;
                ctx.ContextFlags = CONTEXT_FLOATING_POINT;
                SetThreadContext(s_target, &ctx);
            }
            sample_registers(&ctx.FloatSave);
            s_samples++;
            if (ctx.Eip >= G->image_base && ctx.Eip < G->span_end) {
                s_in_game++;
                /* 0..waterfall would charge the whole low image to water on a
                 * title with no _water_ModuleInit: Offroad has none. */
                if (G->water_module_init && ctx.Eip >= G->water_module_init &&
                    ctx.Eip < G->waterfall_module_init)
                    s_in_water++;
                record(ctx.Eip);
            } else {
                s_in_backend++;
            }
            if (sw & 0x0002u) {              /* attribute the DE windows */
                s_samples_de++;
                record_into((uint32_t)ctx.Eip >> BUCKET_SHIFT, s_bucket_de, &s_nbuckets_de);
            }
        }
        ResumeThread(s_target);
    }
    return 0;
}

/* Count geometry submissions and culling decisions to distinguish per-call
 * cost from excessive call volume. */
static uint32_t s_n_drawgroup, s_n_testbound, s_n_lod, s_n_calcunit;
static uint32_t s_n_vtx_hit, s_n_vtx_miss;
static uint32_t s_n_loop[VCT_MAX_MESH_LOOPS];

/* Time an ordered series from leaf geometry routines through DrawGroup. The
 * series separates uniform game-code cost from backend work below DrawGroup. */
static struct call_timer s_t_drawgroup, s_t_testbound, s_t_lod, s_t_calcunit;
/* Three instructions: fld [esp+4], fsqrt, ret. It has no loop, no callee and no
 * memory traffic beyond one dword, so there is no structural reason for it to
 * cost more than ~30 cycles. If it does, x87 itself is being multiplied here
 * and no amount of subsystem attribution was ever going to find that. */
static struct call_timer s_t_sqrt;
static uint64_t s_timer_overhead;

/* Use the independently bounded _xmath_sqrt leaf as the in-situ timing-overhead
 * control. Tight-loop calibration does not include call depth or cache cadence. */
static double s_insitu_overhead;

/* Measured, not assumed: what this process actually achieves when it streams,
 * and how often the game reaches the canary. Their product over one inter-call
 * interval is a ceiling on how much memory can possibly move between two
 * consecutive calls, which decides which row of the pollution sweep is
 * entitled to explain the game's figure. See report_stream_ceiling(). */
static double s_stream_bps;
static double s_sqrt_calls_per_frame;
static LARGE_INTEGER s_qpc_freq, s_last_report_qpc;

/* Report raw mean, minimum, fast fraction, and calls per frame. Instrument
 * overhead is additive and is reported separately rather than used as a
 * multiplicative suppression threshold. */
static void report_call_timer(struct call_timer *t, unsigned frames)
{
    double per;

    if (!t->calls) {
        if (t->offthread)
            LOGI("    %-32s no completed calls, %u entries DECLINED (reached "
                 "from a thread that does not own the timer)",
                 t->name, t->offthread);
        return;
    }
    per = (double)t->cycles / (double)t->calls;
    if (t == &s_t_sqrt) {
        s_insitu_overhead = per;   /* still the standard; a subtrahend, not a
                                    * multiplier */
        s_sqrt_calls_per_frame = frames ? (double)t->calls / frames : 0.0;
    }
    LOGI("    %-32s mean %8.0f  min %6u  %5.1f%% under %u  %7.1f calls/frame"
         "%s%s%s",
         t->name, per,
         t->min == 0xFFFFFFFFu ? 0u : t->min,
         100.0 * (double)t->fast / (double)t->calls, CALL_TIMER_FAST,
         frames ? (double)t->calls / frames : 0.0,
         t->overflow ? "  NESTING OVERFLOW" : "",
         t->depth ? "  UNBALANCED" : "",
         t->offthread ? "  OFF-THREAD ENTRIES DECLINED" : "");
    patch_timer_reset(t);
    t->offthread = 0;
}

/* Compare fld/fsqrt on controlled normal and denormal operands in the render
 * thread. This separates process-wide x87 state from operand-specific assists. */
static void x87_probe(void)
{
    static const volatile float NORMAL = 2.0f;
    static const volatile float DENORM = 1.0e-40f;   /* < 2^-126: f32 denormal */
    volatile float sink;
    uint16_t cw = 0;
    uint64_t t0, c_norm, c_den;
    unsigned i;
    const unsigned N = 20000u;

    for (i = 0; i < 200u; i++)                       /* warm, then measure */
        __asm__ volatile ("flds %1; fsqrt; fstps %0"
                          : "=m" (sink) : "m" (NORMAL));
    t0 = diag_rdtsc();
    for (i = 0; i < N; i++)
        __asm__ volatile ("flds %1; fsqrt; fstps %0"
                          : "=m" (sink) : "m" (NORMAL));
    c_norm = diag_rdtsc() - t0;

    for (i = 0; i < 200u; i++)
        __asm__ volatile ("flds %1; fsqrt; fstps %0"
                          : "=m" (sink) : "m" (DENORM));
    t0 = diag_rdtsc();
    for (i = 0; i < N; i++)
        __asm__ volatile ("flds %1; fsqrt; fstps %0"
                          : "=m" (sink) : "m" (DENORM));
    c_den = diag_rdtsc() - t0;

    __asm__ volatile ("fnstcw %0" : "=m" (cw));   /* live, not the sampler's */
    LOGI("x87 probe (this thread, fld+fsqrt+fstp): normal operand %.1f "
         "cycles/op, f32-denormal operand %.1f cycles/op, ratio %.1fx; "
         "x87 control word 0x%04X",
         (double)c_norm / N, (double)c_den / N,
         c_norm ? (double)c_den / (double)c_norm : 0.0, cw);
}

/* Time fixed-frame attract stages using generated addresses. Frames divided by
 * wall time is authoritative; the nominal-limit percentage is contextual. */

void profile_attract_poll(void)
{
    static int             inited;
    static uint32_t        prev;
    static LARGE_INTEGER   t_prev, freq;
    static unsigned        frames;
    uint32_t stage;
    LARGE_INTEGER now;
    double wall;
    unsigned limit;

    /* Hand-found Hydro addresses (EXTRAS in gen-gamedefs.py), not derived from
     * anything, so there is no equivalent for another title until someone finds
     * one. Read-only diagnostic: silence is the right behaviour, not a warning
     * on every swap. */
    if (!G->attract_stage)
        return;
    stage = *(const volatile uint32_t *)(uintptr_t)G->attract_stage;

    frames++;
    if (!inited) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t_prev);
        prev = stage;
        inited = 1;
        frames = 0;
        return;
    }
    if (stage == prev || !freq.QuadPart)
        return;

    QueryPerformanceCounter(&now);
    wall = (double)(now.QuadPart - t_prev.QuadPart) / (double)freq.QuadPart;
    limit = prev < G->attract_stages
          ? *(const volatile uint16_t *)(uintptr_t)
                (G->attract_table + prev * G->attract_stride)
          : 0u;

    if (wall > 0.05) {
        /* Derive simulation speed from rendered frames and wall time. The stage
         * limit is a cap and is reported only as context. */
        LOGI("attract: stage %u -> %u | %u frames in %.2f s wall = %.2f fps = "
             "%.0f%% of real-time game speed (stage cap %u frames)",
             prev, stage, frames, wall, (double)frames / wall,
             100.0 * ((double)frames / wall) / 30.0, limit);
    }
    t_prev = now;
    prev = stage;
    frames = 0;
}

/* Invoke the hooked sqrt leaf from a controlled loop after reporting game calls.
 * This holds target, instrument, thread, and time constant while changing the
 * caller and cache state. */
static unsigned char *s_pollute;
/* Must exceed the last-level cache, or the sweep measures nothing.
 *
 * The first version of this stopped at 4 MB and topped out at 95 cycles/call,
 * which looked like "cache pressure cannot explain the game's 750". It could not
 * have: this part has a 32 MB L3, so a 4 MB stream only pushes the stub's lines
 * from L1 into L3, and an L3 hit is ~50 cycles. Only a stream LARGER than the L3
 * makes the next touch go to DRAM, which is the case the game is actually in. */
#define POLLUTE_BYTES (64u << 20)

typedef float (__cdecl *sqrt_fn)(float);

/* Repeat timing at a controlled call depth to account for return-stack-buffer
 * disruption caused by the displaced return address. Prevent tail-call removal. */
static float __attribute__((noinline)) depth_call(sqrt_fn f, int d, float x)
{
    if (d > 0)
        return depth_call(f, d - 1, x) + 0.0f;
    return f(x);
}

/* Interleave direct and depth-controlled canary calls at the game's triangle
 * cadence. This separates sparse-instrumentation cache effects from call depth. */
static struct call_timer s_t_ilv_flat, s_t_ilv_deep;
static sqrt_fn s_ilv_flat_fn, s_ilv_deep_fn;
static unsigned s_ilv_tick;
#define INTERLEAVE_DEPTH 16

/* Install one timer, refusing rather than guessing.
 *
 * Two things can be absent and both mean "this title is not that title": the
 * address, and the prologue the generator could not decode into a relocatable
 * length. Either one is a 0, and 0 is a real answer. */
static void install_timer(uint32_t va, uint32_t prologue, struct call_timer *t,
                          const char *name)
{
    if (!game_require("time_calls", name, va && prologue))
        return;
    patch_time_calls(va, t, (int)prologue, name);
}

void profile_interleave_tick(void)
{
    volatile float sink;

    if (s_ilv_tick++ & 1u) {
        if (s_ilv_flat_fn)
            sink = s_ilv_flat_fn(2.0f);
    } else {
        if (s_ilv_deep_fn)
            sink = depth_call(s_ilv_deep_fn, INTERLEAVE_DEPTH, 2.0f);
    }
    (void)sink;
}

/* One line per sub-probe, in the same shape as report_call_timer(), then reset.
 * The min matters more than the mean here: it is the sub-probe's answer to "how
 * cheap can this call be when the state happens to be right", and comparing
 * MINIMA across the sweep is what separates a fixed instrument tax from a
 * genuine stall. */
static void probe_line(const char *what, const char *how)
{
    if (!s_t_sqrt.calls)
        return;
    LOGI("    %-32s mean %8.0f  min %6u  %5.1f%% under %u  <- %s",
         what, (double)s_t_sqrt.cycles / (double)s_t_sqrt.calls,
         s_t_sqrt.min == 0xFFFFFFFFu ? 0u : s_t_sqrt.min,
         100.0 * (double)s_t_sqrt.fast / (double)s_t_sqrt.calls,
         CALL_TIMER_FAST, how);
    patch_timer_reset(&s_t_sqrt);
}

static void sqrt_tight_probe(void)
{
    sqrt_fn f = (sqrt_fn)(uintptr_t)G->xmath_sqrt;
    volatile float sink;
    unsigned i;
    const unsigned N = 20000u;
    uint64_t cyc;
    uint32_t calls;

    if (!G->xmath_sqrt)
        return;
    if (!s_pollute)
        s_pollute = (unsigned char *)VirtualAlloc(NULL, POLLUTE_BYTES,
                                                  MEM_RESERVE | MEM_COMMIT,
                                                  PAGE_READWRITE);

    for (i = 0; i < 200u; i++)                  /* warm i-cache and BTB      */
        sink = f(2.0f);
    patch_timer_reset(&s_t_sqrt);
    for (i = 0; i < N; i++)
        sink = f((float)(i + 1u));
    cyc = s_t_sqrt.cycles;
    calls = s_t_sqrt.calls;
    (void)sink; (void)cyc; (void)calls;

    probe_line("_xmath_sqrt (ours)",
               "SAME function, SAME instrument, TIGHT LOOP here");

    {   /* And the pool stub, re-timed now rather than at startup: if the whole
         * process has gone slow this rises with everything else. */
        uint64_t now = patch_timer_recalibrate();
        if (now)
            LOGI("    %-32s %10llu cycles/call  <- pool stub, re-timed NOW "
                 "(startup figure was ~40)", "timer calibration",
                 (unsigned long long)now);
    }

    {   /* The same call, reached through a call chain of known depth. */
        static const int DEPTH[] = { 2, 6, 12, 24 };
        unsigned d;

        for (d = 0; d < sizeof DEPTH / sizeof DEPTH[0]; d++) {
            const unsigned M = 20000u;
            char how[64];

            for (i = 0; i < 200u; i++)
                sink = depth_call(f, DEPTH[d], 2.0f);
            patch_timer_reset(&s_t_sqrt);
            for (i = 0; i < M; i++)
                sink = depth_call(f, DEPTH[d], (float)(i + 1u));
            snprintf(how, sizeof how, "tight loop, called from %d frames deep",
                     DEPTH[d]);
            probe_line("_xmath_sqrt (ours)", how);
        }
        patch_timer_reset(&s_t_sqrt);
    }

    /* Sweep controlled cache churn between canary calls. The result is a
     * sufficient working-set scale, not an identification of the game workload. */
    if (s_pollute) {
        static const size_t SIZE[] = { 1u << 20, 4u << 20, 16u << 20,
                                       32u << 20, 64u << 20 };
        static const char *const NAME[] = { "1 MB", "4 MB", "16 MB",
                                            "32 MB (= L3)", "64 MB (> L3)" };
        unsigned s;

        for (s = 0; s < sizeof SIZE / sizeof SIZE[0]; s++) {
            const unsigned M = 120u;
            size_t k;
            volatile unsigned drain = 0;
            char how[80];
            LARGE_INTEGER b0, b1;

            patch_timer_reset(&s_t_sqrt);
            QueryPerformanceCounter(&b0);
            for (i = 0; i < M; i++) {
                for (k = 0; k < SIZE[s]; k += 64)
                    drain += s_pollute[k];
                sink = f((float)(i + 1u));
            }
            QueryPerformanceCounter(&b1);
            /* What this loop actually achieves is the only honest ceiling on
             * what the GAME could stream between two of its own calls, but
             * ONLY from the largest block. The small ones never leave the cache
             * and report 140+ GB/s, which is a true statement about L2 and a
             * useless one about how fast a working set can be replaced. */
            if (s_qpc_freq.QuadPart && b1.QuadPart > b0.QuadPart &&
                s == (sizeof SIZE / sizeof SIZE[0]) - 1u) {
                double secs = (double)(b1.QuadPart - b0.QuadPart) /
                              (double)s_qpc_freq.QuadPart;
                s_stream_bps = (double)SIZE[s] * (double)M / secs;
            }
            (void)drain;
            snprintf(how, sizeof how,
                     "tight loop, %s streamed between consecutive calls",
                     NAME[s]);
            probe_line("_xmath_sqrt (ours)", how);
        }
        patch_timer_reset(&s_t_sqrt);
    }
}

/* Bound plausible inter-call churn by measured streaming bandwidth and canary
 * cadence before interpreting the cache sweep. */
static void report_stream_ceiling(unsigned frames)
{
    LARGE_INTEGER now;
    double secs, frame_ms, interval_s, budget_mb, need_gbs;

    if (!frames || s_stream_bps <= 0.0 || s_sqrt_calls_per_frame <= 0.0)
        return;
    QueryPerformanceCounter(&now);
    if (!s_qpc_freq.QuadPart || !s_last_report_qpc.QuadPart) {
        s_last_report_qpc = now;
        return;
    }
    secs = (double)(now.QuadPart - s_last_report_qpc.QuadPart) /
           (double)s_qpc_freq.QuadPart;
    s_last_report_qpc = now;
    if (secs <= 0.0)
        return;

    frame_ms   = 1000.0 * secs / (double)frames;
    interval_s = secs / ((double)frames * s_sqrt_calls_per_frame);
    budget_mb  = s_stream_bps * interval_s / (1024.0 * 1024.0);
    need_gbs   = 32.0 * 1024.0 * 1024.0 / interval_s / 1e9;

    LOGI("    ceiling: this process streams %.1f GB/s; the game reaches the "
         "canary %.0f times per %.1f ms frame, so at most %.2f MB can move "
         "between two consecutive calls. Read THAT row of the sweep. Evicting "
         "a 32 MB L3 in the same interval would need %.0f GB/s.",
         s_stream_bps / 1e9, s_sqrt_calls_per_frame, frame_ms, budget_mb,
         need_gbs);
}

void profile_report_call_times(unsigned frames)
{
    if (!g_cfg.time_calls)
        return;
    if (!s_qpc_freq.QuadPart)
        QueryPerformanceFrequency(&s_qpc_freq);
    x87_probe();
    LOGI("cost per call (inclusive, RAW: the instrument adds ~%llu cycles per "
         "instrumented call, nested ones included, and it is ADDITIVE):",
         (unsigned long long)s_timer_overhead);
    report_call_timer(&s_t_sqrt, frames);
    /* The control, printed next to the thing it controls: same instrument, same
     * thread, same frames, same cadence, same depth; our call, not the
     * game's. */
    report_call_timer(&s_t_ilv_flat, frames);
    report_call_timer(&s_t_ilv_deep, frames);
    sqrt_tight_probe();
    report_stream_ceiling(frames);
    report_call_timer(&s_t_calcunit, frames);
    report_call_timer(&s_t_testbound, frames);
    report_call_timer(&s_t_lod, frames);
    report_call_timer(&s_t_drawgroup, frames);
}

void profile_report_geometry(unsigned frames)
{
    if (!frames || !(s_n_drawgroup | s_n_testbound | s_n_lod | s_n_calcunit))
        return;
    LOGI("geometry census per frame: DrawGroup %.1f, TestBoundWithFrustum %.1f "
         "(cull rate %.0f%%), DetermineLod %.1f, vec3_CalcUnit %.0f",
         (double)s_n_drawgroup / frames, (double)s_n_testbound / frames,
         s_n_testbound ? 100.0 * (1.0 - (double)s_n_drawgroup /
                                        (double)s_n_testbound) : 0.0,
         (double)s_n_lod / frames, (double)s_n_calcunit / frames);
    {   /* Every loop in _mesh3d_DrawGroup_NoFrustumTest, by address. Divide the
         * function's sampled cycles by the largest count to get cycles per
         * iteration of the loop that actually runs: the number that says
         * whether this code is slow or merely busy. */
        unsigned li;
        for (li = 0; li < G->loop_count; li++) {
            if (!s_n_loop[li])
                continue;
            LOGI("    mesh3d loop @0x%08X (body %d B)  %.0f iterations/frame",
                 G->loop[li].va, G->loop[li].body,
                 (double)s_n_loop[li] / frames);
            s_n_loop[li] = 0;
        }
    }
    if (s_n_vtx_hit | s_n_vtx_miss) {
        uint32_t tot = s_n_vtx_hit + s_n_vtx_miss;
        LOGI("vertex cache per frame: %.0f transformed, %.0f reused "
             "(hit rate %.1f%%); %.1f vertices transformed per submitted "
             "triangle",
             (double)s_n_vtx_miss / frames, (double)s_n_vtx_hit / frames,
             tot ? 100.0 * (double)s_n_vtx_hit / (double)tot : 0.0,
             (double)s_n_vtx_miss / frames / 3000.0);
        s_n_vtx_hit = s_n_vtx_miss = 0;
    }
    LOGI("viewport: fov %.3f, frustum pixels %.1f x %.1f, unclipped limits "
         "%.1f %.1f %.1f %.1f, hres %d vres %d",
         (double)*(const float *)(uintptr_t)G->viewport_fov,
         (double)((const float *)(uintptr_t)G->viewport_frustum_pixels)[0],
         (double)((const float *)(uintptr_t)G->viewport_frustum_pixels)[1],
         (double)((const float *)(uintptr_t)G->viewport_unclipped)[0],
         (double)((const float *)(uintptr_t)G->viewport_unclipped)[1],
         (double)((const float *)(uintptr_t)G->viewport_unclipped)[2],
         (double)((const float *)(uintptr_t)G->viewport_unclipped)[3],
         *(const int *)(uintptr_t)G->viewport_hres,
         *(const int *)(uintptr_t)G->viewport_vres);
    s_n_drawgroup = s_n_testbound = s_n_lod = s_n_calcunit = 0;
}

int profile_install(unsigned period_ms)
{
    /* This deliberately destroys the water surface. It exists only to measure
     * a subsystem that the old nearest-symbol profile split across several
     * unnamed static functions and therefore mis-described as diffuse tail.
     * It is independent of sampling so a clean wall-period A/B can run with
     * profile_ms=0 if sampler perturbation is itself under suspicion. */
    if (g_cfg.null_water && game_require("null_water", "_water_Draw",
                                         G->water_draw)) {
        patch_ret_imm(G->water_draw, 0);
        LOGW("null_water=true: _water_Draw @0x%08X stubbed; water will be "
             "MISSING. Diagnostic only; compare the same scene with false.",
             G->water_draw);
    }

    /* Deliberately ahead of the period_ms gate: cost per call is the one
     * measurement the sampler cannot make, so it must be available WITHOUT the
     * sampler running; the sampler suspends the render thread 1,000 times a
     * second, which is exactly the perturbation a cycle count must not carry.
     *
     * Same prologue lengths as the counters below, and the same targets, so a
     * run can be compared against an existing counting run directly. */
    if (g_cfg.time_calls) {
        s_timer_overhead = patch_timer_overhead();
        /* Use generated per-title prologue descriptors. A zero descriptor
         * refuses the hook rather than relocating a partial instruction. */
        install_timer(G->xmath_sqrt, G->hook_time_sqrt, &s_t_sqrt,
                      "_xmath_sqrt");
        install_timer(G->vec3_calcunit, G->hook_time_calcunit, &s_t_calcunit,
                      "_vec3_CalcUnit");
        install_timer(G->mesh3d_testbound, G->hook_time_testbound,
                      &s_t_testbound, "_mesh3d_TestBoundWithFrustum");
        install_timer(G->mesh3d_determinelod, G->hook_time_lod, &s_t_lod,
                      "_mesh3d_DetermineLod");
        install_timer(G->mesh3d_drawgroup, G->hook_time_drawgroup,
                      &s_t_drawgroup, "_mesh3d_DrawGroup_NoFrustumTest");

        /* Two byte-identical copies of the canary, instrumented the same way,
         * for us to call at the game's own cadence from glide_bind.c. Emitted
         * rather than reused so the game's population is never contaminated by
         * our calls, and so the two depths keep separate timers. */
        {
            void *f0 = patch_emit_sqrt();
            void *fd = patch_emit_sqrt();

            if (f0 && patch_time_calls((uint32_t)(uintptr_t)f0, &s_t_ilv_flat,
                                       6, "sqrt (ours, per-triangle)"))
                s_ilv_flat_fn = (sqrt_fn)f0;
            if (fd && patch_time_calls((uint32_t)(uintptr_t)fd, &s_t_ilv_deep,
                                       6, "sqrt (ours, per-tri, 16 deep)"))
                s_ilv_deep_fn = (sqrt_fn)fd;
        }

        LOGW("time_calls=true: four mesh targets plus two interleaved controls "
             "carry an entry/exit timer. The timers are owned by the render "
             "thread and decline calls from any other, so a multi-threaded "
             "target loses samples rather than crashing.");
    }

    if (!period_ms)
        return 0;

    /* Two reasons this run cannot also sample, and the second is the important
     * one. The counters would detour entries the timers have already detoured,
     * relocating a prologue that is now a jmp. And the sampler SUSPENDS the
     * render thread a thousand times a second, while rdtsc keeps counting
     * through a suspension, so every sampled call would bill the sampler's
     * stall to the game. Cost per call and sampling are mutually exclusive
     * instruments, not merely awkward together. */
    if (g_cfg.time_calls) {
        LOGW("profile_ms=%u ignored: time_calls=true owns the same four "
             "targets, and rdtsc keeps counting while the sampler suspends "
             "this thread. Run them in separate sessions.", period_ms);
        return 0;
    }

    /* The same four prologues, from the same generated fields, for the same
     * reason: these were literals too ("verified by disassembly": of Hydro),
     * and the counters relocate exactly what the timers relocate. Forcing
     * Hydro's 5 on Offroad's 10-byte _mesh3d_DetermineLod is a measured
     * ACCESS_VIOLATION at 16 s, so this path had the identical bug waiting. */
    if (game_require("profile_ms", "_mesh3d_DrawGroup_NoFrustumTest",
                     G->mesh3d_drawgroup && G->hook_time_drawgroup))
        patch_count_calls(G->mesh3d_drawgroup, &s_n_drawgroup,
                          (int)G->hook_time_drawgroup,
                          "_mesh3d_DrawGroup_NoFrustumTest");
    if (game_require("profile_ms", "_mesh3d_TestBoundWithFrustum",
                     G->mesh3d_testbound && G->hook_time_testbound))
        patch_count_calls(G->mesh3d_testbound, &s_n_testbound,
                          (int)G->hook_time_testbound,
                          "_mesh3d_TestBoundWithFrustum");
    if (game_require("profile_ms", "_mesh3d_DetermineLod",
                     G->mesh3d_determinelod && G->hook_time_lod))
        patch_count_calls(G->mesh3d_determinelod, &s_n_lod,
                          (int)G->hook_time_lod, "_mesh3d_DetermineLod");
    if (game_require("profile_ms", "_vec3_CalcUnit",
                     G->vec3_calcunit && G->hook_time_calcunit))
        patch_count_calls(G->vec3_calcunit, &s_n_calcunit,
                          (int)G->hook_time_calcunit, "_vec3_CalcUnit");

    /* Mid-function sites, derived by the generator. Both are branch targets of
     * the same compare, so the only way in is the branch itself: nothing
     * jumps into the relocated prologue. 6 relocatable bytes each. */
    patch_count_calls(G->mesh3d_vtx_hit, &s_n_vtx_hit, 6,
                      "mesh3d vertex-cache hit");
    patch_count_calls(G->mesh3d_vtx_miss, &s_n_vtx_miss, 6,
                      "mesh3d vertex-cache miss");
    /* Off by default, and that default is not caution for its own sake: this
     * patches 13 sites INSIDE one hot function. The generator refuses a site
     * whose head is branched into, because patching one of those makes the game
     * drop triangles; even so, a diagnostic that rewrites the middle of a
     * function has no business being on during play. */
    if (g_cfg.count_loops) {
        unsigned li;
        char nm[64];
        for (li = 0; li < G->loop_count; li++) {
            snprintf(nm, sizeof nm, "mesh3d loop @0x%08X",
                     G->loop[li].va);
            patch_count_calls(G->loop[li].va, &s_n_loop[li],
                              G->loop[li].prologue, nm);
        }
    }
    s_period_ms = period_ms;

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                         FALSE, 0)) {
        LOGE("profile: cannot duplicate render-thread handle (%lu)",
             GetLastError());
        return 0;
    }
    s_thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (!s_thread) {
        LOGE("profile: CreateThread failed %lu", GetLastError());
        return 0;
    }
    LOGI("profile: sampling the render thread every %u ms "
         "(diagnostic; resolve addresses with scripts/p1-symbols.py --lookup)",
         s_period_ms);
    report_denormal_cost();
    return 1;
}

/* Selection sort of the top 12; the tables are small and this runs once per
 * report interval, so simplicity beats cleverness. */
static void print_top_ex(const char *title, void *table, unsigned count,
                         uint64_t total, int shifted)
{
    struct bucket { uint32_t addr, hits; } *b = table;
    unsigned i, j, shown = 0;

    if (!count || !total)
        return;
    LOGI("profile: %s (%llu samples)", title, (unsigned long long)total);
    for (i = 0; i < count && shown < 12u; i++) {
        unsigned best = i;
        for (j = i + 1; j < count; j++)
            if (b[j].hits > b[best].hits)
                best = j;
        if (best != i) {
            struct bucket t = b[i];
            b[i] = b[best];
            b[best] = t;
        }
        if (!b[i].hits)
            break;
        LOGI("    %5.1f%%  0x%08X",
             100.0 * (double)b[i].hits / (double)total,
             shifted ? b[i].addr << BUCKET_SHIFT : b[i].addr);
        shown++;
    }
}

void profile_report(void)
{
    unsigned i;

    if (!s_samples)
        return;

    LOGI("profile: %llu samples - %.1f%% in the game image, %.1f%% in the "
         "backend/OS",
         (unsigned long long)s_samples,
         100.0 * (double)s_in_game / (double)s_samples,
         100.0 * (double)s_in_backend / (double)s_samples);
    if (G->water_module_init)
    LOGI("profile: water code region 0x%08X..0x%08X accounts for %.1f%% of "
         "all samples (%.1f%% of game-image samples)",
         G->water_module_init, G->waterfall_module_init,
         100.0 * (double)s_in_water / (double)s_samples,
         s_in_game ? 100.0 * (double)s_in_water / (double)s_in_game : 0.0);

    if (s_fp_seen)
        LOGI("profile: x87 sticky flags over %llu samples - denormal %.1f%%, "
             "underflow %.1f%%, invalid %.1f%%, precision %.1f%%; control word "
             "0x%04X",
             (unsigned long long)s_fp_seen,
             100.0 * (double)s_fp_de / (double)s_fp_seen,
             100.0 * (double)s_fp_ue / (double)s_fp_seen,
             100.0 * (double)s_fp_ie / (double)s_fp_seen,
             100.0 * (double)s_fp_pe / (double)s_fp_seen,
             s_fp_cw_last & 0xFFFFu);

    if (s_reg_live) {
        LOGI("profile: x87 live registers %llu sampled - %.1f%% below the "
             "float32 normal floor (denormal if stored as float32), %.1f%% "
             "below the double floor, %.1f%% true extended denormals",
             (unsigned long long)s_reg_live,
             100.0 * (double)s_reg_sub32 / (double)s_reg_live,
             100.0 * (double)s_reg_sub64 / (double)s_reg_live,
             100.0 * (double)s_reg_extden / (double)s_reg_live);
        for (i = 0; i < 6u; i++)
            if (s_exp_band[i])
                LOGI("    %5.1f%%  %s",
                     100.0 * (double)s_exp_band[i] / (double)s_reg_live,
                     EXP_BAND_NAME[i]);
    }

    print_top_ex("hottest FUNCTIONS (nearest preceding symbol)",
                 s_func, s_nfuncs, s_samples, 0);
    print_top_ex("hottest 64-byte buckets", s_bucket, s_nbuckets, s_samples, 1);
    if (s_samples_de)
        print_top_ex("hottest buckets, denormal windows only",
                     s_bucket_de, s_nbuckets_de, s_samples_de, 1);

    memset(s_bucket, 0, sizeof s_bucket);
    memset(s_func, 0, sizeof s_func);
    memset(s_bucket_de, 0, sizeof s_bucket_de);
    memset(s_exp_band, 0, sizeof s_exp_band);
    s_nbuckets = s_nbuckets_de = s_nfuncs = 0;
    s_samples = s_in_game = s_in_backend = s_in_water = s_samples_de = 0;
    s_fp_de = s_fp_ue = s_fp_pe = s_fp_ie = s_fp_seen = 0;
    s_reg_live = s_reg_sub32 = s_reg_sub64 = s_reg_extden = 0;
}

void profile_shutdown(void)
{
    if (s_thread) {
        InterlockedExchange(&s_stop, 1);
        WaitForSingleObject(s_thread, 500);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    if (s_target) {
        CloseHandle(s_target);
        s_target = NULL;
    }
}
