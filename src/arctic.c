/* arctic.c -- Arctic Thunder: identity, mapping, the boundary table, the run.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game's three images mapped as images (pe_host.c), every import bound
 * through one table, and the four boundaries a boot census proved the game
 * cannot start without.
 * Everything else binds to the real system DLL, and the dry run says which.
 *
 * The renderer is the provider's Glide 3 translator (renderer/core/glide3.c),
 * bound through vgl3Lookup; the window is the host's, on a thread of its own.
 *
 * The cabinet shell, its I/O board, coins and credits are arctic_io.c; the
 * operator system (diag.exe), which TEST hands over to, is arctic_diag.c in a
 * process of its own. Every file either opens is routed by arctic_fs.c: the
 * package read-only, everything written in save/.
 */
#include "vcthunder.h"
#include "arctic.h"
#include "hash.h"
#include "pe_host.h"
#include "glide_int.h"
#include "settings.h"
#include "bezel.h"

#include <tlhelp32.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IM_SNOW = ARCTIC_SNOW, IM_GOOSE = ARCTIC_GOOSE, IM_GOBJ = ARCTIC_GOBJ };

static pe_image_t s_img[ARCTIC_IMAGES];
static int s_gobj_attached;
static char s_data[MAX_PATH];
static HMODULE s_libmmd, s_msvcp60;

/* ---- the four boundaries ------------------------------------------------- */

/* goose.dll refuses to run anywhere but Windows 2000 ("GOOSE Only runs on
 * Windows 2000"). Tell it what it was built for. */
BOOL WINAPI arctic_GetVersionExA(OSVERSIONINFOA *v)
{
    if (!v)
        return FALSE;
    v->dwMajorVersion = 5;
    v->dwMinorVersion = 0;
    v->dwBuildNumber = 2195;
    v->dwPlatformId = VER_PLATFORM_WIN32_NT;
    v->szCSDVersion[0] = 0;
    return TRUE;
}

/* goose reads the IDE serial at init: it keys the RC4 the CABINET'S install
 * applied to its art. The package this host imports is plaintext, so the key
 * is computed and never used (plan §3.4). It is answered with a fixed block so
 * the user's own drive is never asked and goose's third method, raw port I/O
 * on the IDE controller, is never reached. */
/* Nonzero when `n` is a device path and the drive-identity boundary answered
 * it (a handle, or INVALID_HANDLE_VALUE with the error set). */
int arctic_device_open(LPCSTR n, HANDLE *out)
{
    if (n && !strncmp(n, "\\\\.\\", 4)) {
        if (!_stricmp(n, "\\\\.\\PhysicalDrive0")) {
            LOGT("arctic: %s answered by the host", n);
            *out = CreateEventA(NULL, TRUE, FALSE, NULL);
            return 1;
        }
        LOGT("arctic: device %s refused", n);
        SetLastError(ERROR_FILE_NOT_FOUND);
        *out = INVALID_HANDLE_VALUE;
        return 1;
    }
    return 0;
}

/* SMART_GET_VERSION and SMART_RCV_DRIVE_DATA come from the SDK's winioctl.h.
 * IOCTL_SCSI_MINIPORT is ntddscsi.h's, which windows.h does not pull in. */
#ifndef IOCTL_SCSI_MINIPORT
#define IOCTL_SCSI_MINIPORT    0x0004d008u
#endif

/* GETVERSIONINPARAMS and SENDCMDOUTPARAMS (ntdddisk.h): the IDENTIFY block
 * starts 16 bytes into the output, and its words 10-19 are the serial,
 * byte-swapped in pairs (ATA IDENTIFY DEVICE). */
BOOL WINAPI arctic_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD nin,
                                   LPVOID out, DWORD nout, LPDWORD ret,
                                   LPOVERLAPPED ov)
{
    BYTE *o = out;
    BOOL answered;

    if (arctic_keylok_ioctl(h, code, in, nin, out, nout, ret, &answered))
        return answered;
    if (code == SMART_GET_VERSION && o && nout >= 24) {
        memset(o, 0, 24);
        o[0] = 1;                   /* bVersion                          */
        o[1] = 1;                   /* bRevision                         */
        o[3] = 1;                   /* bIDEDeviceMap: primary master     */
        *(DWORD *)(o + 4) = 1 | 4;  /* CAP_ATA_ID_CMD | CAP_SMART_CMD    */
        if (ret)
            *ret = 24;
        return TRUE;
    }
    if (code == SMART_RCV_DRIVE_DATA && o && nout >= 16 + 512) {
        static const char serial[] = "VCTHUNDER-ARCTIC    ";   /* 20 */
        BYTE *id = o + 16;
        int i;

        memset(o, 0, 16 + 512);
        *(DWORD *)o = 512;
        for (i = 0; i < 20; i += 2) {
            id[20 + i] = (BYTE)serial[i + 1];
            id[21 + i] = (BYTE)serial[i];
        }
        if (ret)
            *ret = 16 + 512;
        return TRUE;
    }
    if (code == SMART_GET_VERSION || code == SMART_RCV_DRIVE_DATA ||
        code == IOCTL_SCSI_MINIPORT) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    return DeviceIoControl(h, code, in, nin, out, nout, ret, ov);
}

/* The cabinet shell (boundary 5): goose finds wincsm.exe through psapi
 * functions it fetches by name, then reads the shell's IOBlock value. Both
 * lookups are answered by arctic_io.c; everything else passes through. */
static FARPROC WINAPI b_GetProcAddress(HMODULE m, LPCSTR name)
{
    void *p = arctic_io_psapi(name);

    return p ? (FARPROC)p : GetProcAddress(m, name);
}

/* goose's half of the shell's end of the game (goose.dll 0x1000b570, kill.c):
 * a thread polls OpenEventA("GameKillEvent") every millisecond and, once it
 * opens, waits on it and calls ExitProcess(2). The shell sets it on TEST
 * (arctic_io.c); the event is this process's own, never a named one. */
static HANDLE WINAPI b_OpenEventA(DWORD access, BOOL inherit, LPCSTR name)
{
    HANDLE own = arctic_io_kill_event(), dup = NULL;

    if (name && !strcmp(name, "GameKillEvent") && own) {
        if (DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &dup,
                            0, FALSE, DUPLICATE_SAME_ACCESS))
            return dup;
        return NULL;
    }
    return OpenEventA(access, inherit, name);
}

/* THE WHEEL CALIBRATION. snow.exe will not boot without adjustments 0x3A..0x3C
 * (left range, right range, centre, each 1..255): with none it shows "WHEEL
 * NOT CALIBRATED / ENTER DIAGNOSTICS TO CORRECT" and sleeps forever, because
 * on a cabinet the operator calibrates once in diag.exe (Calibrate Feedback,
 * in the operator system: arctic_diag.c). That calibration drives the wheel
 * motor and needs a force-feedback wheel to follow it; without one it fails,
 * as on a cabinet with a dead motor. So the first time the game asks and
 * finds them absent they are seeded with the values snow.exe itself falls
 * back to (centre 0x80, ranges 0x40; feedback.c's loader), as a cabinet
 * calibrated at the factory; a real calibration replaces them. */
typedef int (__cdecl *adj_get_fn)(int, int *);
typedef int (__cdecl *adj_set_fn)(int, int);
static adj_get_fn r_get_adjustment;
static adj_set_fn r_set_adjustment;

static void seed_wheel_calibration(void)
{
    static const int seed[3] = { 0x40, 0x40, 0x80 };
    static LONG done;
    int n, ok = 1;

    if (InterlockedExchange(&done, 1))
        return;
    for (n = 0; n < 3; n++) {
        int v = 0;

        if (r_get_adjustment(0x3A + n, &v) || v <= 0 || v >= 0x100)
            ok = 0;
    }
    if (ok || !r_set_adjustment)
        return;
    for (n = 0; n < 3; n++)
        r_set_adjustment(0x3A + n, seed[n]);
    LOGW("arctic: no wheel calibration in CMOS (adjustments 58-60), as on a"
         " cabinet whose operator never ran diagnostics; seeded with the"
         " game's own fallback: left %d, right %d, centre %d", seed[0],
         seed[1], seed[2]);
}

static int __cdecl b_get_adjustment(int n, int *v)
{
    if (n >= 0x3A && n <= 0x3C)
        seed_wheel_calibration();
    return r_get_adjustment(n, v);
}

/* A cabinet has nobody to click OK. */
int WINAPI arctic_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT f)
{
    (void)w; (void)f;
    LOGW("arctic: the game asked for a message box [%s] %s",
         cap ? cap : "", text ? text : "");
    return IDOK;
}

/* goose loads gobj.dll at init; it is ours already, mapped and bound, and its
 * DllMain runs at the moment goose asks for it, as on the cabinet. */
static HMODULE WINAPI b_LoadLibraryA(LPCSTR n)
{
    const char *base = n ? strrchr(n, '\\') : NULL;

    base = base ? base + 1 : n;
    if (base && !_stricmp(base, s_img[IM_GOBJ].name)) {
        if (!s_gobj_attached) {
            s_gobj_attached = 1;
            if (!pe_attach(&s_img[IM_GOBJ]))
                return NULL;
        }
        return s_img[IM_GOBJ].mod;
    }
    return LoadLibraryA(n);
}

/* In this process the image Windows calls "the executable" is VCThunder.exe.
 * To the game it is snow.exe: the module it finds its own version resource,
 * its folder and its window instance through. Asking about any other module
 * is answered as normal. */
static HMODULE WINAPI b_GetModuleHandleA(LPCSTR n)
{
    return n ? GetModuleHandleA(n) : s_img[IM_SNOW].mod;
}

static DWORD WINAPI b_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap)
{
    if (!m || m == GetModuleHandleA(NULL))
        m = s_img[IM_SNOW].mod;
    return GetModuleFileNameA(m, out, cap);
}

/* The game's own diagnostic channel: snow.exe is a console program and it,
 * goose and gobj report failures with printf and fprintf(stderr). Those reach
 * the log; a real file passes through untouched. */
static void say(const char *who, const char *fmt, va_list ap)
{
    char buf[512];
    size_t n;

    vsnprintf(buf, sizeof buf, fmt, ap);
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    if (n)
        LOGI("arctic: game %s: %s", who, buf);
}

static int __cdecl b_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    say("printf", fmt, ap);
    va_end(ap);
    return 1;
}

static int __cdecl b_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int r = 1;

    va_start(ap, fmt);
    if (f == stdout || f == stderr)
        say(f == stderr ? "stderr" : "stdout", fmt, ap);
    else
        r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

/* --trace only: every DISTINCT string the game formats, which is how what it
 * puts on screen is read without a renderer. A diagnostic, not a boundary. */
static int __cdecl b_sprintf(char *out, const char *fmt, ...)
{
    static unsigned seen[512];
    static LONG nseen;
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsprintf(out, fmt, ap);
    va_end(ap);
    if (g_log_level >= LOG_TRACE) {
        unsigned h = 2166136261u;
        const char *c;
        LONG i, n = nseen;

        for (c = out; *c; c++)
            h = (h ^ (unsigned char)*c) * 16777619u;
        for (i = 0; i < n && seen[i] != h; i++)
            ;
        if (i == n && n < 512) {
            seen[InterlockedIncrement(&nseen) - 1] = h;
            LOGT("arctic: game text \"%s\"", out);
        }
    }
    return r;
}

/* Start the other half of the cabinet, as wincsm.exe starts diagnostics after
 * the game and the game after diagnostics: this executable again, from this
 * pack's folder, with this process's own arguments and the title added when
 * they lack it (a run started from the launcher has none, and the game must
 * not come back as the launcher). For the operator system the new process
 * carries OPERATOR_VAR naming this one, and it honours that only when this
 * process is its parent (arctic_operator_requested()): the operator system
 * is reached from TEST during the game, as on a cabinet, and no other way. */
#define OPERATOR_VAR "VCTHUNDER_OPERATOR_FOR"

static int names_title(const char *args)
{
    size_t n = strlen(AT->ids->id);

    while (*args) {
        const char *w;

        while (*args == ' ' || *args == '\t')
            args++;
        w = args;
        while (*args && *args != ' ' && *args != '\t')
            args++;
        if ((size_t)(args - w) == n && !_strnicmp(w, AT->ids->id, n))
            return 1;
    }
    return 0;
}

int arctic_spawn(int operator_system)
{
    char dir[MAX_PATH], path[MAX_PATH], cmd[4096], token[16];
    const char *tail = GetCommandLineA();
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *slash;
    int quoted = 0, add;
    BOOL ok;

    if (!GetModuleFileNameA(NULL, dir, sizeof dir) ||
        !(slash = strrchr(dir, '\\')))
        return 0;
    *slash = 0;
    if (!GetModuleFileNameA(NULL, path, sizeof path))
        return 0;
    /* The command line past argv[0], as CommandLineToArgv would split it. */
    while (*tail == ' ' || *tail == '\t')
        tail++;
    for (; *tail && (quoted || (*tail != ' ' && *tail != '\t')); tail++)
        if (*tail == '"')
            quoted = !quoted;
    add = !names_title(tail);
    snprintf(cmd, sizeof cmd, "\"%s\"%s%s", path, add ? " " : "",
             add ? AT->ids->id : "");
    snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), "%s", tail);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    snprintf(token, sizeof token, "%lu", (unsigned long)GetCurrentProcessId());
    SetEnvironmentVariableA(OPERATOR_VAR, operator_system ? token : NULL);
    ok = CreateProcessA(path, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi);
    SetEnvironmentVariableA(OPERATOR_VAR, NULL);
    if (!ok) {
        LOGE("arctic: cannot start %s (error %lu)", path, GetLastError());
        return 0;
    }
    LOGI("arctic: started %s%s (pid %lu)", operator_system ? "the operator"
         " system: " : "", cmd, (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

int arctic_operator_requested(void)
{
    char v[16];
    DWORD want, me = GetCurrentProcessId(), parent = 0;
    PROCESSENTRY32 pe;
    HANDLE snap;

    if (!GetEnvironmentVariableA(OPERATOR_VAR, v, sizeof v))
        return 0;
    SetEnvironmentVariableA(OPERATOR_VAR, NULL);    /* not to the next game */
    want = (DWORD)strtoul(v, NULL, 10);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32First(snap, &pe); more; more = Process32Next(snap, &pe))
        if (pe.th32ProcessID == me) {
            parent = pe.th32ParentProcessID;
            break;
        }
    CloseHandle(snap);
    return want && parent == want;
}

static __attribute__((noreturn)) void arctic_exit(int code)
{
    int diag = code == 2 && arctic_io_test_requested();

    arctic_io_stop();
    arctic_keylok_report();
    arctic_seh_report();
    if (diag) {
        LOGI("arctic: the game exited on the shell's diagnostic request;"
             " starting the operator system, as wincsm.exe runs diag.exe");
        if (!arctic_spawn(1))
            code = 1;
    }
    LOGI("arctic: exit %d", code);
    log_close();
    ExitProcess((UINT)code);
}

static void WINAPI b_ExitProcess(UINT c) { arctic_exit((int)c); }
static void __cdecl b_exit(int c) { arctic_exit(c); }

/* The cabinet's shell started snow.exe with no arguments; the host's own
 * command line is not the game's. */
typedef int (__cdecl *getmainargs_fn)(int *, char ***, char ***, int, void *);
static int __cdecl b_getmainargs(int *argc, char ***argv, char ***env, int wild,
                                 void *si)
{
    static char *args[2];
    getmainargs_fn real = (getmainargs_fn)(void *)
        GetProcAddress(GetModuleHandleA("msvcrt.dll"), "__getmainargs");
    int r = real ? real(argc, argv, env, wild, si) : -1;

    args[0] = s_img[IM_SNOW].path;
    args[1] = NULL;
    *argc = 1;
    *argv = args;
    return r;
}

/* ---- --trace: where every thread is ------------------------------------- */

static void describe(char *out, size_t cap, uint32_t a)
{
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    const char *base;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)a, &m) && m)
        GetModuleFileNameA(m, path, sizeof path);
    base = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    if (m)
        snprintf(out, cap, "%s+%#lx", base,
                 (unsigned long)(a - (uint32_t)(uintptr_t)m));
    else
        snprintf(out, cap, "%#lx", (unsigned long)a);
}

/* Suspend each other thread, read EIP and walk up to six EBP frames that stay
 * inside its stack. A hang reads as the same frames twice. Nothing that takes
 * a lock runs while a thread is suspended: it may hold the loader's or the
 * log's, so the frames are named and logged after it resumes. */
static void stack_census(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();

    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE t;
        CONTEXT c;
        char line[600], w[96];
        size_t k;
        uint32_t ebp, at[7];
        int i, n = 0;

        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
            continue;
        t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t)
            continue;
        SuspendThread(t);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t, &c)) {
            at[n++] = c.Eip;
            ebp = c.Ebp;
            for (i = 0; i < 6 && ebp > c.Esp && ebp - c.Esp < 0x100000 &&
                        !IsBadReadPtr((void *)(uintptr_t)ebp, 8); i++) {
                at[n++] = ((uint32_t *)(uintptr_t)ebp)[1];
                ebp = ((uint32_t *)(uintptr_t)ebp)[0];
            }
        }
        ResumeThread(t);
        CloseHandle(t);
        if (!n)
            continue;
        describe(w, sizeof w, at[0]);
        k = snprintf(line, sizeof line, "t%lu %s", te.th32ThreadID, w);
        for (i = 1; i < n && k < sizeof line - 100; i++) {
            describe(w, sizeof w, at[i]);
            k += snprintf(line + k, sizeof line - k, " < %s", w);
        }
        LOGT("arctic: stack %s", line);
    }
    CloseHandle(snap);
}

static DWORD WINAPI census_thread(LPVOID p)
{
    static const DWORD at_ms[] = { 70000, 75000 };
    DWORD t0 = GetTickCount();
    unsigned i;

    (void)p;
    for (i = 0; i < sizeof at_ms / sizeof at_ms[0]; i++) {
        DWORD now = GetTickCount() - t0;

        if (now < at_ms[i])
            Sleep(at_ms[i] - now);
        LOGT("arctic: stack census at %lus", (unsigned long)(at_ms[i] / 1000));
        stack_census();
    }
    return 0;
}

/* ---- the renderer --------------------------------------------------------- */

typedef void *(__stdcall *vgl3_lookup_fn)(const char *name, int argbytes);
static vgl3_lookup_fn s_vgl3;

/* An import is spelled `_name@bytes`; the provider checks both halves. */
static void *provider3(const char *imp)
{
    char name[64];
    const char *at = imp ? strrchr(imp, '@') : NULL;
    size_t n;

    if (!s_vgl3 || !imp || imp[0] != '_' || !at || (n = (size_t)(at - imp - 1)) >= sizeof name)
        return NULL;
    memcpy(name, imp + 1, n);
    name[n] = 0;
    return s_vgl3(name, atoi(at + 1));
}

/* ---- the window (boundary 6) --------------------------------------------- */

/* The game's draws arrive on goose's process-loop thread and its main thread
 * blocks for good after init, so a window created by either would never be
 * pumped. The host's lives on a thread of its own that does nothing else. */
static HWND s_wnd;
static HANDLE s_wnd_ready;
static DWORD s_wnd_thread;
#define MSG_STARTUP_FULLSCREEN (WM_APP + 1)
static int s_client_w = 640, s_client_h = 480;

/* Borderless fullscreen, as window.c gives the other titles: the window
 * becomes a frameless popup the size of its monitor and the display mode is
 * never changed; the presenters follow the client size each frame. Only the
 * window's own thread restyles it. */
static int s_fullscreen;
static LONG s_saved_style, s_saved_ex;
static RECT s_saved_rect;

static void set_fullscreen(int on)
{
    MONITORINFO mi;

    if (!s_wnd || on == s_fullscreen)
        return;
    if (on) {
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(s_wnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            LOGW("arctic: no monitor info (%lu); staying windowed", GetLastError());
            return;
        }
        GetWindowRect(s_wnd, &s_saved_rect);
        s_saved_style = GetWindowLongA(s_wnd, GWL_STYLE);
        s_saved_ex = GetWindowLongA(s_wnd, GWL_EXSTYLE);
        SetWindowLongA(s_wnd, GWL_STYLE,
                       (s_saved_style & ~(WS_CAPTION | WS_THICKFRAME |
                                          WS_MINIMIZE | WS_MAXIMIZE)) | WS_POPUP);
        SetWindowLongA(s_wnd, GWL_EXSTYLE,
                       s_saved_ex & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                      WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(s_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOSENDCHANGING);
        LOGI("arctic: borderless fullscreen, %ldx%ld",
             (long)(mi.rcMonitor.right - mi.rcMonitor.left),
             (long)(mi.rcMonitor.bottom - mi.rcMonitor.top));
    } else {
        /* The rectangle itself, past WM_WINDOWPOSCHANGING: the renderer's
         * subclass answers that with the position and size it last saw, so
         * a placement restore comes back at 0,0 and monitor-sized. */
        SetWindowLongA(s_wnd, GWL_STYLE, s_saved_style);
        SetWindowLongA(s_wnd, GWL_EXSTYLE, s_saved_ex);
        SetWindowPos(s_wnd, NULL, s_saved_rect.left, s_saved_rect.top,
                     s_saved_rect.right - s_saved_rect.left,
                     s_saved_rect.bottom - s_saved_rect.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOSENDCHANGING);
        LOGI("arctic: windowed at %ld,%ld", (long)s_saved_rect.left,
             (long)s_saved_rect.top);
    }
    s_fullscreen = on;
    SetCursor(on ? NULL : LoadCursorA(NULL, IDC_ARROW));
}

/* Alt+Enter, polled: the renderer's subclass sits in front of this window's
 * procedure (window.c polls for the same reason). One toggle per press, and
 * only while this window has the foreground. */
static void CALLBACK hotkey_tick(HWND h, UINT m, UINT_PTR id, DWORD t)
{
    static int was_down;
    int down = (GetAsyncKeyState(VK_MENU) & 0x8000) &&
               (GetAsyncKeyState(VK_RETURN) & 0x8000);

    (void)h; (void)m; (void)id; (void)t;
    if (down && !was_down && GetForegroundWindow() == s_wnd)
        set_fullscreen(!s_fullscreen);
    was_down = down;
}

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE || (m == WM_KEYDOWN && wp == VK_ESCAPE)) {
        LOGI("arctic: %s; quitting", m == WM_CLOSE ? "window closed" : "Escape");
        arctic_exit(0);
    }
    /* Alt+Enter is taken by the poll; without this the system beeps. */
    if (m == WM_SYSCHAR && wp == VK_RETURN)
        return 0;
    if (m == WM_SETCURSOR && s_fullscreen && LOWORD(lp) == HTCLIENT) {
        SetCursor(NULL);
        return TRUE;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID p)
{
    WNDCLASSA wc;
    RECT r = { 0, 0, 0, 0 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    MSG msg;

    (void)p;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = host_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "VCThunderArctic";
    RegisterClassA(&wc);
    r.right = s_client_w;
    r.bottom = s_client_h;
    AdjustWindowRect(&r, style, FALSE);
    s_wnd = CreateWindowExA(0, wc.lpszClassName, AT->ids->name,
                            style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, NULL, NULL,
                            wc.hInstance, NULL);
    SetEvent(s_wnd_ready);
    if (!s_wnd)
        return 1;
    SetTimer(NULL, 0, 15, hotkey_tick);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == MSG_STARTUP_FULLSCREEN) {
            set_fullscreen(1);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* The surface open, after which the renderer has adopted the window. Going
 * fullscreen before that hands it a borderless, monitor-sized window, and the
 * way back then keeps a stale client size; so [graphics] fullscreen is
 * applied from here, by the window's own thread. */
typedef uint32_t (__stdcall *win_open_fn)(uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t,
                                          uint32_t);
static win_open_fn s_real_open;

static uint32_t __stdcall b_grSstWinOpen(uint32_t hwnd, uint32_t res,
                                         uint32_t refresh, uint32_t format,
                                         uint32_t origin, uint32_t ncolor,
                                         uint32_t naux)
{
    uint32_t r = s_real_open(hwnd, res, refresh, format, origin, ncolor, naux);

    if (g_cfg.fullscreen && s_wnd_thread)
        PostThreadMessageA(s_wnd_thread, MSG_STARTUP_FULLSCREEN, 0, 0);
    return r;
}

/* gobj.dll creates its Glide window and hands it to grSstWinOpen; it gets the
 * host's instead, created once, sized from [graphics] window_scale. */
static HWND WINAPI b_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style,
                                     int x, int y, int w, int h, HWND parent,
                                     HMENU menu, HINSTANCE inst, LPVOID lp)
{
    (void)ex; (void)cls; (void)name; (void)style; (void)x; (void)y; (void)w;
    (void)h; (void)parent; (void)menu; (void)inst; (void)lp;
    if (!s_wnd) {
        unsigned scale = g_cfg.window_scale ? g_cfg.window_scale : 1;
        int aw, ah, hole;

        s_client_w = arctic_ws_picture_width() * (int)scale;
        s_client_h = 480 * (int)scale;
        /* Under bezel art the client is the art's shape with its hole as tall
         * as the picture (bezel.c, the rule window.c uses); the provider puts
         * the picture in the hole. The scale was fitted without the art, so a
         * frame wider than the work area takes a smaller one. */
        if (window_bezel(&aw, &ah, &hole)) {
            RECT work;
            int fits = SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);

            for (;;) {
                bezel_client_size(aw, ah, hole, 480, (int)scale,
                                  &s_client_w, &s_client_h);
                if (scale <= 1 || !fits ||
                    (s_client_w <= work.right - work.left &&
                     s_client_h <= work.bottom - work.top))
                    break;
                scale--;
            }
        }
        s_wnd_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, window_thread, NULL, 0, &s_wnd_thread));
        WaitForSingleObject(s_wnd_ready, INFINITE);
        LOGI("arctic: the game's Glide window is the host's (%dx%d client)",
             s_client_w, s_client_h);
    }
    return s_wnd;
}

/* ---- the table ----------------------------------------------------------- */

static const struct {
    const char *dll, *name;
    void *fn;
} k_bound[] = {
    { "KERNEL32.dll", "GetVersionExA",    (void *)arctic_GetVersionExA },
    { "KERNEL32.dll", "DeviceIoControl",  (void *)arctic_DeviceIoControl },
    { "KERNEL32.dll", "CreateProcessA",   (void *)arctic_CreateProcessA },
    { "KERNEL32.dll", "LoadLibraryA",     (void *)b_LoadLibraryA },
    { "KERNEL32.dll", "ExitProcess",      (void *)b_ExitProcess },
    { "KERNEL32.dll", "GetModuleHandleA",   (void *)b_GetModuleHandleA },
    { "KERNEL32.dll", "GetModuleFileNameA", (void *)b_GetModuleFileNameA },
    { "USER32.dll",   "MessageBoxA",      (void *)arctic_MessageBoxA },
    { "MSVCRT.dll",   "_except_handler3", (void *)arctic_except_handler3 },
    { "MSVCRT.dll",   "exit",             (void *)b_exit },
    { "MSVCRT.dll",   "_exit",            (void *)b_exit },
    { "MSVCRT.dll",   "__getmainargs",    (void *)b_getmainargs },
    { "MSVCRT.dll",   "printf",           (void *)b_printf },
    { "MSVCRT.dll",   "fprintf",          (void *)b_fprintf },
    { "MSVCRT.dll",   "sprintf",          (void *)b_sprintf },
};

static void *resolve(const char *importer, const char *dll, const char *name,
                     unsigned ord, pe_bind_kind_t *kind)
{
    HMODULE h;
    unsigned i;

    if (!_stricmp(dll, "glide3x.dll")) {
        void *fn = provider3(name), *ws = name ? arctic_ws_bind(name, fn) : NULL;

        if (name && fn && !strcmp(name, "_grSstWinOpen@28")) {
            s_real_open = (win_open_fn)(ws ? ws : fn);
            *kind = PE_BIND_HOST;
            return (void *)b_grSstWinOpen;
        }
        *kind = ws ? PE_BIND_HOST : PE_BIND_PROVIDER;
        return ws ? ws : fn;
    }
    /* gobj.dll's Glide window is the host's (boundary 6). */
    if (name && !_stricmp(importer, s_img[IM_GOBJ].name) &&
        !_stricmp(dll, "USER32.dll") && !strcmp(name, "CreateWindowExA")) {
        *kind = PE_BIND_HOST;
        return (void *)b_CreateWindowExA;
    }
    /* The wheel calibration the game reads at boot (b_get_adjustment). */
    if (name && !_stricmp(importer, s_img[IM_SNOW].name) &&
        !_stricmp(dll, s_img[IM_GOOSE].name) && !strcmp(name, "get_adjustment")) {
        r_get_adjustment = (adj_get_fn)pe_export(&s_img[IM_GOOSE], name);
        r_set_adjustment = (adj_set_fn)pe_export(&s_img[IM_GOOSE], "set_adjustment");
        *kind = PE_BIND_HOST;
        return r_get_adjustment ? (void *)b_get_adjustment : NULL;
    }
    /* The cabinet shell's lookup (boundary 5). */
    if (name && !_stricmp(importer, s_img[IM_GOOSE].name)) {
        void *fn = NULL;

        if (!_stricmp(dll, "KERNEL32.dll") && !strcmp(name, "GetProcAddress"))
            fn = (void *)b_GetProcAddress;
        else if (!_stricmp(dll, "ADVAPI32.dll") && !strcmp(name, "RegOpenKeyExA"))
            fn = (void *)arctic_RegOpenKeyExA;
        else if (!_stricmp(dll, "ADVAPI32.dll") && !strcmp(name, "RegQueryValueExA"))
            fn = (void *)arctic_RegQueryValueExA;
        else if (!_stricmp(dll, "ADVAPI32.dll") && !strcmp(name, "RegCloseKey"))
            fn = (void *)arctic_RegCloseKey;
        else if (!_stricmp(dll, "KERNEL32.dll") && !strcmp(name, "OpenEventA"))
            fn = (void *)b_OpenEventA;
        if (fn) {
            *kind = PE_BIND_HOST;
            return fn;
        }
    }
    if (!_stricmp(dll, s_img[IM_GOOSE].name)) {
        *kind = PE_BIND_GAME;
        return name ? pe_export(&s_img[IM_GOOSE], name) : NULL;
    }
    if (!_stricmp(dll, s_img[IM_GOBJ].name)) {
        *kind = PE_BIND_GAME;
        return name ? pe_export(&s_img[IM_GOBJ], name) : NULL;
    }
    for (i = 0; name && i < sizeof k_bound / sizeof k_bound[0]; i++)
        if (!_stricmp(dll, k_bound[i].dll) && !strcmp(name, k_bound[i].name)) {
            *kind = PE_BIND_HOST;
            return k_bound[i].fn;
        }
    if (name) {
        void *fn = arctic_fs_bound(dll, name);

        if (fn) {
            *kind = PE_BIND_HOST;
            return fn;
        }
    }
    *kind = PE_BIND_REAL;
    if (!_stricmp(dll, "libmmd.dll"))
        h = s_libmmd;
    else if (!_stricmp(dll, "MSVCP60.dll"))
        h = s_msvcp60;
    else
        h = LoadLibraryA(dll);
    if (!h)
        return NULL;
    if (!name && !_stricmp(dll, "DSOUND.dll")) {
        void *fn = arctic_dsound_bind(h, ord);

        if (fn) {
            *kind = PE_BIND_HOST;
            return fn;
        }
    }
    return name ? (void *)GetProcAddress(h, name)
                : (void *)GetProcAddress(h, MAKEINTRESOURCEA(ord));
}

/* ---- the settings it does not use ---------------------------------------- */

/* WHAT ARCTIC DOES NOT IMPLEMENT YET, BY KEY, and the one list of it: the run
 * resets the ones that would reach it, and the launcher marks every one on its
 * row. Two kinds. Graphics keys the provider or the window would act on, which
 * wait for later enhancements or for work that is not planned (the ETS
 * video-mode table); render_scale, widescreen,
 * widescreen_hud, fullscreen and bezel are not among them, nor texmag
 * and upscale_2d, which act only above 1x and would otherwise act there at
 * their defaults while the launcher called them unused;
 * and keys only the ETS host
 * reads (its audio interrupt, timebase, link, DIP switches, board poll and
 * derived pad rumble's spring, which acts on a force byte Arctic has none of;
 * every other rumble key drives arctic_ffb.c's model), which reach nothing
 * here and are listed so the launcher
 * can say so. Arctic's timing and link are the game's own; its sound is the
 * game's, routed by [audio] (arctic_dsound.c). */
static const struct { const char *section, *key; } k_unused[] = {
    { "graphics", "aspect" },         { "graphics", "frame_rate" },
    { "graphics", "raster_height" },  { "graphics", "video_mode" },
    { "audio", "dcs_us" },
    { "timing", "cpu_hz" }, { "timing", "speed_percent" },
    { "timing", "limiter_idle" },
    { "controls", "dip_switches" }, { "controls", "io_ms" },
    { "link", "link" }, { "link", "link_rx_budget" },
    { "ffb", "direction" },               { "ffb", "rumble_spring_percent" },
};

int arctic_setting_unused(const char *section, const char *key)
{
    unsigned i;

    for (i = 0; i < sizeof k_unused / sizeof k_unused[0]; i++)
        if (!_stricmp(section, k_unused[i].section) &&
            !_stricmp(key, k_unused[i].key))
            return 1;
    return 0;
}

static void field_text(const setting_desc_t *d, const void *field, char *out,
                       size_t cap)
{
    if (d->store == VCT_STORE_STR || d->store == VCT_STORE_ENUM)
        snprintf(out, cap, "%.*s", (int)d->field_size, (const char *)field);
    else if (d->store == VCT_STORE_BOOL)
        snprintf(out, cap, "%s", *(const int *)field ? "true" : "false");
    else
        snprintf(out, cap, "%d", *(const int *)field);
}

/* Every listed key with a field in the config goes back to its default, so
 * nothing downstream can act on a value set for the other two cabinets. */
static void gate_settings(void)
{
    shim_config_t dflt;
    unsigned i;

    for (i = 0; i < sizeof k_unused / sizeof k_unused[0]; i++) {
        unsigned j;

        for (j = 0; j < VCT_SETTING_COUNT; j++)
            if (!strcmp(vct_setting[j].section, k_unused[i].section) &&
                !strcmp(vct_setting[j].key, k_unused[i].key))
                break;
        if (j == VCT_SETTING_COUNT)
            LOGW("arctic: k_unused names [%s] %s, which settings.def does not"
                 " have", k_unused[i].section, k_unused[i].key);
    }
    config_defaults(&dflt);
    for (i = 0; i < VCT_SETTING_COUNT; i++) {
        const setting_desc_t *d = &vct_setting[i];
        BYTE *have = (BYTE *)&g_cfg + d->field_off;
        const BYTE *want = (const BYTE *)&dflt + d->field_off;
        char was[64];

        if (!d->field_size || !arctic_setting_unused(d->section, d->key) ||
            !memcmp(have, want, d->field_size))
            continue;
        field_text(d, have, was, sizeof was);
        memcpy(have, want, d->field_size);
        LOGI("config: [%s] %s = %s is ignored: %s does not use it yet",
             d->section, d->key, was, AT->ids->name);
    }
}

/* window_scale is a whole multiple of the picture (640x480, or its 16:9
 * raster under widescreen), clamped to the primary monitor's work area as the
 * ETS titles' window clamps it; `auto` is the largest that fits. The operator
 * system's narrower window shares the answer and always fits. */
static void resolve_window_scale(void)
{
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    unsigned want = g_cfg.window_scale, fit;
    RECT work, r;

    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        g_cfg.window_scale = want ? want : 1;
        return;
    }
    for (fit = 8; fit > 1; fit--) {
        SetRect(&r, 0, 0, arctic_ws_wanted_width() * (int)fit, 480 * (int)fit);
        AdjustWindowRect(&r, style, FALSE);
        if (r.right - r.left <= work.right - work.left &&
            r.bottom - r.top <= work.bottom - work.top)
            break;
    }
    if (!want)
        LOGI("config: [graphics] window_scale = auto is %u here", fit);
    else if (want > fit)
        LOGW("window: scale %u needs %ux%u, which does not fit the %ldx%ld work"
             " area: using %u", want, (unsigned)arctic_ws_wanted_width() * want,
             480 * want,
             (long)(work.right - work.left), (long)(work.bottom - work.top), fit);
    g_cfg.window_scale = !want || want > fit ? fit : want;
}

/* The title's own ini sections, the settings it does not use, the window
 * scale, the audio routing, and the game folder as an absolute path (the run changes directory,
 * so nothing may be left relative to the pack). Shared with the operator
 * system's process. */
void arctic_prepare(char *data, size_t cap)
{
    char rel[MAX_PATH];
    int n = config_load_title(&g_cfg, "vcthunder.ini", AT->ids->settings);

    LOGI("config: %d [%s.*] key%s applied", n, AT->ids->settings,
         n == 1 ? "" : "s");
    config_report_notes();
    g_log_level = g_cfg.log_level;
    gate_settings();
    resolve_window_scale();
    arctic_dsound_init();
    snprintf(rel, sizeof rel, "%s\\%s", g_cfg.data_dir, AT->ids->id);
    if (!GetFullPathNameA(rel, (DWORD)cap, data, NULL))
        snprintf(data, cap, "%s", rel);
}

/* ---- identity, mapping, the run ------------------------------------------ */

/* The file, read whole, hashed and sized against generated/arctic_ids.c.
 * Returns the bytes (the caller frees them) or NULL, having said why. */
BYTE *arctic_read_pinned(const char *dir, const arctic_file_t *f, char *path,
                         size_t path_sz, size_t *len)
{
    char rel[MAX_PATH], hex[65];
    HANDLE h;
    DWORD got = 0;
    BYTE *buf;

    snprintf(rel, sizeof rel, "%s\\%s", dir, f->name);
    if (GetFileAttributesA(rel) == INVALID_FILE_ATTRIBUTES &&
        !fs_resolve_ci(dir, f->name, rel, sizeof rel)) {
        LOGE("arctic: %s not found in %s", f->name, dir);
        return NULL;
    }
    snprintf(path, path_sz, "%s", rel);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LOGE("arctic: cannot open %s (error %lu)", path, GetLastError());
        return NULL;
    }
    *len = GetFileSize(h, NULL);
    buf = *len == f->size ? malloc(*len) : NULL;
    if (buf && (!ReadFile(h, buf, (DWORD)*len, &got, NULL) || got != *len)) {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    if (!buf) {
        LOGE("arctic: %s is %lu bytes; the registered %s build's is %lu",
             path, (unsigned long)*len, AT->ids->build, (unsigned long)f->size);
        return NULL;
    }
    sha256_hex(buf, *len, hex);
    if (strcmp(hex, f->sha256)) {
        LOGE("arctic: %s has SHA-256 %s, not the registered %s build's %s",
             path, hex, AT->ids->build, f->sha256);
        free(buf);
        return NULL;
    }
    return buf;
}

static int map_all(void)
{
    unsigned i;

    for (i = 0; i < ARCTIC_IMAGES; i++) {
        const arctic_file_t *f = &AT->ids->file[i];
        char path[MAX_PATH];
        size_t len = 0;
        BYTE *bytes = arctic_read_pinned(s_data, f, path, sizeof path, &len);
        int ok;

        if (!bytes)
            return 0;
        ok = pe_map(&s_img[i], path, bytes, len);
        free(bytes);
        if (!ok)
            return 0;
    }
    return 1;
}

/* The game's two runtime DLLs are generic and load normally, from its folder,
 * before anything that imports them is bound. */
static int load_runtime(void)
{
    char p[MAX_PATH];

    snprintf(p, sizeof p, "%s\\libmmd.dll", s_data);
    s_libmmd = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    snprintf(p, sizeof p, "%s\\msvcp60.dll", s_data);
    s_msvcp60 = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s_libmmd || !s_msvcp60) {
        LOGE("arctic: cannot load libmmd.dll / msvcp60.dll from %s", s_data);
        return 0;
    }
    return 1;
}

void arctic_run(void)
{
    pe_bind_stats_t st;
    unsigned i;
    int ok = 1;

    LOGI("arctic: %s (%s), a win32 title: mapped as images and bound",
         AT->ids->name, AT->ids->build);
    /* THE WIN32 game_require(): the window is sized from the ws table's
     * native_w/native_h, which is the picture the game composes, so a title
     * whose signatures did not match has no size to open at. Refuse by name
     * rather than invent a 640x480 -- hud and ffb are genuinely optional and
     * refuse in their own arm instead. */
    if (!AT->ws) {
        LOGE("arctic: %s has no widescreen table, so no native picture size;"
             " regenerate generated/ for it (scripts/gen-gamedefs.py)",
             AT->ids->id);
        arctic_exit(1);
    }
    arctic_prepare(s_data, sizeof s_data);
    arctic_fs_init(s_data);

    if (!map_all() || !load_runtime() ||
        !arctic_ws_arm(s_img[IM_SNOW].mod, s_img[IM_GOBJ].mod,
                       s_img[IM_SNOW].preferred))
        arctic_exit(1);
    /* A refusal only leaves the pad still: it says so when rumble starts. */
    arctic_ffb_arm(s_img[IM_SNOW].mod, s_img[IM_SNOW].preferred);
    menu_wheel_arctic_arm(s_img[IM_SNOW].mod);
    arctic_keylok_arm(s_img[IM_SNOW].mod);
    if (!glide_preload() ||
        !(s_vgl3 = (vgl3_lookup_fn)glide_provider_proc("vgl3Lookup@8"))) {
        LOGE("arctic: vcglide.dll with a Glide 3 translator (vgl3Lookup@8) is"
             " needed and was not found");
        arctic_exit(1);
    }

    memset(&st, 0, sizeof st);
    for (i = 0; i < ARCTIC_IMAGES; i++)
        ok &= pe_bind(&s_img[i], resolve, &st);

    LOGI("arctic: imports %u: %u real, %u host boundaries, %u between the game's"
         " own images, %u Glide 3 to the provider, %u unresolved",
         st.total, st.kind[PE_BIND_REAL], st.kind[PE_BIND_HOST],
         st.kind[PE_BIND_GAME], st.kind[PE_BIND_PROVIDER], st.missing);
    if (!ok)
        arctic_exit(1);

    /* Before the directory changes, since the ini is found relative to it,
     * and before --dry-run stops, which then logs the resolved controls as
     * the other titles' dry run does. */
    arctic_io_start("vcthunder.ini", 1);
    if (g_cfg.dry_run) {
        LOGI("--dry-run: images mapped and bound; not running");
        LOGI("  images             : %d (%s, %s, %s)", ARCTIC_IMAGES,
             s_img[IM_SNOW].name, s_img[IM_GOOSE].name, s_img[IM_GOBJ].name);
        LOGI("  imports bound      : %u/%u", st.total - st.missing, st.total);
        arctic_exit(arctic_diag_check(s_data) ? 0 : 1);
    }

    /* The game opens its files relative to its own folder, as on the
     * cabinet; arctic_fs.c routes every one that lands there. */
    SetCurrentDirectoryA(s_data);

    if (!pe_attach(&s_img[IM_GOOSE]))
        arctic_exit(1);
    if (g_log_level >= LOG_TRACE)
        CloseHandle(CreateThread(NULL, 0, census_thread, NULL, 0, NULL));

    /* snow.exe looks every game.env key up in the environment before the
     * file (0x473f14), through the msvcrt this host shares with it. The
     * shipped USE_DHCP_TIMEOUT=1 holds each boot on a 60-second "TESTING..."
     * countdown that only waits, for a cabinet's network address; the link
     * code starts after it either way. */
    _putenv("USE_DHCP_TIMEOUT=0");
    LOGI("arctic: USE_DHCP_TIMEOUT=0: no 60 s boot countdown");
    LOGI("arctic: entering %s", s_img[IM_SNOW].name);
    ((void (*)(void))pe_entry(&s_img[IM_SNOW]))();
    arctic_exit(0);
}
