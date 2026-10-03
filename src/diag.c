/* diag.c -- the primitives every instrument in this host needs.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The vocabulary a report is written in: a dword read that cannot fault, an
 * address rendered as `module+offset` so a finished run's log can still be
 * read, a validated frame-pointer walk, and the raw timestamp with its fenced
 * form. AN INSTRUMENT TAKES THESE FROM HERE AND KEEPS NONE OF ITS OWN, because
 * a private copy is one that cannot be fixed for the others. Nothing here
 * patches, allocates or logs on a hot path.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>

/* Read one dword from this process without ever faulting.
 *
 * NOT IsBadReadPtr: it PROBES, and on a bogus pointer the access violation it
 * raises reaches this host's own exception logger and abandons the report
 * before it prints, the instrument destroyed by the thing it is inspecting.
 * Every caller here runs when something is already going wrong, so that is the
 * one failure this cannot have. ReadProcessMemory on ourselves answers the same
 * question and returns FALSE instead of faulting. */
int diag_peek32(uint32_t addr, uint32_t *out)
{
    SIZE_T got = 0;

    if (addr < 0x10000u || (addr & 3u))
        return 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr,
                             out, sizeof *out, &got) && got == sizeof *out;
}

/* Write "0xADDR(module+off)" for an address, so a report is readable on its own.
 *
 * ASLR relocates every DLL, so a bare address from a finished run cannot be
 * matched to a module afterwards: `0x6842754D` alone names nothing. An
 * instrument whose output needs the process it came from is half an
 * instrument. The game's own image is named "game" and left for
 * p1-symbols.py. */
int diag_describe_addr(char *out, size_t cap, uint32_t addr)
{
    HMODULE h = NULL;
    char path[MAX_PATH];
    const char *base;

    if (!addr)
        return snprintf(out, cap, "(unsampled)");
    if (G && addr >= G->image_base && addr < G->span_end)
        return snprintf(out, cap, "0x%08lX(game+0x%lX)", (unsigned long)addr,
                        (unsigned long)(addr - G->image_base));
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &h) && h &&
        GetModuleFileNameA(h, path, sizeof path)) {
        base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        return snprintf(out, cap, "0x%08lX(%s+0x%lX)", (unsigned long)addr, base,
                        (unsigned long)(addr - (uint32_t)(uintptr_t)h));
    }
    return snprintf(out, cap, "0x%08lX(unknown)", (unsigned long)addr);
}

/* Is this address inside the game's own image? Every instrument asks, and the
 * answer decides which of two completely different reports is worth writing. */
int diag_in_game(uint32_t addr)
{
    return G && addr >= G->image_base && addr < G->span_end;
}

/* A FRAME THAT MOVES UP THE STACK BY MORE THAN THIS IS NOT A FRAME. Both
 * readable and increasing is a weak test: a stack slot that happens to hold an
 * unrelated pointer satisfies both, and the walk then follows it and prints
 * plausible-looking nonsense. 64 KB is far above any frame in either title and
 * the failure it prevents is the worse one; this runs only when something has
 * already gone wrong, and an instrument that reports confident, fabricated
 * return addresses there is worse than one that reports none. */
#define DIAG_MAX_FRAME 0x10000u

/* Walk a frame-pointer chain from `ebp`, into `out`, at most `max` deep.
 * Returns how many return addresses it recovered. Stops at the first
 * unreadable link, one that does not move up the stack, or one that moves
 * implausibly far, so a bogus EBP costs one entry rather than a fault or a
 * fiction. */
unsigned diag_return_chain(uint32_t ebp, uint32_t *out, unsigned max)
{
    unsigned depth = 0;

    while (depth < max) {
        uint32_t ret, next;

        if (!diag_peek32(ebp + 4, &ret) || !diag_peek32(ebp, &next) || !ret)
            break;
        out[depth++] = ret;
        if (next <= ebp || next - ebp > DIAG_MAX_FRAME)
            break;
        ebp = next;
    }
    return depth;
}

/* The fenced timestamp. Two bare `rdtsc` around a short call sit in the same
 * out-of-order window and retire at nearly the same value, which is a wrong
 * answer rather than an imprecise one. The switch
 * exists so the A/B that established that stays reproducible. */
uint64_t diag_rdtsc_fenced(void)
{
    uint64_t t;

    if (g_cfg.time_fence)
        __asm__ volatile ("lfence");
    t = diag_rdtsc();
    if (g_cfg.time_fence)
        __asm__ volatile ("lfence");
    return t;
}

/* THE HOTTEST `cap` OF A COUNTER ARRAY, WHICH EVERY PER-INTERVAL REPORT WANTS
 * AND NONE OF THEM SHOULD SPELL OUT. A report is not where a sort belongs, and
 * the three that wanted one, glide cycles, glide mix, the win32 boundary,
 * differed only in `cap` and in the width of the value.
 *
 * Descending, and a zero is never inserted: an entry point that was not called
 * in the interval is not a small contributor, it is absent, and a table whose
 * rows mostly read 0.0/frame says nothing. Returns the new count, which the
 * caller carries back in, so one call per counter is the whole loop. */
unsigned diag_top_insert(diag_top_t *top, unsigned n, unsigned cap,
                         unsigned idx, uint64_t value)
{
    unsigned j, k;

    if (!value || !cap)
        return n;
    for (j = 0; j < n; j++)
        if (value > top[j].value)
            break;
    if (j >= cap)
        return n;                       /* smaller than everything kept */
    if (n < cap)
        n++;
    for (k = n - 1; k > j; k--)
        top[k] = top[k - 1];
    top[j].idx = idx;
    top[j].value = value;
    return n;
}
