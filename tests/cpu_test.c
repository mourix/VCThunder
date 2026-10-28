/* cpu_test.c -- which cores a hybrid processor's process is given.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * src/cpu.c's choice, over CPU set lists built here in the shape
 * GetSystemCpuSetInformation returns: no processor of the kind needs to be
 * attached, which is the point, since the machine running this is one kind.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../src/cpu.c"

void log_printf(log_level_t level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

#define MAX_SETS 64

static SYSTEM_CPU_SET_INFORMATION s_sets[MAX_SETS];
static ULONG s_count;

static void reset(void)
{
    memset(s_sets, 0, sizeof s_sets);
    s_count = 0;
}

/* `cores` physical cores of `threads` logical each, in efficiency class
 * `cls`. Ids start at 0x100 as Windows' do, so an id is never an index. */
static void add(unsigned cores, unsigned threads, BYTE cls)
{
    static BYTE core_index;
    unsigned c, t;

    if (!s_count)
        core_index = 0;
    for (c = 0; c < cores; c++, core_index += (BYTE)threads)
        for (t = 0; t < threads && s_count < MAX_SETS; t++, s_count++) {
            SYSTEM_CPU_SET_INFORMATION *e = &s_sets[s_count];

            e->Size = sizeof *e;
            e->Type = CpuSetInformation;
            e->CpuSet.Id = 0x100 + s_count;
            e->CpuSet.LogicalProcessorIndex = (BYTE)s_count;
            e->CpuSet.CoreIndex = core_index;
            e->CpuSet.EfficiencyClass = cls;
        }
}

static ULONG choose(ULONG *ids, cpu_topology_t *t)
{
    return cpu_choose(s_sets, s_count * sizeof s_sets[0], ids, t);
}

/* Every chosen id is one of class `cls`, and every one of that class is chosen. */
static int exactly_class(const ULONG *ids, ULONG n, BYTE cls)
{
    ULONG i, j, want = 0;

    for (i = 0; i < s_count; i++)
        if (s_sets[i].CpuSet.EfficiencyClass == cls)
            want++;
    if (n != want)
        return 0;
    for (i = 0; i < n; i++) {
        for (j = 0; j < s_count; j++)
            if (s_sets[j].CpuSet.Id == ids[i])
                break;
        if (j == s_count || s_sets[j].CpuSet.EfficiencyClass != cls)
            return 0;
    }
    return 1;
}

int main(void)
{
    ULONG ids[MAX_SETS], n;
    cpu_topology_t t;

    /* 8 P-cores with Hyper-Threading, 16 E-cores: the 13900K's shape. */
    reset();
    add(8, 2, 1);
    add(16, 1, 0);
    n = choose(ids, &t);
    check(n == 16, "8P+16E: the 16 logical P-core sets");
    check(exactly_class(ids, n, 1), "8P+16E: only class 1");
    check(t.fast == 8 && t.cores == 24 && t.logical == 32,
          "8P+16E: 8 of 24 cores, 32 logical");

    /* No Hyper-Threading, and the slow cores enumerated first. */
    reset();
    add(4, 1, 0);
    add(4, 1, 1);
    add(4, 1, 0);
    add(4, 1, 1);
    n = choose(ids, &t);
    check(n == 8 && exactly_class(ids, n, 1),
          "interleaved, no HT: the 8 fast sets, wherever they are listed");
    check(t.fast == 8 && t.cores == 16, "interleaved: 8 of 16 cores");

    /* Three classes: the fastest alone, not everything above the slowest. */
    reset();
    add(6, 2, 2);
    add(8, 1, 1);
    add(2, 1, 0);
    n = choose(ids, &t);
    check(n == 12 && exactly_class(ids, n, 2), "three classes: class 2 only");
    check(t.lo == 0 && t.hi == 2 && t.cores == 16, "three classes: 0..2, 16 cores");

    /* One class, Hyper-Threaded: the machine this was written on. */
    reset();
    add(8, 2, 0);
    n = choose(ids, &t);
    check(n == 0, "one class: every core stays");
    check(t.cores == 8 && t.logical == 16, "one class: 8 cores, 16 logical");

    /* One fast core, Hyper-Threaded or not: every core stays. */
    reset();
    add(1, 1, 1);
    add(4, 1, 0);
    check(choose(ids, &t) == 0 && t.fast == 1, "1P+4E: every core stays");
    reset();
    add(1, 2, 1);
    add(8, 1, 0);
    check(choose(ids, &t) == 0 && t.fast == 1,
          "1 HT P-core: two logical sets are still one core");

    /* Two fast cores is enough. */
    reset();
    add(2, 2, 1);
    add(8, 1, 0);
    n = choose(ids, &t);
    check(n == 4 && exactly_class(ids, n, 1), "2P+8E: the 4 P-core sets");

    /* An entry of another type is not a processor. */
    reset();
    add(4, 1, 1);
    add(4, 1, 0);
    s_sets[0].Type = (CPU_SET_INFORMATION_TYPE)7;
    n = choose(ids, &t);
    check(n == 3 && t.logical == 7, "unknown entry type: skipped");

    /* Nothing reported, and a zero Size, end the walk rather than spin. */
    reset();
    check(choose(ids, &t) == 0 && t.logical == 0, "empty list: nothing chosen");
    add(4, 1, 1);
    add(4, 1, 0);
    s_sets[2].Size = 0;
    n = choose(ids, &t);
    check(t.logical == 2 && n == 0, "zero Size: the walk stops there");

    if (failures) {
        fprintf(stderr, "cpu-test: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("cpu-test: %d checks, all passed\n", checks);
    return 0;
}
