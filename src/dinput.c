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

#include "di_canon.h"
#include "di_role.h"
#include "xinput_hid.h"

typedef HRESULT (WINAPI *di8_create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *,
                                        LPUNKNOWN);

/* Every axis is asked for this range; what it reports is read back and used
 * instead, because a driver may refuse DIPROP_RANGE. */
#define DI_RANGE 10000

/* What the device says each axis is, rather than what we asked for. `present`
 * is the other half: an axis the ini names that the device lacks reads as a
 * constant centre, and in separate-pedal mode that is a permanent half-brake,
 * which is how a combined-pedal wheel came out reporting 64..191. */
typedef struct {
    int  present;
    LONG lmin, lmax;
} di_axis_info_t;

typedef struct {
    char     device_name[INI_MATCH_CAP];

    int      ffb_enabled;
    unsigned ffb_gain_percent;     /* device-wide gain, 0..100            */
    unsigned ffb_strength_percent; /* our own scale on the game's force   */
    int      ffb_invert;
    int      autocenter;
    int      trace;
} di_config_t;

static di_config_t s_cfg;
/* The [devices] label of the opened device, empty when it has none. */
static char        s_profile[INI_LABEL_CAP];
static HMODULE s_module;
static di8_create_fn s_create;
static LPDIRECTINPUT8A s_di;
static LPDIRECTINPUTDEVICE8A s_dev;
static LPDIRECTINPUTEFFECT s_force;
static HWND s_window;
static ATOM s_class;
static char s_name[MAX_PATH];
static int s_have_ffb;
/* WHETHER THE OPENED DEVICE PUBLISHES EFFECTS, WHICH IS NOT s_have_ffb.
 * s_have_ffb is cleared when exclusive access is refused; whether we can
 * reach the motor. This is the enumeration fact and is never cleared: a wheel
 * whose exclusive open failed is still a wheel, and still must not have its hat
 * driving the steering to full lock. */
static int s_publishes_ffb;
static int s_autocenter_taken;   /* we turned it off, so we put it back */

static LONG s_last_magnitude = 0x7FFFFFFF;
/* The largest |magnitude| handed to DirectInput since the census last read it:
 * the OUTPUT end of the chain, so "the force is not reaching the wheel at full
 * strength" is a measurement rather than an argument. 10000 is DirectInput's
 * full scale, and anything below it the DAC did not ask for is ours. */
static LONG s_peak_magnitude;

/* Enumeration result. A wheel and a pad can both be attached; which one this
 * ends up on is a decision the log has to be able to answer. */
typedef struct {
    GUID guid;
    char name[MAX_PATH];              /* the product name, what the log shows */
    /* ini_device_id(), built from the product name: what a [devices] line
     * and `dinput_name` are matched against. */
    char id[INI_MATCH_CAP];
    unsigned long product;            /* guidProduct.Data1: MAKELONG(vid, pid) */
    int  ffb;
} di_device_t;

/* The launcher's limit, so the two lists cannot differ in length. */
#define DI_MAX_DEVICES INPUT_PROBE_MAX_DEVICES
static di_device_t s_devices[DI_MAX_DEVICES];
static unsigned s_device_count;

/* One struct per open controller. Slot 0 is the default device: its handle is
 * s_dev (released once, through s_dev) and it owns the motor. Every other slot
 * is an extra device a [devices] line named, held non-exclusive with no effects
 * and no autocenter. Axes are enumerated on slot 0 and on any slot a role
 * reads. */
typedef struct {
    LPDIRECTINPUTDEVICE8A dev;
    char     label[INI_LABEL_CAP];
    char     name[MAX_PATH];
    unsigned device;                  /* index into s_devices */
    /* The XInput pad number for an `xinput:` slot, -1 for DirectInput. Such a
     * slot has no handle; io_host_xinput_read() reads it. */
    int      xinput;
    int      acquired;
    int      answered;                /* it has returned a state at least once */
    int      live;                    /* ...and did on the last poll          */
    int      axes;                    /* its axes were enumerated */
    /* Indexed by the axis each object's guidType names: what an axis name in
     * the ini means (as in SDL and Supermodel), and where DirectInput writes
     * the value. EnumObjects reports a different dwOfs on most wheels. */
    di_axis_info_t axis[DI_AXIS_COUNT];
    unsigned slider_seen;
    /* The force-feedback actuator, three ways, because a driver may accept the
     * effect on one and refuse it on another: data-format offset, guidType
     * offset, object id. G29/G923/Pro DD refuse the real offset and accept the
     * canonical one. A second actuator is kept because a Fanatec CSL DD flags
     * its pedals first; if the first refuses everything the second is tried. */
    DWORD    ffb_ofs, ffb_canon, ffb_type;
    DWORD    ffb2_ofs, ffb2_canon, ffb2_type;
    unsigned ffb_count;
    DIJOYSTATE2 last;                 /* what the trace last printed */
} di_slot_t;
static di_slot_t s_slot[IO_MAX_SLOTS];
static unsigned  s_slot_count;

/* Which controller steering, gas and brake are read from (di_role.h). */
static di_roles_t s_roles;

/* A second [devices] label for a controller already open is another name for
 * its slot. */
#define DI_MAX_ALIASES 64
typedef struct {
    char     label[INI_LABEL_CAP];
    unsigned slot;
} di_alias_t;
static di_alias_t s_alias[DI_MAX_ALIASES];
static unsigned   s_alias_count;
/* The resolved ini path, kept because the slots are opened from the [devices]
 * table AFTER the primary is set up and pick_device() consults the same table
 * BEFORE it. One value rather than a parameter on four functions. */
static char s_ini_path[MAX_PATH];

/* The [devices] table, resolved once before anything is picked or opened. A
 * pin (ini_devices_resolve()) makes each line's device depend on the others,
 * so the table is resolved as a whole. */
static ini_device_line_t s_line[INI_DEVICES_MAX];
static unsigned s_line_count;
static int      s_line_dev[INI_DEVICES_MAX];
static int      s_name_dev = -1;

/* --- configuration ------------------------------------------------------ */

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

    ini_read_scoped(path, "analog", "dinput_name", "",
                  s_cfg.device_name, sizeof s_cfg.device_name);

    s_cfg.ffb_enabled = di_bool(path, "ffb", "enabled", 1);
    s_cfg.ffb_gain_percent = di_percent(path, "ffb", "device_gain_percent", 100, 100);
    s_cfg.ffb_strength_percent = di_percent(path, "ffb", "strength_percent", 100, 200);
    /* The cabinet's motor wiring; `normal` (ffb_invert set) suits every wheel
     * measured. */
    ini_read_scoped(path, "ffb", "direction", "normal", text, sizeof text);
    s_cfg.ffb_invert = _stricmp(text, "reversed") != 0;
    if (s_cfg.ffb_invert && _stricmp(text, "normal"))
        LOGW("io: unknown [ffb] direction=%s; using normal", text);
    s_cfg.autocenter = di_bool(path, "ffb", "autocenter", 0);
    /* ONE READER OF THIS KEY: io_host.c traces XInput and WinMM on the same
     * switch, and three reads of one key is three places for it to drift.
     * io_config_load() runs before di_preload(). */
    s_cfg.trace = io_config_input_trace();
}

/* --- device selection --------------------------------------------------- */

static BOOL CALLBACK enum_device_cb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    di_device_t *d;
    char name[MAX_PATH];

    (void)ctx;
    if (s_device_count >= DI_MAX_DEVICES)
        return DIENUM_STOP;
    d = &s_devices[s_device_count++];
    d->guid = inst->guidInstance;
    /* Trimmed, because the ini hands values back without surrounding blanks
     * ("Generic   USB  Joystick  "). input_probe.c does the same. */
    snprintf(name, sizeof name, "%s", inst->tszProductName);
    snprintf(d->name, sizeof d->name, "%s", ini_trim(name));
    ini_device_id(d->id, sizeof d->id, d->name, &inst->guidProduct,
                  &inst->guidInstance);
    d->product = inst->guidProduct.Data1;
    d->ffb = 0;                       /* set by the second, filtered pass */
    return DIENUM_CONTINUE;
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

/* An XInput pad is read through XInput, not DirectInput (MAME's winhybrid
 * rule, xinput_hid.h): as the default device when no wheel is chosen, or as an
 * `xinput:` slot. Runs after the force-feedback pass, which the rule needs, and
 * before anything resolves. */
static void hide_xinput_devices(void)
{
    xinput_hid_t hid[XINPUT_HID_MAX];
    unsigned n = xinput_hid_list(hid, XINPUT_HID_MAX), i, k = 0;

    for (i = 0; i < s_device_count; i++) {
        if (xinput_hid_hides(hid, n, s_devices[i].product,
                             s_devices[i].ffb)) {
            LOGI("io: \"%s\" is an XInput pad (its HID path holds IG_), so "
                 "it is read through XInput and not listed here",
                 s_devices[i].name);
            continue;
        }
        s_devices[k++] = s_devices[i];
    }
    s_device_count = k;
}

static void resolve_table(void)
{
    ini_device_id_t ids[DI_MAX_DEVICES];
    const char *value[INI_DEVICES_MAX];
    char name_value[INI_MATCH_CAP + 16];
    unsigned i, total;

    for (i = 0; i < s_device_count; i++) {
        ids[i].id = s_devices[i].id;
        ids[i].name = s_devices[i].name;
        ids[i].index = i;
    }
    total = ini_devices_read(s_ini_path, s_line, INI_DEVICES_MAX);
    s_line_count = total > INI_DEVICES_MAX ? INI_DEVICES_MAX : total;
    if (total > INI_DEVICES_MAX)
        LOGW("io: [devices] has %u lines and this host reads the first %d; "
             "the rest open nothing and their bindings read as unbound.",
             total, INI_DEVICES_MAX);
    for (i = 0; i < s_line_count; i++)
        value[i] = s_line[i].value;
    snprintf(name_value, sizeof name_value, "dinput:%s", s_cfg.device_name);
    s_name_dev = ini_devices_resolve(value, s_line_count,
                                     s_cfg.device_name[0] ? name_value : NULL,
                                     "dinput", ids, s_device_count, s_line_dev);
}

/* The label of the pin holding the first device `value` matches, or NULL: a
 * pinned controller answers to its pins alone, so the log names the pin. */
static const char *pinned_by(const char *value)
{
    unsigned d, i;

    if (ini_device_is_pin(value))
        return NULL;
    for (d = 0; d < s_device_count; d++) {
        if (!ini_device_matches(value, "dinput", s_devices[d].id, d))
            continue;
        for (i = 0; i < s_line_count; i++)
            if (s_line_dev[i] == (int)d && ini_device_is_pin(s_line[i].value))
                return s_line[i].label;
        return NULL;
    }
    return NULL;
}

/* The first [devices] label whose line opens `device`, or NULL. */
static const char *label_of(unsigned device)
{
    unsigned i;

    for (i = 0; i < s_line_count; i++)
        if (s_line_dev[i] == (int)device)
            return s_line[i].label;
    return NULL;
}

/* A [devices] entry opens a device; it never chooses the primary one. The
 * table may list a pad DirectInput cannot read, and a primary that answers
 * zeroes would stop every later provider. A device without effects is chosen
 * with dinput_name. */

/* The primary: dinput_name, else the first device publishing effects. The
 * refusal at the end is load-bearing; see its comment. */
static int pick_device(void)
{
    unsigned i;

    if (!s_device_count)
        return -1;
    if (s_cfg.device_name[0]) {
        char value[INI_MATCH_CAP + 16];
        const char *pin;

        if (s_name_dev >= 0)
            return s_name_dev;
        snprintf(value, sizeof value, "dinput:%s", s_cfg.device_name);
        if ((pin = pinned_by(value)) != NULL)
            LOGW("io: [analog] dinput_name=%s names a controller that [devices] "
                 "%s pins by its device id, and a pinned controller answers "
                 "to its pins alone. Set dinput_name to that line's value.",
                 s_cfg.device_name, pin);
        else
            LOGW("io: [analog] dinput_name=%s matches no attached controller",
                 s_cfg.device_name);
        return -1;
    }
    for (i = 0; i < s_device_count; i++)
        if (s_devices[i].ffb)
            return (int)i;
    /* NOT "THE FIRST ONE". Returning 0 claims whatever enumerated first, and
     * an XInput pad enumerates here while publishing no effects, then
     * sample_dinput() succeeds, sample_host_device() never reaches XInput, and
     * a pad-only machine gets no rumble and two triggers summed onto one axis.
     * Refusing is what makes io_host.c's `auto` order mean what it says.
     *
     * AND WHAT HAPPENS NEXT IS THE PROVIDER'S ANSWER, NOT THIS FUNCTION'S.
     * Under `auto` the refusal hands the run to XInput. Under an EXPLICIT
     * `provider = dinput` there is no next backend, because
     * sample_host_device() calls sample_dinput() and nothing else, so the
     * message must not promise a fall-through. */
    if (io_config()->provider == PROVIDER_DINPUT)
        LOGW("io: no attached DirectInput device publishes force-feedback "
             "effects, so none is claimed, and [analog] provider = dinput asks "
             "for DirectInput alone: nothing else answers, so this run has no "
             "analogue controller. Set [analog] dinput_name to use one anyway, "
             "or provider = auto to fall through to XInput.");
    else
        LOGI("io: no attached DirectInput device publishes force-feedback "
             "effects, so none is claimed; XInput or WinMM will answer "
             "instead. Set [analog] dinput_name to use one anyway.");
    return -1;
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

/* Which member of DIJOYSTATE2 is at a data-format offset. */
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
    di_slot_t *slot = (di_slot_t *)ctx;
    LPDIRECTINPUTDEVICE8A dev = slot->dev;
    DWORD canon = di_canonical_offset(&obj->guidType, &slot->slider_seen);
    di_axis_t id = axis_from_offset(canon);

    if (obj->dwFlags & DIDOI_FFACTUATOR) {
        slot->ffb_count++;
        if (slot->ffb_ofs == 0xFFFFFFFFu) {
            slot->ffb_ofs = obj->dwOfs;
            slot->ffb_type = obj->dwType;
            slot->ffb_canon = canon;
        } else if (slot->ffb2_ofs == 0xFFFFFFFFu) {
            slot->ffb2_ofs = obj->dwOfs;
            slot->ffb2_type = obj->dwType;
            slot->ffb2_canon = canon;
        }
    }

    memset(&range, 0, sizeof range);
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = obj->dwType;
    range.lMin = -DI_RANGE;
    range.lMax = DI_RANGE;
    IDirectInputDevice8_SetProperty(dev, DIPROP_RANGE, &range.diph);

    /* READ IT BACK: the request above is not a guarantee, and every reading is
     * divided by the answer. Asking for -10000..10000 and dividing by 10000
     * regardless is how an axis comes out at a fraction of its travel. */
    {
        di_axis_info_t info;

        info.present = 1;
        info.lmin = -DI_RANGE;
        info.lmax = DI_RANGE;
        memset(&range, 0, sizeof range);
        range.diph.dwSize = sizeof range;
        range.diph.dwHeaderSize = sizeof range.diph;
        range.diph.dwHow = DIPH_BYID;
        range.diph.dwObj = obj->dwType;
        if (SUCCEEDED(IDirectInputDevice8_GetProperty(dev, DIPROP_RANGE,
                                                      &range.diph)) &&
            range.lMax > range.lMin) {
            info.lmin = range.lMin;
            info.lmax = range.lMax;
        }
        /* Set and read back by object id: the range belongs to the
         * object. */
        if (id != DI_AXIS_NONE && !slot->axis[id].present)
            slot->axis[id] = info;
    }

    /* Disable driver deadzone and retain full saturation. Host deadzone is
     * applied later in normalised coordinates. */
    axis_dword_property(dev, obj->dwType, DIPROP_DEADZONE, 0);
    axis_dword_property(dev, obj->dwType, DIPROP_SATURATION, 10000);
    return DIENUM_CONTINUE;
}


/* The role's value as the file spells it: `rz`, or `twcs:z`. */
static void role_text(const di_role_t *role, char *out, size_t size)
{
    if (role->label[0] && role->axis != DI_AXIS_NONE)
        snprintf(out, size, "%s:%s", role->label, di_axis_name(role->axis));
    else
        snprintf(out, size, "%s", di_axis_name(role->axis));
}

/* A role whose label opens nothing reads released; say which of the four
 * reasons applies. */
static void report_unopened_role(unsigned r)
{
    const di_role_t *role = &s_roles.role[r];
    char text[INI_LABEL_CAP + 16], provider[16], match[INI_MATCH_CAP];
    unsigned i;

    role_text(role, text, sizeof text);
    for (i = 0; i < s_line_count; i++)
        if (!_stricmp(s_line[i].label, role->label))
            break;
    if (i == s_line_count)
        LOGW("io: [analog] %s = %s names \"%s\", and [devices] has no `%s =` "
             "line, so the %s reads released.", di_role_desc[r].axis_key, text,
             role->label, role->label, di_role_desc[r].what);
    else if (!ini_device_entry(s_line[i].value, provider, sizeof provider,
                               match, sizeof match) ||
             _stricmp(provider, "dinput") != 0)
        LOGW("io: [analog] %s = %s names [devices] %s = %s, and only a "
             "DirectInput controller or an XInput pad (xinput:0..3) can be "
             "read beside the wheel, so the %s reads released.",
             di_role_desc[r].axis_key, text, role->label, s_line[i].value,
             di_role_desc[r].what);
    else if (s_line_dev[i] < 0)
        LOGW("io: [analog] %s = %s names [devices] %s, which opens no "
             "attached controller (the [devices] lines above say why), so the "
             "%s reads released.", di_role_desc[r].axis_key, text, role->label,
             di_role_desc[r].what);
    else
        LOGW("io: [analog] %s = %s names [devices] %s, \"%s\", which could "
             "not be held open (see above), so the %s reads released.",
             di_role_desc[r].axis_key, text, role->label,
             s_devices[s_line_dev[i]].name, di_role_desc[r].what);
}

/* An axis the device does not have is reported and dropped, never read as a
 * constant centre: with separate pedals that is a permanent half-brake. */
static void validate_role(unsigned r)
{
    di_role_t *role = &s_roles.role[r];
    const di_slot_t *slot = &s_slot[role->slot];
    char text[INI_LABEL_CAP + 16];

    if (slot->axis[role->axis].present)
        return;
    role_text(role, text, sizeof text);
    LOGW("io: [analog] %s = %s, but \"%s\" has no axis whose guidType is "
         "that: ignoring it. Run with [diagnostics] input_trace = true and use "
         "the axis that moves.", di_role_desc[r].axis_key, text, slot->name);
    role->axis = DI_AXIS_NONE;
}

/* What one controller's axes are, said once per controller a role reads. */
static void report_slot_axes(unsigned s)
{
    const di_slot_t *slot = &s_slot[s];
    const di_axis_info_t *table = slot->axis;
    unsigned i;
    /* 320, NOT 128, AND CLAMPED BELOW. Eight axes at the standard range need
     * 149 characters, so this list was TRUNCATED on every wheel that reports
     * all eight, silently, mid-entry, with a trailing comma that reads like
     * the end of a list: a report read the missing `slider1` as
     * "claimed and carries nothing" when the log had simply run out of buffer.
     * 320 covers eight entries even at a native range a driver that refused
     * DIPROP_RANGE would report (two 11-digit bounds). */
    char have[320], who[INI_LABEL_CAP + 32];
    int n = 0;

    have[0] = '\0';
    for (i = DI_AXIS_X; i < DI_AXIS_COUNT; i++)
        if (table[i].present) {
            n += snprintf(have + n, sizeof have - (size_t)n, "%s%s %ld..%ld",
                          n ? ", " : "", di_axis_name((di_axis_t)i),
                          (long)table[i].lmin, (long)table[i].lmax);
            /* snprintf RETURNS WHAT IT WOULD HAVE WRITTEN: unclamped, one
             * truncating call sends `n` past the buffer and the next
             * underflows `sizeof have - n` to near SIZE_MAX. */
            if (n >= (int)sizeof have)
                n = (int)sizeof have - 1;
        }
    /* The ranges are divided by, so they are logged: anything but
     * -10000..10000 is the first suspect. The input regression suite reads
     * slot 0's line. */
    if (s == 0)
        snprintf(who, sizeof who, "wheel");
    else
        snprintf(who, sizeof who, "slot %u \"%s\"", s, slot->label);
    LOGI("io: %s axes readable: %s", who, n ? have : "none");
}

/* Every role against its own controller, then one line saying where each is
 * read from. --dry-run reaches this. */
static void validate_roles(void)
{
    char line[3 * (INI_LABEL_CAP + MAX_PATH + 48)];
    unsigned r, s;
    int n = 0;

    for (s = 0; s < s_slot_count; s++)
        if (s_slot[s].axes)
            report_slot_axes(s);
    for (r = 0; r < DI_ROLE_COUNT; r++) {
        const di_role_t *role = &s_roles.role[r];

        if (role->axis == DI_AXIS_NONE)
            continue;
        if (role->slot < 0) {
            report_unopened_role(r);
            continue;
        }
        validate_role(r);
    }

    line[0] = '\0';
    for (r = 0; r < DI_ROLE_COUNT; r++) {
        const di_role_t *role = &s_roles.role[r];
        char text[INI_LABEL_CAP + 16];

        role_text(role, text, sizeof text);
        if (role->axis == DI_AXIS_NONE)
            n += snprintf(line + n, sizeof line - (size_t)n, "%s%s none",
                          n ? ", " : "", di_role_desc[r].what);
        else if (role->slot < 0)
            n += snprintf(line + n, sizeof line - (size_t)n,
                          "%s%s %s (not open: reads released)",
                          n ? ", " : "", di_role_desc[r].what, text);
        else
            n += snprintf(line + n, sizeof line - (size_t)n,
                          "%s%s %s on \"%s\"", n ? ", " : "", di_role_desc[r].what,
                          text, s_slot[role->slot].name);
        if (n >= (int)sizeof line)
            n = (int)sizeof line - 1;
    }
    LOGI("io: axis roles: %s", line);

    if (s_roles.role[DI_ROLE_STEER].axis == DI_AXIS_NONE)
        LOGW("io: no steering axis; the wheel will not steer");
    if (s_roles.pedals_separate && !di_roles_separate(&s_roles) &&
        s_roles.role[DI_ROLE_THROTTLE].axis != DI_AXIS_NONE)
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

    if (s_slot[0].acquired)
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

/* One constant force on one axis, addressed the way the caller asks. */
static HRESULT try_constant_force(DWORD axis, int by_id)
{
    DIEFFECT eff;
    DICONSTANTFORCE cf;
    LONG direction = 0;

    memset(&cf, 0, sizeof cf);
    cf.lMagnitude = 0;

    memset(&eff, 0, sizeof eff);
    eff.dwSize = sizeof eff;
    eff.dwFlags = DIEFF_CARTESIAN |
                  (by_id ? DIEFF_OBJECTIDS : DIEFF_OBJECTOFFSETS);
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

    s_force = NULL;
    return IDirectInputDevice8_CreateEffect(s_dev, &GUID_ConstantForce, &eff,
                                            &s_force, NULL);
}

/* THE MOTOR IS ON THE ACTUATOR, NOT ON THE CONFIGURED STEERING AXIS. Building
 * the effect on di_axis_offset(s_cfg.steer) means that on a wheel with no `x`
 * the steering resolves to none and the default branch returns DIJOFS_X, so
 * correcting a user's steering configuration would break their force feedback.
 *
 * The order below is measured: on a G29, G923 and Pro DD the actuator's real
 * offset is refused with DIERR_INVALIDPARAM and its canonical offset accepted
 * and felt; on a TMX the two are the same number. By id last, because it is
 * accepted everywhere but is the mode no reference implementation uses.
 * WHICH ONE WORKED IS LOGGED: it cannot be recovered afterwards. */
static int create_effects(void)
{
    /* EACH RUNG CARRIES ITS OWN NAME. Deriving the label from the axis number
     * stopped being true once a second actuator could contribute a rung, and a
     * log line naming the wrong addressing is worse than none. */
    struct { DWORD axis; int by_id; const char *how; } rung[5];
    const di_slot_t *w = &s_slot[0];
    int n = 0, i;
    HRESULT hr = E_FAIL;

    if (w->ffb_canon != 0xFFFFFFFFu) {
        rung[n].axis = w->ffb_canon; rung[n].by_id = 0;
        rung[n++].how = "canonical offset (guidType)";
    }
    if (w->ffb_ofs != 0xFFFFFFFFu && w->ffb_ofs != w->ffb_canon) {
        rung[n].axis = w->ffb_ofs; rung[n].by_id = 0;
        rung[n++].how = "data-format offset";
    }
    if (w->ffb_ofs != 0xFFFFFFFFu) {
        rung[n].axis = w->ffb_type; rung[n].by_id = 1;
        rung[n++].how = "object id";
    }
    /* A SECOND ACTUATOR IS A SECOND CHANCE, NOT A SECOND EFFECT. Only reached
     * when all three addressings of the first were refused, which on a
     * multi-actuator wheel is what "we picked the wrong one" looks like. */
    if (w->ffb2_canon != 0xFFFFFFFFu) {
        rung[n].axis = w->ffb2_canon; rung[n].by_id = 0;
        rung[n++].how = "SECOND actuator, canonical offset";
    }
    if (w->ffb2_ofs != 0xFFFFFFFFu && w->ffb2_ofs != w->ffb2_canon) {
        rung[n].axis = w->ffb2_ofs; rung[n].by_id = 0;
        rung[n++].how = "SECOND actuator, data-format offset";
    }
    if (!n) {
        /* The device publishes effects but flags no actuator axis. DIJOFS_X is
         * a guess and is logged as one. */
        LOGW("io: \"%s\" publishes force feedback but flags no actuator axis; "
             "trying the x axis, which is a guess", s_name);
        rung[n].axis = DIJOFS_X; rung[n].by_id = 0;
        rung[n++].how = "x axis, a guess";
    }
    if (w->ffb_count > 1)
        LOGI("io: \"%s\" flags %u force-feedback actuators; the first is used "
             "and the others are the fallback", s_name, w->ffb_count);

    for (i = 0; i < n; i++) {
        hr = try_constant_force(rung[i].axis, rung[i].by_id);
        if (SUCCEEDED(hr) && s_force) {
            LOGI("io: constant force on the %s (0x%08lX)",
                 rung[i].how, (unsigned long)rung[i].axis);
            IDirectInputEffect_Start(s_force, 1, 0);
            return 1;
        }
        LOGI("io: the wheel refused a constant force on the %s (0x%08lX, "
             "hr=0x%08lX)",
             rung[i].how, (unsigned long)rung[i].axis, (unsigned long)hr);
    }
    LOGW("io: the wheel accepted no constant-force effect (last hr=0x%08lX); "
         "input works, force feedback does not", (unsigned long)hr);
    s_force = NULL;
    return 0;
}

/* --- open / close ------------------------------------------------------- */

static void report_devices(int chosen)
{
    unsigned i, j, dup = 0;

    for (i = 0; i < s_device_count; i++) {
        LOGI("io: dinput device %u: \"%s\"%s%s", i,
             s_devices[i].name, s_devices[i].ffb ? " [force feedback]" : "",
             (int)i == chosen ? "  <- selected" : "");
        /* The string a [devices] line is matched against. */
        LOGI("io:   device id: %s", s_devices[i].id);
    }

    /* TWO DEVICES WITH ONE NAME IS USUALLY ONE PIECE OF HARDWARE. A Fanatec
     * CSL DD presents two HID collections and DirectInput enumerates each as
     * its own device: same product name, same VID/PID, both publishing force
     * feedback. We open one, and which one is enumeration order. */
    for (i = 0; i < s_device_count && !dup; i++)
        for (j = i + 1; j < s_device_count; j++)
            if (!strcmp(s_devices[i].name, s_devices[j].name)) {
                LOGW("io: devices %u and %u are both \"%s\", one piece of "
                     "hardware presenting two HID collections. Only one is "
                     "opened, and if it is the wrong half its axes or its "
                     "motor will be missing. [analog] dinput_name set to the "
                     "other's device id picks it.", i, j, s_devices[i].name);
                dup = 1;
                break;
            }
}

/* --- the extra devices -------------------------------------------------- */

/* Open one extra device read-only: non-exclusive, no effects, autocenter left
 * alone. Not acquired here, because DIPROP_RANGE is refused on an acquired
 * device and the roles are resolved after every slot is open. A failure only
 * leaves that device's bindings unbound. */
static int open_slot(unsigned device, const char *label)
{
    LPDIRECTINPUTDEVICE8A dev = NULL;
    di_slot_t *slot;
    HRESULT hr;

    if (s_slot_count >= IO_MAX_SLOTS) {
        LOGW("io: [devices] %s = \"%s\" is past the %d devices this host can "
             "hold open at once, so it is not opened and any binding naming it "
             "does nothing. Remove an entry above it.",
             label, s_devices[device].name, IO_MAX_SLOTS);
        return 0;
    }
    hr = IDirectInput8_CreateDevice(s_di, &s_devices[device].guid, &dev, NULL);
    if (FAILED(hr) || !dev) {
        LOGW("io: CreateDevice(\"%s\") for [devices] %s failed 0x%08lX",
             s_devices[device].name, label, (unsigned long)hr);
        return 0;
    }
    IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2);
    /* NON-EXCLUSIVE AND BACKGROUND, on this module's own popup rather than the
     * console's: GetConsoleWindow() under Windows Terminal belongs to another
     * process, SetCooperativeLevel is granted anyway and Acquire then fails
     * with ERROR_INVALID_WINDOW_HANDLE. */
    hr = IDirectInputDevice8_SetCooperativeLevel(
             dev, s_window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr)) {
        LOGW("io: \"%s\" refused shared access (0x%08lX); its buttons are not "
             "available", s_devices[device].name, (unsigned long)hr);
        IDirectInputDevice8_Release(dev);
        return 0;
    }
    slot = &s_slot[s_slot_count];
    memset(slot, 0, sizeof *slot);
    slot->dev = dev;
    slot->device = device;
    slot->xinput = -1;
    snprintf(slot->label, sizeof slot->label, "%s", label);
    snprintf(slot->name, sizeof slot->name, "%s", s_devices[device].name);
    s_slot_count++;
    return 1;
}

/* Everything enum_axis_cb() fills, back to its initial value. slider_seen
 * matters most: a stale count sends the next device's first slider to
 * slider1. */
static void slot_reset_axes(di_slot_t *slot)
{
    memset(slot->axis, 0, sizeof slot->axis);
    slot->slider_seen = 0;
    slot->ffb_ofs = slot->ffb_canon = 0xFFFFFFFFu;
    slot->ffb2_ofs = slot->ffb2_canon = 0xFFFFFFFFu;
    slot->ffb_type = slot->ffb2_type = 0;
    slot->ffb_count = 0;
}

/* Ask for, read back and record the range of every axis on one controller.
 * Before Acquire. */
static void enumerate_axes(unsigned s)
{
    di_slot_t *slot = &s_slot[s];

    slot_reset_axes(slot);
    IDirectInputDevice8_EnumObjects(slot->dev, enum_axis_cb, slot, DIDFT_AXIS);
    slot->axes = 1;
}

/* Acquire the extra slots; an unacquired device reads every button as
 * released, so a failure is logged and retried while running. */
static void acquire_extra_slots(void)
{
    unsigned s;

    for (s = 1; s < s_slot_count; s++) {
        di_slot_t *slot = &s_slot[s];

        if (slot->xinput >= 0) {
            LOGI("io: slot %u is XInput pad %d as [devices] %s, its buttons "
                 "and any axis [analog] names on it", s, slot->xinput,
                 slot->label);
            continue;
        }
        slot->acquired = SUCCEEDED(IDirectInputDevice8_Acquire(slot->dev));
        LOGI("io: slot %u is \"%s\" as [devices] %s, read-only, %s%s", s,
             slot->name, slot->label,
             slot->axes ? "its buttons and the axes [analog] names on it"
                        : "buttons only",
             slot->acquired ? "" : " (not acquired yet; retried while running)");
    }
}

/* The label a slot's profile is read under, for di_roles_load(). */
static const char *slot_scope(unsigned slot)
{
    return slot < s_slot_count ? s_slot[slot].label : "";
}

static int slot_is_xinput(unsigned slot)
{
    return slot < s_slot_count && s_slot[slot].xinput >= 0;
}

/* Name `slot` `label` as well, when it does not already answer to it. */
static void add_alias(unsigned slot, const char *label)
{
    unsigned i;

    if (!_stricmp(s_slot[slot].label, label))
        return;
    for (i = 0; i < s_alias_count; i++)
        if (!_stricmp(s_alias[i].label, label))
            return;
    if (s_alias_count >= DI_MAX_ALIASES) {
        LOGW("io: [devices] %s names \"%s\", already open as %s, but this host "
             "keeps at most %d second names; a binding that names %s: reads as "
             "unbound. Bind it as %s: instead.",
             label, s_slot[slot].name, s_slot[slot].label[0] ?
             s_slot[slot].label : "the primary", DI_MAX_ALIASES, label,
             s_slot[slot].label[0] ? s_slot[slot].label : "the bare number");
        return;
    }
    snprintf(s_alias[s_alias_count].label, sizeof s_alias[0].label, "%s",
             label);
    s_alias[s_alias_count].slot = slot;
    s_alias_count++;
    LOGI("io: [devices] %s names \"%s\", which is already open as slot %u%s%s; "
         "a binding that names %s: reaches it too",
         label, s_slot[slot].name, slot, s_slot[slot].label[0] ? " " : "",
         s_slot[slot].label, label);
}

/* An `xinput:<n>` line as a slot: XInput's six axes (`pad:rt`) and ten
 * buttons (1 = A). A second line for the same pad is an alias. Opened whether
 * or not the pad answers now; it is read from its first answer. */
static void open_xinput_line(const char *label, const char *value)
{
    unsigned user, i;
    di_slot_t *slot;

    for (user = 0; user < 4; user++)
        if (ini_device_matches(value, "xinput", "", user))
            break;
    if (user == 4) {
        LOGW("io: [devices] %s = %s is not an XInput pad number (0..3), so it "
             "opens nothing.", label, value);
        return;
    }
    for (i = 1; i < s_slot_count; i++)
        if (s_slot[i].xinput == (int)user) {
            add_alias(i, label);
            return;
        }
    if (s_slot_count >= IO_MAX_SLOTS) {
        LOGW("io: [devices] %s = %s is past the %d devices this host can hold "
             "open at once, so it is not opened and any binding naming it does "
             "nothing. Remove an entry above it.", label, value, IO_MAX_SLOTS);
        return;
    }
    slot = &s_slot[s_slot_count++];
    memset(slot, 0, sizeof *slot);
    slot->xinput = (int)user;
    slot->axes = 1;
    snprintf(slot->label, sizeof slot->label, "%s", label);
    snprintf(slot->name, sizeof slot->name, "XInput pad %u", user);
    for (i = DI_AXIS_LX; i <= DI_AXIS_RT; i++)
        slot->axis[i] = (di_axis_info_t){ 1, -1, 1 };
}

/* Every [devices] entry after the primary, in the table's own order. */
static void open_extra_slots(unsigned primary)
{
    unsigned e;

    for (e = 0; e < s_line_count; e++) {
        const char *label = s_line[e].label, *value = s_line[e].value;
        char provider[16], match[INI_MATCH_CAP];
        int found = s_line_dev[e];
        unsigned i;

        if (!ini_device_entry(value, provider, sizeof provider,
                              match, sizeof match))
            continue;            /* io_config.c's report_device_profiles() */
        /* An XInput pad is a slot of its own. WinMM lines are names only:
         * WinMM sees the same devices DirectInput does. */
        if (!_stricmp(provider, "xinput")) {
            open_xinput_line(label, value);
            continue;
        }
        if (_stricmp(provider, "dinput") != 0)
            continue;
        if (found < 0) {
            const char *pin = pinned_by(value);

            if (pin)
                LOGW("io: [devices] %s = %s names a controller that %s pins by "
                     "its device id, and a pinned controller answers to its "
                     "pins alone, so %s opens nothing. Bind it as %s: instead, "
                     "or remove one of the two lines.",
                     label, value, pin, label, pin);
            else
                LOGI("io: [devices] %s = %s matches nothing attached, so it "
                     "is not opened; a binding that names %s: reads as "
                     "unbound.", label, value, label);
            continue;
        }
        if ((unsigned)found == primary) {
            add_alias(0, label);  /* it is slot 0 already */
            continue;
        }
        for (i = 1; i < s_slot_count; i++)
            if (s_slot[i].xinput < 0 && s_slot[i].device == (unsigned)found)
                break;
        if (i < s_slot_count) {
            add_alias(i, label);
            continue;
        }
        open_slot((unsigned)found, label);
    }
}

int di_slot_for_label(const char *label)
{
    unsigned i;

    if (!label || !*label)
        return 0;                /* no label means the primary */
    for (i = 0; i < s_slot_count; i++)
        if (!_stricmp(s_slot[i].label, label))
            return (int)i;
    for (i = 0; i < s_alias_count; i++)
        if (!_stricmp(s_alias[i].label, label))
            return (int)s_alias[i].slot;
    return -1;
}

unsigned di_slot_count(void)
{
    return s_slot_count;
}

const char *di_slot_name(unsigned slot)
{
    return slot < s_slot_count ? s_slot[slot].name : "";
}

/* The map as it will be used: the pedal mode as di_roles_separate() decides
 * it. The input regression suite reads `steer=.. (.. pedals)`. */
static void report_axis_map(void)
{
    const di_role_t *st = &s_roles.role[DI_ROLE_STEER];
    char text[DI_ROLE_COUNT][INI_LABEL_CAP + 16];
    unsigned r;

    for (r = 0; r < DI_ROLE_COUNT; r++)
        role_text(&s_roles.role[r], text[r], sizeof text[r]);
    if (st->range_percent < 100)
        LOGI("io: the middle %u%% of the wheel's travel covers full lock "
             "(the cabinet's wheel turned about 270 degrees; a modern one "
             "defaults to 900, which stretches the game's own dead band with "
             "everything else)", st->range_percent);
    LOGI("io: wheel axes steer=%s throttle=%s brake=%s (%s pedals), "
         "deadzone steering %u%% accelerator %u%% brake %u%%, "
         "ffb %s gain %u%% strength %u%%%s",
         text[DI_ROLE_STEER], text[DI_ROLE_THROTTLE], text[DI_ROLE_BRAKE],
         di_roles_separate(&s_roles) ? "separate" : "combined",
         st->deadzone_percent,
         s_roles.role[DI_ROLE_THROTTLE].deadzone_percent,
         s_roles.role[DI_ROLE_BRAKE].deadzone_percent,
         s_force ? "on" : "off", s_cfg.ffb_gain_percent,
         s_cfg.ffb_strength_percent,
         s_cfg.ffb_invert ? "" : " direction reversed");
}

int di_preload(const char *ini)
{
    HRESULT hr;
    HMODULE self = NULL;
    DIDEVCAPS caps;
    int chosen;
    char ini_path[MAX_PATH];

    /* ABSOLUTE FIRST: the caller passes the bare name "vcthunder.ini", and the
     * profile API resolves a bare name against the WINDOWS directory (ini.h).
     * Every key this module reads was therefore taking its compiled default
     * while the file said otherwise, silently. */
    ini_resolve_path(ini, ini_path, sizeof ini_path);
    snprintf(s_ini_path, sizeof s_ini_path, "%s", ini_path);
    di_load_ini(ini_path);

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

    /* The slots go with the handles they hold: di_preload() may run twice
     * across a device change. */
    memset(s_slot, 0, sizeof s_slot);
    s_slot_count = 0;
    s_alias_count = 0;
    s_device_count = 0;
    IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_device_cb,
                              NULL, DIEDFL_ATTACHEDONLY);
    IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, enum_ffb_cb,
                              NULL, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    hide_xinput_devices();
    resolve_table();
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

    /* Slot 0 is the default device; its `dev` aliases s_dev. Its label is the
     * profile the role keys are read from. */
    s_slot[0].dev = s_dev;
    s_slot[0].device = (unsigned)chosen;
    s_slot[0].xinput = -1;
    snprintf(s_slot[0].name, sizeof s_slot[0].name, "%s", s_name);
    s_slot_count = 1;

    if (label_of((unsigned)chosen)) {
        snprintf(s_profile, sizeof s_profile, "%s",
                 label_of((unsigned)chosen));
        snprintf(s_slot[0].label, sizeof s_slot[0].label, "%s", s_profile);
        /* Names the LABEL and not only the section, because the section may
         * not exist: a [devices] entry with no [analog.<label>] beside it is
         * exactly the misspelling this line helps a reader catch. */
        LOGI("io: this controller is \"%s\" in [devices], so [analog.%s] is "
             "read before [analog]", s_profile, s_profile);
    } else {
        s_profile[0] = '\0';
    }

    s_have_ffb = s_devices[chosen].ffb;
    s_publishes_ffb = s_devices[chosen].ffb;

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

    /* Always, whatever the roles say: create_effects() needs the actuator
     * found here. */
    enumerate_axes(0);

    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    IDirectInputDevice8_GetCapabilities(s_dev, &caps);
    /* THE FF DRIVER VERSION IS THE FACT THAT SEPARATED THE TWO HALVES of that
     * Fanatec: the working half reports 0x8253 and creates effects, the other
     * reports 0 and refuses every one with REGDB_E_CLASSNOTREG. Both are listed
     * by DIEDFL_FORCEFEEDBACK, so "publishes effects" does not distinguish
     * them. */
    LOGI("io: wheel \"%s\": %lu axes, %lu buttons, %lu POV%s, ff driver 0x%lX",
         s_name, (unsigned long)caps.dwAxes, (unsigned long)caps.dwButtons,
         (unsigned long)caps.dwPOVs,
         (caps.dwFlags & DIDC_FORCEFEEDBACK) ? ", force feedback" : "",
         (unsigned long)caps.dwFFDriverVersion);
    if (s_have_ffb && caps.dwFFDriverVersion == 0) {
        unsigned k, others = 0;
        for (k = 0; k < s_device_count; k++)
            if (s_devices[k].ffb && (int)k != chosen)
                others++;
        LOGW("io: \"%s\" was chosen because it publishes force-feedback "
             "effects, but it has NO force-feedback driver (version 0). "
             "Effects on it are expected to be refused.%s", s_name,
             others ? " Another attached device also publishes effects; "
                      "[analog] dinput_name set to its device id selects it." :
                      "");
    }

    if (s_have_ffb && s_cfg.ffb_enabled) {
        /* Autocenter while unacquired, which is its documented requirement;
         * take_autocenter() repeats it after Acquire and reads it back. */
        take_autocenter(s_cfg.autocenter);
        if (SUCCEEDED(IDirectInputDevice8_Acquire(s_dev)))
            s_slot[0].acquired = 1;
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

    /* Last, once the owner window exists: the extra devices, then the roles
     * (which need every slot open), then the axes of any slot a role reads
     * (which must not be acquired yet). */
    open_extra_slots((unsigned)chosen);
    di_roles_load(&s_roles, s_ini_path, di_slot_for_label, slot_scope,
                  slot_is_xinput);
    {
        unsigned r;

        for (r = 0; r < DI_ROLE_COUNT; r++) {
            const di_role_t *role = &s_roles.role[r];

            if (role->axis != DI_AXIS_NONE && role->slot > 0 &&
                !s_slot[role->slot].axes)
                enumerate_axes((unsigned)role->slot);
        }
    }
    validate_roles();
    report_axis_map();
    acquire_extra_slots();
    return 1;
}

int di_present(void)
{
    return s_dev != NULL;
}

/* The only wheel/pad signal this host has, and it is a HEURISTIC: a device that
 * publishes effects is a wheel far more often than not, but force feedback is a
 * capability and not a type. It decides a DEFAULT the user can override, and
 * io_config_pov_digital() logs what it decided, which is what makes a
 * heuristic allowable here. */
int di_publishes_ffb(void)
{
    return s_dev != NULL && s_publishes_ffb;
}

const char *di_device_name(void)
{
    return s_name;
}

void di_close(void)
{
    unsigned s;

    /* THE EXTRA SLOTS FIRST, and slot 0 is skipped rather than released here:
     * its handle aliases s_dev, which the block below releases along with the
     * autocenter and effect it owns. Releasing twice is a double free of a COM
     * object and takes the process down on exit. */
    for (s = 1; s < s_slot_count; s++) {
        if (!s_slot[s].dev)
            continue;
        IDirectInputDevice8_Unacquire(s_slot[s].dev);
        IDirectInputDevice8_Release(s_slot[s].dev);
        s_slot[s].dev = NULL;
    }
    memset(s_slot, 0, sizeof s_slot);
    s_slot_count = 0;
    s_alias_count = 0;

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
}

/* --- reading ------------------------------------------------------------ */

/* An axis as -1..+1 against the range the device reports. An axis the device
 * lacks is 0, and validate_role() has already dropped any role on one. */
static float unit_axis(const di_slot_t *slot, const DIJOYSTATE2 *js,
                       di_axis_t a)
{
    const di_axis_info_t *ax = &slot->axis[a];
    float span = (float)(ax->lmax - ax->lmin), f;

    if (!ax->present || span <= 0.0f)
        return 0.0f;
    f = ((float)(di_axis_value(js, a) - ax->lmin) / span) * 2.0f - 1.0f;
    return io_clamp_unit(f);
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

/* One controller, one read, retried on its own: a lost throttle must not cost
 * the wheel its reading. A lost device is retried next poll, not logged. */
static int poll_slot(unsigned s, DIJOYSTATE2 *js)
{
    di_slot_t *slot = &s_slot[s];

    if (!slot->dev)
        return 0;
    if (!slot->acquired) {
        if (FAILED(IDirectInputDevice8_Acquire(slot->dev)))
            return 0;
        slot->acquired = 1;
        /* A re-acquired wheel has stopped its effects; restart them. */
        if (s == 0) {
            if (s_force)
                IDirectInputEffect_Start(s_force, 1, 0);
            s_last_magnitude = 0x7FFFFFFF;
        }
    }
    IDirectInputDevice8_Poll(slot->dev);
    memset(js, 0, sizeof *js);
    if (FAILED(IDirectInputDevice8_GetDeviceState(slot->dev, sizeof *js, js))) {
        slot->acquired = 0;
        return 0;
    }
    return 1;
}

/* The input trace, per slot, only when what is read changes. A slot no role
 * reads axes from is compared on its buttons alone, so floating axes on a
 * button box do not fill the log. */
static void trace_slot(unsigned s, const DIJOYSTATE2 *js,
                       const uint32_t *buttons)
{
    di_slot_t *slot = &s_slot[s];
    DIJOYSTATE2 seen = *js;
    char who[INI_LABEL_CAP + 24];

    if (!slot->axes) {
        memset(&seen, 0, sizeof seen);
        memcpy(seen.rgbButtons, js->rgbButtons, sizeof seen.rgbButtons);
    }
    if (!memcmp(&seen, &slot->last, sizeof seen))
        return;
    slot->last = seen;
    if (s == 0)
        snprintf(who, sizeof who, "dinput");
    else
        snprintf(who, sizeof who, "dinput slot %u \"%s\"", s, slot->label);
    /* All four words, high first, so a button past 32 shows. */
    if (slot->axes)
        LOGI("io: %s x=%6ld y=%6ld z=%6ld rx=%6ld ry=%6ld rz=%6ld "
             "s0=%6ld s1=%6ld pov=%ld buttons=0x%08lX%08lX%08lX%08lX", who,
             js->lX, js->lY, js->lZ, js->lRx, js->lRy, js->lRz,
             js->rglSlider[0], js->rglSlider[1], (long)js->rgdwPOV[0],
             (unsigned long)buttons[3], (unsigned long)buttons[2],
             (unsigned long)buttons[1], (unsigned long)buttons[0]);
    else
        LOGI("io: %s buttons=0x%08lX%08lX%08lX%08lX", who,
             (unsigned long)buttons[3], (unsigned long)buttons[2],
             (unsigned long)buttons[1], (unsigned long)buttons[0]);
}

/* A controller that stops answering, or comes back, is logged once per change,
 * with the roles that now read released. The first answer is logged where the
 * slot is read. */
static void report_slot_change(unsigned s, int answered)
{
    di_slot_t *slot = &s_slot[s];
    char roles[64];
    unsigned r;
    int n = 0;

    if (!slot->answered || answered == slot->live) {
        slot->live = answered;
        return;
    }
    slot->live = answered;
    roles[0] = '\0';
    for (r = 0; r < DI_ROLE_COUNT; r++)
        if (s_roles.role[r].slot == (int)s &&
            s_roles.role[r].axis != DI_AXIS_NONE) {
            n += snprintf(roles + n, sizeof roles - (size_t)n, "%s%s",
                          n ? " and " : "", di_role_desc[r].what);
            if (n >= (int)sizeof roles)
                n = (int)sizeof roles - 1;
        }
    if (!answered)
        LOGW("io: slot %u \"%s\" (%s) stopped answering%s%s%s", s,
             slot->label[0] ? slot->label : "the wheel", slot->name,
             roles[0] ? "; the " : "", roles,
             roles[0] ? " read released until it is back" : "");
    else
        LOGI("io: slot %u \"%s\" (%s) answers again", s,
             slot->label[0] ? slot->label : "the wheel", slot->name);
}

/* An XInput slot's read: six axes into the role reading, ten buttons into its
 * row, traced on change. */
static void sample_xinput_slot(unsigned s, di_slot_reading_t *reading,
                               di_reading_t *out)
{
    di_slot_t *slot = &s_slot[s];
    float axis[6];
    unsigned buttons, i;

    if (!io_host_xinput_read((unsigned)slot->xinput, axis, &buttons))
        return;
    reading->answered = 1;
    if (!slot->answered) {
        slot->answered = 1;
        slot->live = 1;
        LOGI("io: slot %u \"%s\" answered its first read; it is being "
             "polled with the wheel", s, slot->label);
    }
    for (i = 0; i < 6; i++)
        reading->unit[DI_AXIS_LX + i] = axis[i];
    for (i = 0; i < 10; i++)
        if (buttons & (1u << i))
            io_button_set(out->buttons[s], i);
    if (s_cfg.trace) {
        DIJOYSTATE2 seen;

        memset(&seen, 0, sizeof seen);
        seen.lX = (LONG)(axis[0] * 10000.0f);
        seen.lY = (LONG)(axis[1] * 10000.0f);
        seen.lRx = (LONG)(axis[2] * 10000.0f);
        seen.lRy = (LONG)(axis[3] * 10000.0f);
        seen.lZ = (LONG)(axis[4] * 10000.0f);
        seen.lRz = (LONG)(axis[5] * 10000.0f);
        seen.rgdwPOV[0] = buttons;
        if (memcmp(&seen, &slot->last, sizeof seen)) {
            slot->last = seen;
            LOGI("io: xinput slot %u \"%s\" lx=%+.2f ly=%+.2f rx=%+.2f "
                 "ry=%+.2f lt=%+.2f rt=%+.2f buttons=0x%03X", s, slot->label,
                 (double)axis[0], (double)axis[1], (double)axis[2],
                 (double)axis[3], (double)axis[4], (double)axis[5], buttons);
        }
    }
}

/* One poll reads every open slot once; each role reads its own slot's
 * reading (di_roles_sample()), and a role whose slot did not answer reads
 * released. `present` is raised only when some slot answered, so `auto` can
 * fall through to XInput when the wheel is gone. */
int di_sample(di_reading_t *out)
{
    di_slot_reading_t reading[IO_MAX_SLOTS];
    unsigned s, i, any = 0;

    memset(out, 0, sizeof *out);
    memset(reading, 0, sizeof reading);
    if (!s_dev)
        return 0;

    for (s = 0; s < s_slot_count; s++) {
        di_slot_t *slot = &s_slot[s];
        DIJOYSTATE2 js;
        unsigned a;

        if (slot->xinput >= 0) {
            sample_xinput_slot(s, &reading[s], out);
            any += reading[s].answered;
            continue;
        }
        if (!poll_slot(s, &js))
            continue;
        any++;
        reading[s].answered = 1;
        /* Answered is not the same as moving: a device returning zeroes
         * reads like one nobody touches. */
        if (!slot->answered) {
            slot->answered = 1;
            slot->live = 1;
            if (s)
                LOGI("io: slot %u \"%s\" answered its first read; it is being "
                     "polled with the wheel", s, slot->label);
        }
        if (slot->axes)
            for (a = DI_AXIS_X; a < DI_AXIS_COUNT; a++)
                reading[s].unit[a] = unit_axis(slot, &js, (di_axis_t)a);
        /* All 128: DIJOYSTATE2 carries them, and a Pro DD has 92. */
        for (i = 0; i < IO_MAX_BUTTONS; i++)
            if (js.rgbButtons[i] & 0x80)
                io_button_set(out->buttons[s], i);
        /* Only the default device's hat, so a second device's hat cannot
         * steer. */
        if (s == 0)
            pov_digital(out, js.rgdwPOV[0]);
        if (s_cfg.trace)
            trace_slot(s, &js, out->buttons[s]);
    }
    for (s = 0; s < s_slot_count; s++)
        report_slot_change(s, reading[s].answered);
    if (!any)
        return 0;
    out->present = 1;
    di_roles_sample(&s_roles, reading, s_slot_count, &out->steer, &out->drive);
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
    /* PARAMETERS ONLY, NOT DIEP_START: this runs at the board's poll rate and
     * DIEP_START restarts the effect from time zero on every one. It is started
     * once when created and again after each re-Acquire, which is the case
     * DIEP_START was there to cover. */
    IDirectInputEffect_SetParameters(s_force, &eff, DIEP_TYPESPECIFICPARAMS);
}

/* Peak |magnitude| sent to DirectInput since the last call, and reset. 10000 is
 * DirectInput's full scale, so a census reporting 10000 has established that
 * anything still felt as weak is the wheel's own force setting or its driver,
 * neither of which this process can see. */
long di_peak_force(void)
{
    long peak = (long)s_peak_magnitude;

    s_peak_magnitude = 0;
    return peak;
}
