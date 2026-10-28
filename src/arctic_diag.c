/* arctic_diag.c -- Arctic Thunder: the cabinet's operator system, diag.exe.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * On a cabinet, TEST makes wincsm.exe end the game and run
 * "c:\Midway Games\Diagnostics\diag.exe" in that folder; when it exits, the
 * game starts again. diag.exe is a GDI program over every installed game:
 * audits, adjustments, pricing, high scores, volume, monitor patterns, and the
 * game's own diagdll.dll (switch, lamp, shaker and blower tests, and the real
 * wheel calibration), which it loads from the game folder.
 *
 * As on the cabinet it is a process of its own: the game, on TEST, starts
 * this executable again for it (arctic_spawn) and exits. diag.exe has no
 * relocations, and its base is inside this executable's own image, the span
 * the ETS titles' loader uses and Arctic does not; so it is laid out there by
 * hand (pe_place), which keeps it inside a mapped image, as SEH dispatch
 * requires. The game's diagdll.dll is mapped by the system loader. Both are
 * bound through one table, as the game's images are (src/arctic.c): the
 * cabinet shell is this process's (arctic_io.c), the registry is the
 * cabinet's (arctic_reg.c), and every path it names is translated into the
 * pack; nothing reaches the machine's registry, drives, privileges or
 * shutdown.
 */
#include "vcthunder.h"
#include "arctic.h"
#include "pe_host.h"
#include "generated/arctic_diag.h"
#include "generated/span.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* diag.exe's fixed range lies in the span, clear of the stub's own sections. */
_Static_assert(ARCTIC_DIAG_BASE >= VCT_IMAGE_BASE + 0x10000 &&
               ARCTIC_DIAG_BASE + ARCTIC_DIAG_SIZE <= VCT_SPAN_END,
               "diag.exe's range must lie inside VCThunder.exe's span");

static pe_image_t s_diag, s_dll;
static int s_dll_attached;
static char s_data[MAX_PATH];           /* \Midway Games\Arctic Thunder  */
static char s_diagdir[MAX_PATH];        /* \Midway Games\Diagnostics      */

/* How the operator system ends: back to the game, as a cabinet does, or out
 * altogether (Escape, the close box). */
enum { TO_GAME, QUIT };
static volatile LONG s_quit;    /* set before diag's own exit is run to quit */

static __attribute__((noreturn)) void diag_exit(int how, int code)
{
    arctic_io_stop();
    arctic_seh_report();
    if (how == TO_GAME && !arctic_spawn(0))
        code = 1;
    LOGI("arctic: operator system exit %d%s", code,
         how == TO_GAME ? "; the game follows" : "");
    log_close();
    ExitProcess((UINT)code);
}

/* ---- the machine: drives, privileges, shutdown --------------------------- */

/* One fixed drive, the cabinet's C:. The floppy CMOS copy and the software
 * update look for removable drives and find none. */
static DWORD WINAPI b_GetLogicalDrives(void) { return 1u << 2; }

static DWORD WINAPI b_GetLogicalDriveStringsA(DWORD cap, LPSTR out)
{
    static const char drives[] = "C:\\\0";

    if (cap < sizeof drives)
        return sizeof drives;
    memcpy(out, drives, sizeof drives);
    return sizeof drives - 1;
}

static UINT WINAPI b_GetDriveTypeA(LPCSTR root)
{
    return root && (root[0] | 0x20) == 'c' && root[1] == ':' ? DRIVE_FIXED
                                                             : DRIVE_NO_ROOT_DIR;
}

static BOOL WINAPI b_OpenProcessToken(HANDLE p, DWORD a, PHANDLE t)
{
    (void)p; (void)a; (void)t;
    LOGI("arctic: diag asked for its process token (to shut the machine"
         " down); refused");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL WINAPI b_LookupPrivilegeValueA(LPCSTR sys, LPCSTR name, PLUID l)
{
    (void)sys; (void)l;
    LOGI("arctic: diag looked up privilege %s; refused", name ? name : "");
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL WINAPI b_AdjustTokenPrivileges(HANDLE t, BOOL all,
                                           PTOKEN_PRIVILEGES p, DWORD n,
                                           PTOKEN_PRIVILEGES prev, PDWORD r)
{
    (void)t; (void)all; (void)p; (void)n; (void)prev; (void)r;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

/* A cabinet restarts into the game; so does this. */
static BOOL WINAPI b_ExitWindowsEx(UINT flags, DWORD reason)
{
    LOGI("arctic: diag asked to restart the machine (flags %#x, reason %#lx);"
         " returning to the game instead", flags, (unsigned long)reason);
    diag_exit(TO_GAME, 0);
}

/* Only diag's decryption of a cabinet-encrypted file and its software
 * verification use CryptoAPI. The pack's files are plaintext, and a key
 * container would be created in the user's profile. */
static BOOL WINAPI b_CryptAcquireContextA(HCRYPTPROV *p, LPCSTR c, LPCSTR prov,
                                          DWORD type, DWORD flags)
{
    (void)p; (void)prov; (void)type;
    LOGI("arctic: diag asked for a CryptoAPI context (container %s, flags"
         " %#lx); refused", c ? c : "default", (unsigned long)flags);
    SetLastError((DWORD)NTE_BAD_KEYSET);
    return FALSE;
}

/* ---- the process: identity, command line, exit --------------------------- */

static HMODULE WINAPI b_GetModuleHandleA(LPCSTR n)
{
    return n ? GetModuleHandleA(n) : s_diag.mod;
}

/* The placed image is not in the loader's list, so its name is ours to say. */
static DWORD WINAPI b_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap)
{
    size_t n;

    if (m && m != GetModuleHandleA(NULL) && m != s_diag.mod)
        return GetModuleFileNameA(m, out, cap);
    if (!out || !cap)
        return 0;
    n = strlen(s_diag.path);
    snprintf(out, cap, "%s", s_diag.path);
    if (n >= cap) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return cap;
    }
    return (DWORD)n;
}

/* What wincsm.exe's run_diags passes (the quoted path, nothing else). */
static LPSTR WINAPI b_GetCommandLineA(void)
{
    static char cmd[] = "\"c:\\Midway Games\\Diagnostics\\diag.exe\"";

    return cmd;
}

/* diag's exit code is what run_diags reads: 0 a normal exit, 1 and 2 after a
 * software update or revert, which this host refuses before they start. */
static void WINAPI b_ExitProcess(UINT code)
{
    if (s_quit)
        diag_exit(QUIT, (int)code);
    if (code == 1 || code == 2)
        LOGW("arctic: diag exited %u, its software update/revert code; that"
             " is not something this host does", code);
    diag_exit(TO_GAME, (int)code);
}

static void __cdecl b_exit(int code) { b_ExitProcess((UINT)code); }

static void WINAPI b_OutputDebugStringA(LPCSTR s)
{
    char buf[512];
    size_t n;

    snprintf(buf, sizeof buf, "%s", s ? s : "");
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    if (n)
        LOGI("arctic: diag debug: %s", buf);
}

static int __cdecl b_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    size_t n;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    if (n)
        LOGI("arctic: diag printf: %s", buf);
    return (int)n;
}

/* psapi for the shell's lookup (arctic_io.c), the rest real. diagdll.dll's
 * exports are fetched by ordinal from its real, loader-listed image. */
static FARPROC WINAPI b_GetProcAddress(HMODULE m, LPCSTR name)
{
    void *p = arctic_io_psapi(name);

    return p ? (FARPROC)p : GetProcAddress(m, name);
}

/* diag loads the game's diagdll.dll by its cabinet path. It is ours already,
 * mapped and bound; its DllMain runs when diag asks, as on the cabinet. */
static HMODULE WINAPI b_LoadLibraryA(LPCSTR n)
{
    const char *base = n ? strrchr(n, '\\') : NULL;
    char buf[MAX_PATH];

    base = base ? base + 1 : n;
    if (base && !_stricmp(base, s_dll.name)) {
        if (!s_dll_attached) {
            s_dll_attached = 1;
            LOGI("arctic: diag loads %s (%s)", s_dll.name, n);
            if (!pe_attach(&s_dll))
                return NULL;
        }
        return s_dll.mod;
    }
    /* A bare module name is the system's search (PSAPI.DLL), never a file of
     * the current folder; only a path names a cabinet folder. */
    if (n && strpbrk(n, "\\/:") &&
        !(n = arctic_fs_route(n, buf, sizeof buf, FS_READ, "load")))
        return NULL;
    return LoadLibraryA(n);
}

/* ---- the window ---------------------------------------------------------- */

/* diag.exe asks for a maximized, borderless popup with the cursor hidden: the
 * cabinet's whole 640x480 monitor. It sizes its fonts from the client height
 * but spaces lines and places boxes in fixed pixels written for 480 lines, so
 * a bigger client breaks its layout. So it keeps the cabinet's monitor: its
 * window is ordinary and window_scale times 640x480, GetClientRect reports
 * 640x480, and every DC it draws its window with carries a GDI world
 * transform of that scale, which magnifies the cabinet's exact layout with
 * text rendered at full resolution. Its menus are owner-drawn popups and are
 * measured, drawn and placed through the same scale.
 * Escape and the close box leave altogether, as they do in the game; the
 * menu's own exit returns to the game. */
#define CABINET_W 640
#define CABINET_H 480

static WNDPROC s_diag_proc;
static HWND s_wnd;
static int s_scale = 1;

static void scale_dc(HDC dc, int dx, int dy)
{
    XFORM x = { (FLOAT)s_scale, 0.0f, 0.0f, (FLOAT)s_scale, (FLOAT)dx, (FLOAT)dy };

    SetGraphicsMode(dc, GM_ADVANCED);
    SetWorldTransform(dc, &x);
}

/* A device rectangle in logical units, rounded outward. */
static void to_logical(RECT *r)
{
    r->left /= s_scale;
    r->top /= s_scale;
    r->right = (r->right + s_scale - 1) / s_scale;
    r->bottom = (r->bottom + s_scale - 1) / s_scale;
}
static HHOOK s_menu_hook;

/* Leaving altogether still leaves the way diag does: its exit routine writes
 * every changed game's CMOS before it exits, and quitting without it would
 * throw the operator's changes away. It refuses only while the test DIP
 * switch is on, which this cabinet's never is; if it ever returns, say so. */
static __attribute__((noreturn)) void quit(const char *why)
{
    int (__cdecl *leave)(void) =
        (int (__cdecl *)(void))(uintptr_t)(ARCTIC_DIAG_BASE + ARCTIC_DIAG_EXIT_RVA);

    LOGI("arctic: %s in the operator system; quitting through diag's own exit,"
         " which saves its changes", why);
    InterlockedExchange(&s_quit, 1);
    leave();
    LOGW("arctic: diag's exit declined (the test DIP switch reads on); quitting"
         " without saving");
    diag_exit(QUIT, 0);
}

static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE)
        quit("window closed");
    if (m == WM_KEYDOWN && wp == VK_ESCAPE)
        quit("Escape");
    if (m == WM_MEASUREITEM && lp &&
        ((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        LRESULT r = CallWindowProcA(s_diag_proc, h, m, wp, lp);

        mi->itemWidth *= (UINT)s_scale;
        mi->itemHeight *= (UINT)s_scale;
        return r;
    }
    if (m == WM_DRAWITEM && lp &&
        ((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU && s_scale > 1) {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        RECT dev = di->rcItem;
        int saved = SaveDC(di->hDC);
        LRESULT r;

        scale_dc(di->hDC, dev.left, dev.top);
        SetRect(&di->rcItem, 0, 0, (dev.right - dev.left + s_scale - 1) / s_scale,
                (dev.bottom - dev.top + s_scale - 1) / s_scale);
        r = CallWindowProcA(s_diag_proc, h, m, wp, lp);
        di->rcItem = dev;
        RestoreDC(di->hDC, saved);
        return r;
    }
    return CallWindowProcA(s_diag_proc, h, m, wp, lp);
}

/* Every page is a popup menu in its own modal loop, which takes the keyboard
 * and the mouse. On the cabinet nothing ever cancels one: diag only posts a
 * Down key before each to highlight its first item, and some prompts' only
 * item has command id 0, so a cancel (0) cannot be told from that selection
 * by the return value. A desktop cancels menus constantly: a click anywhere
 * outside the menu, or the window losing the foreground, and diag then shows
 * the previous page; without the foreground TrackPopupMenu fails at once, so
 * that repeats as fast as it can and the window flips between two pages. So
 * nothing on the desktop may reach diag as a cancel: clicks in its window are
 * the host's (the close box quits, the minimize box minimizes, the rest are
 * dropped), and a menu ended by a click elsewhere or by losing the foreground
 * is shown again once the window has it back. Escape quits. */
static volatile LONG s_cancelled;   /* the desktop ended the menu, not diag */
static volatile LONG s_minimize;

static int is_menu_window(HWND w)
{
    char cls[16];

    return w && GetClassNameA(w, cls, sizeof cls) && !strcmp(cls, "#32768");
}

static LRESULT CALLBACK menu_filter(int code, WPARAM wp, LPARAM lp)
{
    const MSG *msg = (const MSG *)lp;

    if (code != MSGF_MENU || !msg)
        return CallNextHookEx(s_menu_hook, code, wp, lp);
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE)
        quit("Escape");
    if ((msg->message >= WM_LBUTTONDOWN && msg->message <= WM_MBUTTONDBLCLK) ||
        (msg->message >= WM_NCLBUTTONDOWN && msg->message <= WM_NCMBUTTONDBLCLK)) {
        HWND at = WindowFromPoint(msg->pt);

        if (is_menu_window(at))
            return CallNextHookEx(s_menu_hook, code, wp, lp);
        if (s_wnd && at && GetAncestor(at, GA_ROOT) == s_wnd) {
            LRESULT hit = SendMessageA(s_wnd, WM_NCHITTEST, 0,
                                       MAKELPARAM(msg->pt.x, msg->pt.y));

            if (hit == HTCLOSE && (msg->message == WM_NCLBUTTONDOWN ||
                                   msg->message == WM_LBUTTONDOWN))
                quit("window closed");
            if (hit == HTMINBUTTON && (msg->message == WM_NCLBUTTONDOWN ||
                                       msg->message == WM_LBUTTONDOWN)) {
                InterlockedExchange(&s_minimize, 1);
                InterlockedExchange(&s_cancelled, 1);
                EndMenu();
            }
            return 1;                       /* not the menu's to see */
        }
        InterlockedExchange(&s_cancelled, 1);   /* a click elsewhere */
    }
    return CallNextHookEx(s_menu_hook, code, wp, lp);
}

/* Until diag's window is the foreground one again, pumping its messages so
 * the close box, a repaint or a restore still work. */
static void wait_foreground(void)
{
    while (s_wnd && GetForegroundWindow() != s_wnd) {
        MSG m;

        MsgWaitForMultipleObjects(0, NULL, FALSE, 250, QS_ALLINPUT);
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT)
                quit("window destroyed");
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
}

static HWND WINAPI b_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name,
                                     DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst,
                                     LPVOID lp)
{
    DWORD own = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r, work;
    char title[128];
    HWND wnd;

    if (parent || !(style & WS_POPUP) || s_wnd)
        return CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu,
                               inst, lp);
    s_scale = g_cfg.window_scale ? (int)g_cfg.window_scale : 1;
    SetRect(&r, 0, 0, CABINET_W * s_scale, CABINET_H * s_scale);
    AdjustWindowRect(&r, own, FALSE);
    w = r.right - r.left;
    h = r.bottom - r.top;
    x = y = CW_USEDEFAULT;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        x = work.left + ((work.right - work.left) - w) / 2;
        y = work.top + ((work.bottom - work.top) - h) / 2;
    }
    snprintf(title, sizeof title, "%s - operator menu", AT->ids->name);
    wnd = CreateWindowExA(0, cls, title, own | (style & WS_VISIBLE), x, y, w, h,
                          NULL, menu, inst, lp);
    if (!wnd)
        return NULL;
    s_wnd = wnd;
    s_diag_proc = (WNDPROC)SetWindowLongPtrA(wnd, GWLP_WNDPROC,
                                             (LONG_PTR)host_proc);
    s_menu_hook = SetWindowsHookExA(WH_MSGFILTER, menu_filter, NULL,
                                    GetCurrentThreadId());
    LOGI("arctic: diag's %s window (style %#lx): the cabinet's %dx%d, drawn"
         " at %dx (a %dx%d client)", name ? name : "", (unsigned long)style,
         CABINET_W, CABINET_H, s_scale, CABINET_W * s_scale, CABINET_H * s_scale);
    return wnd;
}

static BOOL WINAPI b_GetClientRect(HWND h, LPRECT r)
{
    if (h && h == s_wnd && r) {
        SetRect(r, 0, 0, CABINET_W, CABINET_H);
        return TRUE;
    }
    return GetClientRect(h, r);
}

static HDC WINAPI b_GetDC(HWND h)
{
    HDC dc = GetDC(h);

    if (dc && h && h == s_wnd)
        scale_dc(dc, 0, 0);
    return dc;
}

static HDC WINAPI b_BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    HDC dc = BeginPaint(h, ps);

    if (dc && h && h == s_wnd) {
        scale_dc(dc, 0, 0);
        to_logical(&ps->rcPaint);
    }
    return dc;
}

static BOOL WINAPI b_RedrawWindow(HWND h, const RECT *r, HRGN rgn, UINT f)
{
    RECT dev;

    if (h && h == s_wnd && r) {
        SetRect(&dev, r->left * s_scale, r->top * s_scale, r->right * s_scale,
                r->bottom * s_scale);
        r = &dev;
    }
    return RedrawWindow(h, r, rgn, f);
}

static BOOL WINAPI b_ShowWindow(HWND h, int cmd)
{
    if (h == s_wnd && (cmd == SW_MAXIMIZE || cmd == SW_SHOWMAXIMIZED))
        cmd = SW_SHOWNORMAL;
    return ShowWindow(h, cmd);
}

/* Every menu is a Win32 popup menu, placed at the client's centre column and
 * a row of diag's layout (diag.exe 0x403d4c and four more sites, all
 * TPM_CENTERALIGN | TPM_NONOTIFY | TPM_RETURNCMD). Those are the cabinet
 * client's coordinates passed as screen coordinates, which on its full-screen
 * window at (0,0) are the same thing; here they are scaled and converted. */
static BOOL WINAPI b_TrackPopupMenu(HMENU m, UINT flags, int x, int y, int res,
                                    HWND h, const RECT *r)
{
    POINT pt = { x, y };

    if (s_wnd && (h == s_wnd || IsChild(s_wnd, h) || !h)) {
        pt.x *= s_scale;
        pt.y *= s_scale;
        ClientToScreen(s_wnd, &pt);
    }
    LOGT("arctic: diag menu %p at client %d,%d (owner %p, flags %#x) -> screen"
         " %d,%d", (void *)m, x, y, (void *)h, flags, (int)pt.x, (int)pt.y);
    if (!s_wnd)
        return TrackPopupMenu(m, flags, pt.x, pt.y, res, h, r);
    for (int again = 0;; again = 1) {
        int waited = GetForegroundWindow() != s_wnd;
        BOOL got;

        if (waited)
            wait_foreground();
        /* diag posts a Down key before each menu to highlight its first item.
         * The wait dispatched it to the window, and a menu shown again needs
         * one of its own. */
        if (again || waited)
            PostMessageA(s_wnd, WM_KEYDOWN, VK_DOWN, 0x01500001);
        InterlockedExchange(&s_cancelled, 0);
        got = TrackPopupMenu(m, flags, pt.x, pt.y, res, h, r);
        if (got || (!s_cancelled && GetForegroundWindow() == s_wnd))
            return got;
        if (InterlockedExchange(&s_minimize, 0))
            ShowWindow(s_wnd, SW_MINIMIZE);
        LOGT("arctic: diag menu %p ended by the desktop; shown again", (void *)m);
    }
}

/* ---- the table ----------------------------------------------------------- */

static const struct {
    const char *dll, *name;
    void *fn;
} k_bound[] = {
    { "KERNEL32.dll", "DeviceIoControl",      (void *)arctic_DeviceIoControl },
    { "KERNEL32.dll", "GetVersionExA",        (void *)arctic_GetVersionExA },
    { "KERNEL32.dll", "GetLogicalDrives",     (void *)b_GetLogicalDrives },
    { "KERNEL32.dll", "GetLogicalDriveStringsA", (void *)b_GetLogicalDriveStringsA },
    { "KERNEL32.dll", "GetDriveTypeA",        (void *)b_GetDriveTypeA },
    { "KERNEL32.dll", "GetModuleHandleA",     (void *)b_GetModuleHandleA },
    { "KERNEL32.dll", "GetModuleFileNameA",   (void *)b_GetModuleFileNameA },
    { "KERNEL32.dll", "GetCommandLineA",      (void *)b_GetCommandLineA },
    { "KERNEL32.dll", "ExitProcess",          (void *)b_ExitProcess },
    { "KERNEL32.dll", "OutputDebugStringA",   (void *)b_OutputDebugStringA },
    { "KERNEL32.dll", "GetProcAddress",       (void *)b_GetProcAddress },
    { "KERNEL32.dll", "LoadLibraryA",         (void *)b_LoadLibraryA },
    { "ADVAPI32.dll", "RegConnectRegistryA",  (void *)arctic_RegConnectRegistryA },
    { "ADVAPI32.dll", "RegOpenKeyExA",        (void *)arctic_RegOpenKeyExA },
    { "ADVAPI32.dll", "RegCreateKeyExA",      (void *)arctic_RegCreateKeyExA },
    { "ADVAPI32.dll", "RegCloseKey",          (void *)arctic_RegCloseKey },
    { "ADVAPI32.dll", "RegFlushKey",          (void *)arctic_RegFlushKey },
    { "ADVAPI32.dll", "RegQueryValueExA",     (void *)arctic_RegQueryValueExA },
    { "ADVAPI32.dll", "RegSetValueExA",       (void *)arctic_RegSetValueExA },
    { "ADVAPI32.dll", "RegEnumKeyA",          (void *)arctic_RegEnumKeyA },
    { "ADVAPI32.dll", "RegDeleteKeyA",        (void *)arctic_RegDeleteKeyA },
    { "ADVAPI32.dll", "OpenProcessToken",     (void *)b_OpenProcessToken },
    { "ADVAPI32.dll", "LookupPrivilegeValueA", (void *)b_LookupPrivilegeValueA },
    { "ADVAPI32.dll", "AdjustTokenPrivileges", (void *)b_AdjustTokenPrivileges },
    { "ADVAPI32.dll", "CryptAcquireContextA", (void *)b_CryptAcquireContextA },
    { "USER32.dll",   "CreateWindowExA",      (void *)b_CreateWindowExA },
    { "USER32.dll",   "ShowWindow",           (void *)b_ShowWindow },
    { "USER32.dll",   "TrackPopupMenu",       (void *)b_TrackPopupMenu },
    { "USER32.dll",   "GetClientRect",        (void *)b_GetClientRect },
    { "USER32.dll",   "GetDC",                (void *)b_GetDC },
    { "USER32.dll",   "BeginPaint",           (void *)b_BeginPaint },
    { "USER32.dll",   "RedrawWindow",         (void *)b_RedrawWindow },
    { "USER32.dll",   "ExitWindowsEx",        (void *)b_ExitWindowsEx },
    { "USER32.dll",   "MessageBoxA",          (void *)arctic_MessageBoxA },
    { "MSVCRT.dll",   "printf",               (void *)b_printf },
    { "MSVCRT.dll",   "exit",                 (void *)b_exit },
};

static void *resolve(const char *importer, const char *dll, const char *name,
                     unsigned ord, pe_bind_kind_t *kind)
{
    HMODULE h;
    void *fn;
    unsigned i;

    (void)importer;
    if (!_stricmp(dll, s_diag.name)) {
        *kind = PE_BIND_GAME;
        return name ? pe_export(&s_diag, name) : NULL;
    }
    for (i = 0; name && i < sizeof k_bound / sizeof k_bound[0]; i++)
        if (!_stricmp(dll, k_bound[i].dll) && !strcmp(name, k_bound[i].name)) {
            *kind = PE_BIND_HOST;
            return k_bound[i].fn;
        }
    if (name && (fn = arctic_fs_bound(dll, name))) {
        *kind = PE_BIND_HOST;
        return fn;
    }
    *kind = PE_BIND_REAL;
    h = LoadLibraryA(dll);
    if (!h)
        return NULL;
    if (!name && !_stricmp(dll, "DSOUND.dll")) {
        fn = arctic_dsound_bind(h, ord);

        if (fn) {
            *kind = PE_BIND_HOST;
            return fn;
        }
    }
    return name ? (void *)GetProcAddress(h, name)
                : (void *)GetProcAddress(h, MAKEINTRESOURCEA(ord));
}

/* ---- the run ------------------------------------------------------------- */

static int map_one(pe_image_t *im, const char *dir, const arctic_file_t *f,
                   int place)
{
    char path[MAX_PATH];
    size_t len = 0;
    BYTE *bytes = arctic_read_pinned(dir, f, path, sizeof path, &len);
    int ok;

    if (!bytes)
        return 0;
    ok = place ? pe_place(im, path, bytes, len) : pe_map(im, path, bytes, len);
    free(bytes);
    return ok;
}

/* The folders, then both images placed, mapped and bound. 0 on failure,
 * having said why. */
static int prepare(pe_bind_stats_t *st)
{
    int ok = 1;

    arctic_fs_init(s_data);
    snprintf(s_diagdir, sizeof s_diagdir, "%s", arctic_fs_diag_dir());
    if (!map_one(&s_diag, s_diagdir, &AT->ids->diag[ARCTIC_DIAG_EXE], 1) ||
        !map_one(&s_dll, s_data, &AT->ids->diag[ARCTIC_DIAG_DLL], 0))
        return 0;
    if ((uintptr_t)s_diag.mod != ARCTIC_DIAG_BASE) {
        LOGE("arctic: diag.exe is at %p, not its generated base %#x",
             (void *)s_diag.mod, ARCTIC_DIAG_BASE);
        return 0;
    }
    memset(st, 0, sizeof *st);
    ok &= pe_bind(&s_diag, resolve, st);
    ok &= pe_bind(&s_dll, resolve, st);
    LOGI("arctic: diag imports %u: %u real, %u host boundaries, %u between its"
         " own images, %u unresolved", st->total, st->kind[PE_BIND_REAL],
         st->kind[PE_BIND_HOST], st->kind[PE_BIND_GAME], st->missing);
    return ok;
}

/* The game's --dry-run: the operator system placed and bound in this process
 * too (its span is as free here as in the operator system's), never run. */
int arctic_diag_check(const char *data)
{
    pe_bind_stats_t st;

    snprintf(s_data, sizeof s_data, "%s", data);
    if (!prepare(&st))
        return 0;
    LOGI("  operator system    : %s at %p, %s; imports bound %u/%u", s_diag.name,
         (void *)s_diag.mod, s_dll.name, st.total - st.missing, st.total);
    return 1;
}

void arctic_diag_run(void)
{
    pe_bind_stats_t st;

    LOGI("arctic: %s's operator system (diag.exe), started by the game on TEST",
         AT->ids->name);
    arctic_prepare(s_data, sizeof s_data);
    if (!prepare(&st))
        diag_exit(QUIT, 1);
    arctic_io_start("vcthunder.ini", 0);

    /* run_diags starts it in its own folder. */
    SetCurrentDirectoryA(s_diagdir);
    LOGI("arctic: entering %s", s_diag.name);
    ((void (*)(void))pe_entry(&s_diag))();
    diag_exit(TO_GAME, 0);
}
