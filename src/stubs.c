/* stubs.c -- replace unsupported cabinet hardware with cdecl return stubs.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Later subsystems overwrite the entries for which host implementations exist.
 */
#include "vcthunder.h"

#include <string.h>

/* TIME and CMOS remain native and are excluded from the stub groups. */
static const char *const STUB_GROUPS[] = {
    "SPKR", "IDE", "WDOG", "DIEGO", "MBIO", "UART", "PCI", "DISK", "YMF",
    "ETSGATE", "ETSHW", "ETSDBG"
};
#define NSTUB_GROUPS (int)(sizeof STUB_GROUPS / sizeof STUB_GROUPS[0])

/* Return success for hardware gates whose callers branch on the result. */
static const struct { const char *name; uint32_t ret; } RETVAL[] = {
    { "_diegoio_comm_InitDiegoIOAndCreateBackgroundThread", 1 },
    { "_diegoio_comm_EnableComm",                           1 },
    /* Offroad's MagicBus board. The real one returns its thread handle on success and 0
     * on failure, and _main2 branches on it; 1 is the smallest truthful
     * "success" it accepts. Stubbing it also keeps _mb_init/_mb_flush and
     * __beginthreadex from running: src/diego.c replaces the comm layer
     * above it and drives the decoder itself. */
    { "_mb_io_comm_InitMbIoAndCreateBackgroundThread",      1 },
    { "_mb_io_comm_EnableComm",                             1 },
    /* The naming setters report success; their callers do not check, but a
     * truthful 1 costs nothing and keeps any that might from branching. */
    { "_EtsSetDebugName",                                   1 },
    { "_EtsSetThreadDebugName",                             1 },
    { "_EtsSetCritSecDebugName",                            1 },
    { "_phide_set_deviceNum",                               1 },
    { "_watchdog_Open",                                     1 },
    { "_watchdog_IsTimedOut",                               0 },
    { "_watchdog_TimeHasPassed",                            0 },
};
#define NRETVAL (int)(sizeof RETVAL / sizeof RETVAL[0])

/* Preserve game-side Diego accessors consumed by gameplay or the host. The DAC
 * writer must remain live because its output cell drives force feedback. */
static const char *const NEVER_STUB[] = {
    "_diego_io_receive",
    "_diego_io_read_adc",
    "_diego_io_read_coin_drop",
    "_diego_io_read_switch_byte",
    "_diego_io_write_dac",
    /* Offroad's equivalents. These are not in any stub group today (they sit
     * under _mb_, not _mb_io_comm_), but they are named here because the
     * consequence of ever stubbing them is silent: the game would read zeroes
     * from a board the log says is installed. */
    "_mb_receive",
    "_mb_read_adc",
    "_mb_read_coin_drop",
    "_mb_read_switch_byte",
};
#define NNEVER (int)(sizeof NEVER_STUB / sizeof NEVER_STUB[0])

static int never_stub(const char *name)
{
    int i;
    for (i = 0; i < NNEVER; i++)
        if (strcmp(name, NEVER_STUB[i]) == 0)
            return 1;
    return 0;
}

static uint32_t retval_for(const char *name)
{
    unsigned i;
    for (i = 0; i < NRETVAL; i++)
        if (strcmp(name, RETVAL[i].name) == 0)
            return RETVAL[i].ret;
    return 0;
}

static int in_stub_groups(const char *group)
{
    unsigned i;

    /* Hydro's YMF group contains portable bookkeeping above _audio_dspcmd_*.
     * Preserve it only when that exact backend can be installed: Offroad uses
     * the same group name for five real DMA/MMIO functions, and merely opening
     * WASAPI must never make those hardware functions executable. */
    if (strcmp(group, "YMF") == 0 && audio_ready() &&
        GAME_HAS(dspcmd_play) && GAME_HAS(ymf_init) &&
        GAME_HAS(hook_ymf_alloc) && GAME_HAS(hook_ymf_free))
        return 0;

    for (i = 0; i < NSTUB_GROUPS; i++)
        if (strcmp(group, STUB_GROUPS[i]) == 0)
            return 1;
    return 0;
}

int stubs_install_m1(void)
{
    unsigned i;
    int n = 0;

    for (i = 0; i < G->hw_count; i++) {
        void *t;

        if (never_stub(G->hw[i].name)) {
            LOGT("keep  %-52s [%s] (driven by the shim)",
                 G->hw[i].name, G->hw[i].group);
            continue;
        }
        if (!in_stub_groups(G->hw[i].group)) {
            LOGT("keep  %-52s [%s]", G->hw[i].name, G->hw[i].group);
            continue;
        }
        t = stub_logging(G->hw[i].name, retval_for(G->hw[i].name));
        if (!t)
            return n;
        patch_jmp(G->hw[i].va, t);
        LOGT("stub  %-52s [%s] -> %u",
             G->hw[i].name, G->hw[i].group, retval_for(G->hw[i].name));
        n++;
    }

    /* Stub Glide until the provider binding replaces these entries. Hardware
     * queries fail explicitly while no provider is installed. */
    for (i = 0; i < G->glide_count; i++) {
        void *t = stub_logging(G->glide[i].name, 0);
        if (!t)
            return n;
        patch_jmp(G->glide[i].va, t);
        n++;
    }
    LOGI("stubs: %d hardware + %d Glide entry points stubbed",
         n - G->glide_count, G->glide_count);
    return n;
}
