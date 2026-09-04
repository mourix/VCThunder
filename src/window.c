/* window.c -- host-window policy.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The backend creates the window, while this module owns its title, fixed
 * client geometry, and borderless fullscreen state. Client size is derived from
 * the visible raster rather than the padded Glide surface; the host display
 * mode is never changed.
 */
#include "vcthunder.h"
#include "resource.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WINDOW_SCALE_MAX 8
#define WINDOWED_STYLE_REMOVE (WS_THICKFRAME | WS_MAXIMIZEBOX)

static HWND s_hwnd;
static HWND s_branded;               /* the handle the name and icon went on */
static HICON s_icon_big, s_icon_small;
static int  s_view_w, s_view_h;      /* the game's visible raster */
static int  s_seen, s_gone;

static int  s_fullscreen;
static int  s_saved_valid;
static LONG s_saved_style, s_saved_ex;
static WINDOWPLACEMENT s_saved_place;

typedef struct {
    DWORD pid;
    HWND  hwnd;
    LONG  area;
} window_search_t;

/* Load the icon from this DLL because mapping the game overwrites the
 * executable's resource headers. Resolve the module by address without loading. */
static HICON load_icon(int cx, int cy)
{
    HMODULE self = NULL;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)&load_icon, &self) || !self)
        return NULL;
    /* LR_SHARED: the system keeps the cached copy for the life of the process
     * and it must not be destroyed. That is exactly the lifetime a window icon
     * needs, since the window outlives every call made from here. */
    return (HICON)LoadImageA(self, MAKEINTRESOURCEA(IDI_VCTHUNDER), IMAGE_ICON,
                             cx, cy, LR_SHARED | LR_DEFAULTCOLOR);
}

/* BOTH SIZES, OR THE BRANDING IS HALF DONE: the caption reads ICON_SMALL while
 * the taskbar and Alt-Tab read ICON_BIG, so setting one of them leaves the
 * other showing the backend's default. The metrics are asked for rather than
 * assumed to be 16 and 32, because they are not on a scaled display. */
static void apply_icon(HWND hwnd)
{
    static int loaded;

    if (!loaded) {
        loaded = 1;
        s_icon_big = load_icon(GetSystemMetrics(SM_CXICON),
                               GetSystemMetrics(SM_CYICON));
        s_icon_small = load_icon(GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON));
        if (!s_icon_big && !s_icon_small)
            LOGW("window: no icon in VCThunder.dll (%lu); keeping the "
                 "backend's", GetLastError());
    }
    if (s_icon_big)
        SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)s_icon_big);
    if (s_icon_small)
        SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)s_icon_small);
}

/* Keep configuration as the sole owner of client size. Apply the fixed-size
 * style to every backend window and refresh its non-client frame immediately. */
static void apply_fixed_style(HWND hwnd)
{
    LONG style, fixed;

    if (!hwnd || s_fullscreen)
        return;
    style = GetWindowLongA(hwnd, GWL_STYLE);
    fixed = style & ~WINDOWED_STYLE_REMOVE;
    if (fixed == style)
        return;
    SetWindowLongA(hwnd, GWL_STYLE, fixed);
    if (!SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                      SWP_NOACTIVATE | SWP_FRAMECHANGED))
        LOGW("window: could not lock the window size (%lu)", GetLastError());
}

/* Apply the host title and icon whenever the backend window changes. These
 * synchronous cross-thread operations are excluded from the frame path. */
static void apply_identity(HWND hwnd, int force)
{
    char title[128];

    if (!hwnd || (hwnd == s_branded && !force))
        return;
    apply_fixed_style(hwnd);
    apply_icon(hwnd);
    /* Match the renderer's title format before frame-rate reporting begins. */
    if (G && G->id && G->id[0]) {
        char name[32];
        snprintf(name, sizeof name, "%s", G->id);
        name[0] = (char)toupper((unsigned char)name[0]);
        snprintf(title, sizeof title, "VCThunder - %s", name);
    } else {
        snprintf(title, sizeof title, "VCThunder");
    }
    if (SetWindowTextA(hwnd, title))
        s_branded = hwnd;
    else
        LOGW("window: SetWindowText failed (%lu)", GetLastError());
}

static void hook_window(HWND hwnd);

/* Every path that learns a handle goes through here, so the name and icon
 * cannot depend on which one found the window. */
static void adopt(HWND hwnd)
{
    s_hwnd = hwnd;
    if (!hwnd)
        return;
    s_seen = 1;
    apply_identity(hwnd, 0);
    hook_window(hwnd);
}

static BOOL CALLBACK find_process_window(HWND hwnd, LPARAM opaque)
{
    window_search_t *s = (window_search_t *)opaque;
    DWORD pid = 0;
    RECT r;
    LONG area;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid || !IsWindowVisible(hwnd) || !GetClientRect(hwnd, &r))
        return TRUE;
    area = (r.right - r.left) * (r.bottom - r.top);
    if (area > s->area) {
        s->hwnd = hwnd;
        s->area = area;
    }
    return TRUE;
}

HWND window_game(void)
{
    window_search_t s;

    if (IsWindow(s_hwnd))
        return s_hwnd;
    memset(&s, 0, sizeof s);
    s.pid = GetCurrentProcessId();
    EnumWindows(find_process_window, (LPARAM)&s);
    if (s.hwnd)
        adopt(s.hwnd);
    return s_hwnd;
}

void window_set_hwnd(HWND hwnd)
{
    adopt(IsWindow(hwnd) ? hwnd : NULL);
}

/* Confirmed over two consecutive swaps: a window can be legitimately absent for
 * an instant while a backend recreates it for a mode change, and quitting on
 * that would be a false positive. Only "no visible window anywhere in this
 * process" counts, not "the handle we were holding went stale". */
int window_gone(void)
{
    window_search_t s;

    if (!s_seen)
        return 0;
    if (IsWindow(s_hwnd) && IsWindowVisible(s_hwnd)) {
        s_gone = 0;
        return 0;
    }

    /* The handle we were holding is dead or hidden, so whatever we dressed is
     * not on screen. Forget it: Windows recycles HWND values, and a recreated
     * window that landed on the old number would otherwise keep the backend's
     * name and icon because ours compared equal. */
    s_branded = NULL;

    memset(&s, 0, sizeof s);
    s.pid = GetCurrentProcessId();
    EnumWindows(find_process_window, (LPARAM)&s);
    if (s.hwnd) {
        adopt(s.hwnd);
        s_gone = 0;
        return 0;
    }
    return ++s_gone >= 2;
}

/* The cabinet controls, Escape and Alt+Enter all follow one focus policy, so
 * that "the console still has focus" cannot make one of them work and another
 * not. input_background = true is the portable-pack default because the
 * terminal that launched the pack frequently stays the foreground owner while
 * the render surface is the thing being looked at. */
int window_input_focused(void)
{
    DWORD pid = 0;
    HWND foreground;

    if (g_cfg.input_background)
        return 1;
    foreground = GetForegroundWindow();
    if (!foreground)
        return 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

int window_is_fullscreen(void)
{
    return s_fullscreen;
}

/* Implement non-modal caption dragging so the game's swap-driven message pump
 * continues to run. Remaining system modal loops pause the virtual clock. The
 * subclass preserves the backend window's native character set. */
static WNDPROC s_prev_proc;
static HWND    s_hooked;
static int     s_hooked_unicode;
static int     s_dragging;
static POINT   s_drag_mouse;         /* screen cursor at button-down */
static POINT   s_drag_window;        /* window origin at button-down */

static LRESULT chain(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (s_prev_proc)
        return s_hooked_unicode ? CallWindowProcW(s_prev_proc, h, msg, wp, lp)
                                : CallWindowProcA(s_prev_proc, h, msg, wp, lp);
    return s_hooked_unicode ? DefWindowProcW(h, msg, wp, lp)
                            : DefWindowProcA(h, msg, wp, lp);
}

static void drag_end(HWND h)
{
    if (!s_dragging)
        return;
    s_dragging = 0;                  /* before the release: it comes back here */
    if (h && GetCapture() == h)
        ReleaseCapture();
}

static int drag_begin(HWND h)
{
    RECT r;

    if (s_fullscreen || !GetCursorPos(&s_drag_mouse) || !GetWindowRect(h, &r))
        return 0;
    s_drag_window.x = r.left;
    s_drag_window.y = r.top;
    SetCapture(h);                   /* first: this sends WM_CAPTURECHANGED */
    s_dragging = 1;
    return 1;
}

/* End dragging from the physical primary-button state rather than capture
 * ownership. Honour swapped mouse buttons. */
static int drag_button_down(void)
{
    int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;

    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* The cursor's ABSOLUTE position against where it was grabbed, never
 * accumulated deltas: a coalesced or dropped WM_MOUSEMOVE then costs nothing,
 * and the window cannot drift away from the point on the caption the user is
 * holding. At 30 fps there are plenty of both. */
static void drag_track(HWND h)
{
    POINT p;

    if (!GetCursorPos(&p))
        return;
    SetWindowPos(h, NULL,
                 s_drag_window.x + (p.x - s_drag_mouse.x),
                 s_drag_window.y + (p.y - s_drag_mouse.y), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK shim_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCLBUTTONDOWN:
        if (wp == HTCAPTION && drag_begin(h))
            return 0;
        break;
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
        if (s_dragging) {
            drag_track(h);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
    case WM_NCLBUTTONUP:
        if (s_dragging) {
            drag_end(h);
            return 0;
        }
        break;
    /* Nothing in either cabinet is aimed with a mouse, so a pointer over a
     * full-screen picture is only ever in the way. Windowed keeps the arrow:
     * there is a caption there to grab. */
    case WM_SETCURSOR:
        if (s_fullscreen && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        timebase_pause(1);
        break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        timebase_pause(0);
        break;
    }
    return chain(h, msg, wp, lp);
}

static void hook_window(HWND hwnd)
{
    WNDPROC prev;

    if (!hwnd || hwnd == s_hooked)
        return;

    /* Give a window we are letting go of its own procedure back. A subclass
     * left on a live window would go on calling this file about a handle it no
     * longer owns, and chain through the wrong previous procedure. */
    if (s_hooked && s_prev_proc && IsWindow(s_hooked)) {
        if (s_hooked_unicode)
            SetWindowLongPtrW(s_hooked, GWLP_WNDPROC, (LONG_PTR)s_prev_proc);
        else
            SetWindowLongPtrA(s_hooked, GWLP_WNDPROC, (LONG_PTR)s_prev_proc);
    }
    s_hooked = NULL;
    s_prev_proc = NULL;
    s_dragging = 0;

    s_hooked_unicode = IsWindowUnicode(hwnd) ? 1 : 0;
    prev = s_hooked_unicode
         ? (WNDPROC)(LONG_PTR)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                (LONG_PTR)shim_wndproc)
         : (WNDPROC)(LONG_PTR)SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                                (LONG_PTR)shim_wndproc);
    if (!prev) {
        LOGW("window: could not take the window procedure (%lu); holding the "
             "title bar will stall the game for as long as the button is down",
             GetLastError());
        return;
    }
    s_prev_proc = prev;
    s_hooked = hwnd;
    LOGI("window: window procedure hooked (%s); caption drags no longer enter "
         "a modal loop", s_hooked_unicode ? "unicode" : "ansi");
}

/* WM_SETCURSOR arrives only when the mouse moves or the window changes under
 * it, so the toggle itself has to set the cursor once: otherwise the pointer
 * stays on screen over the new full-screen picture until it is moved. Legal
 * from here because this runs on the thread that owns the window. */
static void apply_cursor(void)
{
    SetCursor(s_fullscreen ? NULL : LoadCursorA(NULL, IDC_ARROW));
}

/* window_scale is an integer presentation multiplier and does not change render
 * resolution. Automatic and explicit values are clamped to the work area. */
static void client_size_for(int scale, int *w, int *h);

static int window_scale_for(int frame_w, int frame_h, const RECT *work)
{
    int want = (int)g_cfg.window_scale;
    int unit_w, unit_h, fit_w, fit_h, fit;

    if (want > WINDOW_SCALE_MAX)
        want = WINDOW_SCALE_MAX;

    /* Fit against the client actually asked for at each scale, which is wider
     * than the raster whenever the pad is being compensated for. Dividing the
     * work area by the bare raster would let `auto` pick a factor whose real
     * window does not fit. */
    client_size_for(1, &unit_w, &unit_h);
    if (unit_w <= 0 || unit_h <= 0)
        return 1;
    fit_w = ((work->right - work->left) - frame_w) / unit_w;
    fit_h = ((work->bottom - work->top) - frame_h) / unit_h;
    fit = fit_w < fit_h ? fit_w : fit_h;
    if (fit < 1)
        fit = 1;
    if (fit > WINDOW_SCALE_MAX)
        fit = WINDOW_SCALE_MAX;

    if (want < 1)                       /* 0 / auto / nonsense */
        return fit;
    if (want > fit) {
        LOGW("window: scale %d needs %dx%d, which does not fit the %ldx%ld work "
             "area: using %d", want, unit_w * want, unit_h * want,
             (long)(work->right - work->left), (long)(work->bottom - work->top),
             fit);
        return fit;
    }
    return want;
}

/* Classify aspect modes that require a fixed output ratio. */
int aspect_is_4_3(const char *aspect)
{
    return _stricmp(aspect, "4:3") == 0 || _stricmp(aspect, "4_3") == 0 ||
           _stricmp(aspect, "43") == 0;
}

int aspect_is_16_9(const char *aspect)
{
    return _stricmp(aspect, "16:9") == 0;
}

/* Compute client size from the visible raster and requested aspect. Preserve
 * an integer raster-height multiple for non-stretch windowed modes. */
static void client_size_for(int scale, int *w, int *h)
{
    *h = s_view_h * scale;
    if (aspect_is_4_3(g_cfg.aspect))
        *w = (*h * 4 + 1) / 3;          /* the cabinet monitor's 4:3 */
    else if (aspect_is_16_9(g_cfg.aspect))
        *w = (*h * 16 + 4) / 9;         /* standard widescreen */
    else
        *w = s_view_w * scale;          /* raster, and stretch while windowed */
}

/* Apply configured geometry without backend WM_GETMINMAXINFO constraints.
 * WM_WINDOWPOSCHANGED and WM_SIZE still update the backend and swapchain. */
static void apply_fullscreen(HWND hwnd)
{
    MONITORINFO mi;
    LONG style, ex;

    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        LOGW("window: no monitor info for the game window (%lu); staying windowed",
             GetLastError());
        s_fullscreen = 0;
        return;
    }

    /* WS_POPUP with no frame bits: the client becomes exactly the monitor
     * rectangle, with no caption to subtract and nothing for the shell to
     * reserve. The display mode itself is untouched: this is a window that
     * happens to be screen-sized, which is why other monitors, other windows
     * and the desktop resolution all survive it. */
    style = GetWindowLongA(hwnd, GWL_STYLE);
    ex    = GetWindowLongA(hwnd, GWL_EXSTYLE);
    SetWindowLongA(hwnd, GWL_STYLE,
                   (style & ~(WS_CAPTION | WS_THICKFRAME | WS_DLGFRAME |
                              WS_MINIMIZE | WS_MAXIMIZE)) | WS_POPUP);
    SetWindowLongA(hwnd, GWL_EXSTYLE,
                   ex & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                          WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

    if (!SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                      mi.rcMonitor.right - mi.rcMonitor.left,
                      mi.rcMonitor.bottom - mi.rcMonitor.top,
                      SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                      SWP_NOSENDCHANGING)) {
        LOGW("window: fullscreen SetWindowPos failed (%lu)", GetLastError());
        return;
    }
    LOGI("window: borderless fullscreen on %ldx%ld at (%ld,%ld); host display "
         "mode unchanged; aspect %s presents the %dx%d raster",
         (long)(mi.rcMonitor.right - mi.rcMonitor.left),
         (long)(mi.rcMonitor.bottom - mi.rcMonitor.top),
         (long)mi.rcMonitor.left, (long)mi.rcMonitor.top, g_cfg.aspect,
         s_view_w, s_view_h);
}

/* The saved placement carries the POSITION back; the size is window_sync's, and
 * has to be. SetWindowPlacement takes no SWP flags, so it is subject to the veto
 * described above and returns TRUE having changed nothing but the origin: it
 * was the only restore step here for a while, which is exactly how the window
 * came back from fullscreen still the size of the monitor. */
static void leave_fullscreen(HWND hwnd)
{
    SetWindowLongA(hwnd, GWL_STYLE, s_saved_style);
    SetWindowLongA(hwnd, GWL_EXSTYLE, s_saved_ex);
    if (s_saved_valid)
        SetWindowPlacement(hwnd, &s_saved_place);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
    LOGI("window: leaving fullscreen");
}

void window_set_fullscreen(int on)
{
    HWND hwnd = window_game();

    on = on ? 1 : 0;
    if (!hwnd || on == s_fullscreen)
        return;

    if (on) {
        s_saved_place.length = sizeof s_saved_place;
        s_saved_valid = GetWindowPlacement(hwnd, &s_saved_place) ? 1 : 0;
        s_saved_style = GetWindowLongA(hwnd, GWL_STYLE);
        s_saved_ex    = GetWindowLongA(hwnd, GWL_EXSTYLE);
        s_fullscreen  = 1;
        apply_fullscreen(hwnd);
    } else {
        s_fullscreen = 0;
        leave_fullscreen(hwnd);
        window_sync(1);
    }
    apply_cursor();
}

/* Poll Alt+Enter on the game thread because the backend window owns the message
 * queue. Edge detection produces one fullscreen toggle per press. */
void window_hotkeys(void)
{
    static int was_down;
    int down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
               (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;

    /* The drag advances here as well as on WM_MOUSEMOVE, and ends here whatever
     * the message queue did: this is the one call that is guaranteed to happen
     * once per frame for as long as frames happen at all. */
    if (s_dragging) {
        HWND hwnd = window_game();

        if (!hwnd || !drag_button_down())
            drag_end(hwnd);
        else
            drag_track(hwnd);
    }

    if (down && !was_down && window_input_focused())
        window_set_fullscreen(!s_fullscreen);
    was_down = down;
}

/* `force` is used directly after a game mode request. For a short period after
 * that, buffer swaps also re-apply the geometry: a watch written for a
 * provider that reset the mode asynchronously, and kept as a general one, since
 * a backend resizing its own window after the fact is not a wrapper-only habit
 * (glide_bind.c, s_resize_watch). Manual
 * resizing and maximising are absent from the window style, so the ini remains
 * the only source of windowed geometry. */
void window_sync(int force)
{
    HWND hwnd = window_game();
    RECT client, window, work;
    int cw, ch, fw, fh, scale, tw, th, ow, oh, x, y;
    /* SWP_NOSENDCHANGING: this is the call the ini's size travels on, and a
     * backend that pins its tracking size clamps it away otherwise. See
     * apply_fullscreen. */
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                 SWP_NOSENDCHANGING;

    apply_fixed_style(hwnd);
    if (!hwnd || !s_view_w || !GetClientRect(hwnd, &client) ||
        !GetWindowRect(hwnd, &window))
        return;

    cw = client.right - client.left;
    ch = client.bottom - client.top;

    /* Fullscreen owns the geometry outright, and still needs the repair path:
     * a backend that resets the client to 120x61 does it whatever the style
     * bits say. */
    if (s_fullscreen) {
        if (force || cw < 256 || ch < 192)
            apply_fullscreen(hwnd);
        return;
    }

    if (IsIconic(hwnd))
        return;

    if (!force && cw >= 256 && ch >= 192)
        return;

    fw = (window.right - window.left) - cw;     /* borders and caption */
    fh = (window.bottom - window.top) - ch;

    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    scale = window_scale_for(fw, fh, &work);
    client_size_for(scale, &tw, &th);
    if (cw == tw && ch == th)
        return;

    ow = tw + fw;
    oh = th + fh;

    /* Keep the user's position, but never grow a window off the work area:
     * nudge it back rather than re-centring, so a deliberate placement survives
     * a backend's delayed mode reset. */
    x = window.left;
    y = window.top;
    if (x + ow > work.right)  x = work.right - ow;
    if (y + oh > work.bottom) y = work.bottom - oh;
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;
    if (x == window.left && y == window.top)
        flags |= SWP_NOMOVE;

    if (SetWindowPos(hwnd, NULL, x, y, ow, oh, flags)) {
        LOGI("window: raster %dx%d at scale %dx, aspect %s -> client %dx%d "
             "(was %dx%d)", s_view_w, s_view_h, scale, g_cfg.aspect, tw, th,
             cw, ch);
        /* A refused resize is the failure mode this path actually has, and it
         * refuses by succeeding, so read the client back rather than trust the
         * return value. Anything else leaves the window at the wrong size with a
         * log line claiming the right one. */
        if (GetClientRect(hwnd, &client) &&
            (client.right - client.left != tw ||
             client.bottom - client.top != th))
            LOGW("window: the backend kept the client at %ldx%ld after a "
                 "successful resize to %dx%d: it is answering "
                 "WM_GETMINMAXINFO with a size of its own",
                 (long)(client.right - client.left),
                 (long)(client.bottom - client.top), tw, th);
    } else {
        LOGW("window: SetWindowPos for %dx%d (scale %dx) failed (%lu)",
             tw, th, scale, GetLastError());
    }
}

/* vcglide presents the visible raster and crops the surface pad away, so the
 * two numbers the window needs are the raster's. */
void window_geometry(int view_w, int view_h)
{
    if (view_w <= 0 || view_h <= 0) {
        s_view_w = s_view_h = 0;
        return;
    }
    s_view_w = view_w;
    s_view_h = view_h;

    /* The startup mode is applied here rather than at init: this is the first
     * moment a window exists to apply it to. The name and icon go on for the
     * same reason, and are forced, because a mode request is exactly when a
     * provider may redress a window it decided to keep. */
    apply_identity(window_game(), 1);
    if (g_cfg.fullscreen && !s_fullscreen)
        window_set_fullscreen(1);
    else
        window_sync(1);
}
