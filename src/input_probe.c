/* input_probe.c -- controller census used only by the pre-game launcher.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Polling is non-exclusive; the explicit, bounded FFB test temporarily takes
 * exclusive DirectInput access and restores non-exclusive polling when it ends.
 */
#define DIRECTINPUT_VERSION 0x0800

#include "vcthunder.h"

#include <dinput.h>
#include <mmsystem.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT (WINAPI *di8_create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *,
                                        LPUNKNOWN);
typedef DWORD (WINAPI *xinput_get_state_fn)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *xinput_set_state_fn)(DWORD, XINPUT_VIBRATION *);
typedef UINT (WINAPI *joy_get_num_devs_fn)(void);
typedef MMRESULT (WINAPI *joy_get_caps_fn)(UINT_PTR, LPJOYCAPSA, UINT);
typedef MMRESULT (WINAPI *joy_get_pos_ex_fn)(UINT, LPJOYINFOEX);

typedef struct {
    input_probe_device_t pub;
    GUID guid;
    JOYCAPSA joy_caps;
} probe_device_t;

typedef struct {
    int present;
    LONG minimum, maximum;
} probe_axis_t;

static probe_device_t s_devices[INPUT_PROBE_MAX_DEVICES];
static unsigned s_device_count;
static int s_selected = -1;
static HWND s_owner;

static HMODULE s_di_module, s_xi_module, s_mm_module;
static LPDIRECTINPUT8A s_di;
static LPDIRECTINPUTDEVICE8A s_di_device;
static LPDIRECTINPUTEFFECT s_test_effect;
static xinput_get_state_fn s_xinput_get_state;
static xinput_set_state_fn s_xinput_set_state;
static joy_get_num_devs_fn s_joy_get_num_devs;
static joy_get_caps_fn s_joy_get_caps;
static joy_get_pos_ex_fn s_joy_get_pos_ex;
static probe_axis_t s_axis[INPUT_PROBE_MAX_AXES];
static DWORD s_ffb_axis = DIJOFS_X;
static DWORD s_test_end, s_test_toggle;
static int s_test_sign, s_test_xinput = -1, s_di_exclusive;

static const char *const s_di_axis_name[] = {
    "x", "y", "z", "rx", "ry", "rz", "slider0", "slider1"
};

static int di_axis_index(DWORD offset)
{
    if (offset == DIJOFS_X) return 0;
    if (offset == DIJOFS_Y) return 1;
    if (offset == DIJOFS_Z) return 2;
    if (offset == DIJOFS_RX) return 3;
    if (offset == DIJOFS_RY) return 4;
    if (offset == DIJOFS_RZ) return 5;
    if (offset == DIJOFS_SLIDER(0)) return 6;
    if (offset == DIJOFS_SLIDER(1)) return 7;
    return -1;
}

static LONG di_read_axis(const DIJOYSTATE2 *js, unsigned index)
{
    switch (index) {
    case 0: return js->lX;
    case 1: return js->lY;
    case 2: return js->lZ;
    case 3: return js->lRx;
    case 4: return js->lRy;
    case 5: return js->lRz;
    case 6: return js->rglSlider[0];
    case 7: return js->rglSlider[1];
    default: return 0;
    }
}

static float normalise(LONG value, LONG minimum, LONG maximum)
{
    if (maximum <= minimum)
        return 0.0f;
    if (value <= minimum)
        return -1.0f;
    if (value >= maximum)
        return 1.0f;
    return (float)(value - minimum) / (float)(maximum - minimum) * 2.0f - 1.0f;
}

static BOOL CALLBACK enum_di_device(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    probe_device_t *d;
    (void)ctx;

    if (s_device_count >= INPUT_PROBE_MAX_DEVICES)
        return DIENUM_STOP;
    d = &s_devices[s_device_count++];
    memset(d, 0, sizeof *d);
    d->pub.backend = INPUT_PROBE_DINPUT;
    d->pub.backend_index = s_device_count - 1;
    /* %.*s, not %s: tszProductName is MAX_PATH and so is name, so the
       literal prefix cannot fit alongside a full-length product name. The
       truncation is intended: bound it explicitly so it is the code saying
       so rather than snprintf doing it silently (gcc 15 -Wformat-truncation
       is right to ask). */
    snprintf(d->pub.name, sizeof d->pub.name, "DirectInput: %.*s",
             (int)(sizeof d->pub.name - sizeof "DirectInput: "),
             inst->tszProductName);
    d->guid = inst->guidInstance;
    return DIENUM_CONTINUE;
}

static BOOL CALLBACK enum_di_ffb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    unsigned i;
    (void)ctx;

    for (i = 0; i < s_device_count; i++)
        if (s_devices[i].pub.backend == INPUT_PROBE_DINPUT &&
            IsEqualGUID(&s_devices[i].guid, &inst->guidInstance))
            s_devices[i].pub.force_feedback = 1;
    return DIENUM_CONTINUE;
}

static BOOL CALLBACK enum_di_axis(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID ctx)
{
    LPDIRECTINPUTDEVICE8A dev = (LPDIRECTINPUTDEVICE8A)ctx;
    DIPROPRANGE range;
    int index = di_axis_index(obj->dwOfs);

    if (index < 0)
        return DIENUM_CONTINUE;
    memset(&range, 0, sizeof range);
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = obj->dwType;
    if (FAILED(IDirectInputDevice8_GetProperty(dev, DIPROP_RANGE, &range.diph))) {
        range.lMin = -32768;
        range.lMax = 32767;
    }
    s_axis[index].present = 1;
    s_axis[index].minimum = range.lMin;
    s_axis[index].maximum = range.lMax;
    if (obj->dwType & DIDFT_FFACTUATOR)
        s_ffb_axis = obj->dwOfs;
    return DIENUM_CONTINUE;
}

static void set_test_magnitude(LONG magnitude)
{
    DIEFFECT effect;
    DICONSTANTFORCE force;

    if (!s_test_effect)
        return;
    memset(&force, 0, sizeof force);
    force.lMagnitude = magnitude;
    memset(&effect, 0, sizeof effect);
    effect.dwSize = sizeof effect;
    effect.cbTypeSpecificParams = sizeof force;
    effect.lpvTypeSpecificParams = &force;
    IDirectInputEffect_SetParameters(s_test_effect, &effect,
                                     DIEP_TYPESPECIFICPARAMS);
}

static void stop_force_test(void)
{
    if (s_test_effect) {
        set_test_magnitude(0);
        IDirectInputEffect_Stop(s_test_effect);
        IDirectInputEffect_Release(s_test_effect);
        s_test_effect = NULL;
    }
    if (s_di_exclusive && s_di_device) {
        IDirectInputDevice8_Unacquire(s_di_device);
        IDirectInputDevice8_SetCooperativeLevel(
            s_di_device, s_owner, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
        IDirectInputDevice8_Acquire(s_di_device);
    }
    if (s_test_xinput >= 0 && s_xinput_set_state) {
        XINPUT_VIBRATION vibration;
        memset(&vibration, 0, sizeof vibration);
        s_xinput_set_state((DWORD)s_test_xinput, &vibration);
    }
    s_di_exclusive = 0;
    s_test_xinput = -1;
    s_test_end = s_test_toggle = 0;
    s_test_sign = 0;
}

static void close_selected(void)
{
    stop_force_test();
    if (s_di_device) {
        IDirectInputDevice8_Unacquire(s_di_device);
        IDirectInputDevice8_Release(s_di_device);
        s_di_device = NULL;
    }
    memset(s_axis, 0, sizeof s_axis);
    s_ffb_axis = DIJOFS_X;
    s_selected = -1;
}

static void enumerate_dinput(void)
{
    di8_create_fn create;
    HMODULE self = NULL;

    s_di_module = LoadLibraryA("dinput8.dll");
    if (!s_di_module)
        return;
    create = (di8_create_fn)(uintptr_t)
        GetProcAddress(s_di_module, "DirectInput8Create");
    if (!create)
        return;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)&input_probe_start, &self);
    if (FAILED(create(self, DIRECTINPUT_VERSION, &IID_IDirectInput8A,
                      (void **)&s_di, NULL)) || !s_di)
        return;
    IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_di_device,
                              NULL, DIEDFL_ATTACHEDONLY);
    IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_di_ffb,
                              NULL, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
}

static void enumerate_xinput(void)
{
    static const char *const dlls[] = {
        "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"
    };
    unsigned i;

    for (i = 0; i < sizeof dlls / sizeof dlls[0]; i++) {
        s_xi_module = LoadLibraryA(dlls[i]);
        if (!s_xi_module)
            continue;
        s_xinput_get_state = (xinput_get_state_fn)(uintptr_t)
            GetProcAddress(s_xi_module, "XInputGetState");
        s_xinput_set_state = (xinput_set_state_fn)(uintptr_t)
            GetProcAddress(s_xi_module, "XInputSetState");
        if (s_xinput_get_state && s_xinput_set_state)
            break;
        FreeLibrary(s_xi_module);
        s_xi_module = NULL;
        s_xinput_get_state = NULL;
        s_xinput_set_state = NULL;
    }
    if (!s_xinput_get_state)
        return;
    for (i = 0; i < XUSER_MAX_COUNT &&
                s_device_count < INPUT_PROBE_MAX_DEVICES; i++) {
        XINPUT_STATE state;
        probe_device_t *d;

        memset(&state, 0, sizeof state);
        if (s_xinput_get_state(i, &state) != ERROR_SUCCESS)
            continue;
        d = &s_devices[s_device_count++];
        memset(d, 0, sizeof *d);
        d->pub.backend = INPUT_PROBE_XINPUT;
        d->pub.backend_index = i;
        d->pub.axis_count = 6;
        d->pub.button_count = 10;
        d->pub.force_feedback = 1;
        snprintf(d->pub.name, sizeof d->pub.name, "XInput controller %u", i);
    }
}

static void enumerate_winmm(void)
{
    UINT i, count;

    s_mm_module = LoadLibraryA("winmm.dll");
    if (!s_mm_module)
        return;
    s_joy_get_num_devs = (joy_get_num_devs_fn)(uintptr_t)
        GetProcAddress(s_mm_module, "joyGetNumDevs");
    s_joy_get_caps = (joy_get_caps_fn)(uintptr_t)
        GetProcAddress(s_mm_module, "joyGetDevCapsA");
    s_joy_get_pos_ex = (joy_get_pos_ex_fn)(uintptr_t)
        GetProcAddress(s_mm_module, "joyGetPosEx");
    if (!s_joy_get_num_devs || !s_joy_get_caps || !s_joy_get_pos_ex)
        return;
    count = s_joy_get_num_devs();
    for (i = 0; i < count && s_device_count < INPUT_PROBE_MAX_DEVICES; i++) {
        probe_device_t *d;
        JOYCAPSA caps;

        memset(&caps, 0, sizeof caps);
        if (s_joy_get_caps(i, &caps, sizeof caps) != JOYERR_NOERROR)
            continue;
        d = &s_devices[s_device_count++];
        memset(d, 0, sizeof *d);
        d->pub.backend = INPUT_PROBE_WINMM;
        d->pub.backend_index = i;
        d->pub.axis_count = caps.wNumAxes;
        d->pub.button_count = caps.wNumButtons > 32 ? 32 : caps.wNumButtons;
        d->joy_caps = caps;
        snprintf(d->pub.name, sizeof d->pub.name, "WinMM: %s", caps.szPname);
    }
}

int input_probe_start(HWND owner)
{
    input_probe_stop();
    s_owner = owner;
    enumerate_dinput();
    enumerate_xinput();
    enumerate_winmm();
    return (int)s_device_count;
}

unsigned input_probe_count(void)
{
    return s_device_count;
}

const input_probe_device_t *input_probe_device(unsigned index)
{
    return index < s_device_count ? &s_devices[index].pub : NULL;
}

int input_probe_select(unsigned index)
{
    probe_device_t *d;
    DIDEVCAPS caps;

    close_selected();
    if (index >= s_device_count)
        return 0;
    d = &s_devices[index];
    s_selected = (int)index;
    if (d->pub.backend != INPUT_PROBE_DINPUT)
        return 1;
    if (!s_di || FAILED(IDirectInput8_CreateDevice(s_di, &d->guid,
                                                    &s_di_device, NULL)) ||
        !s_di_device) {
        s_selected = -1;
        return 0;
    }
    if (FAILED(IDirectInputDevice8_SetDataFormat(s_di_device,
                                                  &c_dfDIJoystick2)) ||
        FAILED(IDirectInputDevice8_SetCooperativeLevel(
                   s_di_device, s_owner,
                   DISCL_NONEXCLUSIVE | DISCL_FOREGROUND))) {
        close_selected();
        return 0;
    }
    IDirectInputDevice8_EnumObjects(s_di_device, enum_di_axis, s_di_device,
                                    DIDFT_AXIS);
    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    if (SUCCEEDED(IDirectInputDevice8_GetCapabilities(s_di_device, &caps))) {
        d->pub.axis_count = caps.dwAxes;
        d->pub.button_count = caps.dwButtons > 32 ? 32 : caps.dwButtons;
    }
    IDirectInputDevice8_Acquire(s_di_device);
    return 1;
}

static uint32_t xinput_buttons(WORD buttons)
{
    static const WORD native[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB
    };
    uint32_t out = 0;
    unsigned i;

    for (i = 0; i < sizeof native / sizeof native[0]; i++)
        if (buttons & native[i])
            out |= 1u << i;
    return out;
}

static int poll_dinput(input_probe_state_t *out)
{
    DIJOYSTATE2 js;
    unsigned i;

    if (!s_di_device)
        return 0;
    if (FAILED(IDirectInputDevice8_Poll(s_di_device))) {
        IDirectInputDevice8_Acquire(s_di_device);
        if (FAILED(IDirectInputDevice8_Poll(s_di_device)))
            return 0;
    }
    memset(&js, 0, sizeof js);
    if (FAILED(IDirectInputDevice8_GetDeviceState(s_di_device, sizeof js, &js)))
        return 0;
    for (i = 0; i < INPUT_PROBE_MAX_AXES; i++)
        if (s_axis[i].present) {
            unsigned n = out->axis_count++;
            out->axis[n] = normalise(di_read_axis(&js, i),
                                     s_axis[i].minimum, s_axis[i].maximum);
            out->axis_name[n] = s_di_axis_name[i];
        }
    for (i = 0; i < 32; i++)
        if (js.rgbButtons[i] & 0x80)
            out->buttons |= 1u << i;
    out->pov = js.rgdwPOV[0] == 0xFFFFFFFFu ? -1 : (int)js.rgdwPOV[0];
    return 1;
}

static int poll_xinput(const probe_device_t *d, input_probe_state_t *out)
{
    static const char *const names[] = { "lx", "ly", "rx", "ry", "lt", "rt" };
    XINPUT_STATE state;
    XINPUT_GAMEPAD *g;
    unsigned i;

    memset(&state, 0, sizeof state);
    if (!s_xinput_get_state ||
        s_xinput_get_state(d->pub.backend_index, &state) != ERROR_SUCCESS)
        return 0;
    g = &state.Gamepad;
    out->axis[0] = (float)g->sThumbLX / (g->sThumbLX < 0 ? 32768.0f : 32767.0f);
    out->axis[1] = (float)g->sThumbLY / (g->sThumbLY < 0 ? 32768.0f : 32767.0f);
    out->axis[2] = (float)g->sThumbRX / (g->sThumbRX < 0 ? 32768.0f : 32767.0f);
    out->axis[3] = (float)g->sThumbRY / (g->sThumbRY < 0 ? 32768.0f : 32767.0f);
    out->axis[4] = (float)g->bLeftTrigger / 255.0f * 2.0f - 1.0f;
    out->axis[5] = (float)g->bRightTrigger / 255.0f * 2.0f - 1.0f;
    for (i = 0; i < 6; i++)
        out->axis_name[i] = names[i];
    out->axis_count = 6;
    out->buttons = xinput_buttons(g->wButtons);
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) out->pov = 0;
    else if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) out->pov = 9000;
    else if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) out->pov = 18000;
    else if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) out->pov = 27000;
    return 1;
}

static float joy_axis(DWORD value, DWORD minimum, DWORD maximum)
{
    return normalise((LONG)value, (LONG)minimum, (LONG)maximum);
}

static int poll_winmm(const probe_device_t *d, input_probe_state_t *out)
{
    static const char *const names[] = { "x", "y", "z", "r", "u", "v" };
    const JOYCAPSA *c = &d->joy_caps;
    JOYINFOEX js;
    unsigned n = 0;

    memset(&js, 0, sizeof js);
    js.dwSize = sizeof js;
    js.dwFlags = JOY_RETURNALL;
    if (!s_joy_get_pos_ex ||
        s_joy_get_pos_ex(d->pub.backend_index, &js) != JOYERR_NOERROR)
        return 0;
#define ADD_AXIS(value, minimum, maximum, label) do { \
    out->axis[n] = joy_axis((value), (minimum), (maximum)); \
    out->axis_name[n] = (label); n++; \
} while (0)
    ADD_AXIS(js.dwXpos, c->wXmin, c->wXmax, names[0]);
    ADD_AXIS(js.dwYpos, c->wYmin, c->wYmax, names[1]);
    if (c->wCaps & JOYCAPS_HASZ) ADD_AXIS(js.dwZpos, c->wZmin, c->wZmax, names[2]);
    if (c->wCaps & JOYCAPS_HASR) ADD_AXIS(js.dwRpos, c->wRmin, c->wRmax, names[3]);
    if (c->wCaps & JOYCAPS_HASU) ADD_AXIS(js.dwUpos, c->wUmin, c->wUmax, names[4]);
    if (c->wCaps & JOYCAPS_HASV) ADD_AXIS(js.dwVpos, c->wVmin, c->wVmax, names[5]);
#undef ADD_AXIS
    out->axis_count = n;
    out->buttons = js.dwButtons;
    out->pov = js.dwPOV == JOY_POVCENTERED ? -1 : (int)js.dwPOV;
    return 1;
}

static void advance_force_test(void)
{
    DWORD now;

    if (!s_test_effect && s_test_xinput < 0)
        return;
    now = GetTickCount();
    if ((LONG)(now - s_test_end) >= 0) {
        stop_force_test();
        return;
    }
    if (s_test_effect && (LONG)(now - s_test_toggle) >= 0) {
        s_test_sign = -s_test_sign;
        set_test_magnitude(s_test_sign * 3500);
        s_test_toggle = now + 65;
    }
}

int input_probe_test_force(void)
{
    probe_device_t *device;
    DWORD now;

    if (s_selected < 0 || (unsigned)s_selected >= s_device_count)
        return 0;
    device = &s_devices[s_selected];
    if (!device->pub.force_feedback)
        return 0;
    stop_force_test();
    now = GetTickCount();

    if (device->pub.backend == INPUT_PROBE_XINPUT) {
        XINPUT_VIBRATION vibration;
        if (!s_xinput_set_state)
            return 0;
        memset(&vibration, 0, sizeof vibration);
        vibration.wLeftMotorSpeed = 42000;
        vibration.wRightMotorSpeed = 52000;
        if (s_xinput_set_state(device->pub.backend_index, &vibration) !=
            ERROR_SUCCESS)
            return 0;
        s_test_xinput = (int)device->pub.backend_index;
        s_test_end = now + 850;
        return 1;
    }
    if (device->pub.backend == INPUT_PROBE_DINPUT && s_di_device) {
        DIEFFECT effect;
        DICONSTANTFORCE force;
        LONG direction = 0;
        HRESULT hr;

        IDirectInputDevice8_Unacquire(s_di_device);
        hr = IDirectInputDevice8_SetCooperativeLevel(
            s_di_device, s_owner, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
        if (FAILED(hr) || FAILED(IDirectInputDevice8_Acquire(s_di_device))) {
            IDirectInputDevice8_SetCooperativeLevel(
                s_di_device, s_owner, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
            IDirectInputDevice8_Acquire(s_di_device);
            return 0;
        }
        s_di_exclusive = 1;
        memset(&force, 0, sizeof force);
        force.lMagnitude = 3500;
        memset(&effect, 0, sizeof effect);
        effect.dwSize = sizeof effect;
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.cAxes = 1;
        effect.rgdwAxes = &s_ffb_axis;
        effect.rglDirection = &direction;
        effect.cbTypeSpecificParams = sizeof force;
        effect.lpvTypeSpecificParams = &force;
        hr = IDirectInputDevice8_CreateEffect(
            s_di_device, &GUID_ConstantForce, &effect, &s_test_effect, NULL);
        if (FAILED(hr) || !s_test_effect ||
            FAILED(IDirectInputEffect_Start(s_test_effect, 1, 0))) {
            stop_force_test();
            return 0;
        }
        s_test_sign = 1;
        s_test_toggle = now + 65;
        s_test_end = now + 850;
        return 1;
    }
    return 0;
}

int input_probe_poll(input_probe_state_t *out)
{
    const probe_device_t *d;

    if (!out || s_selected < 0 || (unsigned)s_selected >= s_device_count)
        return 0;
    advance_force_test();
    memset(out, 0, sizeof *out);
    out->pov = -1;
    d = &s_devices[s_selected];
    if (d->pub.backend == INPUT_PROBE_DINPUT)
        return poll_dinput(out);
    if (d->pub.backend == INPUT_PROBE_XINPUT)
        return poll_xinput(d, out);
    return poll_winmm(d, out);
}

void input_probe_stop(void)
{
    close_selected();
    if (s_di) {
        IDirectInput8_Release(s_di);
        s_di = NULL;
    }
    if (s_di_module) FreeLibrary(s_di_module);
    if (s_xi_module) FreeLibrary(s_xi_module);
    if (s_mm_module) FreeLibrary(s_mm_module);
    s_di_module = s_xi_module = s_mm_module = NULL;
    s_xinput_get_state = NULL;
    s_xinput_set_state = NULL;
    s_joy_get_num_devs = NULL;
    s_joy_get_caps = NULL;
    s_joy_get_pos_ex = NULL;
    s_device_count = 0;
    s_owner = NULL;
}
