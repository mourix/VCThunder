/* present.c -- Win32 presentation for the CPU rasteriser.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Direct3D 9 uploads and scales the completed XRGB8888 frame; GDI is the
 * fallback. Replay disables presentation.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/vgl.h"
#include "../core/vcglide.h"

static HWND      s_wnd;
static HWND      s_given;               /* the caller's window, if it had one */
static HDC       s_dc;
static BITMAPINFO *s_bmi;
static uint32_t *s_rgb;                 /* cropped XRGB8888 raster for upload */
static int       s_w, s_h;              /* the SURFACE */
static int       s_rx, s_ry, s_rw, s_rh;/* the RASTER inside it, presented */
static int       s_vx0, s_vy0, s_vx1, s_vy1; /* drawn part, raster-relative */
static int       s_scale;
static int       s_refresh_hz;
static int       s_owns_window;
static IDirect3D9       *s_d3d;
static IDirect3DDevice9 *s_dev;
static IDirect3DTexture9 *s_tex;
static int       s_tex_w, s_tex_h;
static int       s_back_w, s_back_h;

static int d3d_open(void);

/* grBufferSwap(0) is deliberately immediate: Hydro owns its gameplay cadence
 * and global host VSync causes slow motion when a display wait misses that
 * deadline.  A positive interval is different: both operator renderers pass
 * 1 precisely because they expect the board's 60 Hz retrace to bound their
 * otherwise unpaced loop.  Keep that application request in the presenter,
 * without changing the D3D device's always-immediate policy. */
static LARGE_INTEGER s_pace_freq;
static LONGLONG s_pace_next;
static int s_pace_interval;

static void pace_reset(void)
{
    s_pace_next = 0;
    s_pace_interval = 0;
}

static void wait_swap_interval(int interval)
{
    LARGE_INTEGER now;
    LONGLONG period, remain;

    if (interval <= 0) {
        pace_reset();
        return;
    }
    if (interval > 255)
        interval = 255;
    if (!s_pace_freq.QuadPart && !QueryPerformanceFrequency(&s_pace_freq))
        return;
    QueryPerformanceCounter(&now);
    period = (s_pace_freq.QuadPart * (LONGLONG)interval) /
             (s_refresh_hz > 0 ? s_refresh_hz : 60);
    if (period < 1)
        period = 1;

    if (!s_pace_next || s_pace_interval != interval ||
        now.QuadPart - s_pace_next > s_pace_freq.QuadPart) {
        s_pace_next = now.QuadPart + period;
        s_pace_interval = interval;
    } else {
        while (s_pace_next <= now.QuadPart)
            s_pace_next += period;
    }

    for (;;) {
        QueryPerformanceCounter(&now);
        remain = s_pace_next - now.QuadPart;
        if (remain <= 0)
            break;
        /* Sleep for the coarse part and yield through the final scheduler
         * quantum.  The QPC deadline, rather than Sleep's duration, owns the
         * cadence, so timer granularity cannot accumulate drift. */
        if (remain > s_pace_freq.QuadPart / 500)
            Sleep(1);
        else
            SwitchToThread();
    }
    s_pace_next += period;
}

static int raster_hint(const char *name, int surface_extent)
{
    char value[32];
    char *end;
    DWORD n = GetEnvironmentVariableA(name, value, sizeof value);
    long v;

    if (!n || n >= sizeof value)
        return surface_extent;
    v = strtol(value, &end, 10);
    if (*end || v <= 0 || v > surface_extent) {
        vgl_log(0, "present: ignoring invalid %s='%s' for surface extent %d",
                name, value, surface_extent);
        return surface_extent;
    }
    return (int)v;
}

/* A fixed-size overlapped window: caption, minimise and close remain; the
 * resize frame and maximise affordance do not. The host applies the same
 * policy to third-party provider windows in src/window.c. */
#define VGL_WINDOW_STYLE \
    (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

/* Enable per-monitor physical-pixel coordinates. Resolve the newer DPI context
 * dynamically for compatibility with the build headers. */
static void make_dpi_aware(void)
{
    HMODULE u32 = GetModuleHandleA("user32.dll");
    typedef BOOL (WINAPI *setctx_t)(void *);
    typedef BOOL (WINAPI *setaware_t)(void);
    setctx_t setctx = u32 ? (setctx_t)(void *)GetProcAddress(
        u32, "SetProcessDpiAwarenessContext") : NULL;

    if (setctx && setctx((void *)-4))   /* PER_MONITOR_AWARE_V2 */
        return;
    {
        setaware_t setaware = u32 ? (setaware_t)(void *)GetProcAddress(
            u32, "SetProcessDPIAware") : NULL;
        if (setaware)
            setaware();
    }
}

/* Size the client from the visible raster, excluding padded surface columns. */
static void size_to_raster(void)
{
    RECT r;

    if (!s_wnd || !s_owns_window || s_rw <= 0 || s_rh <= 0)
        return;
    r.left = 0; r.top = 0;
    r.right = s_rw * s_scale; r.bottom = s_rh * s_scale;
    AdjustWindowRect(&r, VGL_WINDOW_STYLE, FALSE);
    SetWindowPos(s_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    vgl_log(0, "present: raster %dx%d at %dx -> client %dx%d (surface %dx%d, "
               "pad not presented, edge extension L%d T%d R%d B%d)",
            s_rw, s_rh, s_scale, s_rw * s_scale, s_rh * s_scale, s_w, s_h,
            s_vx0, s_vy0, s_rw - s_vx1, s_rh - s_vy1);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE) {
        /* Let the host process close requests at the next swap. */
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcA(h, msg, wp, lp);
}

int vgl_present_open(void *caller_window, int w, int h, int scale,
                     int refresh_hz)
{
    WNDCLASSA wc;
    RECT r;
    char name[96];

    vgl_present_close();
    make_dpi_aware();
    s_w = w;
    s_h = h;
    s_scale = scale > 0 ? scale : 1;
    s_refresh_hz = refresh_hz > 0 ? refresh_hz : 60;
    pace_reset();
    /* The host can name the raster at open time because it saw the game's
     * original resolution before substituting a legal 640x400 surface.  This
     * is essential across Hydro's service-mode reinit, which sends no new
     * grClipWindow.  Without a hint, the raster remains the surface until a
     * clip arrives, preserving standalone-provider behaviour. */
    s_rx = s_ry = 0;
    s_rw = raster_hint("VCGLIDE_RASTER_WIDTH", w);
    s_rh = raster_hint("VCGLIDE_RASTER_HEIGHT", h);
    s_vx0 = s_vy0 = 0; s_vx1 = s_rw; s_vy1 = s_rh;

    s_bmi = (BITMAPINFO *)calloc(1, sizeof(BITMAPINFO));
    s_rgb = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!s_bmi || !s_rgb)
        return 0;
    s_bmi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    s_bmi->bmiHeader.biWidth       = w;
    s_bmi->bmiHeader.biHeight      = -h;         /* top-down, as we store it */
    s_bmi->bmiHeader.biPlanes      = 1;
    s_bmi->bmiHeader.biBitCount    = 32;
    s_bmi->bmiHeader.biCompression = BI_RGB;

    if (caller_window && IsWindow((HWND)caller_window)) {
        s_given = (HWND)caller_window;
        s_wnd = s_given;
        s_owns_window = 0;
    } else {
        /* Both games pass a null window handle (the cabinet had no window
         * manager), so a provider that wants to be seen has to make one.
         * Hydro's service-mode reopen passes the small Glide context value 3
         * in this slot.  It is not an HWND; accepting it made GetDC and D3D
         * setup target window 0x3 and left the reopened surface unpresented.
         * Validate a supplied handle before adopting it. */
        if (caller_window)
            vgl_log(0, "present: grSstWinOpen supplied %p, which is not a "
                       "Win32 window: creating our own", caller_window);
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc   = wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = "vcglide";
        RegisterClassA(&wc);

        r.left = r.top = 0;
        r.right = s_rw * s_scale; r.bottom = s_rh * s_scale;
        AdjustWindowRect(&r, VGL_WINDOW_STYLE, FALSE);
        vgl_window_title(name, sizeof name, 0.0);   /* no frame rate yet */
        s_wnd = CreateWindowExA(0, "vcglide", name,
                                VGL_WINDOW_STYLE | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                NULL, NULL, wc.hInstance, NULL);
        s_owns_window = 1;
    }
    if (!s_wnd) {
        vgl_log(0, "present: no window (err=%lu); rendering continues and "
                   "nothing is displayed", GetLastError());
        return 0;
    }
    size_to_raster();
    s_dc = GetDC(s_wnd);
    if (d3d_open())
        vgl_log(0, "present: %dx%d XRGB8888 through Direct3D 9 (GPU bilinear) "
                   "on %s window %p", w, h,
                s_owns_window ? "our own" : "the caller's", (void *)s_wnd);
    else
        vgl_log(0, "present: Direct3D 9 unavailable; %dx%d XRGB8888 through "
                   "GDI HALFTONE on %s window %p", w, h,
                s_owns_window ? "our own" : "the caller's", (void *)s_wnd);
    return 1;
}

void vgl_present_raster(int x, int y, int w, int h,
                        int valid_x0, int valid_y0,
                        int valid_x1, int valid_y1)
{
    if (w <= 0 || h <= 0)
        return;
    valid_x0 -= x; valid_x1 -= x;
    valid_y0 -= y; valid_y1 -= y;
    if (valid_x0 < 0) valid_x0 = 0;
    if (valid_x0 > w) valid_x0 = w;
    if (valid_y0 < 0) valid_y0 = 0;
    if (valid_y0 > h) valid_y0 = h;
    if (valid_x1 < 0) valid_x1 = 0;
    if (valid_x1 > w) valid_x1 = w;
    if (valid_y1 < 0) valid_y1 = 0;
    if (valid_y1 > h) valid_y1 = h;
    if (x == s_rx && y == s_ry && w == s_rw && h == s_rh &&
        valid_x0 == s_vx0 && valid_y0 == s_vy0 &&
        valid_x1 == s_vx1 && valid_y1 == s_vy1)
        return;
    s_rx = x; s_ry = y; s_rw = w; s_rh = h;
    s_vx0 = valid_x0; s_vy0 = valid_y0;
    s_vx1 = valid_x1; s_vy1 = valid_y1;
    size_to_raster();
}

/* Extend nearest drawn texels across the undrawn top/right raster margin only
 * during presentation. Framebuffer readback remains unchanged. */
static void extend_undrawn_edges(void)
{
    int x, y;

    if (s_vx0 >= s_vx1 || s_vy0 >= s_vy1)
        return;
    for (y = 0; y < s_vy0; y++)
        memcpy(s_rgb + (size_t)y * s_rw,
               s_rgb + (size_t)s_vy0 * s_rw, (size_t)s_rw * 4);
    for (y = s_vy1; y < s_rh; y++)
        memcpy(s_rgb + (size_t)y * s_rw,
               s_rgb + (size_t)(s_vy1 - 1) * s_rw, (size_t)s_rw * 4);
    for (y = 0; y < s_rh; y++) {
        uint32_t *row = s_rgb + (size_t)y * s_rw;
        for (x = 0; x < s_vx0; x++)
            row[x] = row[s_vx0];
        for (x = s_vx1; x < s_rw; x++)
            row[x] = row[s_vx1 - 1];
    }
}

/* A borderless popup is the host's fullscreen mode. Windowed geometry already
 * expresses the configured aspect; fullscreen fits the same ini policy inside
 * the monitor and leaves any remainder black. */
static int borderless_fullscreen(void)
{
    LONG style = s_wnd ? GetWindowLongA(s_wnd, GWL_STYLE) : 0;
    return (style & WS_POPUP) != 0 && (style & WS_CAPTION) == 0;
}

static void destination_rect(int cw, int ch, RECT *out)
{
    int x, y, w, h;

    vgl_destination_rect(borderless_fullscreen(), s_rw, s_rh, cw, ch,
                         &x, &y, &w, &h);
    out->left = x;
    out->top = y;
    out->right = x + w;
    out->bottom = y + h;
}

static int pow2_at_least(int n)
{
    int p = 1;
    while (p < n && p < 16384)
        p <<= 1;
    return p;
}

static void d3d_drop_texture(void)
{
    if (s_tex)
        IDirect3DTexture9_Release(s_tex);
    s_tex = NULL;
    s_tex_w = s_tex_h = 0;
}

static void d3d_params(D3DPRESENT_PARAMETERS *pp, int w, int h)
{
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = (UINT)w;
    pp->BackBufferHeight = (UINT)h;
    pp->BackBufferFormat = D3DFMT_UNKNOWN;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->hDeviceWindow = s_wnd;
    pp->Windowed = TRUE;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

static int d3d_open(void)
{
    D3DPRESENT_PARAMETERS pp;
    RECT rc;
    HRESULT hr;

    if (!GetClientRect(s_wnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return 0;
    s_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!s_d3d)
        return 0;
    d3d_params(&pp, rc.right, rc.bottom);
    hr = IDirect3D9_CreateDevice(s_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
             s_wnd, D3DCREATE_FPU_PRESERVE | D3DCREATE_SOFTWARE_VERTEXPROCESSING,
             &pp, &s_dev);
    if (FAILED(hr)) {
        IDirect3D9_Release(s_d3d);
        s_d3d = NULL;
        return 0;
    }
    s_back_w = rc.right;
    s_back_h = rc.bottom;
    return 1;
}

static int d3d_ready(int w, int h)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;

    if (!s_dev)
        return 0;
    hr = IDirect3DDevice9_TestCooperativeLevel(s_dev);
    if (hr == D3DERR_DEVICELOST)
        return 0;
    if (hr == D3DERR_DEVICENOTRESET || w != s_back_w || h != s_back_h) {
        d3d_drop_texture();
        d3d_params(&pp, w, h);
        if (FAILED(IDirect3DDevice9_Reset(s_dev, &pp)))
            return 0;
        s_back_w = w;
        s_back_h = h;
    }
    return 1;
}

static int d3d_texture(void)
{
    int tw = pow2_at_least(s_rw), th = pow2_at_least(s_rh);

    if (s_tex && s_tex_w == tw && s_tex_h == th)
        return 1;
    d3d_drop_texture();
    if (FAILED(IDirect3DDevice9_CreateTexture(s_dev, (UINT)tw, (UINT)th, 1,
               D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
               &s_tex, NULL)))
        return 0;
    s_tex_w = tw;
    s_tex_h = th;
    return 1;
}

static int d3d_present(int cw, int ch)
{
    struct vertex { float x, y, z, rhw, u, v; } q[4];
    D3DLOCKED_RECT lock;
    RECT dst;
    float l, t, r, b, u, v;
    int y;
    HRESULT hr;

    if (!d3d_ready(cw, ch) || !d3d_texture())
        return 0;
    if (FAILED(IDirect3DTexture9_LockRect(s_tex, 0, &lock, NULL,
                                          D3DLOCK_DISCARD)))
        return 0;
    for (y = 0; y < s_rh; y++)
        memcpy((uint8_t *)lock.pBits + (size_t)y * lock.Pitch,
               s_rgb + (size_t)y * s_rw, (size_t)s_rw * 4);
    IDirect3DTexture9_UnlockRect(s_tex, 0);

    destination_rect(cw, ch, &dst);
    l = (float)dst.left - 0.5f;  t = (float)dst.top - 0.5f;
    r = (float)dst.right - 0.5f; b = (float)dst.bottom - 0.5f;
    u = (float)s_rw / (float)s_tex_w;
    v = (float)s_rh / (float)s_tex_h;
    q[0] = (struct vertex){ l, t, 0.0f, 1.0f, 0.0f, 0.0f };
    q[1] = (struct vertex){ r, t, 0.0f, 1.0f, u,    0.0f };
    q[2] = (struct vertex){ l, b, 0.0f, 1.0f, 0.0f, v    };
    q[3] = (struct vertex){ r, b, 0.0f, 1.0f, u,    v    };

    IDirect3DDevice9_Clear(s_dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    if (FAILED(IDirect3DDevice9_BeginScene(s_dev)))
        return 0;
    IDirect3DDevice9_SetRenderState(s_dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(s_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(s_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s_dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetTextureStageState(s_dev, 0, D3DTSS_COLOROP,
                                          D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(s_dev, 0, D3DTSS_COLORARG1,
                                          D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(s_dev, 0, D3DTSS_ALPHAOP,
                                          D3DTOP_DISABLE);
    IDirect3DDevice9_SetSamplerState(s_dev, 0, D3DSAMP_MINFILTER,
                                     D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(s_dev, 0, D3DSAMP_MAGFILTER,
                                     D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(s_dev, 0, D3DSAMP_MIPFILTER,
                                     D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(s_dev, 0, D3DSAMP_ADDRESSU,
                                     D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(s_dev, 0, D3DSAMP_ADDRESSV,
                                     D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetTexture(s_dev, 0, (IDirect3DBaseTexture9 *)s_tex);
    IDirect3DDevice9_SetFVF(s_dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    hr = IDirect3DDevice9_DrawPrimitiveUP(s_dev, D3DPT_TRIANGLESTRIP, 2,
                                          q, sizeof q[0]);
    IDirect3DDevice9_EndScene(s_dev);
    if (FAILED(hr))
        return 0;
    return SUCCEEDED(IDirect3DDevice9_Present(s_dev, NULL, NULL, NULL, NULL));
}

/* Report one-second wall-clock presentation FPS in the title bar. Update on the
 * owning thread and use ASCII with the Win32 A-suffixed API. */
static void title_fps(void)
{
    static LARGE_INTEGER freq, t0;
    static unsigned frames;
    LARGE_INTEGER now;
    char buf[96];

    if (!s_owns_window || !s_wnd)
        return;                         /* the caller's window is the caller's */
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        return;
    }
    frames++;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - t0.QuadPart < freq.QuadPart)
        return;
    {
        double secs = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        vgl_window_title(buf, sizeof buf, (double)frames / secs);
        SetWindowTextA(s_wnd, buf);
    }
    t0 = now;
    frames = 0;
}

void vgl_present_frame(const uint32_t *buf, int w, int h, int swap_interval)
{
    RECT rc, dst;
    int y;

    if (!s_dc || !s_rgb || !buf || w != s_w || h != s_h)
        return;
    wait_swap_interval(swap_interval);
    /* Copy out only the raster. The surface's pad columns are not the game's
     * picture and have no business on screen or in an aspect ratio. */
    for (y = 0; y < s_rh; y++) {
        const uint32_t *src = buf + (size_t)(s_ry + y) * w + s_rx;
        uint32_t *out = s_rgb + (size_t)y * s_rw;
        memcpy(out, src, (size_t)s_rw * 4);
    }
    extend_undrawn_edges();
    s_bmi->bmiHeader.biWidth  = s_rw;
    s_bmi->bmiHeader.biHeight = -s_rh;
    if (!GetClientRect(s_wnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return;
    if (!d3d_present(rc.right, rc.bottom)) {
        destination_rect(rc.right, rc.bottom, &dst);
        PatBlt(s_dc, 0, 0, rc.right, rc.bottom, BLACKNESS);
        SetStretchBltMode(s_dc, HALFTONE);
        SetBrushOrgEx(s_dc, 0, 0, NULL);
        StretchDIBits(s_dc, dst.left, dst.top, dst.right - dst.left,
                      dst.bottom - dst.top, 0, 0, s_rw, s_rh,
                      s_rgb, s_bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    title_fps();
}

/* Pump the provider-owned window from the swap path. */
void vgl_present_pump(void)
{
    MSG msg;

    if (!s_owns_window || !s_wnd)
        return;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        /* WM_CLOSE above posts WM_QUIT for the host's grBufferSwap pump. Do
         * not consume that signal inside the provider: put it back and return
         * so src/glide_bind.c can flush NVRAM and shut the process down. */
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void vgl_present_close(void)
{
    d3d_drop_texture();
    if (s_dev)
        IDirect3DDevice9_Release(s_dev);
    if (s_d3d)
        IDirect3D9_Release(s_d3d);
    s_dev = NULL;
    s_d3d = NULL;
    s_back_w = s_back_h = 0;
    if (s_dc && s_wnd)
        ReleaseDC(s_wnd, s_dc);
    if (s_owns_window && s_wnd)
        DestroyWindow(s_wnd);
    s_dc = NULL;
    s_wnd = s_given = NULL;
    s_owns_window = 0;
    s_refresh_hz = 0;
    pace_reset();
    free(s_bmi); s_bmi = NULL;
    free(s_rgb); s_rgb = NULL;
}
