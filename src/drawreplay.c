/* drawreplay.c -- in-between pictures: the game's own draw on a blended state.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * [graphics] frame_rate = 60 presents a picture between each two of the game's
 * 30 Hz ticks without touching the simulation. After tick N+1's work pass, the
 * game's memory is snapshotted, blended word by word toward tick N's (the
 * state blend), and the game's own draw runs on it -- Hydro's draw function,
 * Offroad's inline draw phase replayed as a script derived from the binary.
 * Then every page that draw wrote is put back and the real picture is drawn
 * half a tick later. The draw cannot simply be called twice: it consumes
 * per-frame lists the work pass fills. After a restore it is the same draw,
 * byte for byte in memory and pixel for pixel on both backends.
 *
 * The game's memory is its image DATA/.bss plus every region it committed with
 * VirtualAlloc and every heap it made with HeapCreate (win32_bind.c records
 * both); reservations are write-watched so a snapshot copies only what changed.
 * The stack is not restored; nothing on it outlives the draw.
 *
 * An in-between draw must not reach outside that memory, because nothing else
 * can be restored: sound is dropped, texture uploads journalled by the
 * provider, frees held, the DCS and board threads held out, the game's
 * CRITICAL_SECTIONs preserved, and a movie open abandons the picture. A fault
 * in the game's code, or a camera cut, abandons it too. What must not be
 * blended is not: pools whose members never move, ranges _mesh3d_SlideST steps,
 * and effect records whose slot changed owner between ticks, which are matched
 * by identity and redrawn. A frame budget on the limiter's own clock skips the
 * in-between when the real picture would be late.
 *
 * The same machinery without the blend is a diagnostic: [diagnostics]
 * draw_replay draws twice from one restored state and compares the memory and
 * the call counts (draw_replay_control: the same window with no draws, the
 * other threads' noise floor).
 */
#include "vcthunder.h"
#include "glide_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define DR_CHUNK     4096u
#define DR_MAX_RNG   4096
#define DR_MAX_CELLS 1024
#define DR_MAX_VALLOC 1024
/* Longer than a replay's window, so the control over-states the noise. */
#define DR_CONTROL_MS 10.0

typedef void (__cdecl *drawfn_t)(int);
typedef void (__stdcall *state_fn)(void *);
typedef void (__stdcall *swap_fn)(int);

typedef struct { uintptr_t lo, hi; } rng_t;

static uint64_t s_cuts;            /* in-between pictures skipped at a cut */
static float    s_cam_prev[24];    /* tick N's camera transform, after its real draw */
static int      s_cam_prev_ok, s_cut_checked;
static void abort_cut(void);

static drawfn_t  s_game_draw;
static state_fn  s_get_state, s_set_state;
static swap_fn   s_swap;
typedef void (__stdcall *spec_fn)(void);
static spec_fn   s_spec_begin, s_spec_end;   /* the provider's texture journal */

static void spec_begin(void) { if (s_spec_begin) s_spec_begin(); }
static void spec_end(void)   { if (s_spec_end) s_spec_end(); }
static uint32_t  s_state_blob[1024];

static rng_t     s_rng[DR_MAX_RNG];
static unsigned  s_nrng;
static size_t    s_total;
static unsigned char *s_dirty;       /* one flag per chunk of the concatenation */
static size_t    s_dirty_cap;

static unsigned  s_skip, s_budget;
static int       s_control, s_reported, s_interp;

static uint64_t  s_samples, s_state_differs, s_calls_differ;
static uint64_t  s_dirty_chunks, s_diff_dwords, s_diff_outside;
static uint64_t  s_bytes_sum;
static double    s_ms_copy, s_ms_draw1, s_ms_scan, s_ms_restore, s_ms_draw2;
static uint32_t  s_cell[DR_MAX_CELLS], s_cell_hits[DR_MAX_CELLS];
static uint8_t   s_cell_in_dirty[DR_MAX_CELLS];
static unsigned  s_cells, s_cell_overflow;
static uint32_t  s_entry_hits[VCT_MAX_GLIDE];
static uint64_t  s_entry_d1[VCT_MAX_GLIDE], s_entry_d2[VCT_MAX_GLIDE];

static double qpc_ms(uint64_t t)
{
    static LARGE_INTEGER f;

    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    return (double)t * 1000.0 / (double)f.QuadPart;
}

static double now_ms(void)
{
    LARGE_INTEGER t;

    QueryPerformanceCounter(&t);
    return qpc_ms((uint64_t)t.QuadPart);
}

/* ---- [diagnostics] draw_interp_profile ---------------------------------------
 *
 * What 60 Hz's candidate speed-ups would act on, as distributions per report:
 * build_ranges split into its VirtualQuery and HeapWalk halves; the blend's
 * word loop, and how often each check in it runs and could be skipped; and
 * the spacing of every present. Off unless asked: it times the word loop per
 * chunk, and its counting pass runs outside that timing. */
#define PR_MAX 2048
typedef struct { double v[PR_MAX]; unsigned n, over; } pr_set_t;

static int      s_prof;
static double   s_prof_pair_ms;        /* one now_ms() pair, mean         */
static pr_set_t s_pr_br, s_pr_snap, s_pr_words, s_pr_phase, s_pr_inbswap,
                s_pr_realswap, s_pr_gap[4], s_pr_hold, s_pr_draw,
                s_pr_wait, s_pr_asleep, s_pr_overshoot, s_pr_held;
static double   s_pr_br_vq, s_pr_br_heap, s_pr_word_acc;
static double   s_pr_last_present;
static int      s_pr_last_kind = -1;   /* 0 real, 1 in-between            */
static struct {
    uint64_t br_calls, vq_calls, heap_entries, ranges, vq_cached, vq_mismatch;
    uint64_t ticks, chunks, timed, changed;
    uint64_t ex_chunks, ex_free_chunks, ex_calls, ex_hits;
    uint64_t nb_calls, nb_hits, not_float;
    uint64_t ga_calls, ga_image, ga_outside, ga_true, disagree;
} s_pc;

static void pr_add(pr_set_t *s, double v)
{
    if (s->n < PR_MAX)
        s->v[s->n++] = v;
    else
        s->over++;
}

static void rng_add(uintptr_t lo, uintptr_t hi)
{
    if (hi <= lo || s_nrng >= DR_MAX_RNG)
        return;
    s_rng[s_nrng].lo = lo;
    s_rng[s_nrng].hi = hi;
    s_nrng++;
}

/* The committed, writable pages inside [lo, hi). */
static void add_committed(uintptr_t lo, uintptr_t hi)
{
    uintptr_t p = lo;

    while (p < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t end;
        DWORD prot;

        s_pc.vq_calls++;
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi))
            return;
        end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        prot = mbi.Protect & 0xFFu;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE ||
             prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY))
            rng_add(p, end < hi ? end : hi);
        p = end;
    }
}

static int rng_cmp(const void *a, const void *b)
{
    const rng_t *x = (const rng_t *)a, *y = (const rng_t *)b;

    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* ---- the pages written since a snapshot ---------------------------------
 *
 * Most of the game's memory is VirtualAlloc reservations, and win32_bind.c
 * makes them with MEM_WRITE_WATCH. Every poll stamps the pages written since
 * the poll before with the current epoch, and a buffer filled at epoch E
 * already holds every page whose stamp is below E, so a snapshot copies and a
 * scan compares only the rest. The image's DATA/.bss and the heaps are not
 * watched and are copied and compared whole. Nothing is trusted across a
 * change to the game's VM or to the range list: `s_layout` moves, and every
 * buffer filled under the old one is filled whole again. */
typedef struct { uintptr_t lo, hi; uint32_t *stamp; } ww_t;

static ww_t      s_ww[DR_MAX_VALLOC];   /* sorted by address                */
static unsigned  s_nww;
static uint32_t  s_ww_vgen, s_ww_built;
static PVOID    *s_ww_addr;
static ULONG_PTR s_ww_addr_cap;
static uint32_t  s_epoch = 1;
static uint32_t  s_layout = 1;
static uint32_t  s_layout_vgen;
static rng_t     s_layout_rng[DR_MAX_RNG];
static unsigned  s_layout_nrng;
static int       s_ww_failed, s_verify;
static uint64_t  s_layout_changes, s_ww_copied, s_ww_compared, s_ww_pages;
static double    s_ww_ms_poll;
static double now_ms(void);
static uint64_t  s_ww_missed_snap, s_ww_missed_scan;

typedef struct {
    unsigned char *p;
    size_t   cap;
    uint32_t epoch;                     /* 0: holds nothing to build on      */
    uint32_t layout;
} snap_t;

/* s_before is this tick's snapshot, s_prev tick N's (60 Hz only) and s_after
 * draw #1's result (the replay gate only). */
static snap_t    s_before, s_after, s_prev;

static int ww_cmp(const void *a, const void *b)
{
    const ww_t *x = (const ww_t *)a, *y = (const ww_t *)b;

    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static void ww_rebuild(const uintptr_t *base, const size_t *size,
                       const unsigned char *watched, unsigned n, uint32_t vgen)
{
    unsigned i;
    ULONG_PTR most = 0;

    for (i = 0; i < s_nww; i++)
        free(s_ww[i].stamp);
    s_nww = 0;
    s_ww_vgen = vgen;
    s_ww_built = 1;
    for (i = 0; i < n; i++) {
        size_t pages = size[i] >> 12;

        if (!watched[i] || !pages)
            continue;
        s_ww[s_nww].lo = base[i];
        s_ww[s_nww].hi = base[i] + (pages << 12);
        s_ww[s_nww].stamp = (uint32_t *)calloc(pages, sizeof(uint32_t));
        if (!s_ww[s_nww].stamp)
            continue;
        if (pages > most)
            most = pages;
        s_nww++;
    }
    qsort(s_ww, s_nww, sizeof s_ww[0], ww_cmp);
    if (most > s_ww_addr_cap) {
        free(s_ww_addr);
        s_ww_addr = (PVOID *)malloc(most * sizeof *s_ww_addr);
        s_ww_addr_cap = s_ww_addr ? most : 0;
    }
    if (!s_ww_addr) {
        for (i = 0; i < s_nww; i++)
            free(s_ww[i].stamp);
        s_nww = 0;
    }
}

/* The stamps hold only while the game's VM is the VM they were built for. */
static int ww_ok(void)
{
    return s_nww && win32_game_vgen() == s_ww_vgen;
}

/* Stamp every page written since the last poll with the current epoch. */
static void ww_poll(void)
{
    unsigned i;
    double t = now_ms();

    for (i = 0; i < s_nww; i++) {
        ULONG_PTR n = s_ww_addr_cap, k;
        ULONG gran;
        size_t pages = (s_ww[i].hi - s_ww[i].lo) >> 12;

        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (PVOID)s_ww[i].lo,
                          s_ww[i].hi - s_ww[i].lo, s_ww_addr, &n, &gran) ||
            gran != 0x1000) {
            if (!s_ww_failed) {
                s_ww_failed = 1;
                LOGW("drawreplay: GetWriteWatch refused [0x%08X, 0x%08X) "
                     "(error %lu); it is compared whole from now on",
                     (unsigned)s_ww[i].lo, (unsigned)s_ww[i].hi, GetLastError());
            }
            for (k = 0; k < pages; k++)
                s_ww[i].stamp[k] = ~0u;
            continue;
        }
        for (k = 0; k < n; k++)
            s_ww[i].stamp[((uintptr_t)s_ww_addr[k] - s_ww[i].lo) >> 12] = s_epoch;
    }
    s_ww_ms_poll += now_ms() - t;
}

/* Every page of [a, a+n) is watched and unwritten since epoch `e`. `cur`
 * walks s_ww and only ever forward, so a pass visits chunks in address order. */
static int ww_clean(uintptr_t a, size_t n, uint32_t e, unsigned *cur)
{
    const ww_t *w;
    uintptr_t p;

    while (*cur < s_nww && s_ww[*cur].hi <= a)
        (*cur)++;
    if (*cur >= s_nww)
        return 0;
    w = &s_ww[*cur];
    if (a < w->lo || a + n > w->hi)
        return 0;
    for (p = a & ~(uintptr_t)0xFFF; p < a + n; p += 0x1000)
        if (w->stamp[(p - w->lo) >> 12] >= e)
            return 0;
    return 1;
}

/* A buffer whose pages written since it was filled are all it lacks. */
static int snap_current(const snap_t *b)
{
    return b->p && b->epoch && b->layout == s_layout && ww_ok();
}

static int snap_reserve(snap_t *b)
{
    if (b->p && b->cap >= s_total)
        return 1;
    VirtualFree(b->p, 0, MEM_RELEASE);
    b->cap = s_total + (s_total >> 2);
    b->epoch = 0;
    b->p = (unsigned char *)VirtualAlloc(NULL, b->cap, MEM_RESERVE | MEM_COMMIT,
                                         PAGE_READWRITE);
    if (!b->p) {
        LOGE("drawreplay: cannot reserve %u KB for a snapshot",
             (unsigned)(b->cap / 1024));
        b->cap = 0;
        return 0;
    }
    return 1;
}

/* The committed, writable pages of the game's VirtualAlloc reservations.
 * They change only through our VirtualAlloc, VirtualFree and VirtualProtect,
 * each of which moves win32_game_vgen(), so the walk is kept until it moves.
 * The heaps grow inside Windows, where nothing tells us, and are walked every
 * time. The replay gate's page watch protects pages behind the binding's
 * back, and turns the cache off. */
static rng_t    s_vq_rng[DR_MAX_RNG];
static unsigned s_vq_n;
static uint32_t s_vq_vgen;
static int      s_vq_ok;

static void vq_walk(const uintptr_t *base, const size_t *size, unsigned nv)
{
    unsigned i;

    for (i = 0; i < nv; i++)
        add_committed(base[i], base[i] + size[i]);
}

/* Rebuilt every sample: the game allocates as it goes. */
static int build_ranges(void)
{
    static uintptr_t vbase[DR_MAX_VALLOC];
    static size_t vsize[DR_MAX_VALLOC];
    static unsigned char vww[DR_MAX_VALLOC];
    static unsigned nv;
    unsigned i, n, k, n0;
    uint32_t vgen;
    double t0 = 0.0, t1 = 0.0;

    /* A restore that misses some of the game's memory lets an in-between
     * picture change the game; win32_bind.c has said why already. */
    if (win32_game_memory_lost())
        return 0;
    if (s_prof)
        t0 = now_ms();
    s_nrng = 0;
    rng_add(G->data_va, G->stack_va);
    n0 = s_nrng;
    vgen = win32_game_vgen();
    if (s_vq_ok && vgen == s_vq_vgen &&
        !(g_cfg.draw_replay_watch && g_cfg.draw_replay_watch_pages)) {
        memcpy(&s_rng[n0], s_vq_rng, s_vq_n * sizeof s_rng[0]);
        s_nrng = n0 + s_vq_n;
        if (s_prof) {
            /* The walk the cache stands for, untimed: any difference is a
             * change to the reservations that did not move vgen. */
            uint64_t q = s_pc.vq_calls;

            t1 = now_ms();
            vq_walk(vbase, vsize, nv);
            s_pc.vq_calls = q;
            if (s_nrng - n0 - s_vq_n != s_vq_n ||
                memcmp(&s_rng[n0 + s_vq_n], s_vq_rng, s_vq_n * sizeof s_rng[0]))
                s_pc.vq_mismatch++;
            s_nrng = n0 + s_vq_n;
            t0 += now_ms() - t1;
            s_pc.vq_cached++;
        }
    } else {
        nv = win32_game_vallocs(vbase, vsize, vww, DR_MAX_VALLOC);
        vq_walk(vbase, vsize, nv);
        s_vq_n = s_nrng - n0;
        memcpy(s_vq_rng, &s_rng[n0], s_vq_n * sizeof s_rng[0]);
        s_vq_vgen = vgen;
        s_vq_ok = s_nrng < DR_MAX_RNG;
    }
    if (s_prof)
        t1 = now_ms();
    for (i = 0, n = win32_game_heaps(); i < n; i++) {
        HANDLE h = (HANDLE)win32_game_heap(i);
        PROCESS_HEAP_ENTRY e;
        uintptr_t rlo = 0, rhi = 0;

        if (!h || !HeapLock(h))
            continue;
        memset(&e, 0, sizeof e);
        while (HeapWalk(h, &e)) {
            uintptr_t d = (uintptr_t)e.lpData;

            s_pc.heap_entries++;
            if (e.wFlags & PROCESS_HEAP_REGION) {
                rlo = d;
                rhi = (uintptr_t)e.Region.lpLastBlock;
                add_committed(rlo, rhi);
            } else if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) &&
                       (d < rlo || d >= rhi)) {
                add_committed(d, d + e.cbData);   /* a large block of its own */
            }
        }
        HeapUnlock(h);
    }
    if (s_nrng >= DR_MAX_RNG) {
        LOGE("drawreplay: more than %d memory ranges; refusing to sample",
             DR_MAX_RNG);
        return 0;
    }
    qsort(s_rng, s_nrng, sizeof s_rng[0], rng_cmp);
    for (i = 0, k = 0; i < s_nrng; i++) {
        if (k && s_rng[i].lo <= s_rng[k - 1].hi) {
            if (s_rng[i].hi > s_rng[k - 1].hi)
                s_rng[k - 1].hi = s_rng[i].hi;
        } else {
            s_rng[k++] = s_rng[i];
        }
    }
    s_nrng = k;
    if (s_prof) {
        double t2 = now_ms();

        s_pr_br_vq += t1 - t0;
        s_pr_br_heap += t2 - t1;
        pr_add(&s_pr_br, t2 - t0);
        s_pc.br_calls++;
        s_pc.ranges += s_nrng;
    }
    for (i = 0, s_total = 0; i < s_nrng; i++)
        s_total += s_rng[i].hi - s_rng[i].lo;
    if (!s_ww_built || vgen != s_ww_vgen)
        ww_rebuild(vbase, vsize, vww, nv, vgen);
    if (vgen != s_layout_vgen || s_nrng != s_layout_nrng ||
        memcmp(s_rng, s_layout_rng, s_nrng * sizeof s_rng[0])) {
        s_layout++;
        s_layout_changes++;
        s_layout_vgen = vgen;
        s_layout_nrng = s_nrng;
        memcpy(s_layout_rng, s_rng, s_nrng * sizeof s_rng[0]);
        for (i = 0, k = 0, s_ww_pages = 0; i < s_nrng; i++) {
            uintptr_t a;

            for (a = s_rng[i].lo; a < s_rng[i].hi; a += DR_CHUNK)
                s_ww_pages += (uint64_t)ww_clean(a, s_rng[i].hi - a < DR_CHUNK ?
                                  s_rng[i].hi - a : DR_CHUNK, ~0u, &k);
        }
    }
    /* 60 Hz never compares with draw #1's result: no s_after, which would
     * be another snapshot's worth of a 32-bit address space. */
    if (!snap_reserve(&s_before) || (!s_interp && !snap_reserve(&s_after)))
        return 0;
    if (s_total / DR_CHUNK + s_nrng > s_dirty_cap) {
        free(s_dirty);
        s_dirty_cap = s_total / DR_CHUNK + s_nrng + 1024;
        s_dirty = (unsigned char *)malloc(s_dirty_cap);
        if (!s_dirty)
            return 0;
    }
    return 1;
}

/* Fill `b` with live memory: whole, or only the pages written since it was
 * last filled. The replay gate (s_verify) compares every page it skipped. */
static void snapshot(snap_t *b)
{
    unsigned i, cur = 0;
    size_t off = 0;
    int inc = snap_current(b);

    if (s_nww)
        ww_poll();
    for (i = 0; i < s_nrng; i++) {
        uintptr_t a = s_rng[i].lo;

        if (!inc) {
            memcpy(b->p + off, (const void *)a, s_rng[i].hi - a);
            off += s_rng[i].hi - a;
            s_ww_copied += (s_rng[i].hi - a + DR_CHUNK - 1) / DR_CHUNK;
            continue;
        }
        while (a < s_rng[i].hi) {
            size_t n = s_rng[i].hi - a < DR_CHUNK ? s_rng[i].hi - a : DR_CHUNK;

            if (!ww_clean(a, n, b->epoch, &cur)) {
                memcpy(b->p + off, (const void *)a, n);
                s_ww_copied++;
            } else if (s_verify && memcmp(b->p + off, (const void *)a, n)) {
                s_ww_missed_snap++;
                memcpy(b->p + off, (const void *)a, n);
            }
            a += n;
            off += n;
        }
    }
    b->epoch = ++s_epoch;
    b->layout = s_layout;
}

/* Flag every chunk of live memory that differs from `b`; return how many. */
static size_t mark_dirty(const snap_t *b)
{
    unsigned i, cur = 0;
    size_t off = 0, c = 0, flagged = 0;
    int inc = snap_current(b);

    if (inc)
        ww_poll();
    for (i = 0; i < s_nrng; i++) {
        uintptr_t a = s_rng[i].lo;

        while (a < s_rng[i].hi) {
            size_t n = s_rng[i].hi - a < DR_CHUNK ? s_rng[i].hi - a : DR_CHUNK;
            int d;

            if (inc && ww_clean(a, n, b->epoch, &cur)) {
                d = s_verify && memcmp((const void *)a, b->p + off, n) != 0;
                s_ww_missed_scan += (uint64_t)d;
            } else {
                d = memcmp((const void *)a, b->p + off, n) != 0;
                s_ww_compared++;
            }
            s_dirty[c++] = (unsigned char)d;
            flagged += (size_t)d;
            a += n;
            off += n;
        }
    }
    return flagged;
}

/* The game's CRITICAL_SECTIONs are Windows' lock state, not the game's: a
 * restore keeps them exactly as they are now (win32_bind.c records them). */
static CRITICAL_SECTION s_cs_keep[256];

static void restore_dirty_chunks(const unsigned char *buf);

static void restore_dirty(const unsigned char *buf)
{
    unsigned i, n = win32_game_css();

    if (n > 256)
        n = 256;
    for (i = 0; i < n; i++)
        memcpy(&s_cs_keep[i], win32_game_cs(i), sizeof s_cs_keep[i]);
    restore_dirty_chunks(buf);
    for (i = 0; i < n; i++)
        memcpy(win32_game_cs(i), &s_cs_keep[i], sizeof s_cs_keep[i]);
}

static void restore_dirty_chunks(const unsigned char *buf)
{
    unsigned i;
    size_t off = 0, c = 0;

    for (i = 0; i < s_nrng; i++) {
        uintptr_t a = s_rng[i].lo;

        while (a < s_rng[i].hi) {
            size_t n = s_rng[i].hi - a < DR_CHUNK ? s_rng[i].hi - a : DR_CHUNK;

            if (s_dirty[c++])
                memcpy((void *)a, buf + off, n);
            a += n;
            off += n;
        }
    }
}

static void note_cell(uint32_t va, int in_dirty)
{
    unsigned i;

    for (i = 0; i < s_cells; i++)
        if (s_cell[i] == va) {
            s_cell_hits[i]++;
            return;
        }
    if (s_cells >= DR_MAX_CELLS) {
        s_cell_overflow++;
        return;
    }
    s_cell[s_cells] = va;
    s_cell_hits[s_cells] = 1;
    s_cell_in_dirty[s_cells++] = (uint8_t)in_dirty;
}

/* Every dword of live memory that differs from `buf`. */
static size_t diff_dwords(const unsigned char *buf)
{
    unsigned i;
    size_t off = 0, c = 0, diffs = 0;

    for (i = 0; i < s_nrng; i++) {
        uintptr_t a = s_rng[i].lo;

        while (a < s_rng[i].hi) {
            size_t n = s_rng[i].hi - a < DR_CHUNK ? s_rng[i].hi - a : DR_CHUNK;

            if (memcmp((const void *)a, buf + off, n) != 0) {
                size_t k;

                for (k = 0; k + 4 <= n; k += 4)
                    if (memcmp((const void *)(a + k), buf + off + k, 4) != 0) {
                        diffs++;
                        if (!s_dirty[c])
                            s_diff_outside++;
                        note_cell((uint32_t)(a + k), s_dirty[c]);
                    }
            }
            c++;
            a += n;
            off += n;
        }
    }
    return diffs;
}

/* Who writes inside the window: the pages [s_watch_lo, s_watch_hi) are made
 * read-only for its duration and the first writers are recorded (thread,
 * instruction, address). A diagnostic of a diagnostic, off unless asked. */
#define DW_MAX 32
static uintptr_t s_watch_lo, s_watch_hi;
static volatile LONG s_dw_n, s_watch_on;
static struct { DWORD tid; uint32_t eip, addr; } s_dw[DW_MAX];

static LONG CALLBACK dw_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uintptr_t a;
    DWORD old;
    LONG i;

    if (!s_watch_on || er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    a = (uintptr_t)er->ExceptionInformation[1];
    if (a < s_watch_lo || a >= s_watch_hi)
        return EXCEPTION_CONTINUE_SEARCH;
    i = InterlockedIncrement(&s_dw_n) - 1;
    if (i < DW_MAX) {
        s_dw[i].tid = GetCurrentThreadId();
        s_dw[i].eip = (uint32_t)ep->ContextRecord->Eip;
        s_dw[i].addr = (uint32_t)a;
    }
    VirtualProtect((void *)(a & ~(uintptr_t)0xFFF), 0x1000, PAGE_EXECUTE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch(int on)
{
    DWORD old;

    if (!s_watch_lo)
        return;
    if (on) {
        s_watch_on = 1;
        VirtualProtect((void *)s_watch_lo, s_watch_hi - s_watch_lo, PAGE_READONLY, &old);
    } else {
        VirtualProtect((void *)s_watch_lo, s_watch_hi - s_watch_lo, PAGE_EXECUTE_READWRITE, &old);
        s_watch_on = 0;
    }
}

static void watch_report(void)
{
    LONG i, n = s_dw_n < DW_MAX ? s_dw_n : DW_MAX;

    if (!s_watch_lo)
        return;
    LOGI("drawreplay: %ld write(s) to [0x%08X, 0x%08X) inside windows; the "
         "first %ld (game thread %lu):", (long)s_dw_n, (unsigned)s_watch_lo,
         (unsigned)s_watch_hi, (long)n, (unsigned long)GetCurrentThreadId());
    for (i = 0; i < n; i++)
        LOGI("    thread %lu  eip 0x%08X  wrote 0x%08X",
             (unsigned long)s_dw[i].tid, s_dw[i].eip, s_dw[i].addr);
}

static void report(void)
{
    unsigned i, g, gn = (unsigned)G->glide_count;
    double n = s_samples ? (double)s_samples : 1.0;

    if (s_reported)
        return;
    s_reported = 1;
    watch_report();
    if (gn > VCT_MAX_GLIDE)
        gn = VCT_MAX_GLIDE;
    LOGI("drawreplay: %s, %llu samples over %u ranges, %u KB of game memory",
         s_control ? "CONTROL (no draws; the other threads' noise)" :
                     "draw, restore what it wrote, draw again",
         (unsigned long long)s_samples, s_nrng, (unsigned)(s_total / 1024));
    if (!s_samples)
        return;
    LOGI("drawreplay: STATE -- %llu of %llu samples ended with memory "
         "differing (%.1f%%); %llu differing dwords, %llu of them outside "
         "the chunks draw #1 wrote; %u distinct cells%s",
         (unsigned long long)s_state_differs, (unsigned long long)s_samples,
         100.0 * (double)s_state_differs / n,
         (unsigned long long)s_diff_dwords,
         (unsigned long long)s_diff_outside, s_cells,
         s_cell_overflow ? " (LIST FULL)" : "");
    LOGI("drawreplay: CALLS -- %llu of %llu samples issued a different Glide "
         "call mix on draw #2", (unsigned long long)s_calls_differ,
         (unsigned long long)s_samples);
    LOGI("drawreplay: COST -- draw #1 wrote %.1f chunks of %u bytes on "
         "average (%.0f KB); ms per sample: snapshot %.2f, draw #1 %.2f, "
         "scan %.2f, restore %.2f, draw #2 %.2f",
         (double)s_dirty_chunks / n, DR_CHUNK,
         (double)s_dirty_chunks * DR_CHUNK / n / 1024.0,
         s_ms_copy / n, s_ms_draw1 / n, s_ms_scan / n, s_ms_restore / n,
         s_ms_draw2 / n);
    for (g = 0; g < gn; g++)
        if (s_entry_hits[g])
            LOGI("    glide %-28s differed on %u/%llu samples: %llu calls in "
                 "draw #1, %llu in draw #2", G->glide[g].name,
                 (unsigned)s_entry_hits[g], (unsigned long long)s_samples,
                 (unsigned long long)s_entry_d1[g],
                 (unsigned long long)s_entry_d2[g]);
    for (i = 0; i < s_cells; i++)
        LOGI("    0x%08X  %u/%llu samples  %s", s_cell[i],
             (unsigned)s_cell_hits[i], (unsigned long long)s_samples,
             s_cell_in_dirty[i] ? "in a chunk draw #1 wrote" :
                                  "OUTSIDE draw #1's chunks");
    if (s_verify)
        LOGI("drawreplay: WRITE-WATCH -- %u watched reservation(s); %llu "
             "chunk(s) it called unwritten differed from the snapshot (%llu "
             "when taking one, %llu after draw #1); 0 is the pass; %llu "
             "range-list change(s) refilled the buffers whole",
             s_nww, (unsigned long long)(s_ww_missed_snap + s_ww_missed_scan),
             (unsigned long long)s_ww_missed_snap,
             (unsigned long long)s_ww_missed_scan,
             (unsigned long long)s_layout_changes);
    if (!s_control && !s_state_differs && !s_calls_differ)
        LOGI("drawreplay: REPLAY HOLDS. Every draw #2 left the game's memory "
             "byte-identical to draw #1 and issued the same call mix.");
}

static void sample_control(int parm)
{
    double t0, t;
    size_t d;

    snapshot(&s_before);
    t0 = now_ms();
    while (now_ms() - t0 < DR_CONTROL_MS)
        YieldProcessor();
    t = now_ms();
    memset(s_dirty, 0, s_total / DR_CHUNK + s_nrng);
    d = diff_dwords(s_before.p);
    s_ms_scan += now_ms() - t;
    s_diff_dwords += d;
    if (d)
        s_state_differs++;
    s_game_draw(parm);
}

static void sample_replay(int parm)
{
    const uint32_t *counts = glide_call_counts();
    uint32_t c0[VCT_MAX_GLIDE], c1[VCT_MAX_GLIDE];
    unsigned g, gn = (unsigned)G->glide_count;
    double t0, t1, t2, t3, t4, t5;
    size_t dirty, d;
    int calls = 0;

    if (gn > VCT_MAX_GLIDE)
        gn = VCT_MAX_GLIDE;
    t0 = now_ms();
    game_hold_side_threads(1);
    win32_hold_frees(1);
    audio_hold_this_thread(1);
    snapshot(&s_before);
    s_get_state(s_state_blob);
    memcpy(c0, counts, gn * sizeof c0[0]);
    t1 = now_ms();
    watch(1);
    spec_begin();
    s_game_draw(parm);                                   /* draw #1          */
    watch(0);
    t2 = now_ms();
    memcpy(c1, counts, gn * sizeof c1[0]);
    dirty = mark_dirty(&s_before);
    snapshot(&s_after);
    t3 = now_ms();
    s_swap(0);                         /* the capture's boundary between them */
    spec_end();
    restore_dirty(s_before.p);
    s_set_state(s_state_blob);
    t4 = now_ms();
    watch(1);
    s_game_draw(parm);                                   /* draw #2          */
    watch(0);
    t5 = now_ms();

    d = diff_dwords(s_after.p);
    win32_hold_frees(0);
    audio_hold_this_thread(0);
    game_hold_side_threads(0);
    for (g = 0; g < gn; g++) {
        uint32_t d1 = c1[g] - c0[g], d2 = counts[g] - c1[g];

        if (d1 != d2) {
            calls = 1;
            s_entry_hits[g]++;
            s_entry_d1[g] += d1;
            s_entry_d2[g] += d2;
        }
    }
    s_dirty_chunks += dirty;
    s_bytes_sum += s_total;
    s_diff_dwords += d;
    if (d)
        s_state_differs++;
    if (calls)
        s_calls_differ++;
    s_ms_copy += t1 - t0;
    s_ms_draw1 += t2 - t1;
    s_ms_scan += t3 - t2;
    s_ms_restore += t4 - t3;
    s_ms_draw2 += t5 - t4;
    if (s_samples < 3)
        LOGI("drawreplay: sample %llu: %u ranges, %u KB; draw #1 wrote %u "
             "chunks; %u dwords differ after draw #2; call mix %s",
             (unsigned long long)s_samples, s_nrng, (unsigned)(s_total / 1024),
             (unsigned)dirty, (unsigned)d, calls ? "DIFFERS" : "same");
}

/* ---- interpolation ------------------------------------------------------------
 *
 * Every object matrix reaches the transform stack through _xfm_Push and every
 * camera through _xfm_InitCameraRot. A draw run on tick N+1's state therefore
 * passes N+1's values as ARGUMENTS, and the only thing missing for a blend is
 * tick N's value of the same argument. The real draw of every tick records
 * them, keyed by call site and by occurrence of that call site within the
 * draw; the in-between draw blends each argument with its key's record from
 * the tick before. A key with no record, a changed transform type or a jump
 * longer than DI_JUMP is drawn unblended, as the cabinet would have drawn it. */
typedef void (__cdecl *push_fn)(const void *);
typedef void (__cdecl *camrot_fn)(void *, uint32_t, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t);

#define DI_SLOTS  16384u              /* power of two                         */
#define DI_OCC    2048u               /* power of two                         */
#define DI_WORDS  27u                 /* a transform: 26 floats + its type    */
#define DI_JUMP   64.0f               /* world units in one tick: a teleport  */
#define DI_ROT    0.35f               /* rotation element change: about 20 deg */
#define DI_ANGLE  4096                /* 16-bit angle change: 22.5 deg         */

typedef struct { uint32_t gen, ret, occ, v[DI_WORDS]; } di_slot_t;

static push_fn   s_orig_push;
static camrot_fn s_orig_camrot;
static di_slot_t *s_tab[2];
static uint32_t  s_tab_gen[2] = { 1, 1 };
static int       s_rec;                /* index of the table being recorded */
static int       s_have_prev;
static struct { uint32_t gen, ret, n; } s_occ[DI_OCC];
static uint32_t  s_occ_gen = 1;
static int       s_ip_mode;            /* 0 pass, 1 record, 2 blend         */
static float     s_alpha = 0.5f;
static uint64_t  s_ip_ticks, s_ip_blend, s_ip_cam, s_ip_nokey, s_ip_reject;
static uint64_t  s_ip_byaddr, s_ip_bystack, s_ip_rotref;

/* Per call site, cumulative: how it keys and what becomes of its blends. */
#define DI_SITES 512
static struct { uint32_t ret, n, stack, found, nokey, jump, rot; } s_site[DI_SITES];
static unsigned s_sites;
static int s_site_cur;

static void site_note(uint32_t ret)
{
    unsigned i;

    for (i = 0; i < s_sites; i++)
        if (s_site[i].ret == ret) {
            s_site_cur = (int)i;
            return;
        }
    if (s_sites < DI_SITES) {
        s_site[s_sites].ret = ret;
        s_site_cur = (int)s_sites++;
    } else {
        s_site_cur = -1;
    }
}
#define SITE(f) do { if (s_site_cur >= 0) s_site[s_site_cur].f++; } while (0)
static double    s_ip_ms_present, s_ip_ms_total, s_ip_late;
static double    s_ip_ms_snap, s_ip_ms_restore;

/* What the snapshot and the restore cost over the last 300 ticks. */
static void ww_log(void)
{
    LOGI("drawinterp: per tick, snapshot %.2f ms and restore scan %.2f ms, "
         "%.2f ms of it write-watch polls; %.0f chunks copied, %.0f compared, "
         "of %u (%u of them in %u watched reservation(s)); %llu range-list "
         "change(s) refilled the buffers",
         s_ip_ms_snap / 300.0, s_ip_ms_restore / 300.0, s_ww_ms_poll / 300.0,
         (double)s_ww_copied / 300.0, (double)s_ww_compared / 300.0,
         (unsigned)((s_total + DR_CHUNK - 1) / DR_CHUNK), (unsigned)s_ww_pages,
         s_nww, (unsigned long long)s_layout_changes);
    s_ip_ms_snap = s_ip_ms_restore = s_ww_ms_poll = 0.0;
    s_ww_copied = s_ww_compared = 0;
    s_layout_changes = 0;
}
static float     s_ip_maxjump;

static uint32_t occ_next(uint32_t ret)
{
    uint32_t h = (ret * 2654435761u) >> 21;

    for (;;) {
        h &= DI_OCC - 1;
        if (s_occ[h].gen != s_occ_gen) {
            s_occ[h].gen = s_occ_gen;
            s_occ[h].ret = ret;
            s_occ[h].n = 0;
        }
        if (s_occ[h].ret == ret)
            return s_occ[h].n++;
        h++;
    }
}

static di_slot_t *di_find(int t, uint32_t ret, uint32_t occ, int create)
{
    uint32_t h = ((ret * 2654435761u) ^ (occ * 40503u)) >> 18, probes;
    di_slot_t *tab = s_tab[t];

    for (probes = 0; probes < DI_SLOTS; probes++, h++) {
        di_slot_t *sl = &tab[h & (DI_SLOTS - 1)];

        if (sl->gen != s_tab_gen[t]) {
            if (!create)
                return NULL;
            sl->gen = s_tab_gen[t];
            sl->ret = ret;
            sl->occ = occ;
            return sl;
        }
        if (sl->ret == ret && sl->occ == occ)
            return sl;
    }
    return NULL;
}

static float lerpf(float a, float b) { return a + (b - a) * s_alpha; }

/* A rotation that changes this much in one tick is a cut or another object.
 * Relative to the matrix's own scale: a model drawn at ten times its size has
 * elements ten times larger, and turns them ten times as far per degree. */
static int rotation_plausible(const float *p, const float *c)
{
    float scale = 0.0f;
    unsigned i;

    for (i = 0; i < 9; i++)
        if (fabsf(p[i]) > scale)
            scale = fabsf(p[i]);
    for (i = 0; i < 9; i++)
        if (fabsf(c[i] - p[i]) > DI_ROT * scale)
            return 0;
    return 1;
}

/* A tick's pushes, grouped by call site: an array of records and, per site, a
 * chain through it. Pairing is by nearest match within the site, because no
 * identity survives a tick: stack temporaries have none, scratch addresses
 * are reused by other objects, and turn order shifts whenever an object is
 * culled in or out. Objects move less in a tick than they are apart. */
#define DI_RECS  8192u
#define DI_HEADS 1024u
typedef struct { uint32_t ret, next, used; uint32_t v[DI_WORDS]; } di_rec_t;
static di_rec_t *s_recs[2];
static uint32_t  s_nrecs[2];
static struct { uint32_t gen, ret, head; } s_head[2][DI_HEADS];

static uint32_t *head_of(int t, uint32_t ret, int create)
{
    uint32_t h = (ret * 2654435761u) >> 22, i;

    for (i = 0; i < DI_HEADS; i++, h++) {
        h &= DI_HEADS - 1;
        if (s_head[t][h].gen != s_tab_gen[t]) {
            if (!create)
                return NULL;
            s_head[t][h].gen = s_tab_gen[t];
            s_head[t][h].ret = ret;
            s_head[t][h].head = 0xFFFFFFFFu;
            return &s_head[t][h].head;
        }
        if (s_head[t][h].ret == ret)
            return &s_head[t][h].head;
    }
    return NULL;
}

static void rec_push(uint32_t ret, const void *x)
{
    int t = s_rec;
    uint32_t *head = head_of(t, ret, 1);
    di_rec_t *r;

    if (!head || s_nrecs[t] >= DI_RECS)
        return;
    r = &s_recs[t][s_nrecs[t]];
    r->ret = ret;
    r->used = 0;
    r->next = *head;
    memcpy(r->v, x, sizeof r->v);
    *head = s_nrecs[t]++;
}

/* The tick-N record nearest this one at the same call site, or NULL. */
static di_rec_t *nearest(uint32_t ret, const float *cf, uint32_t type)
{
    int t = !s_rec;
    uint32_t *head = head_of(t, ret, 0), i;
    di_rec_t *best = NULL;
    float best_d = DI_JUMP * DI_JUMP;

    if (!head)
        return NULL;
    for (i = *head; i != 0xFFFFFFFFu; i = s_recs[t][i].next) {
        di_rec_t *r = &s_recs[t][i];
        const float *pf = (const float *)r->v;
        float d = 0.0f;
        unsigned k;

        if (r->used || r->v[26] != type)
            continue;
        for (k = 9; k < 12; k++)
            d += (cf[k] - pf[k]) * (cf[k] - pf[k]);
        for (k = 0; k < 9; k++)               /* rotation breaks a tie   */
            d += 0.01f * (cf[k] - pf[k]) * (cf[k] - pf[k]);
        if (d < best_d) {
            best_d = d;
            best = r;
        }
    }
    return best;
}

/* A camera transform: floats 0..8 the view rotation, 21..23 the camera's
 * world position (_xfm_BuildInvRotYXZ_XlatFromVec and _xfm_BuildLookat both
 * store the eye there, in both titles). */
#define CAM_POS 21

/* The camera moved further than a tick can carry it. `frac` is how much of a
 * tick separates the two cameras: 1 for tick N against N+1, s_alpha for tick N
 * against an in-between camera built from blended state, which is s_alpha of
 * the way along any move the blend let through and all of the way along one
 * it refused (more than DS_JUMP). */
static int camera_jumped(const float *p, const float *c, float frac)
{
    float j = 0.0f, lim = DI_JUMP * frac;
    unsigned i;

    for (i = 0; i < 3; i++)
        j += (c[i] - p[i]) * (c[i] - p[i]);
    return j > lim * lim;
}

/* Hydro builds its camera inside the draw, so a cut can only be seen once the
 * in-between camera exists: at the first object pushed, the stack's element 0
 * is that camera. More than 15 degrees from tick N's is half of a 30-degree
 * jump in one tick, which is a cut; so is a jump in position. */
static void camera_capture_prev(void)
{
    if (!G->xfm_stack)
        return;
    memcpy(s_cam_prev, (const void *)(uintptr_t)G->xfm_stack, sizeof s_cam_prev);
    s_cam_prev_ok = 1;
}

static void cut_check_at_first_push(void)
{
    float c[24], tr = 0.0f;
    unsigned i;

    if (s_cut_checked || !s_cam_prev_ok || !G->xfm_stack)
        return;
    s_cut_checked = 1;
    memcpy(c, (const void *)(uintptr_t)G->xfm_stack, sizeof c);
    for (i = 0; i < 9; i++)
        tr += s_cam_prev[i] * c[i];
    if (tr < 1.0f + 2.0f * 0.9659f ||   /* cos 15 deg */
        camera_jumped(s_cam_prev + CAM_POS, c + CAM_POS, s_alpha))
        abort_cut();
}

static void __cdecl di_push(const void *x)
{
    uint32_t ret = (uint32_t)(uintptr_t)__builtin_return_address(0);
    const float *cf = (const float *)x;
    const uint32_t *cw = (const uint32_t *)x;
    di_rec_t *pr;

    if (s_ip_mode == 3)
        cut_check_at_first_push();
    if (!s_ip_mode || s_ip_mode == 3) {
        s_orig_push(x);
        return;
    }
    if (s_ip_mode == 1) {
        if (s_recs[0])                 /* read only by the call blend */
            rec_push(ret, x);
        s_orig_push(x);
        return;
    }
    site_note(ret);
    SITE(n);
    pr = nearest(ret, cf, cw[26]);
    if (!pr) {
        SITE(nokey);
        s_ip_nokey++;
        s_orig_push(x);
        return;
    }
    pr->used = 1;
    {
        const float *pf = (const float *)pr->v;
        float dx = cf[9] - pf[9], dy = cf[10] - pf[10], dz = cf[11] - pf[11];
        float j = dx * dx + dy * dy + dz * dz;
        uint32_t b[DI_WORDS];
        float *bf = (float *)b;
        unsigned i;

        if (!rotation_plausible(pf, cf)) {
            SITE(rot);
            s_ip_rotref++;
            s_orig_push(x);
            return;
        }
        if (j > s_ip_maxjump)
            s_ip_maxjump = j;
        for (i = 0; i < 26; i++)
            bf[i] = lerpf(pf[i], cf[i]);
        b[26] = cw[26];
        s_ip_blend++;
        s_orig_push(b);
    }
}

static uint32_t lerp_angle(uint32_t prev, uint32_t cur)
{
    int d = (int16_t)(uint16_t)(cur - prev);          /* the short way round */
    int back = (int)((1.0f - s_alpha) * (float)d + (d < 0 ? -0.5f : 0.5f));

    return (cur - (uint32_t)back) & 0xFFFFu;   /* the game's own range */
}

static void __cdecl di_camrot(void *xfm, uint32_t x, uint32_t y, uint32_t z,
                              uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t ret = (uint32_t)(uintptr_t)__builtin_return_address(0);
    di_slot_t *sl;

    if (s_ip_mode == 1) {
        sl = di_find(s_rec, ret, occ_next(ret), 1);
        if (sl) {
            sl->v[0] = x; sl->v[1] = y; sl->v[2] = z;
            sl->v[3] = a; sl->v[4] = b; sl->v[5] = c;
        }
    } else if (s_ip_mode == 3 &&
               (sl = di_find(!s_rec, ret, occ_next(ret), 0)) != NULL) {
        float q[3];

        memcpy(&q[0], &x, 4); memcpy(&q[1], &y, 4); memcpy(&q[2], &z, 4);
        if (camera_jumped((const float *)sl->v, q, s_alpha) ||
            abs((int16_t)(uint16_t)(a - sl->v[3])) > DI_ANGLE ||
            abs((int16_t)(uint16_t)(b - sl->v[4])) > DI_ANGLE ||
            abs((int16_t)(uint16_t)(c - sl->v[5])) > DI_ANGLE)
            abort_cut();
        a = lerp_angle(sl->v[3], a);
        b = lerp_angle(sl->v[4], b);
        c = lerp_angle(sl->v[5], c);
        s_ip_cam++;
    } else if (s_ip_mode == 2 &&
               (sl = di_find(!s_rec, ret, occ_next(ret), 0)) != NULL) {
        float p[3], q[3], j = 0.0f;
        unsigned i;

        memcpy(p, sl->v, sizeof p);
        q[0] = *(float *)&x; q[1] = *(float *)&y; q[2] = *(float *)&z;
        for (i = 0; i < 3; i++)
            j += (q[i] - p[i]) * (q[i] - p[i]);
        if (j <= DI_JUMP * DI_JUMP &&
            abs((int16_t)(uint16_t)(a - sl->v[3])) <= DI_ANGLE &&
            abs((int16_t)(uint16_t)(b - sl->v[4])) <= DI_ANGLE &&
            abs((int16_t)(uint16_t)(c - sl->v[5])) <= DI_ANGLE) {
            for (i = 0; i < 3; i++)
                q[i] = lerpf(p[i], q[i]);
            memcpy(&x, &q[0], 4); memcpy(&y, &q[1], 4); memcpy(&z, &q[2], 4);
            a = lerp_angle(sl->v[3], a);
            b = lerp_angle(sl->v[4], b);
            c = lerp_angle(sl->v[5], c);
            s_ip_cam++;
        } else {
            s_ip_reject++;
        }
    }
    s_orig_camrot(xfm, x, y, z, a, b, c);
}

/* ---- state blend --------------------------------------------------------------
 *
 * Blending the draw's CALLS leaves everything the draw computes from game state
 * by other routes at tick N+1: Hydro's screen-space sky (panned in Work), the
 * markers for boats behind, anything multiplied through the camera. Blending
 * the STATE makes the draw compute all of it from one moment. Before the
 * in-between draw, every word that changed since tick N's snapshot is written
 * halfway, provided both values are plausible floats, neither is an address
 * inside the game's memory, and it moved less than DS_JUMP. The restore puts
 * every one of them back. */
#define DS_JUMP 4096.0f

static rng_t     s_prev_rng[DR_MAX_RNG];
static unsigned  s_prev_nrng;
static int       s_have_prev_state, s_state_mode;
static uint64_t  s_ds_words, s_ds_skipped;

typedef void (__cdecl *skywork_fn)(uint32_t, uint32_t, uint32_t);
static skywork_fn s_orig_sky;
static uint32_t  s_sky_args[2][3];     /* [0] tick N, [1] tick N+1          */
static int       s_sky_seen;

/* Zero is not plausible: NULL and 0.0 are the same word, and a list head that
 * is empty at tick N+1 and held a reused slot's float at tick N blends into a
 * pointer to nowhere (Offroad's _worldob_BuildLightList). The caller does let
 * a word LEAVE zero: whatever it became at N+1 is a float and not an address,
 * or it would have been refused, and that is how a boat starts to move. */
static int plausible_float(uint32_t u)
{
    uint32_t e = (u >> 23) & 0xFFu;

    return e >= 107u && e <= 150u;     /* about 1e-6 .. 8e6, no NaN or Inf */
}

/* `u` lies inside one of s_rng's ranges. */
static int rng_search(uint32_t u)
{
    unsigned lo = 0, hi = s_nrng;

    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;

        if (u < s_rng[mid].lo)
            hi = mid;
        else if (u >= s_rng[mid].hi)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

/* A pointer into the game's own memory. NOT every committed region in the
 * process: a pointer-shaped float is only a pointer if it points somewhere
 * the game owns, and widening this to the whole address space (tried) treats
 * every float from about 2.0 up as an address once the process has a region
 * near 1 GB, which left Hydro's boat unblended. */
static int game_address(uint32_t u)
{
    if (u >= 0x100000u && u < G->span_end)
        return 1;
    if (!s_nrng || u < s_rng[0].lo || u >= s_rng[s_nrng - 1].hi)
        return 0;                      /* s_rng is sorted and disjoint */
    return rng_search(u);
}

/* Keep this tick's snapshot as the next tick's tick N: swap the buffers. */
static int keep_prev(void)
{
    snap_t b = s_prev;

    s_prev = s_before;
    s_before = b;
    memcpy(s_prev_rng, s_rng, s_nrng * sizeof s_rng[0]);
    s_prev_nrng = s_nrng;
    return s_prev.p != NULL;
}

/* Every word the blend wrote, with its tick N+1 value, so the HUD can be
 * drawn from the real state: its timers drive blinks and pulses designed to
 * be sampled 30 times a second, and halfway between two samples they show a
 * message neither tick shows or a pulse at a phase the game never draws
 * (Offroad's "NITRO +3 S..." in an in-between frame only, "GO!!!" too big). */
typedef struct { uint32_t *at; uint32_t real, blended; } bl_t;
static bl_t   *s_bl;
static size_t  s_nbl, s_bl_cap;
static int     s_unblended;
static uint64_t s_unblend_words, s_unblend_kept;

static void bl_note(uint32_t *at, uint32_t real)
{
    if (s_nbl == s_bl_cap) {
        size_t cap = s_bl_cap ? s_bl_cap * 2 : 65536;
        bl_t *b = (bl_t *)realloc(s_bl, cap * sizeof *b);

        if (!b)
            return;
        s_bl = b;
        s_bl_cap = cap;
    }
    s_bl[s_nbl].at = at;
    s_bl[s_nbl].real = real;
    s_bl[s_nbl].blended = *at;
    s_nbl++;
}

/* Put tick N+1 back under every blended word the draw has not changed since. */
static void unblend(void)
{
    size_t i;

    if (s_unblended)
        return;
    s_unblended = 1;
    for (i = 0; i < s_nbl; i++) {
        if (*s_bl[i].at == s_bl[i].blended) {
            *s_bl[i].at = s_bl[i].real;
            s_unblend_words++;
        } else {
            s_unblend_kept++;
        }
    }
}

typedef void (__cdecl *hud_fn)(void);
static hud_fn s_orig_hud;

static void __cdecl di_hud_draw(void)
{
    if (s_ip_mode == 3)
        unblend();
    s_orig_hud();
}

/* Words the blend must never touch, found by watching what writes them.
 * _mesh3d_SlideST steps a mesh's texture coordinates by whole texels every
 * few ticks (Hydro's ramps and arrow signs move 64 every fourth tick and wrap
 * at 2048): a step animation, and halfway through a step is a picture the
 * game never draws. Its ranges are recorded as it runs, sorted and disjoint
 * for excluded()'s binary search. A range SlideST has not touched for EX_TTL
 * ticks is dropped: the step is written by the same Work that calls SlideST,
 * so a live range is always refreshed before the blend that would see it, and
 * a mesh freed at a track change stops holding its memory out of the blend. */
#define EX_MAX 1024
#define EX_TTL 150u                    /* ticks: five seconds */
static rng_t    s_ex[EX_MAX];
static uint32_t s_ex_seen[EX_MAX];     /* s_ex_now when SlideST last wrote it */
static unsigned s_nex;
static uint32_t s_ex_now;

static void ex_remove(unsigned i, unsigned n)
{
    memmove(&s_ex[i], &s_ex[i + n], (s_nex - i - n) * sizeof s_ex[0]);
    memmove(&s_ex_seen[i], &s_ex_seen[i + n],
            (s_nex - i - n) * sizeof s_ex_seen[0]);
    s_nex -= n;
}

/* Once a tick, before the blend reads the list. */
static void ex_expire(void)
{
    unsigned i, n = 0;

    s_ex_now++;
    for (i = 0; i < s_nex; i++)
        if (s_ex_now - s_ex_seen[i] <= EX_TTL) {
            s_ex[n] = s_ex[i];
            s_ex_seen[n] = s_ex_seen[i];
            n++;
        }
    s_nex = n;
}

static void exclude_range(uintptr_t lo, uintptr_t hi)
{
    unsigned i, j, k;

    if (hi <= lo)
        return;
    for (i = 0; i < s_nex && s_ex[i].hi < lo; i++)
        ;
    for (j = i; j < s_nex && s_ex[j].lo <= hi; j++) {  /* overlap or touch */
        if (s_ex[j].lo < lo)
            lo = s_ex[j].lo;
        if (s_ex[j].hi > hi)
            hi = s_ex[j].hi;
    }
    if (j > i) {                       /* merged: keep i, drop the rest */
        ex_remove(i + 1, j - i - 1);
    } else {
        if (s_nex >= EX_MAX) {         /* full: the stalest one goes */
            for (k = 0, j = 1; j < s_nex; j++)
                if (s_ex_now - s_ex_seen[j] > s_ex_now - s_ex_seen[k])
                    k = j;
            ex_remove(k, 1);
            if (k < i)
                i--;
        }
        memmove(&s_ex[i + 1], &s_ex[i], (s_nex - i) * sizeof s_ex[0]);
        memmove(&s_ex_seen[i + 1], &s_ex_seen[i],
                (s_nex - i) * sizeof s_ex_seen[0]);
        s_nex++;
    }
    s_ex[i].lo = lo;
    s_ex[i].hi = hi;
    s_ex_seen[i] = s_ex_now;
}

static int excluded(uintptr_t a)
{
    unsigned lo = 0, hi = s_nex;

    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;

        if (a < s_ex[mid].lo)
            hi = mid;
        else if (a >= s_ex[mid].hi)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

/* An in-between draw must never open a movie: Offroad's attract switches mode
 * from inside its draw list, and _mvplayer_OpenMovie then starts the stream
 * thread, opens the file and waits for data -- side effects the restore cannot
 * take back, and the attract froze there. Reached in an in-between draw, it
 * abandons that picture before doing anything; the real draw opens the movie
 * as the cabinet does. The arguments are passed through untouched. */
typedef int (__cdecl *openmovie_fn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                    uint32_t, uint32_t, uint32_t, uint32_t);
static openmovie_fn s_orig_openmovie;
static uint64_t s_movie_aborts;

static int __cdecl di_openmovie(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    if (s_ip_mode == 3) {
        s_movie_aborts++;
        abort_cut();
    }
    return s_orig_openmovie(a, b, c, d, e, f, g, h);
}

typedef void (__cdecl *slidest_fn)(void *, void *, float, float);
static slidest_fn s_orig_slidest;

static void __cdecl di_slidest(void *mesh, void *group, float ds, float dt)
{
    const unsigned char *m = (const unsigned char *)mesh;
    const unsigned char *g = (const unsigned char *)group;
    uintptr_t st, base;
    uint32_t count;

    if (m) {
        memcpy(&st, m + 0x3c, 4);
        if (!g) {
            memcpy(&count, m + 0x18, 4);
            base = st;
        } else {
            uint16_t first;

            memcpy(&first, g + 2, 2);
            memcpy(&count, g + 4, 4);
            base = st + (uintptr_t)first * 8u;
        }
        if (st && count < (1u << 20))
            exclude_range(base, base + (uintptr_t)count * 8u);
    }
    s_orig_slidest(mesh, group, ds, dt);
}

/* Memory the blend must never touch, by pool. Some effects are records taken
 * from a recycled pool: a slot holds one record at tick N and another at N+1,
 * and halfway between them is something no tick draws. Offroad's skid marks
 * (corners, and water) became a quad stretched across the track; Hydro's
 * splash droplets, freed and reused last in first out, a droplet halfway
 * between a dying one and a new one. Such records are drawn as the newer tick
 * has them. The pools are rebuilt as the game resets them, so their extent is
 * taken each time _linklist_InitPool threads one of the roots
 * gen-gamedefs.py:d_noblend_pools() derived. */
#define NB_POOLS 4

typedef void (__cdecl *initpool_fn)(void *, void *, uint32_t, uint32_t);
static initpool_fn s_orig_initpool;
static rng_t       s_nb[NB_POOLS];

static uint32_t nb_root(int k)
{
    return k == 0 ? G->noblend_pool_a : k == 1 ? G->noblend_pool_b :
           k == 2 ? G->noblend_pool_c : G->noblend_pool_d;
}

static void __cdecl di_initpool(void *root, void *base, uint32_t elsize,
                                uint32_t count)
{
    uint32_t r = (uint32_t)(uintptr_t)root;
    int k;

    for (k = 0; k < NB_POOLS; k++)
        if (r && r == nb_root(k) && base && elsize < 0x10000u &&
            count < 0x10000u) {
            uintptr_t lo = (uintptr_t)base, hi = lo + (uintptr_t)elsize * count;

            if (lo == s_nb[k].lo && hi == s_nb[k].hi)
                continue;
            s_nb[k].lo = lo;
            s_nb[k].hi = hi;
            LOGI("drawinterp: pool 0x%08X at 0x%08X, %u x %u bytes, never "
                 "blended", (unsigned)r, (unsigned)s_nb[k].lo,
                 (unsigned)count, (unsigned)elsize);
        }
    s_orig_initpool(root, base, elsize, count);
}

static int never_blend(uintptr_t a)
{
    int k;

    for (k = 0; k < NB_POOLS; k++)
        if (a >= s_nb[k].lo && a < s_nb[k].hi)
            return 1;
    return 0;
}

/* Some excluded range overlaps [lo, hi): once per chunk, so a chunk that
 * meets none skips excluded() for every word in it. */
static int ex_overlaps(uintptr_t lo, uintptr_t hi)
{
    unsigned l = 0, h = s_nex;

    while (l < h) {                    /* the first range ending above lo */
        unsigned mid = (l + h) / 2;

        if (s_ex[mid].hi <= lo)
            l = mid + 1;
        else
            h = mid;
    }
    return l < s_nex && s_ex[l].lo < hi;
}

/* A changed word the blend may write: a float on both sides, or one leaving
 * zero; not an address in the game's memory; not in a pool or a range held
 * out. Cheapest test first. `ex`: the word's chunk meets an excluded range. */
static int blendable(uint32_t p, uint32_t c, uintptr_t at, int ex)
{
    return (plausible_float(p) || p == 0) && plausible_float(c) &&
           !game_address(p) && !game_address(c) &&
           !never_blend(at) && !(ex && excluded(at));
}

/* ---- effect records that change owner between ticks -------------------------
 *
 * Some effect tables reuse a slot for a different record from one tick to the
 * next, and a word-by-word blend then draws something halfway between two
 * unrelated records: Hydro's spray droplets became white streaks in mid-air.
 * Each tick's records are copied as tick N+1 has them, before the blend; after
 * it, a record that is not the same one as at tick N is redrawn from the right
 * source:
 *   spray   the same droplet satisfies head(N+1) == head(N) + vel(N), or is
 *           unmoved. A droplet copied into another slot (the game fills a hole
 *           with its last droplet) is found at tick N by the same test and
 *           blended from there. Anything else is new: drawn as at N+1.
 *   splash  an instance inactive at N, or whose frame counter went back, is a
 *           new splash: drawn as at N+1.
 *   fxpart  a record inactive at N is new: drawn as at N+1.
 * Every field is gen-gamedefs.py:d_record_tables(); a table whose anchors are
 * 0 is left alone. */
#define RT_BYTES (64u * 1024u)

typedef struct { unsigned char b[RT_BYTES]; uint32_t n, ptr; uint64_t tick; } rt_buf_t;
static rt_buf_t  s_rt_spray[2], s_rt_splash[2], s_rt_fxpart[2];
static int       s_rt_cur;             /* index of tick N+1's copies */
static uint64_t  s_rt_tick;
static uint64_t  s_rt_moved, s_rt_new, s_rt_splash_new, s_rt_fxpart_new;

static uint32_t rt_splash_bytes(void)
{
    return G->splash_base && G->splash_stride >= 8 &&
           G->splash_counter + 4u <= G->splash_stride
        ? G->splash_n * G->splash_stride : 0;
}

static uint32_t rt_fxpart_bytes(void)
{
    return G->fxpart_base && G->fxpart_rec_stride >= 4 && G->fxpart_recs &&
           G->fxpart_rec_off + G->fxpart_recs * G->fxpart_rec_stride <=
               G->fxpart_block_stride
        ? G->fxpart_blocks * G->fxpart_block_stride : 0;
}

static int rt_spray_ok(void)
{
    return G->spray_cell && G->spray_count && G->spray_stride % 4u == 0 &&
           G->spray_pos + 12u <= G->spray_stride &&
           G->spray_vel + 12u <= G->spray_stride;
}

/* Tick N+1, every tick, before anything blends. */
static void rt_take(void)
{
    rt_buf_t *b;
    uint32_t n;

    s_rt_tick++;
    s_rt_cur = !s_rt_cur;
    b = &s_rt_spray[s_rt_cur];
    b->tick = 0;
    if (rt_spray_ok()) {
        uint32_t p = *(const uint32_t *)(uintptr_t)G->spray_cell;

        n = *(const uint32_t *)(uintptr_t)G->spray_count;
        /* An empty buffer is a copy too: the next tick's droplets are all new. */
        if (p && n <= RT_BYTES / G->spray_stride && game_address(p) &&
            (!n || game_address(p + n * G->spray_stride - 1u))) {
            memcpy(b->b, (const void *)(uintptr_t)p, n * G->spray_stride);
            b->n = n;
            b->ptr = p;
            b->tick = s_rt_tick;
        }
    }
    b = &s_rt_splash[s_rt_cur];
    b->tick = 0;
    n = rt_splash_bytes();
    if (n && n <= RT_BYTES) {
        memcpy(b->b, (const void *)(uintptr_t)G->splash_base, n);
        b->tick = s_rt_tick;
    }
    b = &s_rt_fxpart[s_rt_cur];
    b->tick = 0;
    n = rt_fxpart_bytes();
    if (n && n <= RT_BYTES) {
        memcpy(b->b, (const void *)(uintptr_t)G->fxpart_base, n);
        b->tick = s_rt_tick;
    }
}

/* Both copies are there, and tick N's is from the tick before. */
static int rt_pair(const rt_buf_t *t)
{
    return t[s_rt_cur].tick == s_rt_tick && t[!s_rt_cur].tick + 1 == s_rt_tick;
}

static int rt_near3(const float *a, const float *b)
{
    int k;

    for (k = 0; k < 3; k++)
        if (fabsf(a[k] - b[k]) > 1e-3f + fabsf(b[k]) * 1e-5f)
            return 0;
    return 1;
}

/* Droplet `r` of tick N became `h1` at N+1: moved by its velocity, or not. */
static int rt_became(const unsigned char *r, const float *h1)
{
    const float *h0 = (const float *)(r + G->spray_pos);
    const float *v0 = (const float *)(r + G->spray_vel);
    float pred[3];
    int k;

    for (k = 0; k < 3; k++)
        pred[k] = h0[k] + v0[k];
    return rt_near3(pred, h1) || rt_near3(h0, h1);
}

/* The blend's rule for one word, tick N's `p` against tick N+1's `c`. */
static uint32_t rt_blend_word(uint32_t p, uint32_t c, uintptr_t at)
{
    float fa, fb;

    if (p == c || !blendable(p, c, at, s_nex != 0))
        return c;
    memcpy(&fa, &p, 4);
    memcpy(&fb, &c, 4);
    if (fabsf(fb - fa) > DS_JUMP)
        return c;
    fa += (fb - fa) * s_alpha;
    memcpy(&c, &fa, 4);
    return c;
}

static void rt_fix_spray(void)
{
    const rt_buf_t *cur = &s_rt_spray[s_rt_cur], *prv = &s_rt_spray[!s_rt_cur];
    uint32_t st = G->spray_stride, i, j, w;

    if (!rt_pair(s_rt_spray) || cur->ptr != prv->ptr ||
        *(const uint32_t *)(uintptr_t)G->spray_count != cur->n)
        return;
    for (i = 0; i < cur->n; i++) {
        const unsigned char *c = cur->b + i * st;
        const float *h1 = (const float *)(c + G->spray_pos);
        uint32_t *live = (uint32_t *)(uintptr_t)(cur->ptr + i * st);
        const uint32_t *from = NULL;

        if (i < prv->n && rt_became(prv->b + i * st, h1))
            continue;                  /* the same droplet: the blend is right */
        for (j = 0; j < prv->n && !from; j++)
            if (j != i && rt_became(prv->b + j * st, h1))
                from = (const uint32_t *)(prv->b + j * st);
        if (from) {
            for (w = 0; w < st / 4u; w++)
                live[w] = rt_blend_word(from[w], ((const uint32_t *)c)[w],
                                        (uintptr_t)&live[w]);
            s_rt_moved++;
        } else {
            memcpy(live, c, st);
            s_rt_new++;
        }
    }
}

static void rt_fix_splash(void)
{
    const unsigned char *cur = s_rt_splash[s_rt_cur].b;
    const unsigned char *prv = s_rt_splash[!s_rt_cur].b;
    uint32_t st = G->splash_stride, k, ctr = G->splash_counter / 4u;

    if (!rt_pair(s_rt_splash))
        return;
    for (k = 0; k < G->splash_n; k++) {
        const uint32_t *c = (const uint32_t *)(cur + k * st);
        const uint32_t *q = (const uint32_t *)(prv + k * st);

        if (!c[0] || (q[0] && q[ctr] <= c[ctr]))
            continue;
        memcpy((void *)(uintptr_t)(G->splash_base + k * st), c, st);
        s_rt_splash_new++;
    }
}

static void rt_fix_fxpart(void)
{
    const unsigned char *cur = s_rt_fxpart[s_rt_cur].b;
    const unsigned char *prv = s_rt_fxpart[!s_rt_cur].b;
    uint32_t b, r;

    if (!rt_pair(s_rt_fxpart))
        return;
    for (b = 0; b < G->fxpart_blocks; b++)
        for (r = 0; r < G->fxpart_recs; r++) {
            uint32_t off = b * G->fxpart_block_stride + G->fxpart_rec_off +
                           r * G->fxpart_rec_stride;

            if (!(cur[off] & 1u) || (prv[off] & 1u))
                continue;
            memcpy((void *)(uintptr_t)(G->fxpart_base + off), cur + off,
                   G->fxpart_rec_stride);
            s_rt_fxpart_new++;
        }
}

/* After the blend, before the in-between draws. */
static void rt_fix(void)
{
    rt_fix_spray();
    rt_fix_splash();
    rt_fix_fxpart();
}

/* game_address() without its bounds test, for prof_chunk's reference. */
static int prof_game_address(uint32_t u)
{
    s_pc.ga_calls++;
    if (u >= 0x100000u && u < G->span_end) {
        s_pc.ga_image++;
        return 1;
    }
    if (!s_nrng || u < s_rng[0].lo || u >= s_rng[s_nrng - 1].hi)
        s_pc.ga_outside++;
    if (rng_search(u)) {
        s_pc.ga_true++;
        return 1;
    }
    return 0;
}

/* One changed chunk's words through every check, one at a time and counted,
 * outside the word loop's timing; and blendable()'s verdict against that
 * reference, where any disagreement is a bug in blendable(). Reads only the
 * two snapshots, which the blend does not write. */
static void prof_chunk(const uint32_t *pc, const uint32_t *cv, uintptr_t a,
                       size_t n, int ex)
{
    size_t q;

    s_pc.chunks++;
    if (s_nex) {
        s_pc.ex_chunks++;
        if (!ex_overlaps(a, a + n))
            s_pc.ex_free_chunks++;
    }
    for (q = 0; q < n / 4; q++) {
        uintptr_t w = a + 4 * q;
        int ok = 0;

        if (pc[q] == cv[q])
            continue;
        s_pc.changed++;
        if (s_nex) {
            s_pc.ex_calls++;
            if (excluded(w)) {
                s_pc.ex_hits++;
                goto verdict;
            }
        }
        s_pc.nb_calls++;
        if (never_blend(w)) {
            s_pc.nb_hits++;
            goto verdict;
        }
        if (!((plausible_float(pc[q]) || pc[q] == 0) && plausible_float(cv[q]))) {
            s_pc.not_float++;
            goto verdict;
        }
        ok = !prof_game_address(pc[q]) && !prof_game_address(cv[q]);
verdict:
        if (ok != blendable(pc[q], cv[q], w, ex))
            s_pc.disagree++;
    }
}

/* snapshot() and blend_state() in one pass: each chunk is copied into this
 * tick's snapshot and, while it is still in cache, compared with tick N's. */
static void snapshot_and_blend(void)
{
    unsigned i, k = 0, cur = 0;
    size_t off = 0, poff = 0;
    /* s_before was filled two ticks ago and s_prev one: a page unwritten
     * since s_before's fill is in both, one unwritten since s_prev's is
     * still tick N's and has nothing to blend. */
    int inc_b = snap_current(&s_before), inc_p = snap_current(&s_prev);
    double ts = s_prof ? now_ms() : 0.0, tq = 0.0, tcount = 0.0;
    int ex;

    s_nbl = 0;
    s_unblended = 0;
    s_pr_word_acc = 0.0;
    ex_expire();
    if (s_nww)
        ww_poll();

    for (i = 0; i < s_nrng; i++) {
        uintptr_t lo = s_rng[i].lo, hi = s_rng[i].hi, a;

        /* Chunk by chunk, so a range that grew since tick N still blends
         * wherever tick N had it. */
        for (a = lo; a < hi; a += DR_CHUNK) {
            size_t n = hi - a < DR_CHUNK ? hi - a : DR_CHUNK, q;
            uint32_t *live = (uint32_t *)a;
            uint32_t *cv = (uint32_t *)(s_before.p + off + (a - lo));
            const uint32_t *pc = NULL;

            while (k < s_prev_nrng && s_prev_rng[k].hi <= a) {
                poff += s_prev_rng[k].hi - s_prev_rng[k].lo;
                k++;
            }
            if (k < s_prev_nrng && s_prev_rng[k].lo <= a &&
                s_prev_rng[k].hi >= a + n)
                pc = (const uint32_t *)(s_prev.p + poff + (a - s_prev_rng[k].lo));
            if (inc_b && ww_clean(a, n, s_before.epoch, &cur))
                continue;
            memcpy(cv, live, n);
            s_ww_copied++;
            if (!pc || (inc_p && ww_clean(a, n, s_prev.epoch, &cur)))
                continue;
            s_ww_compared++;
            if (!memcmp(pc, cv, n))
                continue;
            ex = s_nex && ex_overlaps(a, a + n);
            if (s_prof)
                tq = now_ms();
            /* Word by word. Blending runs of floats all-or-nothing, so a
             * position and the target beside it stay together, was tried: it
             * refused Hydro's boat position whenever any field beside it
             * failed, and the boat drew off its own camera. A rule refusing
             * changes larger than half a value (meant for angle wraps) was
             * tried too, and caused more bad frames in both titles than it
             * prevented. */
            for (q = 0; q < n / 4; ) {
                size_t c0 = q, c1, r;
                int ok = 1;

                if (pc[q] == cv[q]) {
                    q++;
                    continue;
                }
                if (blendable(pc[q], cv[q], (uintptr_t)&live[q], ex))
                    q++;
                c1 = q;
                if (c1 == c0) {                /* changed, but not a float */
                    s_ds_skipped++;
                    q++;
                    continue;
                }
                for (r = c0; r < c1 && ok; r++) {
                    float fa, fb, d;

                    memcpy(&fa, &pc[r], 4);
                    memcpy(&fb, &cv[r], 4);
                    d = fb - fa;
                    ok = fabsf(d) <= DS_JUMP;
                }
                if (!ok) {
                    s_ds_skipped += c1 - c0;
                    continue;
                }
                for (r = c0; r < c1; r++) {
                    float fa, fb;

                    memcpy(&fa, &pc[r], 4);
                    memcpy(&fb, &cv[r], 4);
                    fa += (fb - fa) * s_alpha;
                    memcpy(&live[r], &fa, 4);
                    bl_note(&live[r], cv[r]);
                    s_ds_words++;
                }
            }
            if (s_prof) {
                double tc = now_ms();

                s_pr_word_acc += tc - tq;
                s_pc.timed++;
                prof_chunk(pc, cv, a, n, ex);
                tcount += now_ms() - tc;
            }
        }
        off += hi - lo;
    }
    s_before.epoch = ++s_epoch;
    s_before.layout = s_layout;
    if (s_prof) {
        pr_add(&s_pr_words, s_pr_word_acc);
        pr_add(&s_pr_snap, now_ms() - ts - tcount);
        s_pc.ticks++;
    }
}

static void __cdecl di_sky_work(uint32_t a, uint32_t b, uint32_t c)
{
    if (!s_ip_mode) {                  /* the game's own call, from Work    */
        s_sky_args[1][0] = a;
        s_sky_args[1][1] = b;
        s_sky_args[1][2] = c;
        s_sky_seen++;
    }
    s_orig_sky(a, b, c);
}

static void draw_recorded(int parm, int mode)
{
    s_occ_gen++;
    if (mode == 1) {
        s_tab_gen[s_rec]++;
        s_nrecs[s_rec] = 0;
    }
    s_ip_mode = mode;
    s_game_draw(parm);
    s_ip_mode = 0;
}

/* ---- the frame budget --------------------------------------------------------
 *
 * The game keeps the cabinet's speed only while every tick fits its limiter:
 * _Gameloop_fTargetFrameTime, measured from the stamp _timer_ResetHiResTimer
 * takes at the top of the loop. A tick that runs past it is game time lost
 * for good, and the in-between picture and the half-tick wait after it both
 * sit inside that tick. So an in-between is drawn only when the real picture
 * is predicted to go up with a margin to spare, and the wait never runs past
 * the deadline less that margin. The margin stands for what follows the wait,
 * mostly the real swap, which is not timed here: it grows when a tick that
 * drew an in-between overran, and shrinks back while ticks keep time. All
 * times are seconds into the tick on the limiter's own clock.
 *
 * Both costs are learnt only from in-betweens drawn, so a budget that skips
 * every tick would never learn that they got cheaper again: a few slow ones
 * at a load would turn 60 Hz off for the rest of the run. While it skips, it
 * draws one anyway every so often (a probe) on a tick with room for one, and
 * takes that one's costs as they are; while probes stay too slow they get
 * rarer. */
#define FB_MARGIN_MIN  0.002
#define FB_MARGIN_MAX  0.015
#define FB_MARGIN_STEP 0.002           /* most one overrun adds               */
#define FB_MARGIN_DECAY 0.00005        /* per tick on time                    */
#define FB_SLACK       0.0005          /* a tick this much past T overran     */
#define FB_SPIN        0.001           /* spun, not slept: the timer's jitter */
#define FB_IDLE_EARLY  0.002           /* the idle's wake: seen up to 1 ms late */
#define FB_RECOVER     0.00075         /* a held in-between's phase, per tick */
#define FB_PROBE_MIN   30u             /* skipped ticks before a probe        */
#define FB_PROBE_MAX   240u            /* ... after probes that stayed slow   */
#define FB_LOG_TICKS   300u

static double   s_fb_cinb = 0.012;     /* decision to in-between presented    */
static double   s_fb_cpost = 0.008;    /* in-between presented to real ready  */
static double   s_fb_margin = 0.003;
static double   s_fb_t0, s_fb_ti;
static uint64_t s_fb_base;
static int      s_fb_drew;             /* this tick presented an in-between   */
static uint64_t s_fb_skipped, s_fb_overran, s_fb_ticks, s_fb_probes;
static unsigned s_fb_streak;           /* ticks skipped in a row              */
static unsigned s_fb_probe_every = FB_PROBE_MIN;
static int      s_fb_probing;          /* this tick's in-between is a probe   */
static HANDLE   s_fb_timer;            /* high resolution, or NULL: spin    */
static double   s_fb_prev_ti = -1.0;   /* the tick before's s_fb_ti, or -1  */

static double fb_tick(void)
{
    float t = *(const volatile float *)(uintptr_t)G->gameloop_target_frame_time;

    return t > 0.001f && t < 0.2f ? (double)t : 1.0 / 30.0;
}

static uint64_t fb_stamp(void)
{
    return ((uint64_t)*(const volatile uint32_t *)(uintptr_t)G->timer_base_hi << 32) |
           *(const volatile uint32_t *)(uintptr_t)G->timer_base_lo;
}

static double fb_now(void)
{
    return (double)(int64_t)(timebase_ticks() - s_fb_base) /
           (double)timebase_hz();
}

/* Whether an in-between costing `cinb` fits this tick, half a tick before the
 * real picture and with the margin to spare. */
static int fb_fits(double cinb, double T)
{
    return s_fb_t0 + cinb + (s_fb_cpost > T / 2.0 ? s_fb_cpost : T / 2.0) +
           s_fb_margin <= T;
}

static void fb_log(void);

/* The start of a tick's draw: learn from the tick before, then say whether
 * this one has room for an in-between. */
static int fb_begin(void)
{
    uint64_t base = fb_stamp();
    double T = fb_tick();

    if (s_fb_base && base != s_fb_base) {
        double period = (double)(base - s_fb_base) / (double)timebase_hz();

        if (period > T + FB_SLACK) {
            /* Twice T or more is a load or a pause, not an overrun. */
            if (s_fb_drew && period < 2.0 * T) {
                double over = period - T;

                s_fb_overran++;
                s_fb_margin += (over < FB_MARGIN_STEP ? over : FB_MARGIN_STEP) +
                               FB_SLACK;
                if (s_fb_margin > FB_MARGIN_MAX)
                    s_fb_margin = FB_MARGIN_MAX;
            }
        } else if (s_fb_margin > FB_MARGIN_MIN) {
            s_fb_margin -= FB_MARGIN_DECAY;
            if (s_fb_margin < FB_MARGIN_MIN)
                s_fb_margin = FB_MARGIN_MIN;
        }
    }
    s_fb_base = base;
    s_fb_prev_ti = s_fb_drew ? s_fb_ti : -1.0;
    s_fb_drew = 0;
    s_fb_probing = 0;
    if (++s_fb_ticks % FB_LOG_TICKS == 0)
        fb_log();
    s_fb_t0 = fb_now();
    if (fb_fits(s_fb_cinb, T)) {
        s_fb_streak = 0;
        return 1;
    }
    if (++s_fb_streak >= s_fb_probe_every && fb_fits(0.0, T)) {
        s_fb_streak = 0;
        s_fb_probing = 1;
        s_fb_probes++;
        return 1;
    }
    s_fb_skipped++;
    return 0;
}

/* The in-between is up, `held` seconds later than it was ready: the budget
 * learns when it is ready. */
static void fb_presented(double held)
{
    double c;

    s_fb_ti = fb_now();
    c = s_fb_ti - s_fb_t0 - held;
    if (s_fb_probing)
        s_fb_cinb = c;
    else
        s_fb_cinb += 0.1 * (c - s_fb_cinb);
    s_fb_drew = 1;
}

/* Wait until `until` on the tick's clock, or wall time `cap`: the clock can
 * be held (timebase_pause). It sleeps on a high-resolution timer until
 * FB_SPIN before the end and spins the rest, because the timer wakes up to
 * about half a millisecond late. Returns the time asleep. */
static double fb_wait(double until, double cap)
{
    double slept = 0.0;

    for (;;) {
        double left = until - fb_now(), wall = (cap - now_ms()) / 1000.0;
        LARGE_INTEGER due;

        if (wall < left)
            left = wall;
        if (left <= 0.0)
            break;
        due.QuadPart = -(LONGLONG)((left - FB_SPIN) * 1e7);
        if (s_fb_timer && due.QuadPart < 0 &&
            SetWaitableTimer(s_fb_timer, &due, 0, NULL, NULL, FALSE)) {
            double t = s_prof ? now_ms() : 0.0;

            WaitForSingleObject(s_fb_timer, (DWORD)(1000.0 * left) + 1);
            if (s_prof)
                slept += now_ms() - t;
            continue;
        }
        YieldProcessor();
    }
    return slept;
}

/* The real picture is ready: hold it to half a tick after the in-between, but
 * never past the deadline less the margin. */
static void fb_wait_real(void)
{
    double T = fb_tick(), e = fb_now(), until = s_fb_ti + T / 2.0;
    double w0 = now_ms(), slept;

    if (s_fb_probing) {
        s_fb_cpost = e - s_fb_ti;
        if (fb_fits(s_fb_cinb, T))
            s_fb_probe_every = FB_PROBE_MIN;
        else if (s_fb_probe_every < FB_PROBE_MAX)
            s_fb_probe_every *= 2;
    } else {
        s_fb_cpost += 0.1 * ((e - s_fb_ti) - s_fb_cpost);
    }
    if (e > until)
        s_ip_late++;
    if (until > T - s_fb_margin)
        until = T - s_fb_margin;
    slept = fb_wait(until, w0 + 1000.0 * T);
    if (s_prof) {
        pr_add(&s_pr_wait, now_ms() - w0);
        pr_add(&s_pr_asleep, slept);
        pr_add(&s_pr_overshoot, 1000.0 * (fb_now() - until));
    }
}

/* ---- the limiter's idle ------------------------------------------------------
 *
 * Both loops end `grBufferSwap(0); glcount_Advance; spin on the hi-res timer
 * until the tick's deadline`, which is a whole core at 30 Hz. The deadline is
 * the stamp _timer_ResetHiResTimer takes at the top of the loop plus T (Hydro
 * re-stamps after the swap and spins T less the elapsed, which ends at the
 * same point; Offroad's literal is the same 1/30 as the cell). After the
 * loop's swap the game thread sleeps to FB_IDLE_EARLY before that deadline
 * and the game's own spin does the rest, so the tick ends where it did. Once
 * per stamp: the loop's swap is the first of its tick, and the 60 Hz
 * in-between goes to the provider without passing here. */

/* Seconds on the limiter's clock since `stamp`. */
static double limiter_now(uint64_t stamp)
{
    return (double)(int64_t)(timebase_ticks() - stamp) / (double)timebase_hz();
}

static int      s_idle;
static uint64_t s_idle_stamp;
static double   s_idle_slept_ms, s_idle_latest_ms;
static unsigned s_idle_past;

void drawreplay_idle(int interval)
{
    uint64_t stamp;
    double T, until, w0, cap, speed, late;
    int slept = 0;

    if (!s_idle || interval != 0)
        return;
    stamp = fb_stamp();
    if (stamp == s_idle_stamp)
        return;
    s_idle_stamp = stamp;
    T = fb_tick();
    until = T - FB_IDLE_EARLY;
    speed = g_cfg.speed_percent ? g_cfg.speed_percent / 100.0 : 1.0;
    w0 = now_ms();
    cap = w0 + 1000.0 * T / speed;
    for (;;) {
        /* The limiter's clock runs at speed_percent; a held clock (a modal
         * loop) is bounded by the wall cap. */
        double left = (until - limiter_now(stamp)) / speed;
        double wall = (cap - now_ms()) / 1000.0;
        LARGE_INTEGER due;

        if (wall < left)
            left = wall;
        if (left <= 0.0)
            break;
        due.QuadPart = -(LONGLONG)(left * 1e7);
        if (!due.QuadPart ||
            !SetWaitableTimer(s_fb_timer, &due, 0, NULL, NULL, FALSE))
            return;
        WaitForSingleObject(s_fb_timer, (DWORD)(1000.0 * left) + 1);
        slept = 1;
    }
    if (!slept)
        return;
    s_idle_slept_ms += now_ms() - w0;
    late = 1000.0 * (limiter_now(stamp) - until);
    if (late > s_idle_latest_ms)
        s_idle_latest_ms = late;
    if (limiter_now(stamp) >= T)
        s_idle_past++;
}

/* Wall ms slept so far; of the ticks that slept, the latest wake past its
 * target and the wakes past the deadline itself, since the last call. */
double drawreplay_idle_ms(double *latest_ms, unsigned *past)
{
    *latest_ms = s_idle_latest_ms;
    *past = s_idle_past;
    s_idle_latest_ms = 0.0;
    s_idle_past = 0;
    return s_idle_slept_ms;
}

/* Every FB_LOG_TICKS ticks, drawn or not: a budget that skips them all is the
 * case this line exists for. */
static void fb_log(void)
{
    LOGI("drawinterp: frame budget: %llu tick(s), %llu in-between(s) skipped "
         "for time, %llu probe(s) while skipping, %llu tick(s) overran after "
         "one; margin %.1f ms, in-between %.1f ms, restore + real draw %.1f ms "
         "(tick %.1f ms)",
         (unsigned long long)s_fb_ticks, (unsigned long long)s_fb_skipped,
         (unsigned long long)s_fb_probes, (unsigned long long)s_fb_overran,
         1000.0 * s_fb_margin, 1000.0 * s_fb_cinb, 1000.0 * s_fb_cpost,
         1000.0 * fb_tick());
}

static void pr_present(int kind, double at, double swap_ms)
{
    if (s_pr_last_kind >= 0)
        pr_add(&s_pr_gap[s_pr_last_kind * 2 + kind], at - s_pr_last_present);
    s_pr_last_kind = kind;
    s_pr_last_present = at;
    pr_add(kind ? &s_pr_inbswap : &s_pr_realswap, swap_ms);
}

/* Every real present, from the game's own grBufferSwap. */
void drawreplay_swapped(uint64_t before, uint64_t after)
{
    if (s_prof)
        pr_present(0, qpc_ms(after), qpc_ms(after) - qpc_ms(before));
}

/* An in-between ready earlier into its tick than the tick before's, less
 * FB_RECOVER, is held to that point: after a late one, the pictures that
 * follow stay evenly spaced and walk back FB_RECOVER a tick, instead of one
 * short gap jumping back. The phase is the tick's own clock, which every
 * present's fixed offsets cancel out of. Never so late that the real
 * picture's half tick runs past the deadline less the margin. */
static void present_inbetween(void)
{
    double T = fb_tick(), t = now_ms(), u, held = 0.0;
    double until = s_fb_prev_ti - FB_RECOVER;

    if (until > T / 2.0 - s_fb_margin)
        until = T / 2.0 - s_fb_margin;
    if (s_fb_prev_ti >= 0.0 && until > fb_now()) {
        fb_wait(until, t + 500.0 * T);
        held = now_ms() - t;
        t += held;
    }
    s_swap(0);
    u = s_prof ? now_ms() : 0.0;
    fb_presented(held / 1000.0);
    if (s_prof) {
        pr_present(1, u, u - t);
        pr_add(&s_pr_phase, 1000.0 * s_fb_ti);
        pr_add(&s_pr_held, held);
    }
}

static int dbl_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

static void pr_line(const char *what, pr_set_t *s)
{
    double sum = 0.0;
    unsigned i, n = s->n;

    if (n) {
        qsort(s->v, n, sizeof s->v[0], dbl_cmp);
        for (i = 0; i < n; i++)
            sum += s->v[i];
        LOGI("drawprof: %-26s n %4u  min %7.3f  p50 %7.3f  p90 %7.3f  "
             "p99 %7.3f  max %8.3f  mean %7.3f ms%s", what, n, s->v[0],
             s->v[n / 2], s->v[n * 9 / 10], s->v[n * 99 / 100], s->v[n - 1],
             sum / n, s->over ? "  (SET FULL)" : "");
    }
    s->n = s->over = 0;
}

/* Gaps between presents within 1 and 2 ms of half a tick, of all. */
static void pr_spacing(const char *what, const pr_set_t *s, double half)
{
    unsigned i, in1 = 0, in2 = 0;

    for (i = 0; i < s->n; i++) {
        double d = fabs(s->v[i] - half);

        in1 += d <= 1.0;
        in2 += d <= 2.0;
    }
    if (s->n)
        LOGI("drawprof: %-26s %u of %u within 1 ms of %.2f, %u within 2 ms",
             what, in1, s->n, half, in2);
}

static void pr_report(void)
{
    double t = (double)(s_pc.ticks ? s_pc.ticks : 1);
    double half = 500.0 * fb_tick();
    static const char *const gap[4] = {
        "gap real -> real", "gap real -> in-between",
        "gap in-between -> real", "gap in-between -> in-b."
    };
    unsigned g;

    if (!s_prof)
        return;
    LOGI("drawprof: build_ranges %llu call(s), %llu from the reservation "
         "cache, %llu of them differing from a fresh walk (0 is the pass): "
         "%.3f ms VirtualQuery half, "
         "%.3f ms HeapWalk + sort half, per call; %.0f VirtualQuery calls, "
         "%.0f heap entries, %.0f merged ranges per call",
         (unsigned long long)s_pc.br_calls,
         (unsigned long long)s_pc.vq_cached, (unsigned long long)s_pc.vq_mismatch,
         s_pc.br_calls ? s_pr_br_vq / (double)s_pc.br_calls : 0.0,
         s_pc.br_calls ? s_pr_br_heap / (double)s_pc.br_calls : 0.0,
         s_pc.br_calls ? (double)s_pc.vq_calls / (double)s_pc.br_calls : 0.0,
         s_pc.br_calls ? (double)s_pc.heap_entries / (double)s_pc.br_calls : 0.0,
         s_pc.br_calls ? (double)s_pc.ranges / (double)s_pc.br_calls : 0.0);
    pr_line("build_ranges", &s_pr_br);
    pr_line("snapshot+blend (uncounted)", &s_pr_snap);
    pr_line("word loop", &s_pr_words);
    LOGI("drawprof: per blend tick: %.0f changed chunks timed (about %.3f ms "
         "of timer in the word-loop figure), %.0f changed words; excluded() "
         "%.0f calls, %.0f hits, %.0f of %.0f chunks overlap no excluded "
         "range; never_blend %.0f calls, %.0f hits; %.0f not floats; "
         "game_address %.0f calls, %.0f in the image, %.0f outside every "
         "range, %.0f true; %llu word(s) where the blend's check disagreed "
         "with the reference (0 is the pass)",
         (double)s_pc.timed / t, (double)s_pc.timed / t * s_prof_pair_ms,
         (double)s_pc.changed / t,
         (double)s_pc.ex_calls / t, (double)s_pc.ex_hits / t,
         (double)s_pc.ex_free_chunks / t, (double)s_pc.ex_chunks / t,
         (double)s_pc.nb_calls / t, (double)s_pc.nb_hits / t,
         (double)s_pc.not_float / t, (double)s_pc.ga_calls / t,
         (double)s_pc.ga_image / t, (double)s_pc.ga_outside / t,
         (double)s_pc.ga_true / t, (unsigned long long)s_pc.disagree);
    pr_line("hold side threads", &s_pr_hold);
    pr_line("in-between draw", &s_pr_draw);
    pr_line("in-between held to space", &s_pr_held);
    pr_line("in-between at, into tick", &s_pr_phase);
    for (g = 1; g < 3; g++)
        pr_spacing(gap[g], &s_pr_gap[g], half);
    for (g = 0; g < 4; g++)
        pr_line(gap[g], &s_pr_gap[g]);
    pr_line("real picture's wait", &s_pr_wait);
    pr_line("... of it asleep", &s_pr_asleep);
    pr_line("... ended past its target", &s_pr_overshoot);
    pr_line("in-between swap call", &s_pr_inbswap);
    pr_line("real swap call", &s_pr_realswap);
    memset(&s_pc, 0, sizeof s_pc);
    s_pr_br_vq = s_pr_br_heap = 0.0;
}

static int guarded(void (*fn)(int), int parm);
static int s_inb_mode;
static void inbetween_draw(int parm) { draw_recorded(parm, s_inb_mode); }

/* One tick: the in-between picture, presented; the restore; the real picture,
 * presented half a tick later by the loop's own swap. */
static void sample_interp(int parm)
{
    double t0 = now_ms(), ti, tw, th;
    int inb_ok, room = fb_begin();

    rt_take();

    if (!room || !s_have_prev || !build_ranges()) {
        if (s_state_mode && build_ranges()) {
            snapshot(&s_before);
            s_have_prev_state = keep_prev();
            memcpy(s_sky_args[0], s_sky_args[1], sizeof s_sky_args[0]);
        } else {
            s_have_prev_state = 0;     /* never blend across a lost tick */
        }
        draw_recorded(parm, 1);
        camera_capture_prev();
        s_rec = !s_rec;
        s_have_prev = 1;
        return;
    }
    th = now_ms();
    game_hold_side_threads(1);
    win32_hold_frees(1);
    audio_hold_this_thread(1);
    tw = now_ms();
    if (s_prof)
        pr_add(&s_pr_hold, tw - th);
    if (s_state_mode && s_have_prev_state) {
        snapshot_and_blend();
        rt_fix();
    } else {
        snapshot(&s_before);
    }
    s_ip_ms_snap += now_ms() - tw;
    s_get_state(s_state_blob);
    if (s_state_mode && s_have_prev_state) {
        if (s_orig_sky && s_sky_seen > 1) {
            s_ip_mode = 3;             /* not a Work call: do not record it */
            s_orig_sky(lerp_angle(s_sky_args[0][0], s_sky_args[1][0]),
                       lerp_angle(s_sky_args[0][1], s_sky_args[1][1]),
                       lerp_angle(s_sky_args[0][2], s_sky_args[1][2]));
            s_ip_mode = 0;
        }
    }
    s_inb_mode = s_state_mode ? 3 : 2;
    s_cut_checked = 0;
    spec_begin();
    th = now_ms();
    inb_ok = guarded(inbetween_draw, parm);
    if (s_prof)
        pr_add(&s_pr_draw, now_ms() - th);
    s_ip_mode = 0;
    if (inb_ok)
        present_inbetween();
    spec_end();
    ti = now_ms();
    mark_dirty(&s_before);
    restore_dirty(s_before.p);
    s_ip_ms_restore += now_ms() - ti;
    audio_hold_this_thread(0);
    win32_hold_frees(0);
    game_hold_side_threads(0);
    s_set_state(s_state_blob);
    if (s_state_mode)
        s_have_prev_state = keep_prev();
    memcpy(s_sky_args[0], s_sky_args[1], sizeof s_sky_args[0]);
    draw_recorded(parm, 1);
    camera_capture_prev();
    s_rec = !s_rec;
    if (inb_ok)
        fb_wait_real();
    s_ip_ticks++;
    s_ip_ms_present += ti - t0;
    s_ip_ms_total += now_ms() - t0;
    if (s_ip_ticks % 300 == 0) {
        if (s_state_mode)
            LOGI("drawinterp: state blend %.0f words a tick, %.0f skipped "
                 "(not floats, addresses, or jumps)",
                 (double)s_ds_words / 300.0, (double)s_ds_skipped / 300.0);
        LOGI("drawinterp: %u voice command(s) from in-between draws dropped "
             "over 300 ticks; %llu camera cut(s) skipped so far; %u stepped "
             "texture range(s) held out of the blend", audio_held_commands(),
             (unsigned long long)s_cuts, s_nex);
        s_ds_words = s_ds_skipped = 0;
        if (rt_spray_ok() || rt_splash_bytes() || rt_fxpart_bytes())
            LOGI("drawinterp: effect records redrawn a tick: spray %.2f "
                 "from another slot, %.2f new; splash %.2f new; fxpart %.2f "
                 "new", (double)s_rt_moved / 300.0, (double)s_rt_new / 300.0,
                 (double)s_rt_splash_new / 300.0,
                 (double)s_rt_fxpart_new / 300.0);
        s_rt_moved = s_rt_new = s_rt_splash_new = s_rt_fxpart_new = 0;
        LOGI("drawinterp: %llu ticks; per tick %.1f blended pushes, %.2f "
             "camera blends, %.1f unkeyed, %.2f refused (jump/type), %.2f "
             "refused (rotation); "
             "in-between presented "
             "%.2f ms into the tick, tick work %.2f ms; %llu late so far; "
             "largest blended jump %.2f units",
             (unsigned long long)s_ip_ticks,
             (double)s_ip_blend / 300.0, (double)s_ip_cam / 300.0,
             (double)s_ip_nokey / 300.0, (double)s_ip_reject / 300.0,
             (double)s_ip_rotref / 300.0,
             s_ip_ms_present / 300.0, s_ip_ms_total / 300.0,
             (unsigned long long)s_ip_late, sqrt((double)s_ip_maxjump));
        ww_log();
        pr_report();
        s_ip_blend = s_ip_cam = s_ip_nokey = s_ip_reject = 0;
        s_ip_rotref = s_ip_byaddr = s_ip_bystack = 0;
        if (s_ip_ticks == 900) {
            unsigned i;

            for (i = 0; i < s_sites; i++)
                LOGI("drawinterp: site 0x%08X  %u pushes, %u (unused), "
                     "%u unkeyed, %u jump/type, %u rotation",
                     s_site[i].ret, s_site[i].n, s_site[i].stack,
                     s_site[i].nokey, s_site[i].jump, s_site[i].rot);
        }
        s_ip_ms_present = s_ip_ms_total = 0.0;
        s_ip_maxjump = 0.0f;
    }
}

/* ---- Offroad: the draw phase as a derived script ----------------------------
 *
 * Offroad inlines its draw phase in `_gameloop_Start` and has no draw function
 * to call. gen-gamedefs.py:d_draw_script() reads the run from
 * `call _shadow_PerFrameStart` to `call _grBufferSwap` out of the binary as a
 * script, and this replays it. The first call site is patched to come here:
 * the in-between picture is drawn and presented, the game's memory restored,
 * and then the real `_shadow_PerFrameStart` runs and the game's own inline
 * draw carries on. The swap site is patched to wait for half a tick. */
typedef uint32_t (__cdecl *gfn0_t)(void);
typedef uint32_t (__cdecl *gfn1_t)(uint32_t);
typedef void (__cdecl *gswap_t)(int);

static int      s_or_replay_pending;
static uint32_t s_or_c0[VCT_MAX_GLIDE], s_or_c1[VCT_MAX_GLIDE];

static void run_script(void)
{
    uint32_t ret = 0;
    unsigned i = 0, guard = 0;

    while (i < G->draw_step_count && guard++ < 64) {
        const uint32_t *st = &G->draw_step[i * 4];

        switch (st[0]) {
        case 1:
            if (st[2] == 0)
                ret = ((gfn0_t)(uintptr_t)st[1])();
            else
                ret = ((gfn1_t)(uintptr_t)st[1])(st[2] == 1 ? st[3] : ret);
            i++;
            break;
        case 2:
            if (*(volatile uint32_t *)(uintptr_t)st[1])
                return;
            i++;
            break;
        case 3:
            i = *(volatile uint32_t *)(uintptr_t)st[1] ? i + 1 : st[2];
            break;
        default:
            return;
        }
    }
}

/* ---- the in-between draw is speculative: a fault abandons it ---------------
 *
 * A blended state can be inconsistent in ways no word filter sees (Offroad's
 * _worldob_BuildLightList followed a node pointer that read as -0.25). The
 * in-between picture is optional, so a fault inside its draw drops it: the
 * handler, on the drawing thread only, jumps back to the guard; the caller
 * restores memory as it always does and the real picture goes ahead. Only a
 * fault in the game's own code is taken, or a call it made through a pointer
 * to nowhere: one in our code or the renderer is a real crash and stays one.
 * The SEH chain, the x87 unit and our own scope state (widescreen, the DCS
 * mask, the cli depth) are put back by hand, because the jump skips every
 * frame between and every leave step in them. Each fault is counted and the
 * first ones are named. */
#include <setjmp.h>

static jmp_buf        s_gj;
static volatile DWORD s_guard_tid;
static uint32_t       s_guard_seh;
static uint16_t       s_guard_fcw;
static volatile LONG  s_faults;
/* A fault's registers and the top of its stack: what a later look at the
 * disassembly needs to say which record the draw was reading. */
#define FAULT_STACK 12
static struct {
    uint32_t eip, addr, code;
    uint32_t reg[8];                   /* eax ebx ecx edx esi edi ebp esp */
    uint32_t stack[FAULT_STACK];       /* 0xFFFFFFFF where unreadable */
} s_fault_log[8];

/* The game's code faulted, or it called through a bad pointer: execution
 * landed where the fault is, and the return address on top of the stack is
 * the game's. */
static int game_fault(const EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint32_t eip = (uint32_t)ep->ContextRecord->Eip, ret;

    if (diag_in_game(eip))
        return 1;
    return er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
           er->NumberParameters >= 2 &&
           (uint32_t)er->ExceptionInformation[1] == eip &&
           diag_peek32((uint32_t)ep->ContextRecord->Esp, &ret) &&
           diag_in_game(ret);
}

static LONG CALLBACK guard_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    LONG n;

    if (!s_guard_tid || s_guard_tid != GetCurrentThreadId())
        return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!game_fault(ep))
        return EXCEPTION_CONTINUE_SEARCH;
    n = InterlockedIncrement(&s_faults) - 1;
    if (n < 8) {
        s_fault_log[n].eip = (uint32_t)ep->ContextRecord->Eip;
        s_fault_log[n].addr = ep->ExceptionRecord->NumberParameters >= 2 ?
            (uint32_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
        s_fault_log[n].code = (uint32_t)code;
        {
            const CONTEXT *c = ep->ContextRecord;
            int i;

            s_fault_log[n].reg[0] = (uint32_t)c->Eax;
            s_fault_log[n].reg[1] = (uint32_t)c->Ebx;
            s_fault_log[n].reg[2] = (uint32_t)c->Ecx;
            s_fault_log[n].reg[3] = (uint32_t)c->Edx;
            s_fault_log[n].reg[4] = (uint32_t)c->Esi;
            s_fault_log[n].reg[5] = (uint32_t)c->Edi;
            s_fault_log[n].reg[6] = (uint32_t)c->Ebp;
            s_fault_log[n].reg[7] = (uint32_t)c->Esp;
            for (i = 0; i < FAULT_STACK; i++)
                if (!diag_peek32((uint32_t)c->Esp + 4u * (uint32_t)i,
                                 &s_fault_log[n].stack[i]))
                    s_fault_log[n].stack[i] = 0xFFFFFFFFu;
        }
    }
    s_guard_tid = 0;
    longjmp(s_gj, 1);
}

/* A camera cut found while drawing: abandon the in-between picture. A blend
 * across a cut is a camera neither tick had (Hydro's attract once showed a
 * sunset between two shots of a canyon). */
static void abort_cut(void)
{
    if (!s_guard_tid || s_guard_tid != GetCurrentThreadId())
        return;
    s_guard_tid = 0;
    s_cuts++;
    longjmp(s_gj, 2);
}

/* Run fn(parm) with the guard; 1 if it completed, 0 if it faulted. */
static int guarded(void (*fn)(int), int parm)
{
    static int armed;
    uint32_t seh;
    volatile int jv;
    ws_scopes_t ws;
    int mask, irq;

    if (!armed) {
        AddVectoredExceptionHandler(1, guard_handler);
        armed = 1;
    }
    __asm__ volatile("movl %%fs:0, %0" : "=r"(seh));
    s_guard_seh = seh;
    __asm__ volatile("fnstcw %0" : "=m"(s_guard_fcw));
    ws_scopes_save(&ws);
    mask = audio_mask_depth();
    irq = game_irq_depth();
    if ((jv = setjmp(s_gj)) == 0) {
        s_guard_tid = GetCurrentThreadId();
        fn(parm);
        s_guard_tid = 0;
        __asm__ volatile("fldcw %0" : : "m"(s_guard_fcw));
        return 1;
    }
    seh = s_guard_seh;
    __asm__ volatile("movl %0, %%fs:0" : : "r"(seh) : "memory");
    __asm__ volatile("fninit\n\tfldcw %0" : : "m"(s_guard_fcw));
    ws_scopes_restore(&ws);
    audio_mask_unwind(mask);
    game_irq_unwind(irq);
    if (jv == 2)
        return 0;                       /* a camera cut, not a fault */
    {
        LONG n = s_faults;

        if (n <= 8)
            LOGW("drawinterp: in-between draw #%ld FAULTED (0x%08X at eip "
                 "0x%08X, address 0x%08X); that picture is dropped and the "
                 "game's memory restored", (long)n,
                 s_fault_log[n - 1].code, s_fault_log[n - 1].eip,
                 s_fault_log[n - 1].addr);
        if (n <= 8) {
            const uint32_t *r = s_fault_log[n - 1].reg;
            const uint32_t *k = s_fault_log[n - 1].stack;
            char st[FAULT_STACK * 9 + 1];
            int i, len = 0;

            for (i = 0; i < FAULT_STACK; i++)
                len += snprintf(st + len, sizeof st - (size_t)len, " %08X",
                                k[i]);
            LOGW("drawinterp: fault #%ld registers eax %08X ebx %08X ecx %08X "
                 "edx %08X esi %08X edi %08X ebp %08X esp %08X; stack%s",
                 (long)n, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], st);
        }
    }
    return 0;
}

static void script_draw(int unused) { (void)unused; run_script(); }

static void call_phase_start(void)
{
    ((gfn0_t)(uintptr_t)G->draw_step[1])();
}

/* Diagnostic: log a span of words each in-between tick, as tick N, tick N+1
 * and what the blend left in memory. [diagnostics] draw_interp_trace = VA,
 * draw_interp_trace_words = n. Off unless asked. */
static const uint32_t *span_in(const unsigned char *buf, const rng_t *rt,
                               unsigned nr, uint32_t va, uint32_t len)
{
    unsigned i;
    size_t off = 0;

    for (i = 0; i < nr; i++) {
        if (va >= rt[i].lo && va + len <= rt[i].hi)
            return (const uint32_t *)(buf + off + (va - rt[i].lo));
        off += rt[i].hi - rt[i].lo;
    }
    return NULL;
}

static void trace_span(void)
{
    uint32_t va = g_cfg.draw_interp_trace, n = g_cfg.draw_interp_trace_words, i;
    const uint32_t *pv, *cv;
    char line[1024];
    int len = 0;

    if (!va || !n || !s_prev.p)
        return;
    pv = span_in(s_prev.p, s_prev_rng, s_prev_nrng, va, n * 4);
    cv = span_in(s_before.p, s_rng, s_nrng, va, n * 4);
    if (!pv || !cv)
        return;
    for (i = 0; i < n && len < (int)sizeof line - 64; i++) {
        float a, b, c;
        uint32_t lv = ((const uint32_t *)(uintptr_t)va)[i];

        memcpy(&a, &pv[i], 4);
        memcpy(&b, &cv[i], 4);
        memcpy(&c, &lv, 4);
        len += snprintf(line + len, sizeof line - len, " [%u] %.7g>%.7g=%.7g",
                        i, (double)a, (double)b, (double)c);
    }
    LOGI("drawinterp: trace %llu:%s", (unsigned long long)s_ip_ticks, line);
}

/* Offroad builds its camera in Work, so the cut is visible in the snapshots:
 * the transform stack's element 0 is the camera. Turned by more than 30
 * degrees in a tick is a cut, and so is moved further than DI_JUMP. */
static int camera_cut_in_snapshots(void)
{
    const uint32_t *pv, *cv;
    float p[24], c[24], tr;
    unsigned i;

    if (!G->xfm_stack || !s_prev.p)
        return 0;
    pv = span_in(s_prev.p, s_prev_rng, s_prev_nrng, G->xfm_stack, sizeof p);
    cv = span_in(s_before.p, s_rng, s_nrng, G->xfm_stack, sizeof c);
    if (!pv || !cv)
        return 0;
    memcpy(p, pv, sizeof p);
    memcpy(c, cv, sizeof c);
    /* trace(P^T C) = 1 + 2 cos(angle); cos 30 deg = 0.866 */
    for (i = 0, tr = 0.0f; i < 9; i++)
        tr += p[i] * c[i];
    return tr < 1.0f + 2.0f * 0.866f ||
           camera_jumped(p + CAM_POS, c + CAM_POS, 1.0f);
}

/* The in-between picture: blend, draw by the script, present, restore. */
static void or_interp(void)
{
    double t0 = now_ms(), ti, tw, th;
    int ok, room = fb_begin();

    /* A movie is a decoded picture at its own rate, and its stream fills a
     * buffer in game memory from the moment it opens, before the player's own
     * flag is set: a restore inside that window rolls the stream's progress
     * back and the attract froze at OpenMovie. Stand aside while either is
     * set, and seed afresh afterwards rather than blend across the movie. */
    if ((G->mvplayer_inmovie &&
         *(volatile uint32_t *)(uintptr_t)G->mvplayer_inmovie) ||
        (G->stream_file &&
         *(volatile uint32_t *)(uintptr_t)G->stream_file)) {
        if (s_have_prev_state)
            LOGI("drawinterp: a movie is playing; no in-between pictures "
                 "until it ends");
        s_have_prev_state = 0;
        return;
    }

    if (!room || !s_have_prev_state || !build_ranges()) {
        if (build_ranges()) {
            snapshot(&s_before);
            s_have_prev_state = keep_prev();
        } else {
            s_have_prev_state = 0;     /* never blend across a lost tick */
        }
        return;
    }
    th = now_ms();
    game_hold_side_threads(1);
    win32_hold_frees(1);
    audio_hold_this_thread(1);
    tw = now_ms();
    if (s_prof)
        pr_add(&s_pr_hold, tw - th);
    snapshot_and_blend();
    s_ip_ms_snap += now_ms() - tw;
    trace_span();
    s_get_state(s_state_blob);
    s_ip_mode = 3;
    spec_begin();
    if (camera_cut_in_snapshots()) {
        s_cuts++;
        ok = 0;
    } else {
        th = now_ms();
        ok = guarded(script_draw, 0);
        if (s_prof)
            pr_add(&s_pr_draw, now_ms() - th);
    }
    s_ip_mode = 0;
    if (ok)
        present_inbetween();
    spec_end();
    ti = now_ms();
    mark_dirty(&s_before);
    restore_dirty(s_before.p);
    s_ip_ms_restore += now_ms() - ti;
    audio_hold_this_thread(0);
    win32_hold_frees(0);
    game_hold_side_threads(0);
    s_set_state(s_state_blob);
    s_have_prev_state = keep_prev();
    s_ip_ticks++;
    s_ip_ms_present += ti - t0;
    if (s_ip_ticks % 300 == 0) {
        LOGI("drawinterp: state blend %.0f words a tick, %.0f skipped "
             "(not floats, addresses, or jumps)",
             (double)s_ds_words / 300.0, (double)s_ds_skipped / 300.0);
        LOGI("drawinterp: %llu ticks; in-between presented %.2f ms into the "
             "draw phase; %u voice command(s) dropped; so far %llu late, "
             "%llu camera cut(s) skipped, HUD unblended %llu words (%llu "
             "kept), %llu in-between draw(s) abandoned at a movie",
             (unsigned long long)s_ip_ticks, s_ip_ms_present / 300.0,
             audio_held_commands(), (unsigned long long)s_ip_late,
             (unsigned long long)s_cuts, (unsigned long long)s_unblend_words,
             (unsigned long long)s_unblend_kept,
             (unsigned long long)s_movie_aborts);
        ww_log();
        pr_report();
        s_ds_words = s_ds_skipped = 0;
        s_ip_ms_present = 0.0;
    }
}

/* Replay test: draw #1 by the script, restore, and draw #2 is the game's own
 * inline draw; or_swap() compares them. */
static void or_replay_begin(void)
{
    unsigned gn = (unsigned)G->glide_count;

    if (gn > VCT_MAX_GLIDE)
        gn = VCT_MAX_GLIDE;
    if (s_skip) {
        s_skip--;
        return;
    }
    if (!s_budget || !build_ranges()) {
        if (!s_budget)
            report();
        return;
    }
    game_hold_side_threads(1);
    win32_hold_frees(1);
    audio_hold_this_thread(1);
    snapshot(&s_before);
    s_get_state(s_state_blob);
    memcpy(s_or_c0, glide_call_counts(), gn * sizeof s_or_c0[0]);
    watch(1);
    spec_begin();
    run_script();                                        /* draw #1          */
    watch(0);
    memcpy(s_or_c1, glide_call_counts(), gn * sizeof s_or_c1[0]);
    s_dirty_chunks += mark_dirty(&s_before);
    snapshot(&s_after);
    s_swap(0);
    spec_end();
    restore_dirty(s_before.p);
    s_set_state(s_state_blob);
    watch(1);
    s_or_replay_pending = 1;          /* draw #2 is the inline draw, next */
}

static void or_replay_end(void)
{
    const uint32_t *counts = glide_call_counts();
    unsigned g, gn = (unsigned)G->glide_count;
    size_t d;
    int calls = 0;

    watch(0);
    s_or_replay_pending = 0;
    if (gn > VCT_MAX_GLIDE)
        gn = VCT_MAX_GLIDE;
    d = diff_dwords(s_after.p);
    audio_hold_this_thread(0);
    win32_hold_frees(0);
    game_hold_side_threads(0);
    for (g = 0; g < gn; g++) {
        uint32_t d1 = s_or_c1[g] - s_or_c0[g], d2 = counts[g] - s_or_c1[g];

        if (d1 != d2) {
            calls = 1;
            s_entry_hits[g]++;
            s_entry_d1[g] += d1;
            s_entry_d2[g] += d2;
        }
    }
    s_diff_dwords += d;
    if (d)
        s_state_differs++;
    if (calls)
        s_calls_differ++;
    if (s_samples < 3)
        LOGI("drawreplay: sample %llu: %u dwords differ after the inline draw; "
             "call mix %s", (unsigned long long)s_samples, (unsigned)d,
             calls ? "DIFFERS" : "same");
    s_samples++;
    if (!--s_budget)
        report();
}

static void __cdecl or_phase_begin(void)
{
    if (s_interp)
        or_interp();
    else
        or_replay_begin();
    call_phase_start();
}

static void __cdecl or_swap(int interval)
{
    if (s_or_replay_pending)
        or_replay_end();
    if (s_interp && s_fb_drew)
        fb_wait_real();
    ((gswap_t)(uintptr_t)G->grbufferswap)(interval);
}

static int install_script(unsigned samples, int interp)
{
    if (!G->draw_step_count || !G->draw_phase_site || !G->draw_swap_site ||
        G->draw_step[0] != 1 || !G->grbufferswap) {
        LOGE("drawreplay: INAPPLICABLE to %s: no draw-function setter and no "
             "derived draw script, so there is no draw to bracket", G->id);
        return 0;
    }
    if (!patch_call(G->draw_phase_site, (void *)or_phase_begin) ||
        !patch_call(G->draw_swap_site, (void *)or_swap)) {
        LOGE("drawreplay: could not patch the draw phase's two call sites");
        return 0;
    }
    s_interp = interp;
    s_state_mode = interp;
    LOGI("drawreplay: %s's draw phase is a derived script of %u steps; "
         "0x%08X begins it and 0x%08X presents it", G->id, G->draw_step_count,
         G->draw_phase_site, G->draw_swap_site);
    if (interp)
        LOGI("drawinterp: ON, blending the game's STATE; every tick presents "
             "a scripted in-between picture and then the game's own, half a "
             "tick apart");
    else
        LOGI("drawreplay: replaying the %u draw phases after the first %u",
             samples, s_skip);
    return 1;
}

static void __cdecl dr_draw(int parm)
{
    if (s_skip && !s_interp) {
        s_skip--;
        s_game_draw(parm);
        return;
    }
    if (s_interp) {
        sample_interp(parm);
        return;
    }
    if (!s_budget || !build_ranges()) {
        s_game_draw(parm);
        if (!s_budget)
            report();
        return;
    }
    if (s_control)
        sample_control(parm);
    else
        sample_replay(parm);
    s_samples++;
    if (!--s_budget)
        report();
}

/* The loop's own `call [_Gameloop_pDrawFunc]` comes here, so every draw
 * function is bracketed: the race's, which _gameloop_SetDrawFunc installs, and
 * attract's default, which _gameloop_ModuleInit stores without the setter. */
static void __cdecl dr_site(int parm)
{
    s_game_draw = *(drawfn_t *)(uintptr_t)G->gameloop_pdrawfunc;
    if (s_game_draw)
        dr_draw(parm);
}

int drawreplay_install(unsigned samples, unsigned skip, int control,
                       int interp, int interp_state)
{
    if (g_cfg.draw_replay_watch && g_cfg.draw_replay_watch_pages) {
        s_watch_lo = g_cfg.draw_replay_watch & ~0xFFFu;
        s_watch_hi = s_watch_lo + 0x1000u * g_cfg.draw_replay_watch_pages;
        AddVectoredExceptionHandler(1, dw_handler);
    }
    if (interp || g_cfg.limiter_idle)
        s_fb_timer = CreateWaitableTimerExW(NULL, NULL,
                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (g_cfg.limiter_idle) {
        if (!G->timer_base_hi || !G->timer_base_lo ||
            !G->gameloop_target_frame_time)
            LOGI("limiter: INAPPLICABLE to %s, no limiter stamp or deadline; "
                 "the game thread spins out its ticks", G->id);
        else if (!s_fb_timer)
            LOGW("limiter: no high-resolution timer (error %lu); the game "
                 "thread spins out its ticks", GetLastError());
        else {
            s_idle = 1;
            LOGI("limiter: after its swap the game thread sleeps to %.1f ms "
                 "before the tick's deadline instead of spinning",
                 1000.0 * FB_IDLE_EARLY);
        }
    }
    if (!samples && !interp)
        return 1;
    if (interp && g_cfg.draw_interp_profile) {
        double sum = 0.0;
        int i;

        for (i = 0; i < 1000; i++) {
            double a = now_ms();

            sum += now_ms() - a;
        }
        s_prof_pair_ms = sum / 1000.0;
        s_prof = 1;
        LOGI("drawprof: ON ([diagnostics] draw_interp_profile); one timer "
             "pair costs %.3f us here, and the snapshot time it logs leaves "
             "its counting pass out", 1000.0 * s_prof_pair_ms);
    }
    if (interp) {
        if (s_fb_timer)
            LOGI("drawinterp: the real picture's wait sleeps on a "
                 "high-resolution timer and spins its last %.1f ms",
                 1000.0 * FB_SPIN);
        else
            LOGW("drawinterp: no high-resolution timer (error %lu); the real "
                 "picture's wait spins", GetLastError());
    }
    if (interp &&
        (!game_require("drawinterp", "_timer_ResetHiResTimer's stamp (high)",
                       G->timer_base_hi) ||
         !game_require("drawinterp", "_timer_ResetHiResTimer's stamp (low)",
                       G->timer_base_lo) ||
         !game_require("drawinterp", "_Gameloop_fTargetFrameTime",
                       G->gameloop_target_frame_time)))
        return 0;
    s_get_state = (state_fn)glide_provider_proc("_grGlideGetState@4");
    s_set_state = (state_fn)glide_provider_proc("_grGlideSetState@4");
    s_swap = (swap_fn)glide_provider_proc("_grBufferSwap@4");
    s_spec_begin = (spec_fn)glide_provider_proc("vglSpecBegin@0");
    s_spec_end = (spec_fn)glide_provider_proc("vglSpecEnd@0");
    if (!s_spec_begin || !s_spec_end)
        LOGW("drawreplay: the provider keeps no texture journal; uploads made "
             "for a discarded picture will stay in texture memory");
    if (interp && G->mesh3d_slidest && G->hook_mesh3d_slidest) {
        s_orig_slidest = (slidest_fn)hook_call_through(G->mesh3d_slidest,
                             (void *)di_slidest, G->hook_mesh3d_slidest);
        if (!s_orig_slidest)
            LOGW("drawinterp: could not hook _mesh3d_SlideST; stepped "
                 "texture animations will be blended");
    }
    if (interp && G->mvplayer_openmovie && G->hook_mvplayer_openmovie) {
        s_orig_openmovie = (openmovie_fn)hook_call_through(G->mvplayer_openmovie,
                               (void *)di_openmovie, G->hook_mvplayer_openmovie);
        if (!s_orig_openmovie) {
            LOGE("drawinterp: could not hook _mvplayer_OpenMovie, and an "
                 "in-between draw that opens a movie hangs the attract; 60 Hz "
                 "is off for %s", G->id);
            return 0;
        }
    }
    if (interp && G->noblend_pool_a) {
        if (!G->linklist_initpool || !G->hook_linklist_initpool ||
            !(s_orig_initpool = (initpool_fn)hook_call_through(
                  G->linklist_initpool, (void *)di_initpool,
                  G->hook_linklist_initpool)))
            LOGW("drawinterp: could not hook _linklist_InitPool; recycled "
                 "effect pools will be blended and can draw lines");
    }
    if (interp && G->hud_draw && G->hook_hud_draw) {
        s_orig_hud = (hud_fn)hook_call_through(G->hud_draw, (void *)di_hud_draw,
                                               G->hook_hud_draw);
        if (!s_orig_hud)
            LOGW("drawinterp: could not hook _hud_Draw; the HUD stays blended");
    } else if (interp) {
        LOGW("drawinterp: %s has no _hud_Draw to hook; the HUD stays blended",
             G->id);
    }
    if (!s_get_state || !s_set_state || !s_swap) {
        LOGE("drawreplay: the provider does not export Get/SetState and "
             "BufferSwap; refusing");
        return 0;
    }
    if (!G->gameloop_draw_site) {
        s_budget = samples;
        s_skip = skip;
        s_control = 0;
        s_verify = !interp;
        if (!install_script(samples, interp))
            return 0;
        win32_write_watch(1);
        return 1;
    }
    if (!game_require("drawreplay", "_Gameloop_pDrawFunc",
                      G->gameloop_pdrawfunc))
        return 0;
    if (!patch_call_indirect(G->gameloop_draw_site, G->gameloop_pdrawfunc,
                             (void *)dr_site)) {
        LOGE("drawreplay: could not patch the loop's draw call");
        return 0;
    }
    if (interp) {
        if (!game_require("drawinterp", "_xfm_Push", G->xfm_push) ||
            !game_require("drawinterp", "a relocatable _xfm_Push prologue",
                          G->hook_xfm_push) ||
            !game_require("drawinterp", "_xfm_InitCameraRot",
                          G->xfm_initcamerarot) ||
            !game_require("drawinterp", "a relocatable _xfm_InitCameraRot "
                          "prologue", G->hook_xfm_initcamerarot))
            return 0;
        s_tab[0] = (di_slot_t *)calloc(DI_SLOTS, sizeof(di_slot_t));
        s_tab[1] = (di_slot_t *)calloc(DI_SLOTS, sizeof(di_slot_t));
        if (!s_tab[0] || !s_tab[1])
            return 0;
        if (!interp_state) {
            s_recs[0] = (di_rec_t *)calloc(DI_RECS, sizeof(di_rec_t));
            s_recs[1] = (di_rec_t *)calloc(DI_RECS, sizeof(di_rec_t));
            if (!s_recs[0] || !s_recs[1])
                return 0;
        }
        s_orig_push = (push_fn)hook_call_through(G->xfm_push,
                          (void *)di_push, G->hook_xfm_push);
        s_orig_camrot = (camrot_fn)hook_call_through(G->xfm_initcamerarot,
                          (void *)di_camrot, G->hook_xfm_initcamerarot);
        if (!s_orig_push || !s_orig_camrot) {
            LOGE("drawinterp: could not hook the transform stack");
            return 0;
        }
        s_state_mode = interp_state;
        if (interp_state && game_require("drawinterp", "_sky_Work", G->sky_work) &&
            game_require("drawinterp", "a relocatable _sky_Work prologue",
                         G->hook_sky_work)) {
            s_orig_sky = (skywork_fn)hook_call_through(G->sky_work,
                             (void *)di_sky_work, G->hook_sky_work);
            if (!s_orig_sky)
                LOGW("drawinterp: could not hook _sky_Work; the sky keeps "
                     "only the state blend");
        }
        s_interp = 1;
        LOGI("drawinterp: blending %s", interp_state ?
             "the game's STATE ([graphics] frame_rate = 60); camera angles and the "
             "sky by their arguments" : "the draw's matrices and camera");
        LOGI("drawinterp: ON. Every race tick presents a blended in-between "
             "picture and then the real one, half a tick apart, in race and attract");
    }
    s_budget = samples;
    s_skip = skip;
    s_control = control;
    s_verify = !interp && !control;
    win32_write_watch(1);
    LOGI("drawreplay: %s the %u draws after the first %u",
         control ? "CONTROL, no draws, over" : "replaying", samples, skip);
    return 1;
}
