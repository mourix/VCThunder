/* image_probe_dll.c -- a deliberately awkward module, for `--probe-image`.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Loaded BEFORE the game image is mapped and exercised after, so the probe can
 * ask two things that cannot be asked of the host itself: do TLS callbacks and
 * __thread storage still work for a module loaded before the mapping, on a
 * thread created after it, and does delay-load resolution still work when its
 * first call happens after the mapping. (VCThunder.exe must not have a TLS
 * directory at all, and VCThunder.dll has no delay-loaded import to fire.)
 *
 * Nothing ships this: `make probe-dll` builds it, `make pack` does not deliver
 * it, and the probe reports both cases as NOT RUN when it is absent. The
 * delay-load path is mingw's own (`__delayLoadHelper2`, reached through a
 * tail-merge thunk), and its failure hook is installed here so a failure is
 * reported instead of jumping to NULL.
 * /
 */
#include <windows.h>
#include <delayimp.h>

/* version.dll, delay-loaded. Chosen because it is present on every Windows,
 * has no side effects worth mentioning, and nothing in this host loads it. */
DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR, LPDWORD);

static volatile LONG s_tls_callbacks;
static volatile LONG s_delay_failures;
static __thread int s_tls_value;      /* creates this module's TLS directory */

/* ---- TLS ---------------------------------------------------------------- */

static void NTAPI tls_callback(PVOID handle, DWORD reason, PVOID reserved)
{
    (void)handle; (void)reserved;
    if (reason == DLL_THREAD_ATTACH)
        InterlockedIncrement(&s_tls_callbacks);
}

/* .CRT$XLB is where the loader's TLS callback array is assembled. */
PIMAGE_TLS_CALLBACK __crt_xl_probe __attribute__((section(".CRT$XLB"), used))
    = tls_callback;

__declspec(dllexport) long vcprobe_tls_callbacks(void)
{
    return InterlockedCompareExchange(&s_tls_callbacks, 0, 0);
}

__declspec(dllexport) int vcprobe_tls_roundtrip(int v)
{
    s_tls_value = v;
    /* Read it back through a barrier so the compiler cannot fold the store and
     * the load into the argument. */
    __asm__ __volatile__("" ::: "memory");
    return s_tls_value;
}

/* ---- delay load --------------------------------------------------------- */

static FARPROC WINAPI on_delay_failure(unsigned notify, PDelayLoadInfo info)
{
    (void)info;
    if (notify == dliFailLoadLib || notify == dliFailGetProc)
        InterlockedIncrement(&s_delay_failures);
    /* Returning NULL from here makes the thunk jump to NULL. Return a function
     * of the right shape instead, so the caller survives to report. */
    return (FARPROC)(void *)GetFileVersionInfoSizeA;
}

__declspec(dllexport) int vcprobe_delayload(unsigned long *last_error)
{
    DWORD handle = 0, n;

    s_delay_failures = 0;
    SetLastError(0);
    n = GetFileVersionInfoSizeA("kernel32.dll", &handle);
    *last_error = GetLastError();
    if (InterlockedCompareExchange(&s_delay_failures, 0, 0))
        return 0;                       /* the helper could not resolve it */
    return n != 0;
}

/* ---- module ------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        __pfnDliFailureHook2 = on_delay_failure;
    return TRUE;
}
