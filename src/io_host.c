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

#include "xi_axes.h"

typedef DWORD (WINAPI *xinput_get_state_fn)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *xinput_set_state_fn)(DWORD, XINPUT_VIBRATION *);
typedef MMRESULT (WINAPI *joy_get_pos_ex_fn)(UINT, LPJOYINFOEX);
typedef MMRESULT (WINAPI *joy_get_caps_fn)(UINT_PTR, LPJOYCAPSA, UINT);

static xinput_get_state_fn s_xinput_get_state;
static xinput_set_state_fn s_xinput_set_state;
/* Whether the configured pad answered its LAST read. XInput's own answer to a
 * write on a pad that is not there costs milliseconds, and rumble.c drives the
 * motors from the frame-boundary thread, which owns the game's code while it
 * runs; so the output half asks the input half rather than the driver. */
static int s_xinput_live;
static joy_get_pos_ex_fn s_joy_get_pos_ex;
static joy_get_caps_fn s_joy_get_caps;
static HMODULE s_winmm_module;
static JOYCAPSA s_joy_caps;
static unsigned s_joy_caps_device;
static int s_joy_caps_valid;
static int s_input_active;
static int s_reported_provider = -1;
/* R5: EVERY BACKEND IS DIAGNOSED ALIKE. DirectInput has said what it resolved,
 * what the device can read and which configured axis is missing;
 * these two said nothing at all, so a wrong axis on a pad or a
 * WinMM wheel was a silent constant. One flag each, because the report has to
 * wait for a device to answer; neither backend opens anything at preload. */
static int s_xinput_reported, s_winmm_reported;

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
        s_xinput_set_state = (xinput_set_state_fn)(uintptr_t)
            GetProcAddress(mod, "XInputSetState");
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

/* --- what each backend resolved, said out loud -------------------------- */

/* Whether a provider can read an axis AT ALL: a fact about the API, not the
 * device. xi_centered() answers 0.0f for a WinMM axis name and mm_axis() 0.0f
 * for an XInput one, a constant, and a constant in a separate-pedal sum is
 * a permanent half-brake. The launcher only offers the right
 * names; a hand-edited file is where this goes wrong, and R1 requires a
 * hand-edited file to work. */
static int xinput_has_axis(axis_id_t axis)
{
    return axis == AXIS_LX || axis == AXIS_LY || axis == AXIS_RX ||
           axis == AXIS_RY || axis == AXIS_LT || axis == AXIS_RT;
}

/* And for WinMM the DEVICE answers too: X and Y are always there, the other
 * four are capability bits. So a `z` on a two-axis stick is missing in exactly
 * the sense D6 means. */
static int winmm_has_axis(axis_id_t axis)
{
    switch (axis) {
    case AXIS_X: case AXIS_Y: return 1;
    case AXIS_Z: return (s_joy_caps.wCaps & JOYCAPS_HASZ) != 0;
    case AXIS_R: return (s_joy_caps.wCaps & JOYCAPS_HASR) != 0;
    case AXIS_U: return (s_joy_caps.wCaps & JOYCAPS_HASU) != 0;
    case AXIS_V: return (s_joy_caps.wCaps & JOYCAPS_HASV) != 0;
    default: return 0;
    }
}

/* dinput.c's three lines, for the other two providers: what the device can
 * read, which configured axis it cannot, and the resulting map, in that
 * order, because the middle is a warning and the last is the answer a reader
 * compares against their wheel. A provider whose axis set is fixed by its API
 * still prints the list: the point is to let a user match what they see moving
 * against what the ini names. */
static void report_axis_map(input_provider_t provider, const char *prefix,
                            const char *device,
                            int (*has)(axis_id_t), const axis_id_t *all,
                            unsigned all_count, int separate)
{
    static const char *const role_key[] = { "steering", "throttle", "brake" };
    char have[160];
    int n = 0;
    unsigned i;

    have[0] = '\0';
    for (i = 0; i < all_count; i++)
        if (has(all[i])) {
            n += snprintf(have + n, sizeof have - (size_t)n, "%s%s",
                          n ? ", " : "", io_config_axis_name(all[i]));
            if (n >= (int)sizeof have)      /* snprintf returns what it WOULD */
                n = (int)sizeof have - 1;   /* have written. dinput.c D11.    */
        }
    LOGI("io: %s axes readable: %s%s%s", prefix, n ? have : "none",
         device ? ", on " : "", device ? device : "");

    for (i = 0; i < 3; i++) {
        axis_id_t axis = io_config_axis(provider, (axis_role_t)i);
        /* `none` IS A REAL SETTING, not an absence: a combined-pedal wheel is
         * configured with no brake axis on purpose. Only a NAMED axis that is
         * not there is a defect. Same rule as the launcher's own message. */
        if (axis == AXIS_NONE || has(axis))
            continue;
        LOGW("io: [analog] %s_%s_axis = %s, but %s has no axis %s: ignoring "
             "it. Run with [diagnostics] input_trace = true and use the axis "
             "that moves.", prefix, role_key[i], io_config_axis_name(axis),
             device ? device : "this backend", io_config_axis_name(axis));
        io_config_drop_axis(provider, (axis_role_t)i);
    }

    /* THE MODE AS IT WILL BE USED, not as it was configured. Both samplers
     * read `separate` as `configured separate AND there is a brake axis`, so a
     * brake dropped two lines up silently turns the reading combined, and a
     * line still saying "separate" would be describing a branch the code does
     * not take. That is this file's own standing rule: name what RESOLVED. */
    if (separate &&
        io_config_axis(provider, AXIS_ROLE_BRAKE) == AXIS_NONE) {
        separate = 0;
        if (io_config_axis(provider, AXIS_ROLE_THROTTLE) != AXIS_NONE)
            LOGI("io: only one pedal axis, so it is read as a combined one "
                 "(full brake .. full throttle) regardless of "
                 "%s_pedal_mode", prefix);
    }
    LOGI("io: %s axes steer=%s throttle=%s brake=%s (%s pedals), deadzone %u%%",
         prefix,
         io_config_axis_name(io_config_axis(provider, AXIS_ROLE_STEER)),
         io_config_axis_name(io_config_axis(provider, AXIS_ROLE_THROTTLE)),
         io_config_axis_name(io_config_axis(provider, AXIS_ROLE_BRAKE)),
         separate ? "separate" : "combined", s_cfg.deadzone_percent);
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

/* A pad that is not connected is asked at most once a second, per Microsoft's
 * XInputGetState guidance: an unconnected index is the slow call. */
static DWORD xi_get_state(DWORD user, XINPUT_STATE *state)
{
    static DWORD s_absent_since[XUSER_MAX_COUNT];
    static int   s_absent[XUSER_MAX_COUNT];
    DWORD now = GetTickCount(), r;

    if (user < XUSER_MAX_COUNT && s_absent[user] &&
        now - s_absent_since[user] < 1000)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(state, 0, sizeof *state);
    r = s_xinput_get_state(user, state);
    if (user < XUSER_MAX_COUNT) {
        s_absent[user] = r == ERROR_DEVICE_NOT_CONNECTED;
        s_absent_since[user] = now;
    }
    return r;
}

/* An XInput pad held as a slot beside a DirectInput wheel (dinput.c); not the
 * pad that drives, which is sample_xinput()'s. */
int io_host_xinput_read(unsigned user, float axis[6], unsigned *buttons)
{
    XINPUT_STATE state;
    unsigned i;

    if (!s_xinput_get_state || xi_get_state(user, &state) != ERROR_SUCCESS)
        return 0;
    for (i = 0; i < 6; i++)
        axis[i] = xi_axis_unit(&state.Gamepad, i);
    *buttons = xi_button_bits(state.Gamepad.wButtons);
    return 1;
}

static int sample_xinput(host_state_t *out)
{
    XINPUT_STATE state;
    XINPUT_GAMEPAD *g;

    if (!s_xinput_get_state)
        return 0;
    if (xi_get_state(s_cfg.xinput_device, &state) != ERROR_SUCCESS) {
        s_xinput_live = 0;
        return 0;
    }
    s_xinput_live = 1;
    g = &state.Gamepad;

    /* BEFORE the map is applied, not after: report_axis_map() drops an axis
     * this pad cannot deliver, and a frame read on the un-dropped map is a
     * frame of the very constant the warning is about. */
    if (!s_xinput_reported) {
        static const axis_id_t all[] = { AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY,
                                         AXIS_LT, AXIS_RT };
        char who[32];

        s_xinput_reported = 1;
        snprintf(who, sizeof who, "XInput pad %u", s_cfg.xinput_device);
        report_axis_map(PROVIDER_XINPUT, "xinput", who, xinput_has_axis,
                        all, sizeof all / sizeof all[0],
                        s_cfg.xi_brake != AXIS_NONE);
    }
    if (io_config_input_trace()) {
        static XINPUT_GAMEPAD last;
        if (memcmp(g, &last, sizeof *g) != 0) {
            LOGI("io: xinput lx=%6d ly=%6d rx=%6d ry=%6d lt=%4u rt=%4u "
                 "buttons=0x%04X", g->sThumbLX, g->sThumbLY, g->sThumbRX,
                 g->sThumbRY, g->bLeftTrigger, g->bRightTrigger,
                 (unsigned)g->wButtons);
            last = *g;
        }
    }

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
    /* XInput has ten, and they are the low ten. */
    out->buttons[0][0] = xi_button_bits(g->wButtons);
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

    /* Same placement and the same reason as the XInput block above. The caps
     * are already in hand: ensure_joy_caps() ran two lines up, and this
     * backend's axis set IS those caps. */
    if (!s_winmm_reported) {
        static const axis_id_t all[] = { AXIS_X, AXIS_Y, AXIS_Z,
                                         AXIS_R, AXIS_U, AXIS_V };
        s_winmm_reported = 1;
        report_axis_map(PROVIDER_WINMM, "winmm",
                        s_joy_caps.szPname[0] ? s_joy_caps.szPname : NULL,
                        winmm_has_axis, all, sizeof all / sizeof all[0],
                        s_cfg.mm_pedals == PEDALS_SEPARATE &&
                        s_cfg.mm_brake != AXIS_NONE);
    }
    if (io_config_input_trace()) {
        static JOYINFOEX last;
        if (memcmp(&state, &last, sizeof state) != 0) {
            LOGI("io: winmm x=%5lu y=%5lu z=%5lu r=%5lu u=%5lu v=%5lu "
                 "pov=%ld buttons=0x%08lX",
                 (unsigned long)state.dwXpos, (unsigned long)state.dwYpos,
                 (unsigned long)state.dwZpos, (unsigned long)state.dwRpos,
                 (unsigned long)state.dwUpos, (unsigned long)state.dwVpos,
                 state.dwPOV == JOY_POVCENTERED ? -1L : (long)state.dwPOV,
                 (unsigned long)state.dwButtons);
            last = state;
        }
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
    /* joyGetPosEx's mask is 32 wide by its own API; nothing is lost. */
    out->buttons[0][0] = state.dwButtons;
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
    memcpy(out->buttons, r.buttons, sizeof out->buttons);
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

/* The output half of the XInput device the sampler is already reading. It is
 * here and not in rumble.c because WHICH pad this host is on is one fact:
 * duplicating the module load and the device index would let the motors run on
 * a controller the sticks are not coming from. */
/* THE SINK IS XINPUT AND ONLY XINPUT, and nothing said so out loud. A pad that
 * is not an XInput device answers through WinMM here, runs the whole derived
 * model and has nowhere to put the two motor values, while the log has already
 * announced "derived pad rumble". So say it ONCE, at the first undeliverable
 * command: that separates "the game never asked" from "we asked and there was
 * no sink". */
static int s_rumble_gap_reported;

int io_host_rumble(unsigned low_motor, unsigned high_motor)
{
    XINPUT_VIBRATION vibration;

    if (!s_xinput_set_state || !s_xinput_live) {
        if (!s_rumble_gap_reported && (low_motor || high_motor)) {
            s_rumble_gap_reported = 1;
            LOGW("io: the game asked for rumble and there is nowhere to send "
                 "it: the rumble sink is XInput, and the active provider is "
                 "%s. A pad that is not an XInput device cannot buzz through "
                 "this host, whatever [ffb] rumble says.",
                 provider_name((input_provider_t)s_reported_provider));
        }
        return 0;
    }
    if (low_motor > 65535u)  low_motor = 65535u;
    if (high_motor > 65535u) high_motor = 65535u;
    memset(&vibration, 0, sizeof vibration);
    vibration.wLeftMotorSpeed = (WORD)low_motor;
    vibration.wRightMotorSpeed = (WORD)high_motor;
    return s_xinput_set_state(s_cfg.xinput_device, &vibration) == ERROR_SUCCESS;
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
