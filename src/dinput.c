/* dinput.c -- DirectInput wheel input and constant-force output.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game supplies force through cabinet DAC channel 0; this module adds no
 * effects. Device creation occurs before game mapping, and a private hidden
 * window provides stable exclusive-mode ownership across renderer window
 * changes.
 */
#define INITGUID
#define DIRECTINPUT_VERSION 0x0800

#include "vcthunder.h"
#include "io.h"
#include "ini.h"

#include <dinput.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *di8_create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *,
                                        LPUNKNOWN);

/* DIJOYSTATE2's axis members, in the order c_dfDIJoystick2 lays them out. The
 * ini names an axis by the name its driver's control panel uses, which is these
 *: a wheel is x/y/z/rx/ry/rz plus two sliders and nothing else. */
typedef enum {
    DI_AXIS_NONE = 0,
    DI_AXIS_X, DI_AXIS_Y, DI_AXIS_Z,
    DI_AXIS_RX, DI_AXIS_RY, DI_AXIS_RZ,
    DI_AXIS_SLIDER0, DI_AXIS_SLIDER1
} di_axis_t;

/* Every axis is ASKED for this range. What it actually reports is read back and
 * used instead; see s_axis. A driver is free to refuse DIPROP_RANGE, and a
 * refusal that goes unnoticed is indistinguishable from a wheel whose
 * calibration is wrong: every reading comes out compressed or clipped, by a
 * factor nothing in the log accounts for. */
#define DI_RANGE 10000

/* What the device says each of its axes is, rather than what we asked for.
 * `present` is the other half of the same idea: an axis named in the ini that
 * the device does not have would otherwise read as a constant centre, and in
 * separate-pedal mode a constant centre is not neutral; it is a permanent half-brake
 * that halves the throttle range. That is exactly how a combined-pedal wheel
 * came out reporting 64..191 instead of 0..255. */
typedef struct {
    int  present;
    LONG lmin, lmax;
} di_axis_info_t;

typedef struct {
    int      enabled;
    int      device_index;         /* -1 == choose, see pick_device() */
    char     device_name[MAX_PATH];

    di_axis_t steer, throttle, brake;
    int      steer_invert, throttle_invert, brake_invert;
    int      pedals_separate;
    unsigned deadzone_percent;
    unsigned steer_range_percent;  /* wheel travel that reaches full lock */

    int      ffb_enabled;
    unsigned ffb_gain_percent;     /* device-wide gain, 0..100            */
    unsigned ffb_strength_percent; /* our own scale on the game's force   */
    int      ffb_invert;
    int      autocenter;
    int      trace;
} di_config_t;

static di_config_t s_cfg;
static HMODULE s_module;
static di8_create_fn s_create;
static LPDIRECTINPUT8A s_di;
static LPDIRECTINPUTDEVICE8A s_dev;
static LPDIRECTINPUTEFFECT s_force;
static HWND s_window;
static ATOM s_class;
static char s_name[MAX_PATH];
static di_axis_info_t s_axis[DI_AXIS_SLIDER1 + 1];
static int s_have_ffb;
static int s_acquired;
static int s_autocenter_taken;   /* we turned it off, so we put it back */
static LONG s_last_magnitude = 0x7FFFFFFF;
/* The largest |magnitude| actually handed to DirectInput since the census last
 * read it. This is the OUTPUT end of the chain, and it is reported because
 * "the force is not reaching the wheel at full strength" is otherwise an
 * argument rather than a measurement: 10000 here is DirectInput's full scale,
 * and anything below it that the DAC did not ask for is ours. */
static LONG s_peak_magnitude;

/* Enumeration result. A wheel and a pad can both be attached; which one this
 * ends up on is a decision the log has to be able to answer. */
typedef struct {
    GUID guid;
    char name[MAX_PATH];
    int  ffb;
} di_device_t;

#define DI_MAX_DEVICES 16
static di_device_t s_devices[DI_MAX_DEVICES];
static unsigned s_device_count;

/* --- configuration ------------------------------------------------------ */

static di_axis_t parse_di_axis(const char *s)
{
    if (!_stricmp(s, "none"))     return DI_AXIS_NONE;
    if (!_stricmp(s, "x"))        return DI_AXIS_X;
    if (!_stricmp(s, "y"))        return DI_AXIS_Y;
    if (!_stricmp(s, "z"))        return DI_AXIS_Z;
    if (!_stricmp(s, "rx"))       return DI_AXIS_RX;
    if (!_stricmp(s, "ry"))       return DI_AXIS_RY;
    if (!_stricmp(s, "rz"))       return DI_AXIS_RZ;
    if (!_stricmp(s, "slider")  || !_stricmp(s, "slider0")) return DI_AXIS_SLIDER0;
    if (!_stricmp(s, "slider1")) return DI_AXIS_SLIDER1;
    return DI_AXIS_NONE;
}

static const char *di_axis_name(di_axis_t a)
{
    switch (a) {
    case DI_AXIS_X:       return "x";
    case DI_AXIS_Y:       return "y";
    case DI_AXIS_Z:       return "z";
    case DI_AXIS_RX:      return "rx";
    case DI_AXIS_RY:      return "ry";
    case DI_AXIS_RZ:      return "rz";
    case DI_AXIS_SLIDER0: return "slider0";
    case DI_AXIS_SLIDER1: return "slider1";
    default:              return "none";
    }
}

/* The object offset DirectInput identifies an axis by inside c_dfDIJoystick2.
 * An effect names its axes by these, not by the enum above. */
static DWORD di_axis_offset(di_axis_t a)
{
    switch (a) {
    case DI_AXIS_X:       return DIJOFS_X;
    case DI_AXIS_Y:       return DIJOFS_Y;
    case DI_AXIS_Z:       return DIJOFS_Z;
    case DI_AXIS_RX:      return DIJOFS_RX;
    case DI_AXIS_RY:      return DIJOFS_RY;
    case DI_AXIS_RZ:      return DIJOFS_RZ;
    case DI_AXIS_SLIDER0: return (DWORD)DIJOFS_SLIDER(0);
    case DI_AXIS_SLIDER1: return (DWORD)DIJOFS_SLIDER(1);
    default:              return DIJOFS_X;
    }
}

static LONG di_axis_value(const DIJOYSTATE2 *js, di_axis_t a)
{
    switch (a) {
    case DI_AXIS_X:       return js->lX;
    case DI_AXIS_Y:       return js->lY;
    case DI_AXIS_Z:       return js->lZ;
    case DI_AXIS_RX:      return js->lRx;
    case DI_AXIS_RY:      return js->lRy;
    case DI_AXIS_RZ:      return js->lRz;
    case DI_AXIS_SLIDER0: return js->rglSlider[0];
    case DI_AXIS_SLIDER1: return js->rglSlider[1];
    default:              return 0;
    }
}

static int di_bool(const char *path, const char *section, const char *key,
                   int fallback)
{
    char text[32];
    ini_read_scoped(path, section, key, fallback ? "true" : "false",
                    text, sizeof text);
    return ini_bool_or(text, fallback);
}

static unsigned di_percent(const char *path, const char *section,
                           const char *key, unsigned fallback, unsigned cap)
{
    char text[32], def[32];
    unsigned n;

    snprintf(def, sizeof def, "%u", fallback);
    ini_read_scoped(path, section, key, def, text, sizeof text);
    n = ini_uint_or(text, 0, UINT_MAX, fallback);
    return n > cap ? cap : n;
}

static void di_load_ini(const char *path)
{
    char text[MAX_PATH];

    memset(&s_cfg, 0, sizeof s_cfg);

    /* A wheel's whole travel is meaningful and its centre is mechanical, so the
     * 12% a thumbstick needs would eat a fifth of the lock here. Zero, and the
     * key exists for a worn potentiometer rather than for a resting thumb. */
    s_cfg.deadzone_percent = di_percent(path, "analog",
                                        "dinput_deadzone_percent", 0, 49);
    /* Zero-based like xinput_device and winmm_device, but `auto` (not 0) is
     * the default, because 0 is a real device here and the two other providers
     * have no equivalent of "whichever one has a motor". */
    ini_read_scoped(path, "analog", "dinput_device", "auto", text, sizeof text);
    s_cfg.device_index = _stricmp(text, "auto") == 0 ? -1 : atoi(text);

    /* Scale physical wheel travel into the cabinet's full-lock range. Driver-
     * level rotation limits remain preferable when available. */
    s_cfg.steer_range_percent =
        di_percent(path, "analog", "dinput_steering_range_percent", 100, 100);
    if (s_cfg.steer_range_percent < 5)
        s_cfg.steer_range_percent = 5;
    ini_read_scoped(path, "analog", "dinput_name", "",
                  s_cfg.device_name, sizeof s_cfg.device_name);

    ini_read_scoped(path, "analog", "dinput_steering_axis", "x", text, sizeof text);
    s_cfg.steer = parse_di_axis(text);
    ini_read_scoped(path, "analog", "dinput_throttle_axis", "y", text, sizeof text);
    s_cfg.throttle = parse_di_axis(text);
    ini_read_scoped(path, "analog", "dinput_brake_axis", "rz", text, sizeof text);
    s_cfg.brake = parse_di_axis(text);
    ini_read_scoped(path, "analog", "dinput_pedal_mode", "separate", text, sizeof text);
    s_cfg.pedals_separate = _stricmp(text, "combined") != 0;

    /* A pedal rests at its axis MAXIMUM on every wheel this was written
     * against: released is +range, floored is -range. Both pedals therefore
     * default to inverted, and steering does not. */
    s_cfg.steer_invert = di_bool(path, "analog", "dinput_steering_invert", 0);
    s_cfg.throttle_invert = di_bool(path, "analog", "dinput_throttle_invert", 1);
    s_cfg.brake_invert = di_bool(path, "analog", "dinput_brake_invert", 1);

    s_cfg.ffb_enabled = di_bool(path, "ffb", "enabled", 1);
    s_cfg.ffb_gain_percent = di_percent(path, "ffb", "device_gain_percent", 100, 100);
    s_cfg.ffb_strength_percent = di_percent(path, "ffb", "strength_percent", 100, 200);
    /* Default motor direction for tested hardware; configuration may invert it
     * for different device wiring. */
    s_cfg.ffb_invert = di_bool(path, "ffb", "invert", 1);
    s_cfg.autocenter = di_bool(path, "ffb", "autocenter", 0);
    s_cfg.trace = di_bool(path, "diagnostics", "input_trace", 0);
}

/* --- device selection --------------------------------------------------- */

static BOOL CALLBACK enum_device_cb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    di_device_t *d;

    (void)ctx;
    if (s_device_count >= DI_MAX_DEVICES)
        return DIENUM_STOP;
    d = &s_devices[s_device_count++];
    d->guid = inst->guidInstance;
    snprintf(d->name, sizeof d->name, "%s", inst->tszProductName);
    d->ffb = 0;                       /* set by the second, filtered pass */
    return DIENUM_CONTINUE;
}

/* Case-insensitive substring, so `dinput_name = thrustmaster` matches whatever
 * capitalisation the driver publishes. Written out rather than reached for in
 * shlwapi: this DLL imports kernel32/user32/ole32/avrt and adding a fifth
 * system DLL to the import table for one string search is not a trade. */
static int name_contains(const char *haystack, const char *needle)
{
    size_t n = strlen(needle), i;

    if (!n)
        return 0;
    for (i = 0; haystack[i]; i++)
        if (!_strnicmp(haystack + i, needle, n))
            return 1;
    return 0;
}

static BOOL CALLBACK enum_ffb_cb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    unsigned i;

    (void)ctx;
    for (i = 0; i < s_device_count; i++)
        if (IsEqualGUID(&s_devices[i].guid, &inst->guidInstance))
            s_devices[i].ffb = 1;
    return DIENUM_CONTINUE;
}

/* Select a force-feedback DirectInput device. Non-effect controllers remain
 * available to the XInput provider. */
static int pick_device(void)
{
    unsigned i;

    if (!s_device_count)
        return -1;
    if (s_cfg.device_name[0]) {
        for (i = 0; i < s_device_count; i++)
            if (name_contains(s_devices[i].name, s_cfg.device_name))
                return (int)i;
        LOGW("io: [analog] dinput_name=%s matches no attached controller",
             s_cfg.device_name);
        return -1;
    }
    if (s_cfg.device_index >= 0) {
        if ((unsigned)s_cfg.device_index < s_device_count)
            return s_cfg.device_index;
        LOGW("io: [analog] dinput_device = %d but only %u are attached",
             s_cfg.device_index, s_device_count);
        return -1;
    }
    for (i = 0; i < s_device_count; i++)
        if (s_devices[i].ffb)
            return (int)i;
    return 0;
}

/* --- the hidden owner window -------------------------------------------- */

/* The hidden device window is created on the game thread and serviced by the
 * per-frame host message pump. It is never visible. */
static LRESULT CALLBACK di_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static int create_owner_window(void)
{
    WNDCLASSA wc;
    HMODULE self = NULL;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)&create_owner_window, &self);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = di_wndproc;
    wc.hInstance = self;
    wc.lpszClassName = "VCThunderInput";
    s_class = RegisterClassA(&wc);
    if (!s_class) {
        LOGW("io: RegisterClass for the input window failed %lu", GetLastError());
        return 0;
    }
    s_window = CreateWindowExA(0, "VCThunderInput", "VCThunder input",
                               WS_POPUP, 0, 0, 1, 1,
                               NULL, NULL, self, NULL);
    if (!s_window) {
        LOGW("io: CreateWindow for the input window failed %lu", GetLastError());
        return 0;
    }
    return 1;
}

/* --- axis ranges -------------------------------------------------------- */

static void axis_dword_property(LPDIRECTINPUTDEVICE8A dev, DWORD obj,
                                REFGUID prop, DWORD value)
{
    DIPROPDWORD dw;

    memset(&dw, 0, sizeof dw);
    dw.diph.dwSize = sizeof dw;
    dw.diph.dwHeaderSize = sizeof dw.diph;
    dw.diph.dwHow = DIPH_BYID;
    dw.diph.dwObj = obj;
    dw.dwData = value;
    IDirectInputDevice8_SetProperty(dev, prop, &dw.diph);
}

/* Which member of DIJOYSTATE2 an enumerated object lands on. `dwOfs` is exactly
 * that offset once a data format has been set, which it has, so this needs no
 * GUID comparisons and (unlike an instance number) separates the two
 * sliders correctly. */
static di_axis_t axis_from_offset(DWORD ofs)
{
    if (ofs == DIJOFS_X)  return DI_AXIS_X;
    if (ofs == DIJOFS_Y)  return DI_AXIS_Y;
    if (ofs == DIJOFS_Z)  return DI_AXIS_Z;
    if (ofs == DIJOFS_RX) return DI_AXIS_RX;
    if (ofs == DIJOFS_RY) return DI_AXIS_RY;
    if (ofs == DIJOFS_RZ) return DI_AXIS_RZ;
    if (ofs == (DWORD)DIJOFS_SLIDER(0)) return DI_AXIS_SLIDER0;
    if (ofs == (DWORD)DIJOFS_SLIDER(1)) return DI_AXIS_SLIDER1;
    return DI_AXIS_NONE;
}

static BOOL CALLBACK enum_axis_cb(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID ctx)
{
    DIPROPRANGE range;
    LPDIRECTINPUTDEVICE8A dev = (LPDIRECTINPUTDEVICE8A)ctx;
    di_axis_t id = axis_from_offset(obj->dwOfs);

    memset(&range, 0, sizeof range);
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = obj->dwType;
    range.lMin = -DI_RANGE;
    range.lMax = DI_RANGE;
    IDirectInputDevice8_SetProperty(dev, DIPROP_RANGE, &range.diph);

    /* READ IT BACK. The request above is not a guarantee (a driver may refuse
     * it, or clamp it), and every reading this file takes is divided by the
     * answer. Asking for -10000..10000 and then dividing by 10000 regardless is
     * how an axis silently comes out at a fraction of its travel, or pinned at
     * full lock over most of it. */
    if (id != DI_AXIS_NONE) {
        s_axis[id].present = 1;
        s_axis[id].lmin = -DI_RANGE;
        s_axis[id].lmax = DI_RANGE;
        memset(&range, 0, sizeof range);
        range.diph.dwSize = sizeof range;
        range.diph.dwHeaderSize = sizeof range.diph;
        range.diph.dwHow = DIPH_BYID;
        range.diph.dwObj = obj->dwType;
        if (SUCCEEDED(IDirectInputDevice8_GetProperty(dev, DIPROP_RANGE,
                                                      &range.diph)) &&
            range.lMax > range.lMin) {
            s_axis[id].lmin = range.lMin;
            s_axis[id].lmax = range.lMax;
        }
    }

    /* Disable driver deadzone and retain full saturation. Host deadzone is
     * applied later in normalised coordinates. */
    axis_dword_property(dev, obj->dwType, DIPROP_DEADZONE, 0);
    axis_dword_property(dev, obj->dwType, DIPROP_SATURATION, 10000);
    return DIENUM_CONTINUE;
}

/* An axis the ini names that the device does not have is a configuration error
 * the host has to SAY, not absorb. Absorbing it is what produced 64..191: with
 * `separate` pedals and a brake on an axis that does not exist, the missing axis
 * read as a constant centre, which in the throttle-minus-brake sum is a
 * permanent half-brake, so the throttle covered the middle half of its range
 * and rested, plausibly, at exactly 128. Nothing about that looks like a fault
 * until the numbers are read off the operator menu's own analogue test. */
static void validate_axis(di_axis_t *field, const char *key)
{
    if (*field == DI_AXIS_NONE || s_axis[*field].present)
        return;
    LOGW("io: [analog] %s = %s, but \"%s\" has no such axis: ignoring it. "
         "Run with [diagnostics] input_trace = true and use the axis that moves.",
         key, di_axis_name(*field), s_name);
    *field = DI_AXIS_NONE;
}

static void validate_axes(void)
{
    unsigned i;
    char have[128];
    int n = 0;

    validate_axis(&s_cfg.steer, "dinput_steering_axis");
    validate_axis(&s_cfg.throttle, "dinput_throttle_axis");
    validate_axis(&s_cfg.brake, "dinput_brake_axis");

    have[0] = '\0';
    for (i = DI_AXIS_X; i <= DI_AXIS_SLIDER1; i++)
        if (s_axis[i].present)
            n += snprintf(have + n, sizeof have - (size_t)n, "%s%s %ld..%ld",
                          n ? ", " : "", di_axis_name((di_axis_t)i),
                          (long)s_axis[i].lmin, (long)s_axis[i].lmax);
    /* The ranges are logged because they are DIVIDED BY, and because a driver
     * that refused DIPROP_RANGE is otherwise invisible: everything still reads,
     * just at the wrong scale. Anything other than -10000..10000 here is the
     * first thing to suspect when an axis feels wrong. */
    LOGI("io: wheel axes present: %s", n ? have : "none");

    if (s_cfg.steer == DI_AXIS_NONE)
        LOGW("io: no steering axis; the wheel will not steer");
    if (s_cfg.pedals_separate && s_cfg.brake == DI_AXIS_NONE &&
        s_cfg.throttle != DI_AXIS_NONE)
        LOGI("io: only one pedal axis, so it is read as a combined one "
             "(full brake .. full throttle) regardless of dinput_pedal_mode");
}

/* --- effects ------------------------------------------------------------ */

static HRESULT set_dword_property(REFGUID prop, DWORD value)
{
    DIPROPDWORD dw;

    memset(&dw, 0, sizeof dw);
    dw.diph.dwSize = sizeof dw;
    dw.diph.dwHeaderSize = sizeof dw.diph;
    dw.diph.dwHow = DIPH_DEVICE;
    dw.diph.dwObj = 0;
    dw.dwData = value;
    return IDirectInputDevice8_SetProperty(s_dev, prop, &dw.diph);
}

static DWORD get_dword_property(REFGUID prop, DWORD fallback)
{
    DIPROPDWORD dw;

    memset(&dw, 0, sizeof dw);
    dw.diph.dwSize = sizeof dw;
    dw.diph.dwHeaderSize = sizeof dw.diph;
    dw.diph.dwHow = DIPH_DEVICE;
    dw.diph.dwObj = 0;
    if (FAILED(IDirectInputDevice8_GetProperty(s_dev, prop, &dw.diph)))
        return fallback;
    return dw.dwData;
}

/* Disable driver autocenter before and after acquisition, then read it back.
 * A remaining spring conflicts with the game's constant-force command. */
static void take_autocenter(int want_on)
{
    DWORD want = want_on ? DIPROPAUTOCENTER_ON : DIPROPAUTOCENTER_OFF;
    HRESULT hr = set_dword_property(DIPROP_AUTOCENTER, want);
    DWORD got;

    if (s_acquired)
        hr = set_dword_property(DIPROP_AUTOCENTER, want);
    got = get_dword_property(DIPROP_AUTOCENTER, want);
    if (got == want) {
        LOGI("io: wheel autocenter %s", want_on ? "on" : "off");
        s_autocenter_taken = !want_on;
        return;
    }
    LOGW("io: THE WHEEL KEPT ITS OWN AUTOCENTER SPRING (asked for %s, it reports "
         "%s, hr=0x%08lX). The game's force now works against that spring "
         "instead of driving the wheel, so centring will stop short and full "
         "lock will feel weak. Set the wheel's control panel to let the GAME "
         "control centring.",
         want_on ? "on" : "off", got ? "on" : "off", (unsigned long)hr);
    s_autocenter_taken = 0;
}

static int create_effects(void)
{
    DIEFFECT eff;
    DICONSTANTFORCE cf;
    DWORD axis = di_axis_offset(s_cfg.steer);
    LONG direction = 0;
    HRESULT hr;

    memset(&cf, 0, sizeof cf);
    cf.lMagnitude = 0;

    memset(&eff, 0, sizeof eff);
    eff.dwSize = sizeof eff;
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwSamplePeriod = 0;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes = 1;
    eff.rgdwAxes = &axis;
    eff.rglDirection = &direction;
    eff.lpEnvelope = NULL;
    eff.cbTypeSpecificParams = sizeof cf;
    eff.lpvTypeSpecificParams = &cf;
    eff.dwStartDelay = 0;

    hr = IDirectInputDevice8_CreateEffect(s_dev, &GUID_ConstantForce, &eff,
                                          &s_force, NULL);
    if (FAILED(hr) || !s_force) {
        LOGW("io: the wheel accepted no constant-force effect (hr=0x%08lX); "
             "input works, force feedback does not", (unsigned long)hr);
        s_force = NULL;
        return 0;
    }
    IDirectInputEffect_Start(s_force, 1, 0);
    return 1;
}

/* --- open / close ------------------------------------------------------- */

static void report_devices(int chosen)
{
    unsigned i;

    for (i = 0; i < s_device_count; i++)
        LOGI("io: dinput device %u: \"%s\"%s%s", i, s_devices[i].name,
             s_devices[i].ffb ? " [force feedback]" : "",
             (int)i == chosen ? "  <- selected" : "");
}

int di_preload(const char *ini)
{
    HRESULT hr;
    HMODULE self = NULL;
    DIDEVCAPS caps;
    int chosen;

    di_load_ini(ini);

    s_module = LoadLibraryA("dinput8.dll");
    if (!s_module) {
        LOGW("io: dinput8.dll is unavailable; no wheel and no force feedback");
        return 0;
    }
    s_create = (di8_create_fn)(uintptr_t)
        GetProcAddress(s_module, "DirectInput8Create");
    if (!s_create) {
        LOGW("io: dinput8.dll has no DirectInput8Create");
        return 0;
    }

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)&di_preload, &self);
    hr = s_create(self, DIRECTINPUT_VERSION, &IID_IDirectInput8A,
                  (void **)&s_di, NULL);
    if (FAILED(hr) || !s_di) {
        LOGW("io: DirectInput8Create failed 0x%08lX", (unsigned long)hr);
        s_di = NULL;
        return 0;
    }

    s_device_count = 0;
    IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_device_cb,
                              NULL, DIEDFL_ATTACHEDONLY);
    IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_ffb_cb,
                              NULL, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    chosen = pick_device();
    report_devices(chosen);
    if (chosen < 0) {
        if (!s_device_count)
            LOGI("io: no DirectInput game controller is attached");
        return 0;
    }

    hr = IDirectInput8_CreateDevice(s_di, &s_devices[chosen].guid, &s_dev, NULL);
    if (FAILED(hr) || !s_dev) {
        LOGW("io: CreateDevice(\"%s\") failed 0x%08lX",
             s_devices[chosen].name, (unsigned long)hr);
        s_dev = NULL;
        return 0;
    }
    snprintf(s_name, sizeof s_name, "%s", s_devices[chosen].name);
    s_have_ffb = s_devices[chosen].ffb;

    IDirectInputDevice8_SetDataFormat(s_dev, &c_dfDIJoystick2);

    /* Prefer background-exclusive access for effects; retain input on fallback. */
    if (!s_window)
        create_owner_window();
    hr = E_FAIL;
    if (s_window)
        hr = IDirectInputDevice8_SetCooperativeLevel(
                 s_dev, s_window, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr)) {
        LOGW("io: exclusive access to \"%s\" was refused (0x%08lX); "
             "input works, force feedback does not", s_name, (unsigned long)hr);
        s_have_ffb = 0;
        if (s_window)
            IDirectInputDevice8_SetCooperativeLevel(
                s_dev, s_window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    }

    IDirectInputDevice8_EnumObjects(s_dev, enum_axis_cb, s_dev, DIDFT_AXIS);
    validate_axes();

    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    IDirectInputDevice8_GetCapabilities(s_dev, &caps);
    LOGI("io: wheel \"%s\": %lu axes, %lu buttons, %lu POV%s",
         s_name, (unsigned long)caps.dwAxes, (unsigned long)caps.dwButtons,
         (unsigned long)caps.dwPOVs,
         (caps.dwFlags & DIDC_FORCEFEEDBACK) ? ", force feedback" : "");

    if (s_have_ffb && s_cfg.ffb_enabled) {
        /* Autocenter while unacquired, which is its documented requirement;
         * take_autocenter() repeats it after Acquire and reads it back. */
        take_autocenter(s_cfg.autocenter);
        if (SUCCEEDED(IDirectInputDevice8_Acquire(s_dev)))
            s_acquired = 1;
        /* GAIN AFTER ACQUIRE. DIPROP_FFGAIN is a force-feedback property and
         * wants an exclusively acquired device; setting it first is how a
         * requested gain silently stays at whatever the driver had. */
        if (FAILED(set_dword_property(DIPROP_FFGAIN,
                       DI_FFNOMINALMAX * s_cfg.ffb_gain_percent / 100)))
            LOGW("io: the wheel refused a device force-feedback gain of %u%%; "
                 "its own control panel's strength setting is in charge",
                 s_cfg.ffb_gain_percent);
        take_autocenter(s_cfg.autocenter);
        create_effects();
    } else if (s_have_ffb) {
        LOGI("io: [ffb] enabled = false; the wheel's motor is left alone");
    }

    if (s_cfg.steer_range_percent < 100)
        LOGI("io: the middle %u%% of the wheel's travel covers full lock "
             "(the cabinet's wheel turned about 270 degrees; a modern one "
             "defaults to 900, which stretches the game's own dead band with "
             "everything else)", s_cfg.steer_range_percent);
    LOGI("io: wheel axes steer=%s throttle=%s brake=%s (%s pedals), "
         "deadzone %u%%, ffb %s gain %u%% strength %u%%%s",
         di_axis_name(s_cfg.steer), di_axis_name(s_cfg.throttle),
         di_axis_name(s_cfg.brake),
         s_cfg.pedals_separate ? "separate" : "combined",
         s_cfg.deadzone_percent,
         s_force ? "on" : "off", s_cfg.ffb_gain_percent,
         s_cfg.ffb_strength_percent,
         s_cfg.ffb_invert ? " inverted" : "");
    return 1;
}

int di_present(void)
{
    return s_dev != NULL;
}

const char *di_device_name(void)
{
    return s_name;
}

void di_close(void)
{
    if (s_force) {
        IDirectInputEffect_Stop(s_force);
        IDirectInputEffect_Release(s_force);
        s_force = NULL;
    }
    if (s_dev) {
        /* Hand the wheel back the way it was found. A device released while
         * autocenter is off stays limp for whatever the user runs next, and
         * nothing else on the system will turn it back on. */
        if (s_autocenter_taken) {
            set_dword_property(DIPROP_AUTOCENTER, DIPROPAUTOCENTER_ON);
            s_autocenter_taken = 0;
        }
        IDirectInputDevice8_Unacquire(s_dev);
        IDirectInputDevice8_Release(s_dev);
        s_dev = NULL;
    }
    if (s_di) {
        IDirectInput8_Release(s_di);
        s_di = NULL;
    }
    if (s_window) {
        HMODULE self = NULL;
        DestroyWindow(s_window);
        s_window = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)&di_close, &self);
        UnregisterClassA("VCThunderInput", self);
        s_class = 0;
    }
    s_acquired = 0;
}

/* --- reading ------------------------------------------------------------ */

/* An axis reading as -1..+1, normalised against the range the DEVICE reports
 * rather than the one we asked it for. An absent axis is 0 and never reaches
 * here: validate_axes() has already turned it into DI_AXIS_NONE. */
static float unit_axis(const DIJOYSTATE2 *js, di_axis_t a)
{
    const di_axis_info_t *ax;
    float span, f;

    if (a <= DI_AXIS_NONE || a > DI_AXIS_SLIDER1)
        return 0.0f;
    ax = &s_axis[a];
    span = (float)(ax->lmax - ax->lmin);
    if (!ax->present || span <= 0.0f)
        return 0.0f;
    f = ((float)(di_axis_value(js, a) - ax->lmin) / span) * 2.0f - 1.0f;
    if (f < -1.0f) return -1.0f;
    if (f >  1.0f) return  1.0f;
    return f;
}

/* io.h owns the arithmetic and the order of it. This file's copy was the one
 * that had both right, which is why the shared law was taken from here. */
static float deadzone(void)
{
    return (float)s_cfg.deadzone_percent / 100.0f;
}

static float deadzone_centered(float v)
{
    return io_deadzone_centred(v, deadzone());
}

static void pov_digital(di_reading_t *out, DWORD pov)
{
    /* An unpressed hat is documented as -1 (0xFFFFFFFF) and several drivers
     * answer 0xFFFF instead; both have the low word at 0xFFFF, which is the
     * check that covers them. */
    if ((pov & 0xFFFF) == 0xFFFF)
        return;
    out->up    = pov >= 31500 || pov <= 4500;
    out->right = pov >= 4500  && pov <= 13500;
    out->down  = pov >= 13500 && pov <= 22500;
    out->left  = pov >= 22500 && pov <= 31500;
}

int di_sample(di_reading_t *out)
{
    DIJOYSTATE2 js;
    HRESULT hr;
    unsigned i;


    memset(out, 0, sizeof *out);
    if (!s_dev)
        return 0;

    if (!s_acquired) {
        if (FAILED(IDirectInputDevice8_Acquire(s_dev)))
            return 0;
        s_acquired = 1;
        /* A re-acquired device has stopped its effects. This is the one place
         * that has to restart them, now that the per-update SetParameters no
         * longer carries DIEP_START. */
        if (s_force)  IDirectInputEffect_Start(s_force, 1, 0);
        s_last_magnitude = 0x7FFFFFFF;
    }
    IDirectInputDevice8_Poll(s_dev);
    memset(&js, 0, sizeof js);
    hr = IDirectInputDevice8_GetDeviceState(s_dev, sizeof js, &js);
    if (FAILED(hr)) {
        /* A lost device is normal (a session switch or another exclusive
         * client takes it), and is retried on the next poll rather than
         * reported, which at 60 Hz would be a log line per 16 ms. */
        s_acquired = 0;
        return 0;
    }

    out->present = 1;
    out->steer = unit_axis(&js, s_cfg.steer);
    /* Expand the configured fraction of travel to the full range, BEFORE the
     * deadzone, so the deadzone keeps meaning "a fraction of the game's range"
     * rather than a fraction of the wheel's. */
    if (s_cfg.steer_range_percent < 100) {
        out->steer *= 100.0f / (float)s_cfg.steer_range_percent;
        if (out->steer < -1.0f) out->steer = -1.0f;
        if (out->steer >  1.0f) out->steer =  1.0f;
    }
    out->steer = deadzone_centered(out->steer);
    if (s_cfg.steer_invert)
        out->steer = -out->steer;

    /* COMBINED IS ALSO WHAT AN ABSENT BRAKE AXIS MEANS. A wheel whose driver
     * merges its pedals reports one axis running full-brake .. full-throttle,
     * and the second axis a `separate` config names simply is not there. */
    if (!s_cfg.pedals_separate || s_cfg.brake == DI_AXIS_NONE) {
        out->drive = deadzone_centered(unit_axis(&js, s_cfg.throttle));
        if (s_cfg.throttle_invert)
            out->drive = -out->drive;
    } else {
        /* A separate pedal is a unipolar control on a bipolar axis: half its
         * travel is "released". Map -range..+range onto 0..1 first; io.h's law
         * then inverts before it deadzones, which is the whole point of it. */
        out->drive = io_pedals_separate(
            (unit_axis(&js, s_cfg.throttle) + 1.0f) * 0.5f,
            (unit_axis(&js, s_cfg.brake) + 1.0f) * 0.5f,
            s_cfg.throttle_invert, s_cfg.brake_invert, deadzone());
    }
    if (out->drive < -1.0f) out->drive = -1.0f;
    if (out->drive >  1.0f) out->drive =  1.0f;

    for (i = 0; i < 32; i++)
        if (js.rgbButtons[i] & 0x80)
            out->buttons |= 1u << i;
    pov_digital(out, js.rgdwPOV[0]);

    if (s_cfg.trace) {
        static DIJOYSTATE2 last;
        if (memcmp(&js, &last, sizeof js) != 0) {
            LOGI("io: dinput x=%6ld y=%6ld z=%6ld rx=%6ld ry=%6ld rz=%6ld "
                 "s0=%6ld s1=%6ld pov=%ld buttons=0x%08lX",
                 js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz,
                 js.rglSlider[0], js.rglSlider[1], (long)js.rgdwPOV[0],
                 (unsigned long)out->buttons);
            last = js;
        }
    }
    return 1;
}

/* --- the motor ---------------------------------------------------------- */

void di_set_force(float f)
{
    DIEFFECT eff;
    DICONSTANTFORCE cf;
    LONG magnitude;

    if (!s_force)
        return;
    if (f < -1.0f) f = -1.0f;
    if (f >  1.0f) f =  1.0f;
    if (s_cfg.ffb_invert)
        f = -f;

    magnitude = (LONG)(f * (float)DI_FFNOMINALMAX *
                       (float)s_cfg.ffb_strength_percent / 100.0f);

    if (magnitude >  DI_FFNOMINALMAX) magnitude =  DI_FFNOMINALMAX;
    if (magnitude < -DI_FFNOMINALMAX) magnitude = -DI_FFNOMINALMAX;
    {
        LONG a = magnitude < 0 ? -magnitude : magnitude;
        if (a > s_peak_magnitude)
            s_peak_magnitude = a;
    }
    if (magnitude == s_last_magnitude)
        return;
    s_last_magnitude = magnitude;

    memset(&cf, 0, sizeof cf);
    cf.lMagnitude = magnitude;
    memset(&eff, 0, sizeof eff);
    eff.dwSize = sizeof eff;
    eff.cbTypeSpecificParams = sizeof cf;
    eff.lpvTypeSpecificParams = &cf;
    /* PARAMETERS ONLY, NOT DIEP_START. This runs at the board's poll rate, and
     * DIEP_START restarts the effect from time zero on every one of those:
     * harmless in principle for an INFINITE constant force, and not something to
     * ask a driver to absorb 60 times a second. The effect is started once when
     * it is created and again after each re-Acquire, which is the case
     * DIEP_START was there to cover. */
    IDirectInputEffect_SetParameters(s_force, &eff, DIEP_TYPESPECIFICPARAMS);
}

/* Peak |magnitude| sent to DirectInput since the last call, and reset. 10000 is
 * DirectInput's full scale, so a census that reports 10000 has established that
 * the host is asking for everything the API can express, and that anything
 * still felt as weak is the wheel's own force setting or its driver, neither of
 * which this process can see. */
long di_peak_force(void)
{
    long peak = (long)s_peak_magnitude;

    s_peak_magnitude = 0;
    return peak;
}
