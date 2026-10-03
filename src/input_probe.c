/* input_probe.c -- controller census used only by the pre-game launcher.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Polling is non-exclusive; the explicit, bounded FFB test temporarily takes
 * exclusive DirectInput access and restores non-exclusive polling when it ends.
 */
#define DIRECTINPUT_VERSION 0x0800

#include "vcthunder.h"
#include "io.h"          /* IO_MAX_BUTTONS and the one button accessor */
#include "ini.h"         /* ini_trim() and ini_device_id() */

#include <dinput.h>
#include <mmsystem.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>

#include "di_canon.h"
#include "xi_axes.h"
#include "xinput_hid.h"

/* The probe's ID buffer must be the resolver's match buffer. */
typedef char probe_id_cap_check[INPUT_PROBE_ID_CAP == INI_MATCH_CAP ? 1 : -1];

typedef HRESULT (WINAPI *di8_create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *,
                                        LPUNKNOWN);
typedef DWORD (WINAPI *xinput_get_state_fn)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *xinput_set_state_fn)(DWORD, XINPUT_VIBRATION *);
typedef UINT (WINAPI *joy_get_num_devs_fn)(void);
typedef MMRESULT (WINAPI *joy_get_caps_fn)(UINT_PTR, LPJOYCAPSA, UINT);
typedef MMRESULT (WINAPI *joy_get_pos_ex_fn)(UINT, LPJOYINFOEX);

typedef struct {
    int present;
    LONG minimum, maximum;
    /* THE DRIVER'S OWN NAME FOR THE OBJECT, "Throttle", "Combined Pedals",
     * "Wheel axis". R2: show the device's own truth, not ours. It is the one
     * string that lets a user connect the row that moves to the pedal they
     * pressed, and this file was throwing it away. DIDEVICEOBJECTINSTANCEA's
     * tszName is MAX_PATH; 48 is what a page can show and a driver that needs
     * more is truncated deliberately rather than given a row of its own. */
    char label[48];
} probe_axis_t;

/* One device's axes, built once, so the rows a page lays out and the values a
 * poll fills share one order. `index` is what the backend's reader takes: the
 * DIJOYSTATE2 axis for DirectInput, 0..5 for XInput, x,y,z,r,u,v for WinMM. */
typedef struct {
    unsigned index;
    const char *name;           /* the ini name: "rz", "lt", "z"        */
    const char *label;          /* the driver's name, or NULL for none  */
} probe_axis_desc_t;

typedef struct {
    input_probe_device_t pub;
    GUID guid;
    JOYCAPSA joy_caps;
    /* Every DirectInput device is held open, not just the selected one: a
     * binding is captured from whatever the user presses. */
    LPDIRECTINPUTDEVICE8A dev;
    unsigned long product;           /* guidProduct.Data1: MAKELONG(vid, pid) */
    /* Opened and found unusable: do not retry every tick. */
    int open_failed;
    /* This device's axes, in its own names: Set up listens on every
     * controller, so one device's names must never label another's values.
     * Filled when a DirectInput device is opened, before Acquire; at
     * enumeration for XInput and WinMM. Indexed by the axis the guidType
     * names, as in dinput.c, which is where DirectInput writes the value. */
    probe_axis_t axis[INPUT_PROBE_MAX_AXES];
    probe_axis_desc_t desc[INPUT_PROBE_MAX_AXES];
    unsigned desc_count;
    unsigned slider_seen;            /* every slider shares one GUID         */
    /* The actuator, three ways, as dinput.c keeps it: a G29, G923 and Pro DD
     * refuse the data-format offset, so the test tries the canonical one
     * first. */
    DWORD ffb_ofs, ffb_canon, ffb_type;
} probe_device_t;

/* The range this preview asks every axis for. It is src/dinput.c's DI_RANGE
 * and it has to stay that: the two files read the same wheel and their answers
 * are compared by a user looking at a bar and then at a race. */
#define INPUT_PROBE_DI_RANGE 10000

static probe_device_t s_devices[INPUT_PROBE_MAX_DEVICES];
static unsigned s_device_count;
static int s_selected = -1;
static HWND s_owner;

static HMODULE s_di_module, s_xi_module, s_mm_module;
static LPDIRECTINPUT8A s_di;
static LPDIRECTINPUTEFFECT s_test_effect;
static xinput_get_state_fn s_xinput_get_state;
static xinput_set_state_fn s_xinput_set_state;
static joy_get_num_devs_fn s_joy_get_num_devs;
static joy_get_caps_fn s_joy_get_caps;
static joy_get_pos_ex_fn s_joy_get_pos_ex;
static DWORD s_test_end, s_test_toggle;
static int s_test_sign, s_test_xinput = -1, s_di_exclusive;
/* WHICH handle the force test made exclusive. With one open device per
 * controller, restoring the wrong one would leave a device exclusive for the
 * rest of the session. */
static LPDIRECTINPUTDEVICE8A s_exclusive_dev;

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
    char product[MAX_PATH];
    (void)ctx;

    if (s_device_count >= INPUT_PROBE_MAX_DEVICES)
        return DIENUM_STOP;
    d = &s_devices[s_device_count++];
    memset(d, 0, sizeof *d);
    d->pub.backend = INPUT_PROBE_DINPUT;
    /* %.*s: the prefix and a MAX_PATH product name cannot both fit, so the
       truncation is explicit. Trimmed as dinput.c trims it, because the ini
       hands values back without surrounding blanks. */
    snprintf(product, sizeof product, "%s", inst->tszProductName);
    snprintf(d->pub.name, sizeof d->pub.name, "DirectInput: %.*s",
             (int)(sizeof d->pub.name - sizeof "DirectInput: "),
             ini_trim(product));
    /* The ID as dinput.c builds it, so a line is proved against the string
     * the host matches. */
    snprintf(d->pub.id_name, sizeof d->pub.id_name, "%s", ini_trim(product));
    ini_device_id(d->pub.id, sizeof d->pub.id, d->pub.id_name,
                  &inst->guidProduct, &inst->guidInstance);
    d->guid = inst->guidInstance;
    d->product = inst->guidProduct.Data1;
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
    probe_device_t *d = (probe_device_t *)ctx;
    LPDIRECTINPUTDEVICE8A dev = d->dev;
    DIPROPRANGE range;
    DIPROPDWORD dw;
    probe_axis_t info;
    /* Once per axis object, in order, before the early return: an object
     * with no canonical axis still advances the slider count. */
    DWORD canon = di_canonical_offset(&obj->guidType, &d->slider_seen);
    int index = di_axis_index(canon);

    if ((obj->dwFlags & DIDOI_FFACTUATOR) && d->ffb_ofs == 0xFFFFFFFFu) {
        d->ffb_ofs = obj->dwOfs;
        d->ffb_type = obj->dwType;
        d->ffb_canon = canon;
    }
    if (index < 0)
        return DIENUM_CONTINUE;

    /* THE SAME RANGE THE GAME ASKS FOR, ASKED THE SAME WAY, THEN READ BACK.
     * src/dinput.c sets -10000..10000 and reads the answer; normalising here
     * against whatever raw range the driver happens to report would let the
     * live preview and the running game disagree about the same wheel, which
     * is the one thing this preview exists to rule out. A refusal is honoured
     * rather than assumed away, and a degenerate range is rejected here as it
     * is there. */
    memset(&range, 0, sizeof range);
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = obj->dwType;
    range.lMin = -INPUT_PROBE_DI_RANGE;
    range.lMax = INPUT_PROBE_DI_RANGE;
    IDirectInputDevice8_SetProperty(dev, DIPROP_RANGE, &range.diph);

    memset(&info, 0, sizeof info);
    info.present = 1;
    info.minimum = -INPUT_PROBE_DI_RANGE;
    info.maximum = INPUT_PROBE_DI_RANGE;
    snprintf(info.label, sizeof info.label, "%s", obj->tszName);
    memset(&range, 0, sizeof range);
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = obj->dwType;
    if (SUCCEEDED(IDirectInputDevice8_GetProperty(dev, DIPROP_RANGE,
                                                  &range.diph)) &&
        range.lMax > range.lMin) {
        info.minimum = range.lMin;
        info.maximum = range.lMax;
    }
    /* The first object a guidType names keeps it, as in dinput.c. */
    if (!d->axis[index].present)
        d->axis[index] = info;

    /* And the same two properties, for the same reason: a driver deadzone the
     * game does not have would make the preview's centre wider than the
     * game's. Host deadzone is applied later, in normalised coordinates. */
    memset(&dw, 0, sizeof dw);
    dw.diph.dwSize = sizeof dw;
    dw.diph.dwHeaderSize = sizeof dw.diph;
    dw.diph.dwHow = DIPH_BYID;
    dw.diph.dwObj = obj->dwType;
    dw.dwData = 0;
    IDirectInputDevice8_SetProperty(dev, DIPROP_DEADZONE, &dw.diph);
    dw.dwData = 10000;
    IDirectInputDevice8_SetProperty(dev, DIPROP_SATURATION, &dw.diph);
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
    if (s_di_exclusive && s_exclusive_dev) {
        IDirectInputDevice8_Unacquire(s_exclusive_dev);
        IDirectInputDevice8_SetCooperativeLevel(
            s_exclusive_dev, s_owner, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
        IDirectInputDevice8_Acquire(s_exclusive_dev);
    }
    s_exclusive_dev = NULL;
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

/* The selected device's DirectInput handle, or NULL. The handle belongs to the
 * device and outlives the selection, so this is a lookup rather than a
 * variable that could go stale. */
static LPDIRECTINPUTDEVICE8A selected_dev(void)
{
    if (s_selected < 0 || (unsigned)s_selected >= s_device_count)
        return NULL;
    return s_devices[s_selected].dev;
}

/* Forget the selection, without releasing anything: every device stays open
 * so a capture can hear it; input_probe_stop() releases the handles. */
static void close_selected(void)
{
    stop_force_test();
    s_selected = -1;
}

/* Everything enum_di_axis() fills, to its declared initial value. */
static void reset_axes(probe_device_t *d)
{
    memset(d->axis, 0, sizeof d->axis);
    d->slider_seen = 0;
    d->ffb_ofs = d->ffb_canon = 0xFFFFFFFFu;
    d->ffb_type = 0;
    d->desc_count = 0;
}

static void build_axis_desc(probe_device_t *d);

/* An XInput pad is listed once, as XInput, by the host's own rule
 * (xinput_hid_hides()). */
static void hide_xinput_devices(void)
{
    xinput_hid_t hid[XINPUT_HID_MAX];
    unsigned n = xinput_hid_list(hid, XINPUT_HID_MAX), i, k = 0;

    for (i = 0; i < s_device_count; i++) {
        if (s_devices[i].pub.backend == INPUT_PROBE_DINPUT &&
            xinput_hid_hides(hid, n, s_devices[i].product,
                             s_devices[i].pub.force_feedback))
            continue;
        if (k != i)
            s_devices[k] = s_devices[i];
        k++;
    }
    s_device_count = k;
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
    hide_xinput_devices();
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
        build_axis_desc(d);
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
        /* 32 HERE IS THE API, NOT D4's OLD CEILING. `joyGetPosEx` reports
         * buttons in one `DWORD`, so a WinMM device cannot have a 33rd
         * whatever its caps claim. The DirectInput path above is 128. */
        d->pub.button_count = caps.wNumButtons > 32 ? 32 : caps.wNumButtons;
        d->joy_caps = caps;
        snprintf(d->pub.name, sizeof d->pub.name, "WinMM: %s", caps.szPname);
        build_axis_desc(d);
    }
}

/* XInput's six are fixed by its API, so naming them is a statement about
 * XInput and not an invention of ours. WinMM has no equivalent: JOYCAPS
 * carries ranges and capability bits and not one axis name, so those rows show
 * no driver name rather than a made-up one. */
static const char *const s_xi_axis_name[] = {
    "lx", "ly", "rx", "ry", "lt", "rt"
};
static const char *const s_xi_axis_label[] = {
    "Left stick X", "Left stick Y", "Right stick X", "Right stick Y",
    "Left trigger", "Right trigger"
};
static const char *const s_mm_axis_name[] = { "x", "y", "z", "r", "u", "v" };

static void desc_add(probe_device_t *d, unsigned index, const char *name,
                     const char *label)
{
    if (d->desc_count >= INPUT_PROBE_MAX_AXES)
        return;
    d->desc[d->desc_count].index = index;
    d->desc[d->desc_count].name = name;
    d->desc[d->desc_count].label = (label && label[0]) ? label : NULL;
    d->desc_count++;
}

/* Built once per device, walked by both the poll and the launcher. */
static void build_axis_desc(probe_device_t *d)
{
    unsigned i;

    d->desc_count = 0;
    if (d->pub.backend == INPUT_PROBE_XINPUT) {
        for (i = 0; i < 6; i++)
            desc_add(d, i, s_xi_axis_name[i], s_xi_axis_label[i]);
        return;
    }
    if (d->pub.backend == INPUT_PROBE_WINMM) {
        static const WORD cap[] = { 0, 0, JOYCAPS_HASZ, JOYCAPS_HASR,
                                    JOYCAPS_HASU, JOYCAPS_HASV };
        for (i = 0; i < 6; i++)
            if (!cap[i] || (d->joy_caps.wCaps & cap[i]))
                desc_add(d, i, s_mm_axis_name[i], NULL);
        return;
    }
    for (i = 0; i < INPUT_PROBE_MAX_AXES; i++)
        if (d->axis[i].present)
            desc_add(d, i, s_di_axis_name[i], d->axis[i].label);
}

unsigned input_probe_device_axis_count(unsigned device)
{
    return device < s_device_count ? s_devices[device].desc_count : 0;
}

const char *input_probe_device_axis_name(unsigned device, unsigned index)
{
    return index < input_probe_device_axis_count(device) ?
           s_devices[device].desc[index].name : NULL;
}

const char *input_probe_device_axis_label(unsigned device, unsigned index)
{
    return index < input_probe_device_axis_count(device) ?
           s_devices[device].desc[index].label : NULL;
}

unsigned input_probe_axis_count(void)
{
    return s_selected < 0 ? 0 :
           input_probe_device_axis_count((unsigned)s_selected);
}

const char *input_probe_axis_name(unsigned index)
{
    return s_selected < 0 ? NULL :
           input_probe_device_axis_name((unsigned)s_selected, index);
}

const char *input_probe_axis_label(unsigned index)
{
    return s_selected < 0 ? NULL :
           input_probe_device_axis_label((unsigned)s_selected, index);
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

/* OPEN ONE CONTROLLER AND KEEP IT OPEN, read-only: non-exclusive foreground,
 * no effects. A controller that is not driving the car is a source of button
 * presses and nothing else.
 *
 * Idempotent, and a failure is remembered, without that, a device DirectInput
 * enumerates but will not open is retried 30 times a second. */
static int open_dinput(probe_device_t *d)
{
    DIDEVCAPS caps;

    if (d->dev)
        return 1;
    if (d->open_failed || !s_di || d->pub.backend != INPUT_PROBE_DINPUT)
        return 0;
    if (FAILED(IDirectInput8_CreateDevice(s_di, &d->guid, &d->dev, NULL)) ||
        !d->dev) {
        d->dev = NULL;
        d->open_failed = 1;
        return 0;
    }
    if (FAILED(IDirectInputDevice8_SetDataFormat(d->dev, &c_dfDIJoystick2)) ||
        FAILED(IDirectInputDevice8_SetCooperativeLevel(
                   d->dev, s_owner, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND))) {
        IDirectInputDevice8_Release(d->dev);
        d->dev = NULL;
        d->open_failed = 1;
        return 0;
    }
    /* Named here, once, before Acquire, setting and reading back the ranges
     * as the host does. */
    reset_axes(d);
    IDirectInputDevice8_EnumObjects(d->dev, enum_di_axis, d, DIDFT_AXIS);
    build_axis_desc(d);
    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    if (SUCCEEDED(IDirectInputDevice8_GetCapabilities(d->dev, &caps))) {
        d->pub.axis_count = caps.dwAxes;
        d->pub.button_count = caps.dwButtons > IO_MAX_BUTTONS
                              ? IO_MAX_BUTTONS : caps.dwButtons;
    }
    IDirectInputDevice8_Acquire(d->dev);
    return 1;
}

int input_probe_select(unsigned index)
{
    probe_device_t *d;

    close_selected();
    if (index >= s_device_count)
        return 0;
    d = &s_devices[index];
    if (d->pub.backend == INPUT_PROBE_DINPUT && !open_dinput(d))
        return 0;
    s_selected = (int)index;
    return 1;
}

/* Every device answers with its own axes, in its own names. */
static int poll_dinput(const probe_device_t *d, input_probe_state_t *out)
{
    LPDIRECTINPUTDEVICE8A dev = d->dev;
    DIJOYSTATE2 js;
    unsigned i;

    if (!dev)
        return 0;
    if (FAILED(IDirectInputDevice8_Poll(dev))) {
        IDirectInputDevice8_Acquire(dev);
        if (FAILED(IDirectInputDevice8_Poll(dev)))
            return 0;
    }
    memset(&js, 0, sizeof js);
    if (FAILED(IDirectInputDevice8_GetDeviceState(dev, sizeof js, &js)))
        return 0;
    /* THE SAME TABLE THE HOST WILL READ, AND THE SAME INDEX INTO THE STATE.
     * The walk is build_axis_desc()'s, so the rows the page laid out and the
     * values arriving here are one list read twice. */
    for (i = 0; i < d->desc_count; i++) {
        unsigned q = d->desc[i].index;
        out->axis[i] = normalise(di_read_axis(&js, q),
                                 d->axis[q].minimum, d->axis[q].maximum);
        out->axis_name[i] = d->desc[i].name;
    }
    out->axis_count = d->desc_count;
    for (i = 0; i < IO_MAX_BUTTONS; i++)
        if (js.rgbButtons[i] & 0x80)
            io_button_set(out->buttons, i);
    out->pov = js.rgdwPOV[0] == 0xFFFFFFFFu ? -1 : (int)js.rgdwPOV[0];
    return 1;
}

static int poll_xinput(const probe_device_t *d, input_probe_state_t *out)
{
    XINPUT_STATE state;
    XINPUT_GAMEPAD *g;
    unsigned i;

    memset(&state, 0, sizeof state);
    if (!s_xinput_get_state ||
        s_xinput_get_state(d->pub.backend_index, &state) != ERROR_SUCCESS)
        return 0;
    g = &state.Gamepad;
    for (i = 0; i < d->desc_count; i++) {
        out->axis[i] = xi_axis_unit(g, d->desc[i].index);
        out->axis_name[i] = d->desc[i].name;
    }
    out->axis_count = d->desc_count;
    out->buttons[0] = xi_button_bits(g->wButtons);
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

/* The value and the range for one of WinMM's six, by the same index
 * build_axis_desc() hands out. The caps bits decide which of them EXIST; that
 * question is the descriptor's and is not asked twice. */
static float mm_axis_value(const JOYCAPSA *c, const JOYINFOEX *js,
                           unsigned index)
{
    switch (index) {
    case 0: return joy_axis(js->dwXpos, c->wXmin, c->wXmax);
    case 1: return joy_axis(js->dwYpos, c->wYmin, c->wYmax);
    case 2: return joy_axis(js->dwZpos, c->wZmin, c->wZmax);
    case 3: return joy_axis(js->dwRpos, c->wRmin, c->wRmax);
    case 4: return joy_axis(js->dwUpos, c->wUmin, c->wUmax);
    default: return joy_axis(js->dwVpos, c->wVmin, c->wVmax);
    }
}

static int poll_winmm(const probe_device_t *d, input_probe_state_t *out)
{
    const JOYCAPSA *c = &d->joy_caps;
    JOYINFOEX js;
    unsigned i;

    memset(&js, 0, sizeof js);
    js.dwSize = sizeof js;
    js.dwFlags = JOY_RETURNALL;
    if (!s_joy_get_pos_ex ||
        s_joy_get_pos_ex(d->pub.backend_index, &js) != JOYERR_NOERROR)
        return 0;
    for (i = 0; i < d->desc_count; i++) {
        out->axis[i] = mm_axis_value(c, &js, d->desc[i].index);
        out->axis_name[i] = d->desc[i].name;
    }
    out->axis_count = d->desc_count;
    out->buttons[0] = js.dwButtons;
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
    if (device->pub.backend == INPUT_PROBE_DINPUT && selected_dev()) {
        DIEFFECT effect;
        DICONSTANTFORCE force;
        LONG direction = 0;
        HRESULT hr;

        LPDIRECTINPUTDEVICE8A dev = selected_dev();

        IDirectInputDevice8_Unacquire(dev);
        hr = IDirectInputDevice8_SetCooperativeLevel(
            dev, s_owner, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
        if (FAILED(hr) || FAILED(IDirectInputDevice8_Acquire(dev))) {
            IDirectInputDevice8_SetCooperativeLevel(
                dev, s_owner, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
            IDirectInputDevice8_Acquire(dev);
            return 0;
        }
        s_di_exclusive = 1;
        s_exclusive_dev = dev;
        /* THREE WAYS OF NAMING ONE ACTUATOR, canonical first. The data-format
         * offset is REFUSED with DIERR_INVALIDPARAM by every Logitech wheel
         * measured, so trying only that one reports "no force feedback" on
         * hardware that has it. src/dinput.c:create_effects() is the same
         * ladder. */
        {
            DWORD axis[3];
            int by_id[3], n = 0, i;

            if (device->ffb_canon != 0xFFFFFFFFu) {
                axis[n] = device->ffb_canon; by_id[n++] = 0;
            }
            if (device->ffb_ofs != 0xFFFFFFFFu &&
                device->ffb_ofs != device->ffb_canon) {
                axis[n] = device->ffb_ofs;   by_id[n++] = 0;
            }
            if (device->ffb_ofs != 0xFFFFFFFFu) {
                axis[n] = device->ffb_type;  by_id[n++] = 1;
            }
            /* The device publishes effects but flags no actuator: x is a
             * guess, and it is the guess the shipped default already makes. */
            if (!n) {
                axis[n] = DIJOFS_X;    by_id[n++] = 0;
            }

            hr = E_FAIL;
            for (i = 0; i < n; i++) {
                memset(&force, 0, sizeof force);
                force.lMagnitude = 3500;
                memset(&effect, 0, sizeof effect);
                effect.dwSize = sizeof effect;
                effect.dwFlags = DIEFF_CARTESIAN |
                    (by_id[i] ? DIEFF_OBJECTIDS : DIEFF_OBJECTOFFSETS);
                effect.dwDuration = INFINITE;
                effect.dwGain = DI_FFNOMINALMAX;
                effect.dwTriggerButton = DIEB_NOTRIGGER;
                effect.cAxes = 1;
                effect.rgdwAxes = &axis[i];
                effect.rglDirection = &direction;
                effect.cbTypeSpecificParams = sizeof force;
                effect.lpvTypeSpecificParams = &force;
                s_test_effect = NULL;
                hr = IDirectInputDevice8_CreateEffect(
                    dev, &GUID_ConstantForce, &effect,
                    &s_test_effect, NULL);
                if (SUCCEEDED(hr) && s_test_effect)
                    break;
                if (s_test_effect) {
                    IDirectInputEffect_Release(s_test_effect);
                    s_test_effect = NULL;
                }
            }
        }
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

/* One controller by index, selected or not, axes and all. */
static int poll_one(unsigned index, input_probe_state_t *out)
{
    probe_device_t *d;

    if (!out || index >= s_device_count)
        return 0;
    memset(out, 0, sizeof *out);
    out->pov = -1;
    d = &s_devices[index];
    if (d->pub.backend == INPUT_PROBE_DINPUT) {
        if (!open_dinput(d))
            return 0;
        return poll_dinput(d, out);
    }
    if (d->pub.backend == INPUT_PROBE_XINPUT)
        return poll_xinput(d, out);
    return poll_winmm(d, out);
}

int input_probe_poll(input_probe_state_t *out)
{
    if (s_selected < 0)
        return 0;
    advance_force_test();
    return poll_one((unsigned)s_selected, out);
}

int input_probe_poll_device(unsigned index, input_probe_state_t *out)
{
    return poll_one(index, out);
}

void input_probe_stop(void)
{
    unsigned i;

    close_selected();
    /* EVERY HANDLE, not just the selected one; that is what changed when
     * every controller started being held open. A device left acquired here is
     * a device the GAME then has to take off us. */
    for (i = 0; i < s_device_count; i++)
        if (s_devices[i].dev) {
            IDirectInputDevice8_Unacquire(s_devices[i].dev);
            IDirectInputDevice8_Release(s_devices[i].dev);
            s_devices[i].dev = NULL;
        }
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
