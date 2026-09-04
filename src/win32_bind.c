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

static LPVOID WINAPI ov_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type,
                                     DWORD protect)
{
    return VirtualAlloc(addr, size, type,
                        strip_cache_bits(protect, size, "VirtualAlloc"));
}

static BOOL WINAPI ov_VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect,
                                     PDWORD old)
{
    return VirtualProtect(addr, size,
                          strip_cache_bits(protect, size, "VirtualProtect"),
                          old);
}

static const struct { const char *name; void *fn; } OVERRIDE[] = {
    { "VirtualAlloc",          (void *)ov_VirtualAlloc          },
    { "VirtualProtect",        (void *)ov_VirtualProtect        },
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
    struct { unsigned idx; uint32_t d; } top[15];
    unsigned n = 0, i, j, k;
    uint64_t total = 0;

    if (!frames || !g_cfg.win32_counting)
        return;
    for (i = 0; i < (unsigned)(G->imp_count + G->w32_count); i++) {
        uint32_t d = s_w32_calls[i] - s_w32_prev[i];
        s_w32_prev[i] = s_w32_calls[i];
        total += d;
        if (!d)
            continue;
        for (j = 0; j < n; j++)
            if (d > top[j].d)
                break;
        if (j >= 15u)
            continue;
        if (n < 15u)
            n++;
        for (k = n - 1; k > j; k--)
            top[k] = top[k - 1];
        top[j].idx = i;
        top[j].d = d;
    }
    if (!total)
        return;
    LOGI("win32 boundary: %.0f calls/frame total across 123 entry points",
         (double)total / (double)frames);
    for (i = 0; i < n; i++)
        LOGI("    %10.1f/frame  %s", (double)top[i].d / (double)frames,
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
    if (!game_require("network", "_network_Enable / _Network_bEnabled",
                      G->network_enable && G->network_enabled))
        return 0;
    patch_jmp(G->network_enable, (void *)ov_network_Enable);
    return 1;
}
