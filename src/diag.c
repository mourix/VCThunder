/* diag.c -- the primitives every instrument in this host needs.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The vocabulary a report is written in: a dword read that cannot fault, an
 * address rendered as `module+offset` so a finished run's log can still be
 * read, a validated frame-pointer walk, and the raw timestamp with its fenced
 * form.
 *
 * AN INSTRUMENT TAKES THESE FROM HERE AND KEEPS NONE OF ITS OWN. Each is the
 * kind of primitive a new instrument reinvents in three lines without noticing,
 * and a private copy is one that cannot be fixed for the others: the trap on
 * diag_peek32() below applies to every caller and was learned by one of them.
 *
 * Nothing here patches, allocates or logs on a hot path.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>

/* Read one dword from this process without ever faulting.
 *
 * NOT IsBadReadPtr: it PROBES, and on a bogus pointer the access violation it
 * raises reached this host's own exception logger and abandoned the report
 * before it printed -- the instrument destroyed by the thing it was inspecting,
 * 2026-08-24. Every caller here runs when something is already going wrong, so
 * that is the one failure this cannot have. ReadProcessMemory on ourselves
 * answers the same question and returns FALSE instead of faulting. */
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
 * matched to a module afterwards -- the first version of this printed
 * `0x6842754D` and nothing afterwards could say what that was. An
 * instrument whose output needs the process it came from is half an instrument.
 * The game's own image is named "game" and left for p1-symbols.py. */
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

/* Walk a frame-pointer chain from `ebp`, into `out`, at most `max` deep.
 * Returns how many return addresses it recovered. Stops at the first
 * unreadable link or one that does not move up the stack, so a bogus EBP
 * costs one entry rather than a fault. */
unsigned diag_return_chain(uint32_t ebp, uint32_t *out, unsigned max)
{
    unsigned depth = 0;

    while (depth < max) {
        uint32_t ret, next;

        if (!diag_peek32(ebp + 4, &ret) || !diag_peek32(ebp, &next) || !ret)
            break;
        out[depth++] = ret;
        if (next <= ebp)
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
