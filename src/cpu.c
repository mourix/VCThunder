/* cpu.c -- which processor cores the host and the game run on.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * On a hybrid processor (Intel's performance and efficiency cores, or any
 * other part that reports more than one efficiency class) Windows may run the
 * game thread, or a slice of every triangle, on a slow core. The game thread
 * sleeps out most of its tick (drawreplay.c's limiter), which is exactly the
 * load the scheduler sends to efficiency cores, and the CPU renderer's row
 * split waits for its slowest worker.
 *
 * `[timing] cpu_cores`:
 *   performance  opt out of power throttling, and give the process the
 *                fastest efficiency class as its default CPU set
 *   all          opt out of power throttling only; every core stays eligible
 *   system       touch nothing: Windows decides; the A/B control
 *
 * A process-wide CPU set, not an affinity mask: it covers every thread
 * without a set of its own, it is what Windows offers for exactly this, and an
 * affinity a user started the process with still wins over it, which is why
 * a narrowed affinity is left alone below. The renderer's pool counts the
 * cores this leaves it (renderer/win32/thread.c).
 *
 * Sources, all Microsoft's API documentation: SYSTEM_CPU_SET_INFORMATION
 * ("CPU Sets with higher numerical values of [EfficiencyClass] have home
 * processors that are faster but less power-efficient"),
 * SetProcessDefaultCpuSets, and SetProcessInformation with
 * PROCESS_POWER_THROTTLING_STATE (a control bit with a clear state bit opts
 * the process out of that throttling).
 */
#include "vcthunder.h"

#include <stdlib.h>
#include <string.h>

typedef BOOL (WINAPI *set_info_fn)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID,
                                   DWORD);
typedef BOOL (WINAPI *get_sets_fn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG,
                                   HANDLE, ULONG);
typedef BOOL (WINAPI *set_default_fn)(HANDLE, const ULONG *, ULONG);

static FARPROC kernel32(const char *name)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");

    return k ? GetProcAddress(k, name) : NULL;
}

/* EcoQoS is what moves a mostly-idle thread to an efficiency core and slows
 * it there; the timer-resolution bit keeps the limiter's 1 ms sleeps honoured
 * while the window is occluded. Windows 10 before 2004 rejects that second
 * bit, so the first is retried alone. */
static void power_throttling_off(void)
{
    set_info_fn set = (set_info_fn)(uintptr_t)kernel32("SetProcessInformation");
    PROCESS_POWER_THROTTLING_STATE st;

    if (!set) {
        LOGI("cpu: no SetProcessInformation; power throttling left to Windows");
        return;
    }
    memset(&st, 0, sizeof st);
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                     PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    if (set(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st)) {
        LOGI("cpu: power throttling off (execution speed, timer resolution)");
        return;
    }
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    if (set(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st))
        LOGI("cpu: power throttling off (execution speed)");
    else
        LOGW("cpu: could not opt out of power throttling (error %lu)",
             GetLastError());
}

#define CPU_SET_NEXT(e) \
    ((SYSTEM_CPU_SET_INFORMATION *)((BYTE *)(e) + (e)->Size))

/* Distinct physical cores among the entries of one efficiency class. */
static unsigned cores_in_class(SYSTEM_CPU_SET_INFORMATION *buf, ULONG len,
                               BYTE cls)
{
    SYSTEM_CPU_SET_INFORMATION *e, *f, *end;
    unsigned n = 0;

    end = (SYSTEM_CPU_SET_INFORMATION *)((BYTE *)buf + len);
    for (e = buf; e < end && e->Size; e = CPU_SET_NEXT(e)) {
        if (e->Type != CpuSetInformation || e->CpuSet.EfficiencyClass != cls)
            continue;
        for (f = buf; f < e; f = CPU_SET_NEXT(f))
            if (f->Type == CpuSetInformation &&
                f->CpuSet.EfficiencyClass == cls &&
                f->CpuSet.Group == e->CpuSet.Group &&
                f->CpuSet.CoreIndex == e->CpuSet.CoreIndex)
                break;
        if (f == e)
            n++;
    }
    return n;
}

/* What the CPU set list says, and which of its sets to keep. */
typedef struct {
    ULONG    logical;           /* CPU sets, every class */
    unsigned cores;             /* physical cores, every class */
    unsigned fast;              /* physical cores in the fastest class */
    BYTE     lo, hi;            /* the slowest and fastest class */
} cpu_topology_t;

/* Writes the fastest class's CPU set ids to `ids` (room for every entry) and
 * returns how many, or 0 when the process should keep every core: one class,
 * or a fastest class of one core, which would put the game thread, the sound
 * and the renderer on one core's threads. */
static ULONG cpu_choose(SYSTEM_CPU_SET_INFORMATION *buf, ULONG len,
                        ULONG *ids, cpu_topology_t *t)
{
    SYSTEM_CPU_SET_INFORMATION *e, *end;
    ULONG n = 0;
    unsigned c;

    memset(t, 0, sizeof *t);
    t->lo = 0xFF;
    end = (SYSTEM_CPU_SET_INFORMATION *)((BYTE *)buf + len);
    for (e = buf; e < end && e->Size; e = CPU_SET_NEXT(e)) {
        if (e->Type != CpuSetInformation)
            continue;
        t->logical++;
        if (e->CpuSet.EfficiencyClass < t->lo) t->lo = e->CpuSet.EfficiencyClass;
        if (e->CpuSet.EfficiencyClass > t->hi) t->hi = e->CpuSet.EfficiencyClass;
    }
    if (!t->logical)
        return 0;
    for (c = t->lo; c <= t->hi; c++)
        t->cores += cores_in_class(buf, len, (BYTE)c);
    t->fast = cores_in_class(buf, len, t->hi);
    if (t->lo == t->hi || t->fast < 2)
        return 0;
    for (e = buf; e < end && e->Size; e = CPU_SET_NEXT(e))
        if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass == t->hi)
            ids[n++] = e->CpuSet.Id;
    return n;
}

static void fastest_class_only(void)
{
    get_sets_fn get = (get_sets_fn)(uintptr_t)
        kernel32("GetSystemCpuSetInformation");
    set_default_fn set = (set_default_fn)(uintptr_t)
        kernel32("SetProcessDefaultCpuSets");
    SYSTEM_CPU_SET_INFORMATION *buf;
    cpu_topology_t t;
    DWORD_PTR mine = 0, all = 0;
    ULONG len = 0, *ids, n;

    if (!get || !set) {
        LOGI("cpu: no CPU set API; every core stays eligible");
        return;
    }
    if (GetProcessAffinityMask(GetCurrentProcess(), &mine, &all) &&
        mine != all) {
        LOGI("cpu: started with affinity 0x%llX of 0x%llX; left as given",
             (unsigned long long)mine, (unsigned long long)all);
        return;
    }
    get(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len || !(buf = (SYSTEM_CPU_SET_INFORMATION *)malloc(len)))
        return;
    /* An entry's Size and Type alone outweigh one id: room for every id. */
    ids = (ULONG *)malloc(len);
    if (!ids || !get(buf, len, &len, GetCurrentProcess(), 0)) {
        if (ids)
            LOGW("cpu: GetSystemCpuSetInformation failed (error %lu)",
                 GetLastError());
        free(ids);
        free(buf);
        return;
    }
    n = cpu_choose(buf, len, ids, &t);
    if (!n && t.lo == t.hi)
        LOGI("cpu: %u cores (%lu logical), one efficiency class; every core "
             "stays eligible", t.cores, t.logical);
    else if (!n)
        LOGI("cpu: hybrid, but %u of %u cores in the fastest class; every "
             "core stays eligible", t.fast, t.cores);
    else if (set(GetCurrentProcess(), ids, n))
        LOGI("cpu: hybrid: running on the %u fastest of %u cores (%lu of %lu "
             "logical, efficiency class %u)", t.fast, t.cores, n, t.logical,
             t.hi);
    else
        LOGW("cpu: SetProcessDefaultCpuSets failed (error %lu); every core "
             "stays eligible", GetLastError());
    free(ids);
    free(buf);
}

void cpu_policy_apply(const char *mode)
{
    if (!_stricmp(mode, "system")) {
        LOGI("cpu: cpu_cores = system; scheduling left to Windows");
        return;
    }
    power_throttling_off();
    if (!_stricmp(mode, "performance"))
        fastest_class_only();
}
