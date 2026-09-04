/* image_probe.c -- is overwriting our own image below the game actually safe?
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Runs off `--probe-image`, after the image is mapped, unpacked and patched --
 * the only state in which the question means anything -- and then stops where
 * --dry-run does rather than handing a probed process to the game. It exercises
 * delay-load resolution, TLS on modules loaded before the mapping, and
 * exception dispatch. Every case reports PASS, FAIL or NOT RUN, and NOT RUN is
 * never counted as a pass: two cases need build/vcprobe.dll
 * (tests/image_probe_dll.c, built by `make probe-dll` and shipped by nothing),
 * and if it is absent they say so.
 *
 * Every case has a control that must fail if the instrument is wrong, and
 * nothing here writes into the game's span: the one case that needs code inside
 * this module's image uses the .bss tail above the game's span end.
 */
#include "vcthunder.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A customer-bit code, so main.c's vectored handler takes its "may be handled
 * normally by runtime libraries" path and CONTINUES THE SEARCH, which is what
 * lets frame-based dispatch happen at all. A non-customer code would be logged
 * as a host fault and the log closed under us. */
#define PROBE_EXCEPTION 0x2ABC0001u

typedef struct {
    const char *name;
    int         state;                  /* PASS, FAIL, NOTRUN */
    char        note[160];
} probe_case_t;

enum { PROBE_FAIL = 0, PROBE_PASS = 1, PROBE_NOTRUN = 2 };

#define MAX_CASES 16
static probe_case_t s_case[MAX_CASES];
static int s_cases;

static void record(const char *name, int state, const char *fmt, ...)
{
    va_list ap;
    probe_case_t *c;

    if (s_cases >= MAX_CASES)
        return;
    c = &s_case[s_cases++];
    c->name = name;
    c->state = state;
    va_start(ap, fmt);
    vsnprintf(c->note, sizeof c->note, fmt, ap);
    va_end(ap);
    /* Also as it happens, not only in the summary: a case that takes the
     * process down with it must still leave a mark saying which one it was. */
    LOGT("image probe: [%s] %s", c->name, c->note);
}

/* Same reason: the steps between records are where a probe can die. */
#define STEP(...) LOGT("image probe: step: " __VA_ARGS__)

/* ---- frame-based SEH ---------------------------------------------------- */

/* The games do not only fault: both link the MSVC frame-based SEH runtime
 * (__except_handler3, __global_unwind2, __local_unwind2, __except_list), whose
 * handler addresses are INSIDE the region this host overwrites. Our own
 * vectored handler runs before any of that and has never let it be reached, so
 * this is the half of "exception dispatch" that production evidence does not
 * cover. */

typedef struct exc_frame {
    struct exc_frame *prev;
    void             *handler;
    volatile LONG    *ran;
} exc_frame_t;

static EXCEPTION_DISPOSITION __cdecl probe_seh_handler(
    EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp)
{
    exc_frame_t *f = (exc_frame_t *)frame;

    (void)ctx; (void)disp;
    if (rec->ExceptionCode == PROBE_EXCEPTION &&
        !(rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) {
        InterlockedIncrement(f->ran);
        /* Resume where RaiseException left off: it returns to its caller and
         * the probe carries on with the frame still installed, which it then
         * pops itself. */
        return ExceptionContinueExecution;
    }
    return ExceptionContinueSearch;
}

/* Raise one PROBE_EXCEPTION under a frame whose handler is `handler`, and say
 * whether that handler ran. `handler` is a parameter because the interesting
 * case is not our own function: it is a handler that lives inside the image
 * the game has overwritten, which is where the games' own handler lives.
 *
 * TWO frames go on, not one. If the dispatcher refuses the handler under test,
 * the exception must still be caught by something, or a refusal takes the
 * process down instead of being reported and the case can only ever pass. The
 * outer frame's handler is this module's own and always continues, so a
 * refusal comes back as `inner did not run, outer did`, which is a result. */
static int raise_under_frame(void *handler, volatile LONG *ran,
                             volatile LONG *fallback)
{
    exc_frame_t outer, inner;
    LONG before = InterlockedCompareExchange(ran, 0, 0);

    outer.handler = (void *)probe_seh_handler;
    outer.ran = fallback;
    inner.handler = handler;
    inner.ran = ran;

    __asm__ __volatile__("movl %%fs:0, %0" : "=r"(outer.prev));
    inner.prev = &outer;
    __asm__ __volatile__("movl %0, %%fs:0" :: "r"(&inner) : "memory");

    RaiseException(PROBE_EXCEPTION, 0, 0, NULL);

    __asm__ __volatile__("movl %0, %%fs:0" :: "r"(outer.prev) : "memory");
    return InterlockedCompareExchange(ran, 0, 0) > before;
}

static volatile LONG s_seh_ran;
static volatile LONG s_seh_fallback;

/* A five-byte `jmp probe_seh_handler` placed inside this process's own EXE
 * image, above the game's span, so the dispatcher resolves the handler to the
 * module whose contents the game overwrote. That is the property under test:
 * not whether our function works, but whether Windows will dispatch to a
 * handler that lives in that image at all. */
static void *install_handler_thunk_in_exe_image(char *why, size_t whysz)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)(UINT_PTR)G->image_base;
    IMAGE_NT_HEADERS *nt;
    UINT_PTR image_end, slot;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old = 0;
    unsigned char *p;
    LONG rel;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        snprintf(why, whysz, "our PE headers are gone from 0x%08X", G->image_base);
        return NULL;
    }
    nt = (IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);
    image_end = (UINT_PTR)G->image_base + nt->OptionalHeader.SizeOfImage;
    if (image_end <= G->span_end + 0x1000) {
        snprintf(why, whysz, "no image tail above the span: end 0x%08lX vs "
                 "span end 0x%08X", (unsigned long)image_end, G->span_end);
        return NULL;
    }

    /* One page below the end of our own image, which is stub.c's .bss
     * reservation and is above everything any title unpacks. */
    slot = (image_end - 0x1000) & ~(UINT_PTR)0xF;
    if (VirtualQuery((LPCVOID)slot, &mbi, sizeof mbi) != sizeof mbi ||
        mbi.State != MEM_COMMIT) {
        snprintf(why, whysz, "0x%08lX is not committed", (unsigned long)slot);
        return NULL;
    }
    if (!VirtualProtect((LPVOID)slot, 16, PAGE_EXECUTE_READWRITE, &old)) {
        snprintf(why, whysz, "VirtualProtect failed, err %lu", GetLastError());
        return NULL;
    }

    p = (unsigned char *)slot;
    rel = (LONG)((UINT_PTR)probe_seh_handler - (slot + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return p;
}

/* ---- a thread created after the mapping --------------------------------- */

/* CDECL, deliberately and with the reason written down: tests/image_probe_dll.c
 * defines these as plain C functions, and declaring them WINAPI here compiles,
 * links and runs. It then leaves ESP four bytes low per call, because the
 * caller compensates for a pop the callee never made, and the thread returns
 * into whatever that puts under its return address. Measured as a DEP fault at
 * a heap address with the thread's own return value still in EAX. */
typedef long (*tls_count_fn)(void);
typedef int  (*tls_round_fn)(int);

typedef struct {
    HMODULE      probe_dll;
    tls_round_fn roundtrip;
    void        *thunk;
    volatile LONG seh_ran;
    volatile LONG seh_ran_dll;
    volatile LONG seh_fallback;
    int          tls_roundtrip_ok;
    int          seh_in_exe_ok;
    int          seh_in_dll_ok;
    DWORD        tls_slot;
    int          tls_api_ok;
} thread_work_t;

static DWORD WINAPI probe_thread(LPVOID arg)
{
    thread_work_t *w = (thread_work_t *)arg;

    LOGT("image probe: step: thread running");
    if (w->roundtrip)
        w->tls_roundtrip_ok = (w->roundtrip(0x5A5A) == 0x5A5A);
    /* A thread that has never touched this slot must read NULL, then hold its
     * own value: the system TLS control for the module TLS above. */
    w->tls_api_ok = (TlsGetValue(w->tls_slot) == NULL);
    TlsSetValue(w->tls_slot, (LPVOID)(UINT_PTR)0xC0DE);
    w->tls_api_ok = w->tls_api_ok &&
                    (TlsGetValue(w->tls_slot) == (LPVOID)(UINT_PTR)0xC0DE);
    /* The control first: the same raise with the handler in VCThunder.dll,
     * so a failure below can be attributed to WHERE the handler lives rather
     * than to raising on a thread created after the mapping. */
    LOGT("image probe: step: thread TLS done, raising under a DLL handler");
    w->seh_in_dll_ok = raise_under_frame((void *)probe_seh_handler,
                                         &w->seh_ran_dll, &w->seh_fallback);
    LOGT("image probe: step: thread DLL handler ok=%d, raising under the thunk",
         w->seh_in_dll_ok);
    if (w->thunk)
        w->seh_in_exe_ok = raise_under_frame(w->thunk, &w->seh_ran,
                                             &w->seh_fallback);
    LOGT("image probe: step: thread returning");
    return 0xB0B0;
}

/* ---- preload ------------------------------------------------------------ */

/* The awkward module has to be loaded BEFORE the mapping, or the two cases it
 * carries are not the cases they claim to be: a module loaded afterwards has
 * nothing to say about whether an earlier one survived. Called from the same
 * place as every other preload, and its absence is NOT RUN rather than fatal. */
void image_probe_preload(void)
{
    char path[MAX_PATH], *cut;
    HMODULE h;

    if (!GetModuleFileNameA(NULL, path, sizeof path))
        return;
    cut = strrchr(path, '\\');
    snprintf(cut ? cut + 1 : path,
             sizeof path - (size_t)(cut ? cut + 1 - path : 0), "vcprobe.dll");

    h = LoadLibraryA(path);
    if (h)
        LOGI("image probe: vcprobe.dll preloaded at %p (TLS callbacks and a "
             "delay-loaded import, both fired after the mapping)", (void *)h);
    else
        LOGW("image probe: %s did not load, err %lu; the TLS and delay-load "
             "cases will report NOT RUN. Build it with `make probe-dll`.",
             path, GetLastError());
}

/* ---- the battery -------------------------------------------------------- */

void image_probe_run(void)
{
    char why[160];
    int i, pass = 0, fail = 0, notrun = 0;

    LOGI("image probe: the game is mapped, unpacked and patched; asking what "
         "of this process still works");

    /* 1. The three that were already known to survive, as controls. If any of
     *    these fails the instrument is wrong, not the finding. */
    {
        char path[MAX_PATH] = "";
        DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
        record("GetModuleFileName(NULL)", n && path[0] ? PROBE_PASS : PROBE_FAIL,
               "%lu chars, \"%s\"", (unsigned long)n, path);
    }
    {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        FARPROC f = k ? GetProcAddress(k, "GetTickCount") : NULL;
        record("GetProcAddress", f ? PROBE_PASS : PROBE_FAIL,
               "kernel32!GetTickCount = %p", (void *)f);
    }
    {
        /* Small and large: the large one forces the heap to take a fresh
         * region from the VM, which is the part that could have been sitting
         * in the span. */
        void *a = malloc(64), *b = malloc(4u << 20);
        int ok = a && b;
        if (ok) { memset(a, 0x11, 64); memset(b, 0x22, 4u << 20); }
        record("malloc small + 4 MiB", ok ? PROBE_PASS : PROBE_FAIL,
               "%p and %p", a, b);
        free(a); free(b);
    }

    /* 2. Our own PE headers, which are the reason any of the rest works. */
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)(UINT_PTR)G->image_base;
        int ok = dos->e_magic == IMAGE_DOS_SIGNATURE;
        IMAGE_NT_HEADERS *nt = ok ? (IMAGE_NT_HEADERS *)((char *)dos +
                                     dos->e_lfanew) : NULL;
        ok = ok && nt->Signature == IMAGE_NT_SIGNATURE;
        record("our PE headers survived the mapping", ok ? PROBE_PASS : PROBE_FAIL,
               ok ? "MZ + PE at 0x%08X, SizeOfHeaders 0x%lX, game CODE at 0x%lX"
                  : "no PE image at 0x%08X",
               G->image_base,
               ok ? (unsigned long)nt->OptionalHeader.SizeOfHeaders : 0UL,
               ok ? (unsigned long)(G->image_base + G->code_file_off) : 0UL);
    }

    /* 3. LoadLibrary. The host preloads every DLL before the game is mapped,
     *    on the rule that a load stops working once the game lands on our own
     *    PE headers. This host preserves those headers, so that rule may now
     *    be carrying a stale reason: measure it rather than repeat it. */
    {
        /* Each candidate carries an export of its own, so the case cannot
         * pass on a load that produced an unusable module. Whichever is not
         * already in the process is the one used. */
        static const struct { const char *dll, *sym; } cand[] = {
            { "version.dll",  "GetFileVersionInfoSizeA" },
            { "imagehlp.dll", "MapFileAndCheckSumA" },
            { "netapi32.dll", "NetApiBufferFree" },
            { NULL, NULL }
        };
        int pick = -1;
        HMODULE h;

        for (i = 0; cand[i].dll; i++)
            if (!GetModuleHandleA(cand[i].dll)) { pick = i; break; }

        if (pick < 0) {
            record("LoadLibrary after the mapping", PROBE_NOTRUN,
                   "every candidate was already loaded");
        } else if ((h = LoadLibraryA(cand[pick].dll)) != NULL) {
            FARPROC f = GetProcAddress(h, cand[pick].sym);
            record("LoadLibrary after the mapping", f ? PROBE_PASS : PROBE_FAIL,
                   "%s at %p, %s = %p", cand[pick].dll, (void *)h,
                   cand[pick].sym, (void *)f);
            FreeLibrary(h);
        } else {
            record("LoadLibrary after the mapping", PROBE_FAIL,
                   "%s: err %lu", cand[pick].dll, GetLastError());
        }
    }

    /* 4, 5, 6. Everything that needs a thread, a module with TLS, or a handler
     *    in the overwritten image. */
    {
        thread_work_t w;
        HANDLE t;
        DWORD rc = 0;
        char dll_path[MAX_PATH];
        tls_count_fn count = NULL;
        long before = 0, after = 0;

        memset(&w, 0, sizeof w);
        w.tls_slot = TlsAlloc();

        /* build/vcprobe.dll sits beside the host binaries in a build tree and
         * in nothing that ships. Absent is NOT RUN, never a pass. */
        {
            char *cut;
            GetModuleFileNameA(NULL, dll_path, sizeof dll_path);
            cut = strrchr(dll_path, '\\');
            snprintf(cut ? cut + 1 : dll_path,
                     sizeof dll_path - (size_t)(cut ? cut + 1 - dll_path : 0),
                     "vcprobe.dll");
            w.probe_dll = GetModuleHandleA("vcprobe.dll");
        }
        if (w.probe_dll) {
            count = (tls_count_fn)(void *)GetProcAddress(w.probe_dll,
                                                         "vcprobe_tls_callbacks");
            w.roundtrip = (tls_round_fn)(void *)GetProcAddress(w.probe_dll,
                                                         "vcprobe_tls_roundtrip");
        }
        if (count)
            before = count();

        STEP("installing the handler thunk in our own image");
        w.thunk = install_handler_thunk_in_exe_image(why, sizeof why);
        STEP("thunk = %p", w.thunk);

        STEP("creating a thread");
        t = CreateThread(NULL, 0, probe_thread, &w, 0, NULL);
        if (t) {
            STEP("joining it");
            WaitForSingleObject(t, 10000);
            STEP("joined");
            GetExitCodeThread(t, &rc);
            STEP("exit code 0x%lX", (unsigned long)rc);
            CloseHandle(t);
            STEP("handle closed");
        }
        record("a thread created after the mapping", rc == 0xB0B0 ?
               PROBE_PASS : PROBE_FAIL, "exit 0x%lX", (unsigned long)rc);
        record("TlsAlloc/TlsSetValue on that thread", w.tls_api_ok ?
               PROBE_PASS : PROBE_FAIL, "slot %lu", (unsigned long)w.tls_slot);

        if (count) {
            after = count();
            record("TLS callback of a module loaded before the mapping",
                   after > before ? PROBE_PASS : PROBE_FAIL,
                   "DLL_THREAD_ATTACH count %ld -> %ld", before, after);
            record("__thread storage in that module, on that thread",
                   w.tls_roundtrip_ok ? PROBE_PASS : PROBE_FAIL,
                   "round trip of 0x5A5A %s",
                   w.tls_roundtrip_ok ? "held" : "did not hold");
        } else {
            record("TLS callback of a module loaded before the mapping",
                   PROBE_NOTRUN, "build/vcprobe.dll not loaded (make probe-dll)");
            record("__thread storage in that module, on that thread",
                   PROBE_NOTRUN, "build/vcprobe.dll not loaded (make probe-dll)");
        }

        /* Frame-based SEH, this thread, handler in VCThunder.dll. The control
         * for the case below: if this fails, dispatch is broken generally and
         * says nothing about the overwritten image. */
        STEP("raising under a frame whose handler is in VCThunder.dll");
        record("frame-based SEH, handler in VCThunder.dll",
               raise_under_frame((void *)probe_seh_handler, &s_seh_ran,
                                 &s_seh_fallback) ?
               PROBE_PASS : PROBE_FAIL, "handler at %p",
               (void *)probe_seh_handler);

        if (w.thunk)
            record("frame-based SEH, handler INSIDE the overwritten image",
                   w.seh_in_exe_ok ? PROBE_PASS : PROBE_FAIL,
                   "thunk at %p, on a thread created after the mapping; this is "
                   "where both titles' __except_handler3 lives", w.thunk);
        else
            record("frame-based SEH, handler INSIDE the overwritten image",
                   PROBE_NOTRUN, "%s", why);

        TlsFree(w.tls_slot);
    }

    /* 7. Delay load, first call after the mapping. */
    {
        HMODULE h = GetModuleHandleA("vcprobe.dll");
        typedef int (*delay_fn)(unsigned long *);   /* cdecl; see above */
        delay_fn f = h ? (delay_fn)(void *)GetProcAddress(h, "vcprobe_delayload")
                       : NULL;
        if (!f) {
            record("delay-load resolved after the mapping", PROBE_NOTRUN,
                   "build/vcprobe.dll not loaded (make probe-dll)");
        } else {
            unsigned long err = 0;
            int ok = f(&err);
            record("delay-load resolved after the mapping",
                   ok ? PROBE_PASS : PROBE_FAIL,
                   "version.dll!GetFileVersionInfoSizeA through mingw's "
                   "__delayLoadHelper2, last error %lu", err);
        }
    }

    for (i = 0; i < s_cases; i++) {
        const char *tag = s_case[i].state == PROBE_PASS ? "PASS  " :
                          s_case[i].state == PROBE_FAIL ? "FAIL  " : "NOT RUN";
        if (s_case[i].state == PROBE_PASS) pass++;
        else if (s_case[i].state == PROBE_FAIL) fail++;
        else notrun++;
        LOGI("image probe: %s  %-52s %s", tag, s_case[i].name, s_case[i].note);
    }
    LOGI("image probe: %d passed, %d failed, %d NOT RUN%s", pass, fail, notrun,
         notrun ? " (a case that did not run is not a case that passed)" : "");
}

/* The control that says whether the case above discriminates. A handler that
 * is in NO image should be refused; if the dispatcher accepts anything, then
 * "it accepted a handler in our overwritten image" is not evidence.
 *
 * It gets its own flag because of what a refusal turns out to be. Measured
 * 2026-08-21: the dispatcher does not decline the handler and walk on to the
 * next frame. It TERMINATES THE PROCESS, and an outer catch-all frame
 * installed underneath does not run. So this cannot share a run with the
 * battery above, whose summary would never be printed, and the result is read
 * off the exit: dying here is the pass, and surviving is the finding. */
void image_probe_refusal(void)
{
    unsigned char *page = (unsigned char *)VirtualAlloc(
        NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    volatile LONG ran = 0, fallback = 0;
    LONG rel;
    int called;

    if (!page) {
        LOGE("image probe: refusal control NOT RUN: VirtualAlloc failed, "
             "err %lu", GetLastError());
        return;
    }
    rel = (LONG)((UINT_PTR)probe_seh_handler - ((UINT_PTR)page + 5));
    page[0] = 0xE9;
    memcpy(page + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), page, 5);

    LOGI("image probe: refusal control: raising under a frame whose handler is "
         "at %p, which is inside no image at all. If this process now dies, "
         "the dispatcher checks WHERE a handler lives and the image case is "
         "evidence. If the next line prints, it does not.", (void *)page);
    /* log_printf fflushes every line, so the message above is on disk. */

    called = raise_under_frame(page, &ran, &fallback);
    LOGE("image probe: refusal control FAILED: the exception under a non-image "
         "handler was survivable (inner ran=%d, outer ran=%ld). The dispatcher "
         "accepts a handler anywhere, so the image case proves less than it "
         "looks.", called, (long)fallback);
    VirtualFree(page, 0, MEM_RELEASE);
}
