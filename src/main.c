/* main.c -- main host entry point.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The executable reserves the game image, while this DLL retains all persistent
 * host logic. shim_run() must not return because its caller is overwritten.
 * Libraries are preloaded before mapping; patches are installed before DATA is
 * unpacked and the game CRT is entered.
 */
#include "vcthunder.h"

#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
    default:                              return "?";
    }
}

/* Emulate executed CLI/STI instructions as an interrupt-disable depth. The
 * processor fault distinguishes instructions from embedded data. Host threads
 * that represent cabinet interrupts wait on this depth; timeouts, a fault-rate
 * budget, and frame-boundary resynchronisation prevent permanent stalls. */
#define IRQ_SITES_MAX   32
/* Maximum emulated faults per window. A windowed counter detects bursts and
 * avoids lifetime-average dilution and arithmetic overflow. */
#define IRQ_FAULT_BUDGET 2000
#define IRQ_WINDOW_MS    1000u

static struct {
    volatile LONG eip;
    unsigned char op;
    volatile LONG hits;
} s_irq_site[IRQ_SITES_MAX];

static volatile LONG s_irq_depth;          /* > 0 == the game says interrupts off */
static volatile LONG s_irq_emulate = 1;
static volatile LONG s_irq_faults;
static volatile LONG s_irq_leaks;
static volatile LONG s_irq_steals;
static volatile LONG s_irq_window;         /* faults in the current window */
static DWORD         s_irq_t0;

static void irq_degrade(const char *why, DWORD eip)
{
    if (InterlockedExchange(&s_irq_emulate, 0)) {
        InterlockedExchange(&s_irq_depth, 0);
        LOGE("irq: emulation OFF; %s (last site 0x%08lX, %ld faults). The "
             "game's cli/sti go back to being nops from here, so its critical "
             "sections no longer exclude our threads. This is a REPORTED "
             "degradation, not a silent one.",
             why, (unsigned long)eip, InterlockedCompareExchange(&s_irq_faults, 0, 0));
    }
}

/* Claim or find the per-site slot. Lock-free, same shape as the thread census;
 * a site past the table is emulated but not counted, which costs the report a
 * line rather than costing correctness. */
static void irq_count(DWORD eip, unsigned char op)
{
    LONG me = (LONG)eip;
    int i;

    for (i = 0; i < IRQ_SITES_MAX; i++) {
        LONG got = InterlockedCompareExchange(&s_irq_site[i].eip, me, 0);

        if (got == me) {
            InterlockedIncrement(&s_irq_site[i].hits);
            return;
        }
        if (got == 0) {
            s_irq_site[i].op = op;
            InterlockedIncrement(&s_irq_site[i].hits);
            LOGI("irq: %s at 0x%08lX is the game's own critical section; "
                 "emulated, not healed away (site %d)",
                 op == 0xFA ? "cli" : "sti", (unsigned long)eip, i + 1);
            return;
        }
    }
}

static int heal_privileged(EXCEPTION_POINTERS *ep)
{
    DWORD eip = ep->ContextRecord->Eip;
    unsigned char *p = (unsigned char *)(uintptr_t)eip;
    unsigned char op = *p;
    LONG n;

    if (op != 0xFA && op != 0xFB)          /* cli / sti */
        return 0;

    if (!InterlockedCompareExchange(&s_irq_emulate, 0, 0)) {
        LOGI("healed: %s at 0x%08lX -> nop (emulation is off)",
             op == 0xFA ? "cli" : "sti", (unsigned long)eip);
        *p = 0x90;
        FlushInstructionCache(GetCurrentProcess(), p, 1);
        return 1;                          /* resume ON the nop, as before */
    }

    irq_count(eip, op);
    if (op == 0xFA) {
        InterlockedIncrement(&s_irq_depth);
    } else if (InterlockedDecrement(&s_irq_depth) < 0) {
        /* A bare `sti` with no matching `cli`: __p_SetThreadFPUEmuState has
         * one. Interrupts were already on; that is not an error. */
        InterlockedExchange(&s_irq_depth, 0);
    }

    /* The budget, checked on every fault and with no arithmetic that can
     * overflow: more than IRQ_FAULT_BUDGET faults inside one IRQ_WINDOW_MS is
     * over budget, full stop. The worst overshoot is one budget's worth:
     * 2,000 x 5.05 us, about 10 ms, which is a third of a frame, once. */
    InterlockedIncrement(&s_irq_faults);
    {
        DWORD now = GetTickCount();

        if (!s_irq_t0)
            s_irq_t0 = now;
        n = InterlockedIncrement(&s_irq_window);
        if (now - s_irq_t0 >= IRQ_WINDOW_MS) {
            s_irq_t0 = now;
            InterlockedExchange(&s_irq_window, 0);
        } else if (n > IRQ_FAULT_BUDGET) {
            irq_degrade("the game executes cli/sti far more often than the "
                        "budget allows", eip);
        }
    }

    /* Step over the instruction we just performed. cli and sti are one byte, so
     * this is the address the game would have resumed at either way. */
    ep->ContextRecord->Eip = eip + 1;
    return 1;
}

int game_irq_disabled(void)
{
    return InterlockedCompareExchange(&s_irq_depth, 0, 0) > 0;
}

/* Wait with a QPC deadline. SwitchToThread permits the lower-priority game
 * thread to release the interrupt-disabled region. */
int game_irq_wait(unsigned max_us)
{
    LARGE_INTEGER freq, start, now;
    LONGLONG limit;

    if (!game_irq_disabled())
        return 1;                          /* the overwhelmingly common case */

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    limit = (LONGLONG)((double)freq.QuadPart * (double)max_us / 1e6);

    for (;;) {
        int k;

        /* The regions are a handful of instructions, so spin first: a context
         * switch costs more than the wait usually does. */
        for (k = 0; k < 64; k++) {
            YieldProcessor();
            if (!game_irq_disabled())
                return 1;
        }
        SwitchToThread();
        if (!game_irq_disabled())
            return 1;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart >= limit)
            break;
    }
    InterlockedIncrement(&s_irq_steals);
    return 0;
}

void game_irq_resync(const char *where)
{
    LONG depth = InterlockedExchange(&s_irq_depth, 0);

    if (depth > 0) {
        LONG n = InterlockedIncrement(&s_irq_leaks);

        if (n <= 4 || (n % 256) == 0)
            LOGW("irq: the game was still inside a cli region at %s (depth %ld,"
                 " leak #%ld): a critical section it entered and did not "
                 "leave. Forced back to interrupts-enabled; a waiter would "
                 "otherwise have stalled until its own timeout.",
                 where, depth, n);
    }
}

void game_irq_report(void)
{
    int i;

    if (!InterlockedCompareExchange(&s_irq_emulate, 0, 0)) {
        LOGI("irq: emulation is OFF; cli/sti are nops");
        return;
    }
    LOGI("irq: %ld cli/sti faults emulated, %ld deferred ticks gave up, "
         "%ld unbalanced regions resynced at the frame boundary",
         InterlockedCompareExchange(&s_irq_faults, 0, 0),
         InterlockedCompareExchange(&s_irq_steals, 0, 0),
         InterlockedCompareExchange(&s_irq_leaks, 0, 0));
    for (i = 0; i < IRQ_SITES_MAX; i++) {
        LONG eip = InterlockedCompareExchange(&s_irq_site[i].eip, 0, 0);

        if (!eip)
            break;
        LOGI("irq:   %s 0x%08lX  %ld", s_irq_site[i].op == 0xFA ? "cli" : "sti",
             (unsigned long)eip,
             InterlockedCompareExchange(&s_irq_site[i].hits, 0, 0));
    }
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '\\');

    return s ? s + 1 : path;
}

/* The linked ImageBase of a loaded module, so a fault's EIP can be turned back
 * into the address addr2line understands. The module is already mapped, so its
 * headers are simply there to read. */
static uintptr_t pe_image_base(HMODULE h)
{
    const unsigned char *p = (const unsigned char *)h;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)p;
    const IMAGE_NT_HEADERS32 *nt;

    if (!h || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (const IMAGE_NT_HEADERS32 *)(p + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return (uintptr_t)nt->OptionalHeader.ImageBase;
}

/* A fault inside the game is the expected outcome for a while, so make it
 * actionable: report EIP and the exact command that names the function. */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD eip  = ep->ContextRecord->Eip;
    int in_game = (eip >= G->image_base && eip < G->span_end);

    /* Benign notifications, not faults. 0x406D1388 is MS_VC_EXCEPTION (the
     * debugger "name this thread" signal, which graphics runtimes raise for
     * each of their worker threads) SDL and the D3D12 driver do it by the
     * dozen. Logging them as errors buries the real ones. */
    if (code == 0x406D1388u || code == DBG_PRINTEXCEPTION_C ||
        code == 0x4001000Au)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Customer exceptions may be handled normally by runtime libraries. Log a
     * bounded sample before continuing the exception search. */
    if (code & 0x20000000u) {
        static LONG seen;
        LONG n = InterlockedIncrement(&seen);
        if (n <= 8)
            LOGW("exception 0x%08lX (customer bit) at EIP=0x%08lX %s%s",
                 (unsigned long)code, (unsigned long)eip,
                 in_game ? "inside the game image" : "outside the game image",
                 code == 0xE06D7363u ? ": MSVC C++ throw" : "");
        else if (n == 9)
            LOGW("exception: further customer-bit exceptions not logged");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code == EXCEPTION_PRIV_INSTRUCTION && in_game && heal_privileged(ep))
        return EXCEPTION_CONTINUE_EXECUTION;

    {
        char mod[MAX_PATH] = "";
        char where[160];
        HMODULE h = NULL;

        if (!in_game &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)eip, &h) && h)
            GetModuleFileNameA(h, mod, sizeof mod);
        /* Named the same way every other instrument names an address, so a
         * fault and a watchdog report can be read against each other. */
        diag_describe_addr(where, sizeof where, eip);
        LOGE("EXCEPTION 0x%08lX %s at %s",
             (unsigned long)code, exception_name(code), where);
        /* The module-relative offset, because the absolute EIP of a
         * relocated DLL names nothing: addr2line wants the RVA, and working it
         * out afterwards needs a load base nobody logged. */
        if (h)
            LOGE("  = %s+0x%lX: name it with:  i686-w64-mingw32-addr2line "
                 "-e build/%s 0x%lX", mod[0] ? base_name(mod) : "module",
                 (unsigned long)(eip - (uintptr_t)h),
                 mod[0] ? base_name(mod) : "VCThunder.dll",
                 (unsigned long)(eip - (uintptr_t)h +
                                 (uintptr_t)pe_image_base(h)));
    }

    if (code == EXCEPTION_PRIV_INSTRUCTION)
        LOGE("  a privileged instruction ran: some boundary is still unstubbed");

    if (code == EXCEPTION_ACCESS_VIOLATION)
        LOGE("  access violation %s address 0x%08lX",
             ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
             (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);

    if (in_game)
        LOGE("  identify it with:  python3 scripts/p1-symbols.py --lookup 0x%lX",
             (unsigned long)eip);

    LOGE("  esp=0x%08lX ebp=0x%08lX eax=0x%08lX ecx=0x%08lX edx=0x%08lX",
         (unsigned long)ep->ContextRecord->Esp, (unsigned long)ep->ContextRecord->Ebp,
         (unsigned long)ep->ContextRecord->Eax, (unsigned long)ep->ContextRecord->Ecx,
         (unsigned long)ep->ContextRecord->Edx);

    log_close();
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The stub is freestanding and has no argv, so recover one here. Use Windows'
 * own quote/backslash rules: argv[0] is quoted whenever the pack lives under a
 * path with spaces, and --exe / --data explicitly accept arbitrary paths. */
static int split_cmdline(char *buf, size_t bufsz, char **argv, int maxargs)
{
    LPWSTR *wide;
    size_t used = 0;
    int argc, i;

    wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide)
        return -1;
    if (argc > maxargs) {
        LocalFree(wide);
        return -1;
    }
    for (i = 0; i < argc; i++) {
        int need = WideCharToMultiByte(CP_ACP, 0, wide[i], -1, NULL, 0,
                                       NULL, NULL);

        if (need <= 0 || (size_t)need > bufsz - used) {
            LocalFree(wide);
            return -1;
        }
        argv[i] = buf + used;
        if (!WideCharToMultiByte(CP_ACP, 0, wide[i], -1, argv[i], need,
                                 NULL, NULL)) {
            LocalFree(wide);
            return -1;
        }
        used += (size_t)need;
    }
    LocalFree(wide);
    return argc;
}

/* Perform orderly host shutdown without unwinding the game's permanent frame
 * loop. Persist NVRAM and stop host threads before terminating the process;
 * mapped game memory remains valid until termination. */
void shim_exit(int code, const char *reason)
{
    static LONG s_exiting;

    if (InterlockedExchange(&s_exiting, 1)) {
        /* Another thread got here first and is already tearing down. Do not
         * race it through the same shutdowns: just wait to be terminated. */
        for (;;)
            Sleep(1000);
    }

    log_flush_game_printf();      /* a last line the game never terminated */
    LOGI("exit: %s", reason ? reason : "requested");
    nvram_flush();
    audio_shutdown();
    diego_shutdown();
    profile_shutdown();
    log_close();
    ExitProcess((UINT)code);
}

/* Resolve the command-line title to its runtime profile, data directory, and
 * image. Title selection is intentionally not read from configuration. */
static int select_title(void)
{
    const game_profile_t *p;
    unsigned i;

    if (!g_cfg.title[0]) {
        LOGE("game: no title given. Run one of:");
        for (i = 0; g_profiles[i]; i++)
            LOGE("    VCThunder.exe %-8s %s", g_profiles[i]->id,
                 g_profiles[i]->name);
        return 0;
    }

    p = game_find(g_cfg.title);
    if (!p) {
        LOGE("game: unknown title '%s'; this build carries:", g_cfg.title);
        for (i = 0; g_profiles[i]; i++)
            LOGE("    %-10s %s", g_profiles[i]->id, g_profiles[i]->name);
        return 0;
    }

    if (!game_select(p))
        return 0;

    /* Now that the game is known, its own sections of the SAME file override
     * the shared ones. This has to happen HERE: after selection, because the
     * sections are named after the title, and before the paths below, because
     * data_dir and exe are among the keys they may carry. */
    {
        int n = config_load_title(&g_cfg, "vcthunder.ini", p->id);

        if (n > 0)
            LOGI("config: [%s.*] applied over the shared sections (%d key%s)",
                 p->id, n, n == 1 ? "" : "s");
        else
            LOGI("config: no [%s.*] setting overrides; shared values apply",
                 p->id);
        config_report_notes();
    }
    if (g_log_level != g_cfg.log_level) {
        g_log_level = g_cfg.log_level;
        LOGI("config: log_level applied for %s", p->id);
    }

    /* An explicit exe= in the ini wins; otherwise derive both paths from the
     * profile and whichever data layout is on disk. */
    if (g_cfg.exe[0]) {
        LOGI("game: exe overridden by config: %s", g_cfg.exe);
        return 1;
    }
    {
        /* data_root and the destination are the same field, and snprintf with
         * overlapping buffers is undefined: take a copy of the root first. */
        char root[MAX_PATH];
        snprintf(root, sizeof root, "%s", g_cfg.data_dir);
        return game_resolve_paths(p, root,
                                  g_cfg.data_dir, sizeof g_cfg.data_dir,
                                  g_cfg.exe, sizeof g_cfg.exe);
    }
}

/* Everything the pack owns is named relative to VCThunder.exe: the ini, the
 * log, data/ and save/. That only holds if the current directory IS the pack,
 * which is true when it is run from its own folder and false for every other
 * way Windows starts a program: a shortcut, the Start menu, a file association,
 * another process. So make it true rather than requiring it. */
static void chdir_to_own_folder(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    char *slash;

    if (!n || n >= sizeof path)
        return;
    slash = strrchr(path, '\\');
    if (!slash)
        return;
    *slash = '\0';
    SetCurrentDirectoryA(path);
}

__declspec(dllexport) int shim_run(void)
{
    char cmdbuf[32768];
    char *argv[64];
    int argc = split_cmdline(cmdbuf, sizeof cmdbuf, argv, 64);

    if (argc < 0) {
        fprintf(stderr, "VCThunder: cannot parse the Windows command line\n");
        ExitProcess(2);
    }

    chdir_to_own_folder();
    if (argc == 1) {
        launcher_result_t selected = launcher_run("vcthunder.ini");

        if (selected == LAUNCHER_CANCEL)
            ExitProcess(0);
        config_defaults(&g_cfg);
        config_load_shared(&g_cfg, "vcthunder.ini");
        snprintf(g_cfg.title, sizeof g_cfg.title, "%s",
                 selected == LAUNCHER_HYDRO ? "hydro" : "offroad");
    } else {
        config_defaults(&g_cfg);
        config_load_shared(&g_cfg, "vcthunder.ini");
        if (!config_parse_argv(&g_cfg, argc, argv))
            ExitProcess(2);
    }

    log_open(g_cfg.log_file, g_cfg.log_level);
    /* After the log is open, so their warnings are not lost, and before any
     * consumer reads the keys they settle. */
    config_report_notes();
    config_resolve_graphics(&g_cfg);
    AddVectoredExceptionHandler(1, on_exception);

    /* The title is chosen ONCE, before anything reads G. Nothing below this
     * point works without it (fs_map, the preloads and every patch address are
     * all per-title), so a failure here is fatal rather than a fallback. */
    if (!select_title())
        ExitProcess(2);

    patch_init();
    fs_map_init(g_cfg.data_dir, g_cfg.save_dir);
    CreateDirectoryA(g_cfg.save_dir, NULL);

    /* Before anything touches the span: LoadLibrary stops working once the
     * game's CODE lands on this image's PE headers. */
    if (!win32_preload())
        goto fail;
    /* Same ordering rule, and for the same reason: it resolves the real
     * ws2_32 entries our socket overrides call through, and reads this
     * machine's adapter address out of iphlpapi. Both need LoadLibrary. */
    net_link_preload();
    glide_preload();                     /* absence is survivable, not fatal */
    /* The control maps live in the same file, and diego.c reads them itself
     * through the profile API rather than through config.c; see the comment
     * on ini_string(). XInput/WinMM load here. */
    diego_preload("vcthunder.ini");
    /* WASAPI is COM and pulls in its endpoint stack lazily; open the device now
     * or not at all. No device selects the title's safe silent fallback. */
    if (g_cfg.audio && !audio_preload(g_cfg.dcs_ms, g_cfg.audio_volume,
                                      g_cfg.audio_routing, g_cfg.audio_balance))
        LOGW("audio: no output device; raw hardware stays stubbed and the "
             "game uses its safe silent path");

    /* Last of the preloads, and only when asked: the module whose TLS
     * callbacks and delay-loaded import the probe fires after the mapping. */
    if (g_cfg.probe_image || g_cfg.probe_refusal)
        image_probe_preload();

    if (!game_map(g_cfg.exe))
        goto fail;

    /* The only moment CODE is the game's own: mapped, and not yet patched.
     * Nothing may be installed between here and game_map(). */
    cksum_snapshot();

    win32_bind();
    stubs_install_m1();

    /* Before anything that could fail inside the game: this is the channel the
     * game reports its own failures on, and it is the first thing you want
     * already installed when one happens. */
    log_install_game_printf();
    /* The arcade link, and it is one choice with two halves. With no ether0,
     * enabled-but-uninitialised fails _gamenet_ModuleInit and drops the
     * machine into the operator menu, so a title that would enable the link
     * has to be held standalone. With one, that override is exactly what
     * would prevent the link, so it is not installed. Whether the link then
     * runs, and which unit this cabinet is, remain the game's own operator
     * settings. */
    if (!net_link_install())
        net_link_disable();

    /* After the stubs, so these overwrite them rather than the other way round. */
    glide_bind();
    if (audio_ready() && !audio_install())
        goto fail;
    timebase_init(g_cfg.cpu_hz, g_cfg.speed_percent);
    if (!timebase_install())
        goto fail;
    if (!nvram_install(g_cfg.save_dir))
        goto fail;

    if (!game_unpack())
        goto fail;

    /* The range table is DATA: it did not exist until the line above. */
    cksum_report_pristine();

    /* And the measurement is the fix: answer the link's compatibility gate
     * with the value the line above measured, not with the sum this host's own
     * patches produce. Here because it needs that value, and before
     * cksum_report_live(), whose account of our displacement covers it. */
    cksum_install_shim();

    /* After the unpack, because the fallback writes a DATA cell, and before the
     * game runs, because _GameInit consults it on the way to the first frame. */
    audio_install_passthrough();

    if (!r2_cache_install())
        goto fail;

    /* Start host input only after DATA/.bss exists. The poller calls the
     * game's own decoder, whose state lives in that unpacked region.
     *
     * Fatal only for a title whose board we DO emulate. For one we do not, the
     * game still boots and runs its attract sequence, which is exactly what is
     * needed to work the board out: refusing to start would remove the only
     * way to observe it. */
    if (G->io_receive) {
        if (!diego_install(g_cfg.dip_switches, g_cfg.io_ms))
            goto fail;
    } else {
        LOGW("input: %s's %s board is not emulated yet; the game runs, but "
             "nothing is controllable", G->id, G->io_board);
    }

    /* Diagnostic only, and off unless the INI asks for it. */
    profile_install(g_cfg.profile_ms);

    /* Last, because it has to see every patch this host installs, and before
     * the dry-run exit, because the comparison it makes needs no game thread:
     * --dry-run reports the pristine value and our own displacement into it. */
    cksum_report_live();

    /* After every patch, because the question is what survives the state the
     * game actually runs in, and before the dry-run exit, because a probed
     * process must never be handed to the game. */
    if (g_cfg.probe_image)
        image_probe_run();
    if (g_cfg.probe_refusal)
        image_probe_refusal();

    if (g_cfg.dry_run) {
        LOGI("--dry-run: image mapped, unpacked and patched; not running");
        LOGI("  glide entry points : %d", G->glide_count);
        LOGI("  __imp__ slots      : %d", G->imp_count);
        LOGI("  direct win32       : %d", G->w32_count);
        LOGI("  hardware boundaries: %d", G->hw_count);
        audio_shutdown();
        diego_shutdown();
        game_unmap();
        patch_shutdown();
        log_close();
        ExitProcess(0);
    }

    glide_watchdog_arm();
    game_run();

    audio_shutdown();
    diego_shutdown();
    game_unmap();
    patch_shutdown();
    log_close();
    ExitProcess(0);

fail:
    LOGE("startup failed");
    audio_shutdown();
    diego_shutdown();
    game_unmap();
    patch_shutdown();
    log_close();
    ExitProcess(1);
}
