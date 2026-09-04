/* io_host.c -- host devices to one provider-neutral reading.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * XInput, WinMM, DirectInput and the keyboard, resolved against the axis map
 * in io_config.c into a single host_state_t. Nothing here knows what a board
 * frame looks like; nothing here knows which title is running. See src/io.h
 * for the split.
 */
#include "vcthunder.h"
#include "io.h"

#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <xinput.h>

typedef DWORD (WINAPI *xinput_get_state_fn)(DWORD, XINPUT_STATE *);
typedef MMRESULT (WINAPI *joy_get_pos_ex_fn)(UINT, LPJOYINFOEX);
typedef MMRESULT (WINAPI *joy_get_caps_fn)(UINT_PTR, LPJOYCAPSA, UINT);

static xinput_get_state_fn s_xinput_get_state;
static joy_get_pos_ex_fn s_joy_get_pos_ex;
static joy_get_caps_fn s_joy_get_caps;
static HMODULE s_winmm_module;
static JOYCAPSA s_joy_caps;
static unsigned s_joy_caps_device;
static int s_joy_caps_valid;
static int s_input_active;
static int s_reported_provider = -1;

/* The map, fetched once per translation unit rather than threaded through
 * twenty signatures. io_config_load() runs before anything here. */
#define s_cfg (*io_config())

static void preload_xinput(void)
{
    static const char *const names[] = {
        "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"
    };
    unsigned i;

    if (s_cfg.provider == PROVIDER_NONE || s_cfg.provider == PROVIDER_WINMM)
        return;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        HMODULE mod = LoadLibraryA(names[i]);
        if (!mod)
            continue;
        s_xinput_get_state = (xinput_get_state_fn)(uintptr_t)
            GetProcAddress(mod, "XInputGetState");
        if (s_xinput_get_state) {
            LOGI("io: analogue backend preloaded: %s", names[i]);
            return;
        }
    }
    if (s_cfg.provider == PROVIDER_XINPUT)
        LOGW("io: XInput requested but no XInput DLL is available; keyboard remains active");
}

static void preload_winmm(void)
{
    if (s_cfg.provider == PROVIDER_NONE || s_cfg.provider == PROVIDER_XINPUT)
        return;
    s_winmm_module = GetModuleHandleA("winmm.dll");
    if (!s_winmm_module)
        s_winmm_module = LoadLibraryA("winmm.dll");
    if (s_winmm_module) {
        s_joy_get_pos_ex = (joy_get_pos_ex_fn)(uintptr_t)
            GetProcAddress(s_winmm_module, "joyGetPosEx");
        s_joy_get_caps = (joy_get_caps_fn)(uintptr_t)
            GetProcAddress(s_winmm_module, "joyGetDevCapsA");
    }
    if ((!s_joy_get_pos_ex || !s_joy_get_caps) && s_cfg.provider == PROVIDER_WINMM)
        LOGW("io: WinMM requested but joystick APIs are unavailable; keyboard remains active");
}

/* --- host sampling ----------------------------------------------------- */

static int input_enabled(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();

    if (g_cfg.input_background)
        return 1;
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int key_down(int vk)
{
    return vk && s_input_active && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* The map's deadzone, as a fraction. The arithmetic itself is io.h's, shared
 * with dinput.c and the launcher; see the note there for why the ORDER of the
 * inversion and the deadzone is not a detail. */
static float deadzone(void)
{
    return (float)s_cfg.deadzone_percent / 100.0f;
}

static float deadzone_centered(float v)
{
    return io_deadzone_centred(v, deadzone());
}

static float deadzone_unipolar(float v)
{
    return io_deadzone_unipolar(v, deadzone());
}

static float short_axis(SHORT v)
{
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static int axis_is_trigger(axis_id_t axis)
{
    return axis == AXIS_LT || axis == AXIS_RT;
}

static float xi_centered(const XINPUT_GAMEPAD *g, axis_id_t axis)
{
    switch (axis) {
    case AXIS_LX: return short_axis(g->sThumbLX);
    case AXIS_LY: return short_axis(g->sThumbLY);
    case AXIS_RX: return short_axis(g->sThumbRX);
    case AXIS_RY: return short_axis(g->sThumbRY);
    case AXIS_LT: return (float)g->bLeftTrigger / 255.0f * 2.0f - 1.0f;
    case AXIS_RT: return (float)g->bRightTrigger / 255.0f * 2.0f - 1.0f;
    default: return 0.0f;
    }
}

static float xi_unipolar(const XINPUT_GAMEPAD *g, axis_id_t axis)
{
    float v;
    switch (axis) {
    case AXIS_LT: return (float)g->bLeftTrigger / 255.0f;
    case AXIS_RT: return (float)g->bRightTrigger / 255.0f;
    default:
        v = xi_centered(g, axis);
        return (v + 1.0f) * 0.5f;
    }
}

static unsigned xi_buttons(WORD buttons)
{
    static const WORD native[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB
    };
    unsigned out = 0, i;
    for (i = 0; i < sizeof native / sizeof native[0]; i++)
        if (buttons & native[i])
            out |= 1u << i;
    return out;
}

static int sample_xinput(host_state_t *out)
{
    XINPUT_STATE state;
    XINPUT_GAMEPAD *g;

    if (!s_xinput_get_state)
        return 0;
    memset(&state, 0, sizeof state);
    if (s_xinput_get_state(s_cfg.xinput_device, &state) != ERROR_SUCCESS)
        return 0;
    g = &state.Gamepad;

    memset(out, 0, sizeof *out);
    out->present = 1;
    out->provider = PROVIDER_XINPUT;
    out->steer = deadzone_centered(xi_centered(g, s_cfg.xi_steer));
    if (s_cfg.xi_steer_invert)
        out->steer = -out->steer;

    if (s_cfg.xi_brake == AXIS_NONE) {
        if (axis_is_trigger(s_cfg.xi_throttle))
            out->drive = deadzone_unipolar(xi_unipolar(g, s_cfg.xi_throttle));
        else
            out->drive = deadzone_centered(xi_centered(g, s_cfg.xi_throttle));
        if (s_cfg.xi_throttle_invert)
            out->drive = -out->drive;
    } else {
        out->drive = io_pedals_separate(xi_unipolar(g, s_cfg.xi_throttle),
                                        xi_unipolar(g, s_cfg.xi_brake),
                                        s_cfg.xi_throttle_invert,
                                        s_cfg.xi_brake_invert, deadzone());
    }
    out->drive = io_clamp_unit(out->drive);
    out->buttons = xi_buttons(g->wButtons);
    out->left  = (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    out->right = (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    out->up    = (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    out->down  = (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    return 1;
}

static int ensure_joy_caps(void)
{
    if (!s_joy_get_caps)
        return 0;
    if (s_joy_caps_valid && s_joy_caps_device == s_cfg.winmm_device)
        return 1;
    memset(&s_joy_caps, 0, sizeof s_joy_caps);
    if (s_joy_get_caps(s_cfg.winmm_device, &s_joy_caps,
                       sizeof s_joy_caps) != JOYERR_NOERROR)
        return 0;
    s_joy_caps_device = s_cfg.winmm_device;
    s_joy_caps_valid = 1;
    return 1;
}

static int mm_axis(const JOYINFOEX *j, axis_id_t axis,
                   DWORD *value, DWORD *minimum, DWORD *maximum)
{
    switch (axis) {
    case AXIS_X: *value=j->dwXpos; *minimum=s_joy_caps.wXmin; *maximum=s_joy_caps.wXmax; return 1;
    case AXIS_Y: *value=j->dwYpos; *minimum=s_joy_caps.wYmin; *maximum=s_joy_caps.wYmax; return 1;
    case AXIS_Z: *value=j->dwZpos; *minimum=s_joy_caps.wZmin; *maximum=s_joy_caps.wZmax; return 1;
    case AXIS_R: *value=j->dwRpos; *minimum=s_joy_caps.wRmin; *maximum=s_joy_caps.wRmax; return 1;
    case AXIS_U: *value=j->dwUpos; *minimum=s_joy_caps.wUmin; *maximum=s_joy_caps.wUmax; return 1;
    case AXIS_V: *value=j->dwVpos; *minimum=s_joy_caps.wVmin; *maximum=s_joy_caps.wVmax; return 1;
    default: return 0;
    }
}

static float mm_unipolar(const JOYINFOEX *j, axis_id_t axis)
{
    DWORD value, minimum, maximum;
    if (!mm_axis(j, axis, &value, &minimum, &maximum) || maximum <= minimum)
        return 0.0f;
    if (value <= minimum) return 0.0f;
    if (value >= maximum) return 1.0f;
    return (float)(value - minimum) / (float)(maximum - minimum);
}

static float mm_centered(const JOYINFOEX *j, axis_id_t axis)
{
    DWORD value, minimum, maximum;
    if (!mm_axis(j, axis, &value, &minimum, &maximum) || maximum <= minimum)
        return 0.0f;
    if (value <= minimum) return -1.0f;
    if (value >= maximum) return 1.0f;
    return (float)(value - minimum) / (float)(maximum - minimum) * 2.0f - 1.0f;
}

static void mm_pov(host_state_t *out, DWORD pov)
{
    if (pov == JOY_POVCENTERED)
        return;
    out->up    = pov >= 31500 || pov <= 4500;
    out->right = pov >= 4500  && pov <= 13500;
    out->down  = pov >= 13500 && pov <= 22500;
    out->left  = pov >= 22500 && pov <= 31500;
}

static int sample_winmm(host_state_t *out)
{
    JOYINFOEX state;

    if (!s_joy_get_pos_ex || !ensure_joy_caps())
        return 0;
    memset(&state, 0, sizeof state);
    state.dwSize = sizeof state;
    state.dwFlags = JOY_RETURNALL;
    if (s_joy_get_pos_ex(s_cfg.winmm_device, &state) != JOYERR_NOERROR) {
        s_joy_caps_valid = 0;
        return 0;
    }

    memset(out, 0, sizeof *out);
    out->present = 1;
    out->provider = PROVIDER_WINMM;
    out->steer = deadzone_centered(mm_centered(&state, s_cfg.mm_steer));
    if (s_cfg.mm_steer_invert)
        out->steer = -out->steer;

    if (s_cfg.mm_pedals == PEDALS_COMBINED || s_cfg.mm_brake == AXIS_NONE) {
        out->drive = deadzone_centered(mm_centered(&state, s_cfg.mm_throttle));
        if (s_cfg.mm_throttle_invert)
            out->drive = -out->drive;
    } else {
        out->drive = io_pedals_separate(mm_unipolar(&state, s_cfg.mm_throttle),
                                        mm_unipolar(&state, s_cfg.mm_brake),
                                        s_cfg.mm_throttle_invert,
                                        s_cfg.mm_brake_invert, deadzone());
    }
    out->drive = io_clamp_unit(out->drive);
    out->buttons = state.dwButtons;
    mm_pov(out, state.dwPOV);
    return 1;
}

/* DirectInput resolves its own axis map in dinput.c, so the reading arrives in
 * the shape the packer already wants and this is the whole of the bridge. */
static int sample_dinput(host_state_t *out)
{
    di_reading_t r;

    if (!di_sample(&r) || !r.present)
        return 0;
    memset(out, 0, sizeof *out);
    out->present = 1;
    out->provider = PROVIDER_DINPUT;
    out->steer = r.steer;
    out->drive = r.drive;
    out->buttons = r.buttons;
    out->left = r.left; out->right = r.right;
    out->up = r.up; out->down = r.down;
    return 1;
}

static const char *provider_name(input_provider_t provider)
{
    switch (provider) {
    case PROVIDER_XINPUT: return "XInput";
    case PROVIDER_WINMM: return "WinMM joystick/wheel";
    case PROVIDER_DINPUT: return di_device_name();
    default: return "keyboard only";
    }
}

/* `auto` ORDER IS DIRECTINPUT, THEN XINPUT, THEN WINMM, and the reason it is
 * safe to put DirectInput first is in dinput.c's pick_device(): with no device
 * named it selects only a controller that publishes force-feedback effects, so
 * an XInput pad (which publishes none, and whose two triggers DirectInput
 * sums onto one axis) is not claimed here and still reaches XInput below. */
static int sample_host_device(host_state_t *out)
{
    int ok = 0;
    memset(out, 0, sizeof *out);
    if (!s_input_active || s_cfg.provider == PROVIDER_NONE)
        goto report;
    if (s_cfg.provider == PROVIDER_XINPUT)
        ok = sample_xinput(out);
    else if (s_cfg.provider == PROVIDER_WINMM)
        ok = sample_winmm(out);
    else if (s_cfg.provider == PROVIDER_DINPUT)
        ok = sample_dinput(out);
    else {
        ok = sample_dinput(out);
        if (!ok)
            ok = sample_xinput(out);
        if (!ok)
            ok = sample_winmm(out);
    }

report:
    {
        int provider = ok ? (int)out->provider : (int)PROVIDER_NONE;
        if (provider != s_reported_provider) {
            LOGI("io: active host input: %s", provider_name((input_provider_t)provider));
            s_reported_provider = provider;
        }
    }
    return ok;
}
void io_host_preload(void)
{
    preload_xinput();
    preload_winmm();
}

int io_host_active(void)
{
    return s_input_active;
}

int io_host_key_down(int vk)
{
    return key_down(vk);
}

int io_host_sample(host_state_t *out)
{
    return sample_host_device(out);
}

/* The [controls] input_background policy, re-evaluated once a cycle. The
 * transition is logged HERE rather than by the caller: whether this host is
 * allowed to read the cabinet's controls is a host-input fact. */
void io_host_refresh_active(void)
{
    int had = s_input_active;

    s_input_active = input_enabled();
    if (s_input_active != had)
        LOGI("io: input %s", s_input_active ?
             (g_cfg.input_background ? "active (background sampling)"
                                     : "captured (window focused)")
             : "released (window not focused)");
}
