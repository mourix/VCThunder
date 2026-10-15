/* win32_bind.c -- replace the ETS Win32 boundary with native Windows exports.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Import slots are rewritten directly and remaining entry points receive
 * detours. Unresolved bindings retain the game's implementation and are
 * reported.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>
#include <objbase.h>

static const char *const MODULES[] = {
    "kernel32.dll", "user32.dll", "ws2_32.dll", "ntdll.dll"
};

/* Decorated functions implemented by the game rather than ETS. Offroad's
 * joyGetPosEx@8 reads its cabinet I/O board and must not bind to WinMM. */
static const struct { const char *name; const char *why; } NEVER_BIND[] = {
    { "joyGetPosEx@8",
      "the game IMPLEMENTS this over its MagicBus I/O board; it does not call it" },
};
#define NNEVER_BIND (int)(sizeof NEVER_BIND / sizeof NEVER_BIND[0])

static const char *never_bind(const char *decorated)
{
    int i;
    for (i = 0; i < NNEVER_BIND; i++)
        if (strcmp(decorated, NEVER_BIND[i].name) == 0)
            return NEVER_BIND[i].why;
    return NULL;
}
#define NMODULES (int)(sizeof MODULES / sizeof MODULES[0])

/* Not used for symbol resolution: loaded purely so they are already resident
 * before the game image lands on our PE headers and LoadLibrary stops working.
 *
 * This list exists because of a real crash: a Glide provider that brings up
 * D3D through COM lazily, on the first Glide call, does it long after the headers
 * are gone. The fault landed in combase.dll reading 0x64 off a null pointer.
 * Anything the Glide backend might pull in has to be resident up front. */
static const char *const SUPPORT[] = {
    "ole32.dll", "combase.dll", "oleaut32.dll", "advapi32.dll", "gdi32.dll",
    "shell32.dll", "shcore.dll", "version.dll", "imm32.dll",
    "dxgi.dll", "d3d11.dll", "d3d9.dll", "d3dcompiler_47.dll", "dwmapi.dll",
    "setupapi.dll", "cfgmgr32.dll", "bcrypt.dll", "winmm.dll",
    /* WASAPI. audio_preload() opens the endpoint before the headers go, which
     * loads these anyway; listed so a device that appears later cannot try. */
    "mmdevapi.dll", "audioses.dll", "avrt.dll",
};
#define NSUPPORT (int)(sizeof SUPPORT / sizeof SUPPORT[0])

static HMODULE s_mod[NMODULES];

/* ---- overrides: entries that need shim behaviour, not passthrough -------- */

static HANDLE WINAPI ov_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                    LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl)
{
    char buf[MAX_PATH];
    /* The access mask is what separates the game's files from the cabinet's
     * (fs_map.c). It is also printed, because a file taking the write route
     * unexpectedly is the only way this rule can be wrong, and a trace that
     * did not say which route a path took could not show it. */
    int writing = (access & (GENERIC_WRITE | FILE_APPEND_DATA)) != 0;
    const char *path = fs_map_path_mode(name, buf, sizeof buf,
                                        writing ? FS_WRITE : FS_READ);
    HANDLE h = CreateFileA(path, access, share, sa, disp, flags, tmpl);

    if (h == INVALID_HANDLE_VALUE)
        LOGW("CreateFileA FAILED '%s' (from '%s') access=0x%08lX err=%lu",
             path, name ? name : "(null)", (unsigned long)access,
             GetLastError());
    else
        LOGT("CreateFileA '%s' access=0x%08lX (%s) -> %p", path,
             (unsigned long)access, writing ? "write" : "read", h);
    return h;
}

static BOOL WINAPI ov_DeleteFileA(LPCSTR name)
{
    char buf[MAX_PATH];
    /* Only ever out of the save directory: a delete that reached the dump
     * would be the one write this host must never make. */
    return DeleteFileA(fs_map_path_mode(name, buf, sizeof buf, FS_DELETE));
}

static BOOL WINAPI ov_SetCurrentDirectoryA(LPCSTR name)
{
    /* _main calls __chdir early. The game's notion of a current directory is
     * meaningless here because fs_map_path() always produces an absolute-ish
     * path, so accept it and move on rather than letting a failure propagate. */
    LOGT("SetCurrentDirectoryA('%s') ignored", name ? name : "(null)");
    return TRUE;
}

static UINT WINAPI ov_GetDriveTypeA(LPCSTR root)
{
    (void)root;
    return DRIVE_FIXED;            /* C: and D: were both IDE partitions */
}

/* Route the game's ETS console diagnostics to the host log. */
static BOOL WINAPI ov_WriteConsoleA(HANDLE h, const void *buf, DWORD n,
                                    DWORD *written, void *reserved)
{
    const char *p = (const char *)buf;
    char line[512];
    DWORD i, out = 0;

    for (i = 0; i < n && out + 1 < sizeof line; i++) {
        if (p[i] == '\r')
            continue;
        if (p[i] == '\n') {
            line[out] = 0;
            if (out)
                LOGI("game: %s", line);
            out = 0;
            continue;
        }
        line[out++] = p[i];
    }
    if (out) {
        line[out] = 0;
        LOGI("game: %s", line);
    }
    if (written)
        *written = n;
    (void)h; (void)reserved;
    return TRUE;
}

/* Who decided to quit, and with what.
 *
 * The arcade binary has no quit path of its own (a cabinet is switched off at
 * the mains), so any ExitProcess reaching here is a startup check giving up.
 * The return address names the caller, which is the whole question. */
static void WINAPI ov_ExitProcess(UINT code)
{
    void *caller = __builtin_return_address(0);

    LOGE("game: ExitProcess(%u) from 0x%08X; identify it with:  "
         "python3 scripts/p1-symbols.py --title %s --lookup 0x%X",
         code, (unsigned)(uintptr_t)caller, G->id, (unsigned)(uintptr_t)caller);
    shim_exit((int)code, "the game called ExitProcess");
}

/* cdecl entry for the ETS-side _ExitProcess@4, patched over its entry point.
 * ETS's own is stdcall, but a jmp patch never returns to it, so the argument is
 * read off the stack the same way either way and nothing unwinds. */
static void __cdecl ov_ets_exit_process(UINT code)
{
    void *caller = __builtin_return_address(0);

    LOGE("game: ETS ExitProcess(%u) from 0x%08X; identify it with:  "
         "python3 scripts/p1-symbols.py --title %s --lookup 0x%X",
         code, (unsigned)(uintptr_t)caller, G->id, (unsigned)(uintptr_t)caller);
    shim_exit((int)code, "the game exited through the ETS kernel");
}

/* The other two ways a process ends, both bound to the real Win32 export and
 * therefore both able to end the run leaving nothing behind. TerminateProcess
 * in particular exits with a status of the caller's choosing and raises no
 * exception, which is indistinguishable in a log from "it stopped". */
static BOOL WINAPI ov_TerminateProcess(HANDLE proc, UINT code)
{
    void *caller = __builtin_return_address(0);

    LOGE("game: TerminateProcess(%p, %u) from 0x%08X; identify it with:  "
         "python3 scripts/p1-symbols.py --title %s --lookup 0x%X",
         proc, code, (unsigned)(uintptr_t)caller, G->id,
         (unsigned)(uintptr_t)caller);
    if (proc == GetCurrentProcess() || proc == (HANDLE)-1)
        shim_exit((int)code, "the game called TerminateProcess on itself");
    return TerminateProcess(proc, code);
}

static void WINAPI ov_ExitThread(DWORD code)
{
    void *caller = __builtin_return_address(0);

    LOGW("game: ExitThread(%lu) on thread %lu from 0x%08X",
         (unsigned long)code, GetCurrentThreadId(),
         (unsigned)(uintptr_t)caller);
    ExitThread(code);
}

/* Remove DMA-oriented PAGE_NOCACHE and PAGE_WRITECOMBINE flags. The software
 * renderer reads these allocations on the CPU and requires cacheable memory;
 * all other protection flags pass through unchanged. */
#define PAGE_CACHE_BITS (PAGE_NOCACHE | PAGE_WRITECOMBINE)

static unsigned s_nocache_calls;
static uint64_t s_nocache_bytes;

static DWORD strip_cache_bits(DWORD protect, SIZE_T size, const char *who)
{
    if (!(protect & PAGE_CACHE_BITS))
        return protect;
    s_nocache_calls++;
    s_nocache_bytes += (uint64_t)size;
    if (s_nocache_calls <= 8u)
        LOGI("vm: %s asked for 0x%04lX (%s%s) over %llu KB; cache bits "
             "STRIPPED. On the cabinet this memory was read by the Voodoo2 over "
             "DMA; here the CPU reads it and uncached costs ~100x per vertex.",
             who, (unsigned long)protect,
             (protect & PAGE_NOCACHE) ? "PAGE_NOCACHE" : "",
             (protect & PAGE_WRITECOMBINE) ? " PAGE_WRITECOMBINE" : "",
             (unsigned long long)(size / 1024u));
    return protect & ~(DWORD)PAGE_CACHE_BITS;
}

void win32_report_nocache(void)
{
    if (s_nocache_calls)
        LOGI("vm: %u allocation(s)/protect(s) totalling %llu KB had "
             "PAGE_NOCACHE/PAGE_WRITECOMBINE stripped",
             s_nocache_calls, (unsigned long long)(s_nocache_bytes / 1024u));
}

/* The game's own memory, as it asked for it: every VirtualAlloc reservation it
 * holds, every private heap and every CRITICAL_SECTION it made. The draw
 * interpolation restores exactly this memory after an in-between picture, so a
 * list that misses something lets that picture change the game. A list that
 * fills up says so through win32_game_memory_lost() rather than dropping
 * entries quietly. */
#define GAME_VALLOC_MAX 1024
#define GAME_HEAP_MAX   16
#define GAME_CS_MAX     256
static SRWLOCK       s_mem_lock = SRWLOCK_INIT;
static struct { uintptr_t base; size_t size; int watched; } s_game_valloc[GAME_VALLOC_MAX];
static unsigned      s_game_vallocs;
static volatile LONG s_vgen;          /* bumped by every change to the game's VM */
static volatile LONG s_write_watch;
static HANDLE        s_game_heap[GAME_HEAP_MAX];
static volatile LONG s_game_heaps;
static const char * volatile s_mem_lost;

static void mem_lost(const char *what)
{
    if (!InterlockedCompareExchangePointer((PVOID volatile *)&s_mem_lost,
                                           (PVOID)what, NULL))
        LOGE("vm: more %s than the host tracks; the game's memory is no "
             "longer fully known, so frame_rate = 60 turns itself off", what);
}

const char *win32_game_memory_lost(void) { return s_mem_lost; }

/* Copy out at most `max` reservations; returns how many. */
unsigned win32_game_vallocs(uintptr_t *base, size_t *size,
                            unsigned char *watched, unsigned max)
{
    unsigned i, n;

    AcquireSRWLockShared(&s_mem_lock);
    n = s_game_vallocs < max ? s_game_vallocs : max;
    for (i = 0; i < n; i++) {
        base[i] = s_game_valloc[i].base;
        size[i] = s_game_valloc[i].size;
        if (watched)
            watched[i] = (unsigned char)s_game_valloc[i].watched;
    }
    ReleaseSRWLockShared(&s_mem_lock);
    return n;
}

/* Changes whenever the game allocates, commits, decommits or frees VM, both
 * before the call and after it: a reader that sees the same value twice saw
 * the same reservations, holding the same committed pages, both times. */
uint32_t win32_game_vgen(void) { return (uint32_t)s_vgen; }

/* From now on the game's new reservations are made with MEM_WRITE_WATCH. */
void win32_write_watch(int on) { InterlockedExchange(&s_write_watch, on); }

/* NULL in a slot is a heap the game has destroyed. */
unsigned win32_game_heaps(void) { return (unsigned)s_game_heaps; }
void *win32_game_heap(unsigned i) { return s_game_heap[i]; }

/* Record the whole reservation `p` lies in, once: a commit inside a region
 * already reserved is the same memory, and keeps the reservation's `watched`. */
static void valloc_note(const void *p, int watched)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t base, a;
    unsigned i;

    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State == MEM_FREE)
        return;
    base = (uintptr_t)mbi.AllocationBase;
    for (a = base; VirtualQuery((const void *)a, &mbi, sizeof mbi) &&
                   (uintptr_t)mbi.AllocationBase == base &&
                   mbi.State != MEM_FREE;
         a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
        ;
    AcquireSRWLockExclusive(&s_mem_lock);
    for (i = 0; i < s_game_vallocs; i++)
        if (s_game_valloc[i].base == base)
            break;
    if (i < GAME_VALLOC_MAX) {
        s_game_valloc[i].base = base;
        s_game_valloc[i].size = a - base;
        if (i == s_game_vallocs) {
            s_game_valloc[i].watched = watched;
            s_game_vallocs++;
        }
    }
    ReleaseSRWLockExclusive(&s_mem_lock);
    if (i == GAME_VALLOC_MAX)
        mem_lost("VirtualAlloc reservations");
}

/* A released reservation is no longer the game's: the address range can be
 * handed to anyone next, and must not be snapshotted or restored as the
 * game's. */
static int valloc_forget(uintptr_t base)
{
    unsigned i;
    int watched = 0;

    AcquireSRWLockExclusive(&s_mem_lock);
    for (i = 0; i < s_game_vallocs; i++)
        if (s_game_valloc[i].base == base) {
            watched = s_game_valloc[i].watched;
            s_game_valloc[i] = s_game_valloc[--s_game_vallocs];
            break;
        }
    ReleaseSRWLockExclusive(&s_mem_lock);
    return watched;
}

/* A call that makes a new reservation can take MEM_WRITE_WATCH: one that
 * reserves, or commits with no address (which reserves too). */
static LPVOID WINAPI ov_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type,
                                     DWORD protect)
{
    DWORD prot = strip_cache_bits(protect, size, "VirtualAlloc");
    int ww = s_write_watch &&
             !(type & ~(DWORD)(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN)) &&
             ((type & MEM_RESERVE) || (!addr && (type & MEM_COMMIT)));
    LPVOID p = NULL;

    InterlockedIncrement(&s_vgen);
    if (ww)
        p = VirtualAlloc(addr, size, type | MEM_RESERVE | MEM_WRITE_WATCH, prot);
    if (!p) {
        ww = 0;
        p = VirtualAlloc(addr, size, type, prot);
    }
    if (p && (type & (MEM_RESERVE | MEM_COMMIT)))
        valloc_note(p, ww);
    InterlockedIncrement(&s_vgen);
    return p;
}

/* A diagnostic that restores the game's memory after running game code (the
 * draw replay) must not let that code give memory back to the OS: the restore
 * would roll the heap's bookkeeping back over pages that no longer exist.
 * While held, the game's frees do nothing and a growing realloc copies
 * instead of freeing; the restore then puts the bookkeeping back exactly as
 * it was, so nothing leaks either. */
static volatile LONG s_hold_frees;

void win32_hold_frees(int on) { InterlockedExchange(&s_hold_frees, on); }

/* A block this large never shares pages with a heap's small ones: the heap
 * gives it a VM allocation of its own, inside Windows where write-watch cannot
 * see it. While write-watch is on, a game heap's large block is that
 * allocation made here instead, with MEM_WRITE_WATCH, so the 60 Hz snapshot
 * copies only its written pages. Offroad keeps its whole memory pool in one
 * (53 MB, HeapAlloc'd once from its runtime's heap). */
#define BIG_BLOCK (1u << 20)
#define BIG_MAX   64
static struct { uintptr_t p; size_t n; HANDLE heap; } s_big[BIG_MAX];
static volatile unsigned s_nbig;

static int is_game_heap(HANDLE h)
{
    LONG i, n = s_game_heaps;

    for (i = 0; i < n; i++)
        if (s_game_heap[i] == h)
            return 1;
    return 0;
}

/* The size of the large block at `p`, or 0 if it is not one. */
static size_t big_size(const void *p)
{
    unsigned i;
    size_t n = 0;

    AcquireSRWLockShared(&s_mem_lock);
    for (i = 0; i < s_nbig; i++)
        if (s_big[i].p == (uintptr_t)p)
            n = s_big[i].n;
    ReleaseSRWLockShared(&s_mem_lock);
    return n;
}

static LPVOID big_alloc(HANDLE h, SIZE_T n)
{
    static volatile LONG warned;
    LPVOID p;

    if (s_nbig >= BIG_MAX) {
        if (!InterlockedExchange(&warned, 1))
            LOGW("vm: more than %d large heap blocks; the rest stay in their "
                 "heap, unwatched, and the 60 Hz snapshot copies them whole",
                 BIG_MAX);
        return NULL;
    }
    InterlockedIncrement(&s_vgen);
    p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                     PAGE_READWRITE);
    if (p) {
        AcquireSRWLockExclusive(&s_mem_lock);
        if (s_nbig < BIG_MAX) {
            s_big[s_nbig].p = (uintptr_t)p;
            s_big[s_nbig].n = n;
            s_big[s_nbig].heap = h;
            s_nbig++;
        } else {
            VirtualFree(p, 0, MEM_RELEASE);
            p = NULL;
        }
        ReleaseSRWLockExclusive(&s_mem_lock);
    }
    if (p)
        valloc_note(p, 1);
    InterlockedIncrement(&s_vgen);
    return p;
}

static void big_free(const void *p)
{
    unsigned i;

    InterlockedIncrement(&s_vgen);
    AcquireSRWLockExclusive(&s_mem_lock);
    for (i = 0; i < s_nbig; i++)
        if (s_big[i].p == (uintptr_t)p) {
            s_big[i] = s_big[--s_nbig];
            break;
        }
    ReleaseSRWLockExclusive(&s_mem_lock);
    valloc_forget((uintptr_t)p);
    VirtualFree((LPVOID)p, 0, MEM_RELEASE);
    InterlockedIncrement(&s_vgen);
}

static LPVOID WINAPI ov_HeapAlloc(HANDLE h, DWORD flags, SIZE_T n)
{
    LPVOID p;

    if (s_write_watch && !s_hold_frees && n >= BIG_BLOCK && is_game_heap(h) &&
        (p = big_alloc(h, n)) != NULL)
        return p;                      /* zeroed, as HEAP_ZERO_MEMORY asks */
    return HeapAlloc(h, flags, n);
}

static BOOL WINAPI ov_HeapFree(HANDLE h, DWORD flags, LPVOID p)
{
    if (s_hold_frees)
        return TRUE;
    if (p && s_nbig && big_size(p)) {
        big_free(p);
        return TRUE;
    }
    return HeapFree(h, flags, p);
}

static BOOL WINAPI ov_VirtualFree(LPVOID p, SIZE_T size, DWORD type)
{
    BOOL ok;
    int watched;

    if (s_hold_frees)
        return TRUE;
    InterlockedIncrement(&s_vgen);
    if (!(type & MEM_RELEASE)) {
        ok = VirtualFree(p, size, type);
        InterlockedIncrement(&s_vgen);
        return ok;
    }
    watched = valloc_forget((uintptr_t)p);
    ok = VirtualFree(p, size, type);
    if (!ok)
        valloc_note(p, watched);
    InterlockedIncrement(&s_vgen);
    return ok;
}

static LPVOID WINAPI ov_HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n)
{
    SIZE_T old = p && s_nbig ? big_size(p) : 0;
    LPVOID q;

    if (old) {
        if (n <= old)
            return p;
        if (flags & HEAP_REALLOC_IN_PLACE_ONLY)
            return NULL;
        q = ov_HeapAlloc(h, flags, n);
        if (q) {
            memcpy(q, p, old);
            if (!s_hold_frees)
                big_free(p);
        }
        return q;
    }
    if (!s_hold_frees) {
        /* Grown into a large block: one of ours, as ov_HeapAlloc would make. */
        if (p && n >= BIG_BLOCK && s_write_watch &&
            !(flags & HEAP_REALLOC_IN_PLACE_ONLY) && is_game_heap(h) &&
            (old = HeapSize(h, 0, p)) != (SIZE_T)-1 &&
            (q = big_alloc(h, n)) != NULL) {
            memcpy(q, p, old < n ? old : n);
            HeapFree(h, 0, p);
            return q;
        }
        return HeapReAlloc(h, flags, p, n);
    }
    old = HeapSize(h, 0, p);
    if (old != (SIZE_T)-1 && n <= old)
        return p;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY)
        return NULL;
    q = HeapAlloc(h, flags & ~HEAP_REALLOC_IN_PLACE_ONLY, n);
    if (q && old != (SIZE_T)-1)
        memcpy(q, p, old);
    return q;
}

/* Every CRITICAL_SECTION the game initialises lives in the game's own memory,
 * and its fields are Windows' lock state. A diagnostic that restores that
 * memory must put them back as they are, never as they were: Offroad's
 * _stream_critSection rolled back under its streaming thread and the attract
 * hung for ever in _mvplayer_OpenMovie. */
static CRITICAL_SECTION *s_game_cs[GAME_CS_MAX];
static volatile LONG     s_game_css;

unsigned win32_game_css(void) { return (unsigned)s_game_css; }
void *win32_game_cs(unsigned i) { return s_game_cs[i]; }

/* Slots are written before the count that publishes them, so a reader on
 * another thread never sees an empty one. */
static VOID WINAPI ov_InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    LONG i, n;

    AcquireSRWLockExclusive(&s_mem_lock);
    n = s_game_css;
    for (i = 0; i < n; i++)
        if (s_game_cs[i] == cs)
            break;
    if (i == n && n < GAME_CS_MAX) {
        s_game_cs[n] = cs;
        MemoryBarrier();
        s_game_css = n + 1;
    }
    ReleaseSRWLockExclusive(&s_mem_lock);
    if (i == n && n == GAME_CS_MAX)
        mem_lost("CRITICAL_SECTIONs");
    InitializeCriticalSection(cs);
}

static HANDLE WINAPI ov_HeapCreate(DWORD options, SIZE_T initial, SIZE_T max)
{
    HANDLE h = HeapCreate(options, initial, max);
    LONG n;

    if (!h)
        return h;
    AcquireSRWLockExclusive(&s_mem_lock);
    n = s_game_heaps;
    if (n < GAME_HEAP_MAX) {
        s_game_heap[n] = h;
        MemoryBarrier();
        s_game_heaps = n + 1;
    }
    ReleaseSRWLockExclusive(&s_mem_lock);
    if (n == GAME_HEAP_MAX)
        mem_lost("private heaps");
    return h;
}

static BOOL WINAPI ov_HeapDestroy(HANDLE h)
{
    LONG i;

    if (s_hold_frees)
        return TRUE;
    for (;;) {
        LPVOID p = NULL;
        unsigned k;

        AcquireSRWLockShared(&s_mem_lock);
        for (k = 0; k < s_nbig && !p; k++)
            if (s_big[k].heap == h)
                p = (LPVOID)s_big[k].p;
        ReleaseSRWLockShared(&s_mem_lock);
        if (!p)
            break;
        big_free(p);
    }
    AcquireSRWLockExclusive(&s_mem_lock);
    for (i = 0; i < s_game_heaps; i++)
        if (s_game_heap[i] == h)
            s_game_heap[i] = NULL;
    ReleaseSRWLockExclusive(&s_mem_lock);
    return HeapDestroy(h);
}

static BOOL WINAPI ov_VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect,
                                     PDWORD old)
{
    BOOL ok;

    InterlockedIncrement(&s_vgen);
    ok = VirtualProtect(addr, size,
                        strip_cache_bits(protect, size, "VirtualProtect"), old);
    InterlockedIncrement(&s_vgen);
    return ok;
}

static const struct { const char *name; void *fn; } OVERRIDE[] = {
    { "VirtualAlloc",          (void *)ov_VirtualAlloc          },
    { "VirtualProtect",        (void *)ov_VirtualProtect        },
    { "HeapCreate",            (void *)ov_HeapCreate            },
    { "HeapDestroy",           (void *)ov_HeapDestroy           },
    { "InitializeCriticalSection", (void *)ov_InitializeCriticalSection },
    { "HeapAlloc",             (void *)ov_HeapAlloc             },
    { "HeapFree",              (void *)ov_HeapFree              },
    { "HeapReAlloc",           (void *)ov_HeapReAlloc           },
    { "VirtualFree",           (void *)ov_VirtualFree           },
    { "WriteConsoleA",         (void *)ov_WriteConsoleA         },
    { "ExitProcess",           (void *)ov_ExitProcess           },
    { "TerminateProcess",      (void *)ov_TerminateProcess      },
    { "ExitThread",            (void *)ov_ExitThread            },
    { "CreateFileA",           (void *)ov_CreateFileA           },
    { "DeleteFileA",           (void *)ov_DeleteFileA           },
    { "SetCurrentDirectoryA",  (void *)ov_SetCurrentDirectoryA  },
    { "GetDriveTypeA",         (void *)ov_GetDriveTypeA         },
};
#define NOVERRIDE (int)(sizeof OVERRIDE / sizeof OVERRIDE[0])

/* ---- resolution --------------------------------------------------------- */

/* "CreateThread@24" -> "CreateThread" */
static void undecorate(const char *decorated, char *out, size_t n)
{
    size_t i = 0;
    while (decorated[i] && decorated[i] != '@' && i + 1 < n) {
        out[i] = decorated[i];
        i++;
    }
    out[i] = '\0';
}

static void *resolve(const char *decorated, const char **whence)
{
    char base[128];
    int i;

    undecorate(decorated, base, sizeof base);

    for (i = 0; i < NOVERRIDE; i++) {
        if (strcmp(base, OVERRIDE[i].name) == 0) {
            *whence = "shim";
            return OVERRIDE[i].fn;
        }
    }
    /* The four Winsock entry points the arcade link needs shim behaviour on.
     * They live in net_link.c because <winsock2.h> has to precede
     * <windows.h> and this file does not need it, but they are bound HERE,
     * counted here and logged here, like every other entry point. They pass
     * straight through when [link] link = off. */
    {
        void *p = net_link_override(base);
        if (p) {
            *whence = "shim (link)";
            return p;
        }
    }
    for (i = 0; i < NMODULES; i++) {
        void *p;
        if (!s_mod[i])
            continue;
        p = (void *)GetProcAddress(s_mod[i], base);
        if (p) {
            *whence = MODULES[i];
            return p;
        }
    }
    *whence = NULL;
    return NULL;
}

/* MUST be called before game_map(). Mapping the game overwrites this process's
 * own PE headers: the span is VCThunder.exe's image (src/stub.c) and the
 * game's CODE starts at 0x1006b0, inside the header page. Windows tolerates that
 * remarkably well: GetProcAddress, GetModuleFileName and malloc all keep
 * working. LoadLibrary does not. So every DLL we will ever need has to be
 * resolved while the headers are still intact. Measured, not assumed. */
int win32_preload(void)
{
    int ok = 0, i;

    for (i = 0; i < NMODULES; i++) {
        s_mod[i] = LoadLibraryA(MODULES[i]);
        if (s_mod[i])
            ok++;
        else
            LOGE("LoadLibrary('%s') failed %lu: must succeed BEFORE the game "
                 "image is mapped", MODULES[i], GetLastError());
    }
    for (i = 0; i < NSUPPORT; i++) {
        /* Best effort: several of these are optional depending on the backend
         * and the Windows edition, so a miss is logged at trace level only. */
        if (!LoadLibraryA(SUPPORT[i]))
            LOGT("win32: support module '%s' unavailable", SUPPORT[i]);
    }

    /* Initialise COM on this thread while the image is still intact: the game
     * runs on this same thread, so this is the apartment the Glide backend finds
     * when it brings up D3D. MULTITHREADED, not APARTMENTTHREADED: DXGI on an STA
     * deadlocks, which is what an earlier APARTMENTTHREADED attempt did (four
     * live threads, zero windows, stuck inside grSstWinOpen). */
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        LOGI("win32: CoInitializeEx -> 0x%08lX%s", (unsigned long)hr,
             (hr == S_OK || hr == S_FALSE) ? " (ok)" : " (FAILED)");
    }

    LOGI("win32: pre-loaded %d/%d core + %d support modules",
         ok, NMODULES, NSUPPORT);
    return ok == NMODULES;
}

/* Count ETS-to-Windows calls with minimal instrumentation and no argument or
 * stack dependency. */
static uint32_t s_w32_calls[VCT_MAX_IMP + VCT_MAX_W32];
static uint32_t s_w32_prev[VCT_MAX_IMP + VCT_MAX_W32];

static const char *w32_name(unsigned i)
{
    return i < (unsigned)G->imp_count ? G->imp[i].name
                                         : G->w32[i - G->imp_count].name;
}

void win32_report_calls(uint64_t frames)
{
    enum { TOP = 15 };
    diag_top_t top[TOP];
    unsigned n = 0, i;
    uint64_t total = 0;

    if (!frames || !g_cfg.win32_counting)
        return;
    for (i = 0; i < (unsigned)(G->imp_count + G->w32_count); i++) {
        uint32_t d = s_w32_calls[i] - s_w32_prev[i];

        s_w32_prev[i] = s_w32_calls[i];
        total += d;
        n = diag_top_insert(top, n, TOP, i, d);
    }
    if (!total)
        return;
    /* The width of the boundary is per-title DATA and comes from the profile.
     * The two titles do not have the same one. */
    LOGI("win32 boundary: %.0f calls/frame total across %d entry points",
         (double)total / (double)frames, G->imp_count + G->w32_count);
    for (i = 0; i < n; i++)
        LOGI("    %10.1f/frame  %s", (double)top[i].value / (double)frames,
             w32_name(top[i].idx));
}

int win32_bind(void)
{
    int bound = 0, unresolved = 0;
    unsigned i;

    for (i = 0; i < NMODULES; i++)
        if (!s_mod[i])
            LOGE("win32_bind: '%s' was never pre-loaded", MODULES[i]);

    /* 98 indirect entries: overwrite the pointer, leave the code alone. */
    for (i = 0; i < G->imp_count; i++) {
        const char *whence;
        void *fn = resolve(G->imp[i].name, &whence);

        if (!fn) {
            LOGW("unbound (slot 0x%08X): %s; ETS implementation left in place",
                 G->imp[i].slot, G->imp[i].name);
            unresolved++;
            continue;
        }
        if (g_cfg.win32_counting) {
            void *c = thunk_count_only(fn, &s_w32_calls[i]);
            if (c)
                fn = c;
        }
        patch_slot(G->imp[i].slot, fn);
        LOGT("slot 0x%08X <- %-30s [%s]", G->imp[i].slot,
             G->imp[i].name, whence);
        bound++;
    }

    /* 25 direct entries: jmp over the entry point. */
    for (i = 0; i < G->w32_count; i++) {
        const char *whence;
        const char *why = never_bind(G->w32[i].name);
        void *fn;

        if (why) {
            LOGI("kept  %-28s: %s", G->w32[i].name, why);
            continue;
        }
        fn = resolve(G->w32[i].name, &whence);

        if (!fn) {
            LOGW("unbound (entry 0x%08X): %s; ETS implementation left in place",
                 G->w32[i].slot, G->w32[i].name);
            unresolved++;
            continue;
        }
        if (g_cfg.win32_counting) {
            void *c = thunk_count_only(fn, &s_w32_calls[G->imp_count + i]);
            if (c)
                fn = c;
        }
        patch_jmp(G->w32[i].slot, fn);
        LOGT("jmp  0x%08X -> %-30s [%s]", G->w32[i].slot,
             G->w32[i].name, whence);
        bound++;
    }

    /* Cover the direct callers the __imp__ slot cannot reach. Diagnostic and
     * terminal: it never returns, so no ABI question arises. */
    if (G->ets_exit_process) {
        patch_jmp(G->ets_exit_process, (void *)ov_ets_exit_process);
        LOGI("win32: ETS _ExitProcess@4 entry @ 0x%08X hooked (direct callers "
             "bypass the __imp__ slot)", G->ets_exit_process);
    }

    LOGI("win32: %d/%d entry points bound, %d unresolved",
         bound, G->imp_count + G->w32_count, unresolved);
    return bound;
}

/* ---- the arcade link ---------------------------------------------------- */
/* Force Offroad's supported standalone state. Called only when [link] link =
 * off, which is the default: net_link.c supplies the ETS device in the other
 * two modes and main.c chooses between them. Enabled-but-uninitialised is
 * what fails _gamenet_ModuleInit and drops the machine into the operator
 * menu, so with no ether0 this is the correct state and not a workaround. */
static void __cdecl ov_network_Enable(int enable)
{
    static int announced;

    if (G->network_enabled)
        *(uint32_t *)(uintptr_t)G->network_enabled = 0;
    if (!announced) {
        announced = 1;
        LOGW("network: the operator setting asks for link=%d, but [link] link "
             "= off, so this host has no ETS ether0 device: forcing "
             "_Network_bEnabled=0 (standalone). "
             "Enabled-but-uninitialised is what made _gamenet_ModuleInit fail "
             "and left the game in the operator menu.", enable);
    }
}

int net_link_disable(void)
{
    if (!game_optional("network", "_network_Enable / _Network_bEnabled",
                       G->network_enable && G->network_enabled))
        return 0;
    patch_jmp(G->network_enable, (void *)ov_network_Enable);
    return 1;
}
