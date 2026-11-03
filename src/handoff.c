/* handoff.c -- from the launcher to a game, and back.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Play ends the launcher and the game starts in the same process, so between
 * the launcher's window closing and the game's opening there is nothing of
 * ours on screen. Three things cross that gap:
 *
 *   THE CURTAIN. A black window in the launcher's place -- its window, or the
 *   whole monitor when it was fullscreen -- with the game's name, until the
 *   game's window is up. It runs on a thread of its own, so it keeps answering
 *   while the main thread maps and boots the game and never shows as Not
 *   Responding. A game's window opens above it; handoff_lift() takes it away.
 *
 *   THE MONITOR. The game opens on the monitor the launcher was on, windowed
 *   or fullscreen, rather than on the primary one.
 *
 *   THE WAY BACK. A game the launcher started reopens the launcher when it
 *   exits cleanly, on the monitor it came from. A new process, not a return:
 *   nothing the game mapped has to be torn down. A game started by name exits
 *   as it always did.
 *
 * The monitor and the way back outlive one process: Arctic's game and its
 * operator system start each other (arctic_spawn()), and handoff_env() passes
 * both to the next one in HANDOFF_VAR.
 */
#include "vcthunder.h"

#include <stdio.h>
#include <string.h>

/* The cap, if no game window ever says it is up: a curtain must not outlive a
 * startup that failed somewhere this file does not hear about. */
#define CURTAIN_MS 30000
/* HOW LONG IT STAYS UNDER THE GAME after the first picture. A new window is
 * not opaque on screen the moment it presents (Offroad's first swap comes a
 * quarter of a second after its window opens), so for that moment the curtain
 * under it is what shows through. */
#define CURTAIN_GRACE_MS 500
/* From handoff_lift(): start the grace. */
#define WM_CURTAIN_LIFT (WM_APP + 1)
/* "<have monitor> <monitor l t r b> <work l t r b>", for a process this one
 * starts; read once and cleared, so nothing started after it inherits it. */
#define HANDOFF_VAR "VCTHUNDER_HANDOFF"
/* "<monitor l t r b>", for the launcher a game reopens. Its own variable: a
 * launcher that read HANDOFF_VAR would count as started by one, and reopen
 * another launcher when it closed. */
#define RETURN_VAR "VCTHUNDER_RETURN"

static RECT s_where, s_monitor, s_work;
static int s_have_monitor, s_from_launcher, s_settled;
static char s_name[64];
static COLORREF s_colour;
static HANDLE s_thread;
static HWND volatile s_curtain;

/* THE READY EVENT HAS TWO OWNERS. handoff_play() stops waiting after two
 * seconds and the curtain thread may signal it later, so whichever of them
 * lets go of it last closes it, and neither can close it under the other. */
typedef struct {
    HANDLE event;
    LONG   refs;
} ready_t;

static void ready_release(ready_t *ready)
{
    if (InterlockedDecrement(&ready->refs) == 0) {
        CloseHandle(ready->event);
        HeapFree(GetProcessHeap(), 0, ready);
    }
}

static LRESULT CALLBACK curtain_proc(HWND h, UINT message, WPARAM wparam,
                                     LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(h, &paint);
        RECT rc, line;
        int height;
        HFONT big, small;
        HGDIOBJ old;

        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        /* In proportion to what it covers: the same picture on a window and
         * on a television. */
        height = max(rc.bottom / 14, 16);
        big = CreateFontA(-height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        small = CreateFontA(-height / 2, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            "Segoe UI");
        SetBkMode(dc, TRANSPARENT);
        old = SelectObject(dc, big);
        SetTextColor(dc, s_colour);
        line = rc;
        line.bottom = rc.bottom / 2;
        DrawTextA(dc, s_name, -1, &line,
                  DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, small);
        SetTextColor(dc, RGB(176, 176, 180));
        line.top = rc.bottom / 2 + height / 3;
        line.bottom = rc.bottom;
        DrawTextA(dc, "Starting...", -1, &line,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(big);
        DeleteObject(small);
        EndPaint(h, &paint);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_APPSTARTING));
        return TRUE;
    case WM_CURTAIN_LIFT:
        SetTimer(h, 2, CURTAIN_GRACE_MS, NULL);
        return 0;
    case WM_TIMER:
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, message, wparam, lparam);
}

static DWORD WINAPI curtain_main(LPVOID opaque)
{
    ready_t *ready = (ready_t *)opaque;
    WNDCLASSA wc;
    MSG message;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = curtain_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_APPSTARTING);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "VCThunderCurtain";
    RegisterClassA(&wc);
    /* Not topmost: a game's window, or a message box from a startup that
     * failed, opens above it like above anything else. */
    s_curtain = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, s_name,
                                WS_POPUP | WS_VISIBLE, s_where.left,
                                s_where.top, s_where.right - s_where.left,
                                s_where.bottom - s_where.top, NULL, NULL,
                                wc.hInstance, NULL);
    SetEvent(ready->event);
    ready_release(ready);
    if (!s_curtain)
        return 1;
    SetTimer(s_curtain, 1, CURTAIN_MS, NULL);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    s_curtain = NULL;
    return 0;
}

/* A game a launcher-started process started: its state, from the variable. */
static void inherit(void)
{
    char v[192];
    RECT m, w;
    int have = 0;
    DWORD n;

    if (s_settled)
        return;
    s_settled = 1;
    n = GetEnvironmentVariableA(HANDOFF_VAR, v, sizeof v);
    if (!n || n >= sizeof v)
        return;
    SetEnvironmentVariableA(HANDOFF_VAR, NULL);
    s_from_launcher = 1;
    if (sscanf(v, "%d %ld %ld %ld %ld %ld %ld %ld %ld", &have, &m.left, &m.top,
               &m.right, &m.bottom, &w.left, &w.top, &w.right,
               &w.bottom) == 9 && have) {
        s_monitor = m;
        s_work = w;
        s_have_monitor = 1;
    }
}

void handoff_env(int set)
{
    char v[192];

    inherit();
    if (!set || !s_from_launcher) {
        SetEnvironmentVariableA(HANDOFF_VAR, NULL);
        return;
    }
    snprintf(v, sizeof v, "%d %ld %ld %ld %ld %ld %ld %ld %ld", s_have_monitor,
             s_monitor.left, s_monitor.top, s_monitor.right, s_monitor.bottom,
             s_work.left, s_work.top, s_work.right, s_work.bottom);
    SetEnvironmentVariableA(HANDOFF_VAR, v);
}

void handoff_play(HWND launcher, const char *name, COLORREF colour)
{
    MONITORINFO monitor;
    ready_t *ready;

    s_settled = 1;
    s_from_launcher = 1;
    memset(&monitor, 0, sizeof monitor);
    monitor.cbSize = sizeof monitor;
    if (GetMonitorInfoA(MonitorFromWindow(launcher, MONITOR_DEFAULTTONEAREST),
                        &monitor)) {
        s_monitor = monitor.rcMonitor;
        s_work = monitor.rcWork;
        s_have_monitor = 1;
    }
    if (!GetWindowRect(launcher, &s_where))
        return;
    snprintf(s_name, sizeof s_name, "%s", name);
    s_colour = colour;
    ready = HeapAlloc(GetProcessHeap(), 0, sizeof *ready);
    if (!ready)
        return;
    ready->event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ready->event) {
        HeapFree(GetProcessHeap(), 0, ready);
        return;
    }
    ready->refs = 2;
    s_thread = CreateThread(NULL, 0, curtain_main, ready, 0, NULL);
    if (s_thread)
        WaitForSingleObject(ready->event, 2000);
    else
        ready_release(ready);       /* the thread's reference */
    ready_release(ready);
    /* The foreground goes to the curtain before the launcher closes, so the
     * process keeps it for the game's window to take. */
    if (s_curtain)
        SetForegroundWindow(s_curtain);
}

int handoff_is_curtain(HWND h)
{
    char cls[32];

    return h && GetClassNameA(h, cls, sizeof cls) &&
           !strcmp(cls, "VCThunderCurtain");
}

int handoff_curtain_up(void)
{
    return s_thread != NULL;
}

void handoff_lift(HWND game)
{
    HANDLE thread = s_thread;

    if (!thread)
        return;
    s_thread = NULL;
    /* THE GAME FIRST, WHILE THIS PROCESS STILL HOLDS THE FOREGROUND. Closing
     * the curtain first hands the foreground to the next window down, which
     * is the console's and another process's, and Windows then refuses this
     * call. The curtain then goes on its own, under the game, after the
     * grace. */
    if (game)
        SetForegroundWindow(game);
    if (s_curtain)
        PostMessageA(s_curtain, WM_CURTAIN_LIFT, 0, 0);
    CloseHandle(thread);
}

int handoff_work_area(RECT *work)
{
    inherit();
    if (s_have_monitor)
        *work = s_work;
    else if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, work, 0))
        return 0;
    return 1;
}

int handoff_centre(int w, int h, int *x, int *y)
{
    RECT work;

    if (!handoff_work_area(&work))
        return 0;
    *x = work.left + max(0, (int)(work.right - work.left) - w) / 2;
    *y = work.top + max(0, (int)(work.bottom - work.top) - h) / 2;
    return 1;
}

/* THE WORK AREA IS IN PHYSICAL PIXELS, as the launcher, which is per-monitor
 * aware, read it. A process that is not DPI aware -- the operator system,
 * which never starts SDL -- sees every coordinate scaled, so a window is
 * moved with the thread made per-monitor aware for the call. Windows older
 * than 1607 have no such switch, and their processes see one scale anyway. */
typedef HANDLE (WINAPI *set_context_fn)(HANDLE);

static HANDLE physical_begin(set_context_fn *set)
{
    *set = (set_context_fn)(void (*)(void))GetProcAddress(
        GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext");
    return *set ? (*set)((HANDLE)-4) : NULL;  /* PER_MONITOR_AWARE_V2 */
}

static void physical_end(set_context_fn set, HANDLE previous)
{
    if (set && previous)
        set(previous);
}

static void centre_window(HWND h)
{
    RECT r;
    int x, y;

    if (GetWindowRect(h, &r) &&
        handoff_centre(r.right - r.left, r.bottom - r.top, &x, &y))
        SetWindowPos(h, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void handoff_place(HWND game)
{
    set_context_fn set;
    HANDLE previous;

    inherit();
    if (!s_have_monitor || !game)
        return;
    previous = physical_begin(&set);
    if (MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST) !=
        MonitorFromRect(&s_monitor, MONITOR_DEFAULTTONEAREST))
        centre_window(game);
    physical_end(set, previous);
}

void handoff_centre_window(HWND h)
{
    set_context_fn set;
    HANDLE previous;

    if (!h)
        return;
    previous = physical_begin(&set);
    centre_window(h);
    physical_end(set, previous);
}

void handoff_exit(int code)
{
    wchar_t exe[MAX_PATH + 2], command[MAX_PATH + 4];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD n;

    inherit();
    if (!s_from_launcher || code != 0)
        return;
    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;
    handoff_env(0);
    if (s_have_monitor) {
        char v[64];

        snprintf(v, sizeof v, "%ld %ld %ld %ld", s_monitor.left,
                 s_monitor.top, s_monitor.right, s_monitor.bottom);
        SetEnvironmentVariableA(RETURN_VAR, v);
    }
    _snwprintf(command, sizeof command / sizeof command[0], L"\"%ls\"", exe);
    command[sizeof command / sizeof command[0] - 1] = 0;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    if (CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL,
                       &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

int handoff_return_monitor(RECT *monitor)
{
    char v[64];
    DWORD n = GetEnvironmentVariableA(RETURN_VAR, v, sizeof v);
    RECT m;

    if (!n || n >= sizeof v)
        return 0;
    SetEnvironmentVariableA(RETURN_VAR, NULL);
    if (sscanf(v, "%ld %ld %ld %ld", &m.left, &m.top, &m.right,
               &m.bottom) != 4 || m.right <= m.left || m.bottom <= m.top)
        return 0;
    *monitor = m;
    return 1;
}
