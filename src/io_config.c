/* io_config.c -- the control map.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The ini, the MAME key names, the axis map and the per-title defaults, and
 * nothing else: no device is opened here, no packet is built here, and the
 * word "board" appears only where a default differs between the two cabinets.
 * See src/io.h for the split.
 */
#include "vcthunder.h"
#include "io.h"
#include "ini.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static controls_config_t s_cfg;
/* A [diagnostics] key, so it is read with the rest of the file; the census
 * it drives lives with the force feedback in io_board.c. */
static int s_ffb_trace;

int io_config_ffb_trace(void)
{
    return s_ffb_trace;
}

const controls_config_t *io_config(void)
{
    return &s_cfg;
}

typedef struct { const char *name; int vk; } key_name_t;

static const key_name_t KEY_NAMES[] = {
    { "NONE", 0 },
    { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
    { "UP", VK_UP }, { "DOWN", VK_DOWN },
    { "LCTRL", VK_LCONTROL }, { "LCONTROL", VK_LCONTROL },
    { "RCTRL", VK_RCONTROL }, { "RCONTROL", VK_RCONTROL },
    { "LALT", VK_LMENU }, { "LMENU", VK_LMENU },
    { "RALT", VK_RMENU }, { "RMENU", VK_RMENU },
    { "LSHIFT", VK_LSHIFT }, { "RSHIFT", VK_RSHIFT },
    { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN },
    { "ESC", VK_ESCAPE }, { "ESCAPE", VK_ESCAPE },
    { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK },
    { "MINUS", VK_OEM_MINUS }, { "EQUALS", VK_OEM_PLUS },
    { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD },
    { "HOME", VK_HOME }, { "END", VK_END },
    { "PGUP", VK_PRIOR }, { "PGDN", VK_NEXT },
    { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE },
};

/* Load title-scoped control sections before shared sections. Missing keys fall
 * through by control group, preserving distinct keyboard and gamepad schemas.
 * The precedence rule itself, and the grammar of every value below, are
 * src/ini.c's: see ini_read_scoped().
 */
static int ini_bool(const char *path, const char *section, const char *key,
                    int fallback)
{
    char text[32];
    ini_read_scoped(path, section, key, fallback ? "true" : "false",
                    text, sizeof text);
    return ini_bool_or(text, fallback);
}

static unsigned ini_unsigned(const char *path, const char *section,
                             const char *key, unsigned fallback)
{
    char text[32], def[32];
    snprintf(def, sizeof def, "%u", fallback);
    ini_read_scoped(path, section, key, def, text, sizeof text);
    return ini_uint_or(text, 0, UINT_MAX, fallback);
}

/* Reverse of parse_vk, for the startup report. A log line that names the keys
 * from a literal is a log line that keeps saying "LCtrl boost" after the ini
 * says otherwise: report what was actually resolved. */
static const char *vk_name(int vk, char *buf, size_t n)
{
    unsigned i;

    if (!vk)
        return "-";
    for (i = 0; i < sizeof KEY_NAMES / sizeof KEY_NAMES[0]; i++)
        if (KEY_NAMES[i].vk == vk && KEY_NAMES[i].vk) {
            snprintf(buf, n, "%s", KEY_NAMES[i].name);
            return buf;
        }
    if (vk >= VK_F1 && vk <= VK_F24)
        snprintf(buf, n, "F%d", vk - VK_F1 + 1);
    else if (isalnum(vk))
        snprintf(buf, n, "%c", (char)vk);
    else
        snprintf(buf, n, "0x%02X", vk);
    return buf;
}

static int parse_vk(const char *input)
{
    const char *s = input;
    char *end;
    unsigned long n;
    unsigned i;

    if (!_strnicmp(s, "KEYCODE_", 8))
        s += 8;
    if (s[0] && !s[1] && isalnum((unsigned char)s[0]))
        return toupper((unsigned char)s[0]);
    if (s[0] == '-' && !s[1])
        return VK_OEM_MINUS;
    if (s[0] == '=' && !s[1])
        return VK_OEM_PLUS;
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        n = strtoul(s + 1, &end, 10);
        if (*end == '\0' && n >= 1 && n <= 24)
            return VK_F1 + (int)n - 1;
    }
    for (i = 0; i < sizeof KEY_NAMES / sizeof KEY_NAMES[0]; i++)
        if (!_stricmp(s, KEY_NAMES[i].name))
            return KEY_NAMES[i].vk;
    n = strtoul(s, &end, 0);
    if (end != s && *end == '\0' && n <= 0xFF)
        return (int)n;
    return -1;
}

/* The default is a NAME, and it goes through parse_vk() exactly as a value out
 * of the file does. It is NOT spelled a second time as a VK constant in
 * controls_defaults(): two spellings of one default can disagree with nobody
 * the wiser, so a default that cannot be written in the ini cannot exist. */
static int ini_key(const char *path, const char *name,
                   const char *fallback_name)
{
    char text[64];
    int vk;

    ini_read_scoped(path, "digital", name, fallback_name, text, sizeof text);
    vk = parse_vk(text);
    if (vk < 0) {
        LOGW("io: unknown [digital] %s=%s; using %s",
             name, text, fallback_name);
        vk = parse_vk(fallback_name);
    }
    return vk < 0 ? 0 : vk;     /* an unparsable default is a bug in the table */
}

static axis_id_t parse_axis(const char *s)
{
    if (!_stricmp(s, "none")) return AXIS_NONE;
    if (!_stricmp(s, "x"))  return AXIS_X;
    if (!_stricmp(s, "y"))  return AXIS_Y;
    if (!_stricmp(s, "z"))  return AXIS_Z;
    if (!_stricmp(s, "r"))  return AXIS_R;
    if (!_stricmp(s, "u"))  return AXIS_U;
    if (!_stricmp(s, "v"))  return AXIS_V;
    if (!_stricmp(s, "lx")) return AXIS_LX;
    if (!_stricmp(s, "ly")) return AXIS_LY;
    if (!_stricmp(s, "rx")) return AXIS_RX;
    if (!_stricmp(s, "ry")) return AXIS_RY;
    if (!_stricmp(s, "lt")) return AXIS_LT;
    if (!_stricmp(s, "rt")) return AXIS_RT;
    return AXIS_NONE;
}

static axis_id_t ini_axis(const char *path, const char *key,
                          const char *fallback)
{
    char text[32];
    ini_read_scoped(path, "analog", key, fallback, text, sizeof text);
    return parse_axis(text);
}

static input_provider_t ini_provider(const char *path)
{
    char text[32];
    ini_read_scoped(path, "analog", "provider", "auto", text, sizeof text);
    if (!_stricmp(text, "auto")) return PROVIDER_AUTO;
    if (!_stricmp(text, "xinput")) return PROVIDER_XINPUT;
    if (!_stricmp(text, "winmm") || !_stricmp(text, "joystick"))
        return PROVIDER_WINMM;
    if (!_stricmp(text, "dinput") || !_stricmp(text, "directinput") ||
        !_stricmp(text, "wheel"))
        return PROVIDER_DINPUT;
    if (!_stricmp(text, "none") || !_stricmp(text, "keyboard"))
        return PROVIDER_NONE;
    LOGW("io: unknown [analog] provider=%s; using auto", text);
    return PROVIDER_AUTO;
}

/* Nonzero for Offroad's MagicBus (_mb_io_comm_) board. Read from the profile rather than
 * from s_board_mb, which is only set at install: the key defaults are needed
 * one step earlier, in diego_preload(). */
int io_board_is_magicbus(void)
{
    return G && G->io_ack_write != 0;
}

/* Everything that is NOT a control binding. The bindings are in
 * src/bindings.def and are applied by controls_load_ini() below, which reaches
 * every one of them whether or not the file carries it. */
static void controls_defaults(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.provider = PROVIDER_AUTO;
    s_cfg.deadzone_percent = 12;
    s_cfg.xi_steer = AXIS_LX; s_cfg.xi_throttle = AXIS_RT;
    s_cfg.xi_brake = AXIS_LT;
    s_cfg.mm_steer = AXIS_X; s_cfg.mm_throttle = AXIS_Y;
    s_cfg.mm_brake = AXIS_NONE; s_cfg.mm_pedals = PEDALS_COMBINED;
    s_cfg.mm_throttle_invert = 1;

    s_cfg.pad_boost = 1; s_cfg.pad_view_high = 2;
    s_cfg.pad_view_low = 3; s_cfg.pad_view_pilot = 4;
    s_cfg.pad_coin1 = 7; s_cfg.pad_start1 = 8;
    s_cfg.pov_digital = 1;
}

/* Default key NAME for an ini fallback, so a missing key logs and defaults to
 * the same thing the table above chose. Only the keys that differ per board
 * need this. */
static const char *dflt(const char *hydro, const char *offroad)
{
    return io_board_is_magicbus() ? offroad : hydro;
}

static void controls_load_ini(const char *path)
{
    char text[32];

    controls_defaults();

    /* Every control, from src/bindings.def, in one pass. `key` names the ini
     * key in both sections and the two members that hold what it resolved to,
     * so a row cannot bind half of itself. */
#define BINDING(key, hydro, offroad, pad_default, button, hl, ol)             \
    s_cfg.key = ini_key(path, #key, dflt(hydro, offroad));                    \
    s_cfg.pad_##key = ini_unsigned(path, "gamepad", #key, pad_default);
#define BINDING_AXIS(key, hydro, offroad, hl, ol)                             \
    s_cfg.key = ini_key(path, #key, dflt(hydro, offroad));
#define BINDING_PAD(key, pad_default, hl, ol)                                 \
    s_cfg.pad_##key = ini_unsigned(path, "gamepad", #key, pad_default);
#include "bindings.def"
#undef BINDING
#undef BINDING_AXIS
#undef BINDING_PAD

    s_cfg.provider = ini_provider(path);
    s_cfg.xinput_device = ini_unsigned(path, "analog", "xinput_device", 0);
    s_cfg.winmm_device = ini_unsigned(path, "analog", "winmm_device", 0);
    s_cfg.deadzone_percent = ini_unsigned(path, "analog", "deadzone_percent", 12);
    if (s_cfg.deadzone_percent > 49)
        s_cfg.deadzone_percent = 49;

    s_cfg.xi_steer = ini_axis(path, "xinput_steering_axis", "lx");
    s_cfg.xi_throttle = ini_axis(path, "xinput_throttle_axis", "rt");
    s_cfg.xi_brake = ini_axis(path, "xinput_brake_axis", "lt");
    s_cfg.xi_steer_invert = ini_bool(path, "analog", "xinput_steering_invert", 0);
    s_cfg.xi_throttle_invert = ini_bool(path, "analog", "xinput_throttle_invert", 0);
    s_cfg.xi_brake_invert = ini_bool(path, "analog", "xinput_brake_invert", 0);

    s_cfg.mm_steer = ini_axis(path, "winmm_steering_axis", "x");
    s_cfg.mm_throttle = ini_axis(path, "winmm_throttle_axis", "y");
    s_cfg.mm_brake = ini_axis(path, "winmm_brake_axis", "none");
    ini_read_scoped(path, "analog", "winmm_pedal_mode", "combined", text, sizeof text);
    s_cfg.mm_pedals = !_stricmp(text, "separate") ? PEDALS_SEPARATE : PEDALS_COMBINED;
    s_cfg.mm_steer_invert = ini_bool(path, "analog", "winmm_steering_invert", 0);
    s_cfg.mm_throttle_invert = ini_bool(path, "analog", "winmm_throttle_invert", 1);
    s_cfg.mm_brake_invert = ini_bool(path, "analog", "winmm_brake_invert", 0);

    s_cfg.pov_digital = ini_bool(path, "analog", "pov_digital", 1);
    s_ffb_trace = ini_bool(path, "diagnostics", "ffb_trace", 0);
}



/* Format each resolved binding into independent storage before logging it. */
static void report_key_map(void)
{
    char n[12][16];

    vk_name(s_cfg.coin1,        n[0], sizeof n[0]);
    vk_name(s_cfg.coin2,        n[1], sizeof n[1]);
    vk_name(s_cfg.start1,       n[2], sizeof n[2]);
    vk_name(s_cfg.service1,     n[3], sizeof n[3]);
    vk_name(s_cfg.service_mode, n[4], sizeof n[4]);
    vk_name(s_cfg.steer_left,   n[5], sizeof n[5]);
    vk_name(s_cfg.steer_right,  n[6], sizeof n[6]);
    vk_name(s_cfg.volume_down,  n[7], sizeof n[7]);
    vk_name(s_cfg.volume_up,    n[8], sizeof n[8]);
    LOGI("io: keys  coin %s/%s  start %s  credit %s  test %s  "
         "steer %s/%s  volume %s/%s",
         n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8]);

    vk_name(s_cfg.throttle,   n[0], sizeof n[0]);
    vk_name(s_cfg.brake,      n[1], sizeof n[1]);
    vk_name(s_cfg.boost,      n[2], sizeof n[2]);
    vk_name(s_cfg.view_high,  n[3], sizeof n[3]);
    vk_name(s_cfg.view_low,   n[4], sizeof n[4]);
    vk_name(s_cfg.view_pilot, n[5], sizeof n[5]);
    vk_name(s_cfg.shift1,     n[6], sizeof n[6]);
    vk_name(s_cfg.shift2,     n[7], sizeof n[7]);
    vk_name(s_cfg.shift3,     n[8], sizeof n[8]);
    if (io_board_is_magicbus())
        LOGI("io: keys  gas %s  brake %s  NITRO %s  "
             "SLAM/CHASE/CHOPPER CAM %s/%s/%s  SHIFT 1/2/3 %s/%s/%s "
             "(none held = neutral)",
             n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8]);
    else
        LOGI("io: keys  throttle %s  brake %s  boost/start %s  "
             "views %s/%s/%s", n[0], n[1], n[2], n[3], n[4], n[5]);

    /* The same rule as the key report: name what was RESOLVED. A wheel is the
     * one device whose numbering nobody knows by heart, so an ordinal that
     * silently stayed at its default is the mistake this line exists to catch. */
    LOGI("io: buttons  start %u  boost/nitro %u  views %u/%u/%u  "
         "coin %u/%u  credit %u  test %u  volume %u/%u",
         s_cfg.pad_start1, s_cfg.pad_boost, s_cfg.pad_view_high,
         s_cfg.pad_view_low, s_cfg.pad_view_pilot, s_cfg.pad_coin1,
         s_cfg.pad_coin2, s_cfg.pad_service1, s_cfg.pad_service_mode,
         s_cfg.pad_volume_down, s_cfg.pad_volume_up);
    if (io_board_is_magicbus())
        LOGI("io: buttons  SHIFT 1/2/3 %u/%u/%u  paddles up %u down %u%s",
             s_cfg.pad_shift1, s_cfg.pad_shift2, s_cfg.pad_shift3,
             s_cfg.pad_shift_up, s_cfg.pad_shift_down,
             (s_cfg.pad_shift_up || s_cfg.pad_shift_down)
                 ? " (latched gate, starts in neutral)" : "");
    if (!s_cfg.pov_digital)
        LOGI("io: the POV hat does not override the analogue axes "
             "([analog] pov_digital = false)");
}

void io_config_load(const char *ini)
{
    char path[MAX_PATH];

    ini_resolve_path(ini, path, sizeof path);
    controls_load_ini(path);
}

void io_config_report(void)
{
    report_key_map();
}
