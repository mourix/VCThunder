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
static int s_ffb_events;
/* [diagnostics] input_trace. Here rather than in each backend because THREE
 * backends trace on it, and a second and third copy of that read is the shape
 * src/ini.h exists to forbid. io_config_load() runs before di_preload(). */
static int s_input_trace;
/* Labels for the four trace marker keys, from [diagnostics] mark1..mark4.
 * Empty means the key is unbound and logs nothing. */
static char s_mark[4][32];
/* [diagnostics] ffb_record: a file every control tick's force, the pad's
 * channels and its motors are written to, for replaying the pad model offline.
 * Empty is off. */
static char s_ffb_record[260];

int io_config_ffb_trace(void)
{
    return s_ffb_trace;
}

int io_config_ffb_events(void)
{
    return s_ffb_events;
}

int io_config_input_trace(void)
{
    return s_input_trace;
}

const char *io_config_ffb_record(void)
{
    return s_ffb_record[0] ? s_ffb_record : NULL;
}

const char *io_config_mark_label(unsigned i)
{
    if (i >= 4 || !s_mark[i][0])
        return NULL;
    return s_mark[i];
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

/* Load title-scoped control sections before shared ones; missing keys fall
 * through by control group, preserving distinct keyboard and gamepad schemas.
 * The precedence rule and the grammar of every value below are src/ini.c's,
 * see ini_read_scoped(). */
static int ini_bool(const char *path, const char *section, const char *key,
                    int fallback)
{
    char text[32];
    ini_read_scoped(path, section, key, fallback ? "true" : "false",
                    text, sizeof text);
    return ini_bool_or(text, fallback);
}

/* A [gamepad] VALUE: a comma list of `[<label>:]<ordinal>`, ADDITIVE. `9`
 * parses to one unlabelled reference and behaves as the single `unsigned` it
 * replaced.
 *
 * THE LABEL IS NOT RESOLVED HERE AND CANNOT BE: this runs a step BEFORE
 * di_preload(), so no device is open and no [devices] label names anything yet.
 * io_config_bind_devices() turns it into a slot afterwards.
 *
 * AND IT COMPLAINS ABOUT EVERY PART OF THE GRAMMAR IT REFUSES: an unparseable
 * value that silently becomes the compiled default is the failure a new syntax
 * lands in. A value with one bad element loses THAT ELEMENT and keeps the
 * others. */
static void ini_pad_bind(const char *path, const char *key, unsigned fallback,
                         io_pad_bind_t *out)
{
    char text[128], def[32], item[64];
    unsigned i;
    /* Whether ANY element was legal, which is not whether anything ended up
     * bound: `boost = 0` is legal and binds nothing, and must not be
     * overwritten by the default the way `boost = nonsense` is. */
    int legal = 0;

    memset(out, 0, sizeof *out);
    snprintf(def, sizeof def, "%u", fallback);
    ini_read_scoped(path, "gamepad", key, def, text, sizeof text);

    for (i = 0; ini_list_item(text, i, item, sizeof item); i++) {
        io_pad_ref_t *ref;
        unsigned ordinal;
        char label[INI_LABEL_CAP];

        if (out->n >= IO_MAX_PAD_REFS) {
            LOGW("io: [gamepad] %s = %s names more than %d controls; the rest "
                 "are ignored.", key, text, IO_MAX_PAD_REFS);
            break;
        }
        if (!ini_parse_pad_ref(item, label, sizeof label, &ordinal)) {
            LOGW("io: [gamepad] %s: \"%s\" is not a button. A button is a "
                 "number counting from 1, optionally with the controller it is "
                 "on in front of it and a colon, `9` or `shifter:1`, and "
                 "several may be separated by commas. The launcher's Controls "
                 "page shows which number each button on your controller is.",
                 key, item[0] ? item : "(empty)");
            continue;
        }
        if (ordinal > IO_MAX_BUTTONS) {
            LOGW("io: [gamepad] %s = %s is above button %d, which is as many "
                 "as this host can read from one controller.",
                 key, item, IO_MAX_BUTTONS);
            continue;
        }
        if (!ordinal && !label[0]) {
            legal = 1;           /* 0 is how the file spells "unbound" */
            continue;
        }
        if (!ordinal) {
            LOGW("io: [gamepad] %s = %s asks for button 0 of \"%s\", and "
                 "buttons count from 1. Nothing is bound.", key, item, label);
            /* UNDERSTOOD AND REFUSED, which is not unreadable: the line above
             * has said exactly what is wrong, so the value must not also
             * collect the vaguer "could not be read at all" below. */
            legal = 1;
            continue;
        }
        ref = &out->ref[out->n++];
        snprintf(ref->label, sizeof ref->label, "%s", label);
        ref->ordinal = ordinal;
        ref->slot = 0;
        ref->resolved = label[0] ? 0 : 1;
        legal = 1;
    }
    /* NOTHING IN THE VALUE WAS LEGAL, so the table's default stands. It says
     * "falls back to" rather than "could not be read at all": on a one-element
     * value the line above has already said WHY. */
    if (!legal) {
        LOGW("io: [gamepad] %s = %s: nothing in it could be used, so it falls "
             "back to %u.", key, text[0] ? text : "(empty)", fallback);
        if (fallback) {
            out->ref[0].ordinal = fallback;
            out->ref[0].resolved = 1;
            out->n = 1;
        }
    }
}

/* AND IT COMPLAINS, which it was alone among these helpers in not doing: every
 * [gamepad] ordinal came through here and an unparseable one silently became
 * the compiled default. What is left here is the three [analog] numbers,
 * ini_pad_bind() above handles the binding syntax, with the same complaint. */
static unsigned ini_unsigned(const char *path, const char *section,
                             const char *key, unsigned fallback)
{
    char text[32], def[32];
    unsigned value;

    snprintf(def, sizeof def, "%u", fallback);
    ini_read_scoped(path, section, key, def, text, sizeof text);
    if (ini_parse_uint(text, 0, UINT_MAX, &value))
        return value;
    LOGW("io: [%s] %s = %s is not a whole number; using %u. A button ordinal "
         "is a single number, counting from 1, and the launcher's Controls "
         "page shows which one each button on your controller is.",
         section, key, text[0] ? text : "(empty)", fallback);
    return fallback;
}

/* Reverse of parse_vk, for the startup report. A log line that names the keys
 * from a literal is a log line that keeps saying "LCtrl boost" after the ini
 * says otherwise: report what was actually resolved. */
static const char *vk_name(int vk, char *buf, size_t n)
{
    unsigned i;

    /* THE BUFFER IS THE CONTRACT HERE, NOT THE RETURN VALUE. report_key_map()
     * renders nine bindings into independent storage and prints the STORAGE, so
     * a path returning a literal without writing `buf` hands LOGI an
     * uninitialised slot, and with no NUL inside 16 bytes it reads on into
     * the next one. */
    if (!vk) {
        snprintf(buf, n, "-");
        return buf;
    }
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

/* THE WHOLE BINDING, NOT ONE KEY, for the startup report, the same rule the
 * [gamepad] census follows: name what was RESOLVED, and a binding that names
 * two keys must not report as one. */
static const char *key_bind_name(const io_key_bind_t *bind, char *buf, size_t n)
{
    unsigned i;
    size_t at = 0;

    if (!bind->n) {
        snprintf(buf, n, "-");
        return buf;
    }
    buf[0] = '\0';
    for (i = 0; i < bind->n && i < IO_MAX_KEY_REFS; i++) {
        char one[32];
        int w = snprintf(buf + at, n - at, "%s%s", at ? "+" : "",
                         vk_name(bind->vk[i], one, sizeof one));
        if (w < 0 || (size_t)w >= n - at)
            break;
        at += (size_t)w;
    }
    /* An empty render is the same answer as an absent binding, and it has to
     * arrive the same way: in `buf`. */
    if (!at)
        snprintf(buf, n, "-");
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
static void ini_key(const char *path, const char *name,
                    const char *fallback_name, io_key_bind_t *out)
{
    char text[INI_MATCH_CAP], item[64];
    unsigned i;
    int bad = 0;

    memset(out, 0, sizeof *out);
    ini_read_scoped(path, "digital", name, fallback_name, text, sizeof text);
    /* A COMMA LIST, AND ONE ELEMENT IS THE ORDINARY CASE. `boost = LCTRL`
     * parses to one reference and behaves exactly as it did before the list
     * existed, which is what makes the syntax additive, the same property
     * [gamepad]'s ordinals gained in Phase 4 and for the same reason. */
    for (i = 0; ini_list_item(text, i, item, sizeof item); i++) {
        int vk;

        if (out->n >= IO_MAX_KEY_REFS) {
            LOGW("io: [digital] %s names more than %d keys; the rest are "
                 "ignored", name, IO_MAX_KEY_REFS);
            break;
        }
        vk = parse_vk(item);
        if (vk < 0) {
            /* PER ELEMENT, so one bad name loses that name and not the whole
             * binding, the rule [gamepad] already follows. */
            LOGW("io: unknown [digital] %s key \"%s\"; that part of the "
                 "binding does nothing", name, item);
            bad = 1;
            continue;
        }
        if (vk)
            out->vk[out->n++] = vk;
    }
    if (out->n || !bad)
        return;
    /* Every element was unparsable, so the binding says nothing at all: fall
     * back to the table rather than leave the control unreachable. */
    LOGW("io: [digital] %s named no key this host knows; using %s",
         name, fallback_name);
    {
        int vk = parse_vk(fallback_name);
        if (vk > 0)
            out->vk[out->n++] = vk;
    }
}

/* parse_vk()'s answer for a caller outside this file. -1 (unparsable) and the
 * NONE row both come back as 0, because a caller asking "which key is this"
 * has the same use for both. */
int io_config_key_code(const char *name)
{
    int vk;

    if (!name || !*name)
        return 0;
    vk = parse_vk(name);
    return vk < 0 ? 0 : vk;
}

/* ONE TABLE, READ BOTH WAYS. It was an if-chain, and the reverse direction did
 * not exist, which is why the two non-DirectInput providers could not say
 * what they had resolved. The order is axis_id_t's own. */
static const char *const AXIS_NAMES[] = {
    "none",
    "x", "y", "z", "r", "u", "v",
    "lx", "ly", "rx", "ry", "lt", "rt"
};

static axis_id_t parse_axis(const char *s)
{
    unsigned i;

    for (i = 0; i < sizeof AXIS_NAMES / sizeof AXIS_NAMES[0]; i++)
        if (!_stricmp(s, AXIS_NAMES[i]))
            return (axis_id_t)i;
    return AXIS_NONE;
}

/* The reverse, for the resolved-axis lines. Never NULL: an id outside the
 * table is a bug here, not a configuration, and "?" in a log line is easier to
 * chase than a crash. */
const char *io_config_axis_name(axis_id_t axis)
{
    unsigned i = (unsigned)axis;

    return i < sizeof AXIS_NAMES / sizeof AXIS_NAMES[0] ? AXIS_NAMES[i] : "?";
}

/* The three analogue roles, by provider, addressed rather than spelled out at
 * each call site. io_host.c asks the same question of two backends and
 * dinput.c keeps its own map, so this covers the two that share s_cfg. */
static axis_id_t *axis_field(input_provider_t provider, axis_role_t role)
{
    if (provider == PROVIDER_XINPUT)
        return role == AXIS_ROLE_STEER    ? &s_cfg.xi_steer :
               role == AXIS_ROLE_THROTTLE ? &s_cfg.xi_throttle :
                                            &s_cfg.xi_brake;
    if (provider == PROVIDER_WINMM)
        return role == AXIS_ROLE_STEER    ? &s_cfg.mm_steer :
               role == AXIS_ROLE_THROTTLE ? &s_cfg.mm_throttle :
                                            &s_cfg.mm_brake;
    return NULL;
}

axis_id_t io_config_axis(input_provider_t provider, axis_role_t role)
{
    const axis_id_t *f = axis_field(provider, role);

    return f ? *f : AXIS_NONE;
}

/* A CONFIGURED AXIS THE DEVICE DOES NOT REPORT IS DROPPED, NOT ABSORBED:
 * dinput.c's validate_axis() applied to the other two backends. An axis nothing
 * writes reads as a constant, and a constant in a separate-pedal sum is a
 * permanent half-brake. The map lives here so the edit
 * happens here; WHAT a device reports is io_host.c's question. */
void io_config_drop_axis(input_provider_t provider, axis_role_t role)
{
    axis_id_t *f = axis_field(provider, role);

    if (f)
        *f = AXIS_NONE;
}

/* [analog], through the device's own section first when it has one. `label`
 * is NULL on a device that is not in [devices], and the walk is then exactly
 * ini_read_scoped()'s, see ini_read_device_scoped(). */
static axis_id_t ini_axis(const char *path, const char *label, const char *key,
                          const char *fallback)
{
    char text[32];
    ini_read_device_scoped(path, "analog", label, key, fallback,
                           text, sizeof text);
    return parse_axis(text);
}

static int ini_analog_bool(const char *path, const char *label,
                           const char *key, int fallback)
{
    char text[32];
    ini_read_device_scoped(path, "analog", label, key,
                           fallback ? "true" : "false", text, sizeof text);
    return ini_bool_or(text, fallback);
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

/* `auto`, or a boolean in any spelling ini.c accepts. A value that is neither
 * is `auto` WITH A COMPLAINT, the same way ini_provider() treats an unknown
 * provider: silently picking a side of a steering override is how a user ends
 * up at full lock with no idea why. */
static pov_digital_mode_t ini_pov_digital(const char *path)
{
    char text[32];
    int b;

    ini_read_scoped(path, "analog", "pov_digital", "auto", text, sizeof text);
    if (!_stricmp(text, "auto"))
        return POV_DIGITAL_AUTO;
    if (ini_parse_bool(text, &b))
        return b ? POV_DIGITAL_ON : POV_DIGITAL_OFF;
    LOGW("io: unknown [analog] pov_digital=%s; using auto", text);
    return POV_DIGITAL_AUTO;
}

/* THE ANSWER, as opposed to the setting. `auto` resolves against the device the
 * host actually opened, so it cannot be resolved in controls_load_ini(), which
 * runs a step BEFORE di_preload(). Resolved on each call rather than cached for
 * the same reason.
 *
 * The signal is a heuristic (di_publishes_ffb()). What keeps that honest is the
 * log line below: it fires once and says which way it went and what decided it,
 * so a pad losing its D-pad is a two-second diagnosis. */
int io_config_pov_digital(void)
{
    static int reported = -1;
    int on, wheel;

    if (s_cfg.pov_digital != POV_DIGITAL_AUTO)
        return s_cfg.pov_digital == POV_DIGITAL_ON;
    wheel = di_publishes_ffb();
    on = !wheel;
    if (reported != on) {
        reported = on;
        LOGI("io: [analog] pov_digital = auto resolves to %s: %s",
             on ? "true" : "false",
             wheel ? "the open device publishes force-feedback effects, so it "
                     "is taken for a wheel and its hat must not steer"
                   : "no force-feedback device is open, so the hat is taken "
                     "for a pad's D-pad and drives the steering");
    }
    return on;
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

    /* NO PAD ORDINALS HERE. Spelling them a second time beside bindings.def's
     * own column is what that file's header forbids, and a per-title column is
     * exactly the change that makes two spellings disagree.
     * controls_load_ini() reaches every row through bindings.def. */
    s_cfg.pov_digital = POV_DIGITAL_AUTO;
}

/* Default key NAME for an ini fallback, so a missing key logs and defaults to
 * the same thing the table above chose. Only the keys that differ per board
 * need this. */
static const char *dflt(const char *hydro, const char *offroad)
{
    return io_board_is_magicbus() ? offroad : hydro;
}

/* The same choice for the controller half. It is a separate function rather
 * than a macro over dflt() because the two columns are different types and a
 * cast between them would compile on either mistake. */
static unsigned pad_dflt(unsigned hydro, unsigned offroad)
{
    return io_board_is_magicbus() ? offroad : hydro;
}

static void controls_load_ini(const char *path)
{
    char text[32];
    char xi_label[INI_LABEL_CAP] = "", mm_label[INI_LABEL_CAP] = "";
    ini_device_id_t xi, mm;

    controls_defaults();

    /* Every control, from src/bindings.def, in one pass. `key` names the ini
     * key in both sections and the two members that hold what it resolved to,
     * so a row cannot bind half of itself. */
#define BINDING(key, hydro, offroad, pad_hydro, pad_offroad, button, hl, ol)  \
    ini_key(path, #key, dflt(hydro, offroad), &s_cfg.key);                    \
    ini_pad_bind(path, #key, pad_dflt(pad_hydro, pad_offroad),                \
                 &s_cfg.pad_##key);
#define BINDING_AXIS(key, hydro, offroad, hl, ol)                             \
    ini_key(path, #key, dflt(hydro, offroad), &s_cfg.key);
#define BINDING_PAD(key, pad_hydro, pad_offroad, hl, ol)                      \
    ini_pad_bind(path, #key, pad_dflt(pad_hydro, pad_offroad),                \
                 &s_cfg.pad_##key);
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

    /* R7: EACH DEVICE'S AXIS MAP UNDER THAT DEVICE'S OWN LABEL.
     *
     * These two providers select by INDEX and nothing else, so the [devices]
     * entry carries the index too: `pad = xinput:0`. The profile is exactly as
     * stable as the selection it belongs to, which is the honest ceiling: a pad
     * that moves to another XInput slot was already a different device as far
     * as this file is concerned.
     *
     * DirectInput is the other half, in dinput.c, where the product NAME is
     * known, but only after enumeration, which is why that one reads its keys
     * twice and this one does not. */
    xi.id = xi.name = mm.id = mm.name = "";
    xi.index = s_cfg.xinput_device;
    mm.index = s_cfg.winmm_device;
    ini_device_label(path, "xinput", &xi, 1, 0, xi_label, sizeof xi_label);
    ini_device_label(path, "winmm", &mm, 1, 0, mm_label, sizeof mm_label);

    if (xi_label[0])
        LOGI("io: [analog.%s] is XInput pad %u's own settings section",
             xi_label, s_cfg.xinput_device);
    if (mm_label[0])
        LOGI("io: [analog.%s] is WinMM device %u's own settings section",
             mm_label, s_cfg.winmm_device);

    s_cfg.xi_steer = ini_axis(path, xi_label, "xinput_steering_axis", "lx");
    s_cfg.xi_throttle = ini_axis(path, xi_label, "xinput_throttle_axis", "rt");
    s_cfg.xi_brake = ini_axis(path, xi_label, "xinput_brake_axis", "lt");
    s_cfg.xi_steer_invert = ini_analog_bool(path, xi_label, "xinput_steering_invert", 0);
    s_cfg.xi_throttle_invert = ini_analog_bool(path, xi_label, "xinput_throttle_invert", 0);
    s_cfg.xi_brake_invert = ini_analog_bool(path, xi_label, "xinput_brake_invert", 0);

    s_cfg.mm_steer = ini_axis(path, mm_label, "winmm_steering_axis", "x");
    s_cfg.mm_throttle = ini_axis(path, mm_label, "winmm_throttle_axis", "y");
    s_cfg.mm_brake = ini_axis(path, mm_label, "winmm_brake_axis", "none");
    ini_read_device_scoped(path, "analog", mm_label, "winmm_pedal_mode",
                           "combined", text, sizeof text);
    s_cfg.mm_pedals = !_stricmp(text, "separate") ? PEDALS_SEPARATE : PEDALS_COMBINED;
    s_cfg.mm_steer_invert = ini_analog_bool(path, mm_label, "winmm_steering_invert", 0);
    s_cfg.mm_throttle_invert = ini_analog_bool(path, mm_label, "winmm_throttle_invert", 1);
    s_cfg.mm_brake_invert = ini_analog_bool(path, mm_label, "winmm_brake_invert", 0);

    s_cfg.pov_digital = ini_pov_digital(path);
    s_ffb_trace = ini_bool(path, "diagnostics", "ffb_trace", 0);
    s_ffb_events = ini_bool(path, "diagnostics", "ffb_events", 0);
    s_input_trace = ini_bool(path, "diagnostics", "input_trace", 0);
    ini_read_scoped(path, "diagnostics", "ffb_record", "",
                    s_ffb_record, sizeof s_ffb_record);
    {
        unsigned i;
        for (i = 0; i < 4; i++) {
            char key[8];
            snprintf(key, sizeof key, "mark%u", i + 1);
            ini_read_scoped(path, "diagnostics", key, "",
                            s_mark[i], sizeof s_mark[i]);
        }
    }
}



/* ---- R6: a binding's labels become slots -------------------------------- */

/* One binding as the file spells it: `9`, `th8:1`, `9+th8:1`, or `-`.
 *
 * IT QUOTES THE LABEL RATHER THAN THE SLOT NUMBER; a slot number is ours,
 * `th8` is theirs, and an unresolved reference is marked `(absent)` HERE as
 * well as warned about above. Leaving it to the warning gave three WARN lines
 * saying the binding does nothing and then this census printing it as though it
 * had: a warning followed by something that reads as confirmation is not a
 * warning. */
static const char *pad_desc(const io_pad_bind_t *bind, char *buf, size_t n)
{
    unsigned i;
    size_t at = 0;

    if (!bind->n) {
        snprintf(buf, n, "-");
        return buf;
    }
    buf[0] = '\0';
    for (i = 0; i < bind->n && at + 1 < n; i++) {
        const io_pad_ref_t *ref = &bind->ref[i];
        int w;

        if (ref->label[0])
            w = snprintf(buf + at, n - at, "%s%s:%u%s",
                         i ? "+" : "", ref->label, ref->ordinal,
                         ref->resolved ? "" : "(absent)");
        else
            w = snprintf(buf + at, n - at, "%s%u", i ? "+" : "", ref->ordinal);
        if (w < 0)
            break;
        at += (size_t)w;
    }
    return buf;
}

/* A LABEL THAT NAMES NOTHING OPEN IS THE ONE NEW SILENT FAILURE R6 CAN PRODUCE,
 * the same shape as a profile whose label nothing binds: the label appears in
 * two places and the binding reads perfectly while doing nothing.
 *
 * It says so once per binding, and says all three reasons rather than guessing;
 * the host knows the label did not resolve and does NOT know whether the
 * entry is missing, the device unplugged, or the entry names a provider that
 * cannot be held open beside the wheel. */
static void bind_one(const char *key, io_pad_bind_t *bind)
{
    unsigned i;

    for (i = 0; i < bind->n; i++) {
        io_pad_ref_t *ref = &bind->ref[i];
        int slot;

        if (!ref->label[0]) {
            ref->slot = 0;
            ref->resolved = 1;
            continue;
        }
        slot = di_slot_for_label(ref->label);
        if (slot < 0) {
            ref->resolved = 0;
            LOGW("io: [gamepad] %s = ...%s:%u names a controller called \"%s\" "
                 "and no such controller is open, so that part of the binding "
                 "does nothing. Either [devices] has no `%s =` line, or the "
                 "device it names is not plugged in, or it names a WinMM "
                 "view, or no DirectInput wheel is driving; only DirectInput "
                 "controllers and XInput pads are held open beside a wheel.",
                 key, ref->label, ref->ordinal, ref->label, ref->label);
            continue;
        }
        ref->slot = (unsigned char)slot;
        ref->resolved = 1;
    }
}

void io_config_bind_devices(void)
{
    unsigned slots = di_slot_count();

#define BINDING(key, hydro, offroad, pad_hydro, pad_offroad, button, hl, ol)  \
    bind_one(#key, &s_cfg.pad_##key);
#define BINDING_AXIS(key, hydro, offroad, hl, ol)     /* no controller half */
#define BINDING_PAD(key, pad_hydro, pad_offroad, hl, ol)                      \
    bind_one(#key, &s_cfg.pad_##key);
#include "bindings.def"
#undef BINDING
#undef BINDING_AXIS
#undef BINDING_PAD

    if (slots > 1) {
        unsigned i;
        for (i = 1; i < slots; i++)
            LOGI("io: extra controller %u: \"%s\", read-only, and nothing "
                 "on it can reach the wheel's motor", i, di_slot_name(i));
    }
}

/* Format each resolved binding into independent storage before logging it. */
static void report_key_map(void)
{
    char n[12][16];

    key_bind_name(&s_cfg.coin1,        n[0], sizeof n[0]);
    key_bind_name(&s_cfg.coin2,        n[1], sizeof n[1]);
    key_bind_name(&s_cfg.start1,       n[2], sizeof n[2]);
    key_bind_name(&s_cfg.service1,     n[3], sizeof n[3]);
    key_bind_name(&s_cfg.service_mode, n[4], sizeof n[4]);
    key_bind_name(&s_cfg.steer_left,   n[5], sizeof n[5]);
    key_bind_name(&s_cfg.steer_right,  n[6], sizeof n[6]);
    key_bind_name(&s_cfg.volume_down,  n[7], sizeof n[7]);
    key_bind_name(&s_cfg.volume_up,    n[8], sizeof n[8]);
    LOGI("io: keys  coin %s/%s  start %s  credit %s  test %s  "
         "steer %s/%s  volume %s/%s",
         n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8]);

    key_bind_name(&s_cfg.throttle,   n[0], sizeof n[0]);
    key_bind_name(&s_cfg.brake,      n[1], sizeof n[1]);
    key_bind_name(&s_cfg.boost,      n[2], sizeof n[2]);
    key_bind_name(&s_cfg.view_high,  n[3], sizeof n[3]);
    key_bind_name(&s_cfg.view_low,   n[4], sizeof n[4]);
    key_bind_name(&s_cfg.view_pilot, n[5], sizeof n[5]);
    key_bind_name(&s_cfg.shift1,     n[6], sizeof n[6]);
    key_bind_name(&s_cfg.shift2,     n[7], sizeof n[7]);
    key_bind_name(&s_cfg.shift3,     n[8], sizeof n[8]);
    if (io_board_is_magicbus())
        LOGI("io: keys  gas %s  brake %s  NITRO %s  "
             "SLAM/CHASE/CHOPPER CAM %s/%s/%s  SHIFT 1/2/3 %s/%s/%s "
             "(none held = neutral)",
             n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8]);
    else
        LOGI("io: keys  throttle %s  brake %s  boost/start %s  "
             "views %s/%s/%s", n[0], n[1], n[2], n[3], n[4], n[5]);

    /* Name what was RESOLVED, as the key report does: a wheel's numbering is
     * what nobody knows by heart, so an ordinal that silently stayed at its
     * default is the mistake this line catches.
     *
     * Every pad_desc() in one argument list needs its OWN buffer, exactly as
     * vk_name() does: all of them run before the printf, so a shared scratch
     * prints whichever was last. */
    {
        /* Wide enough for IO_MAX_PAD_REFS references of a 15-character label,
         * a three-digit ordinal, the `(absent)` mark and the separators. */
        char b[11][112];

        LOGI("io: buttons  start %s  boost/nitro %s  views %s/%s/%s  "
             "coin %s/%s  credit %s  test %s  volume %s/%s",
             pad_desc(&s_cfg.pad_start1, b[0], sizeof b[0]),
             pad_desc(&s_cfg.pad_boost, b[1], sizeof b[1]),
             pad_desc(&s_cfg.pad_view_high, b[2], sizeof b[2]),
             pad_desc(&s_cfg.pad_view_low, b[3], sizeof b[3]),
             pad_desc(&s_cfg.pad_view_pilot, b[4], sizeof b[4]),
             pad_desc(&s_cfg.pad_coin1, b[5], sizeof b[5]),
             pad_desc(&s_cfg.pad_coin2, b[6], sizeof b[6]),
             pad_desc(&s_cfg.pad_service1, b[7], sizeof b[7]),
             pad_desc(&s_cfg.pad_service_mode, b[8], sizeof b[8]),
             pad_desc(&s_cfg.pad_volume_down, b[9], sizeof b[9]),
             pad_desc(&s_cfg.pad_volume_up, b[10], sizeof b[10]));
        if (io_board_is_magicbus())
            LOGI("io: buttons  SHIFT 1/2/3 %s/%s/%s  paddles up %s down %s%s",
                 pad_desc(&s_cfg.pad_shift1, b[0], sizeof b[0]),
                 pad_desc(&s_cfg.pad_shift2, b[1], sizeof b[1]),
                 pad_desc(&s_cfg.pad_shift3, b[2], sizeof b[2]),
                 pad_desc(&s_cfg.pad_shift_up, b[3], sizeof b[3]),
                 pad_desc(&s_cfg.pad_shift_down, b[4], sizeof b[4]),
                 (io_pad_bound(&s_cfg.pad_shift_up) ||
                  io_pad_bound(&s_cfg.pad_shift_down))
                     ? " (latched gate, starts in neutral)" : "");
    }
    /* NAME WHAT RESOLVED. Under `auto` the file says neither true nor false,
     * so a line quoting the file would be reporting a value nothing uses,
     * which is this function's own standing rule, three lines up. */
    if (!io_config_pov_digital())
        LOGI("io: the POV hat does not override the analogue axes "
             "([analog] pov_digital%s)",
             s_cfg.pov_digital == POV_DIGITAL_AUTO ? " = auto, on a wheel"
                                                   : " = false");
}

/* A DEVICE PROFILE WHOSE LABEL NOTHING BINDS DOES NOTHING AND SAYS NOTHING,
 * the likeliest typo in the whole schema, because the label appears twice and
 * [analog.tmz] beside a [devices] entry called `tmx` reads perfectly. The
 * walker cannot notice (an absent section is what a device with no profile
 * looks like), so the file is audited from the other end. */
static void report_device_profiles(const char *path)
{
    char names[8192];
    const char *p;

    if (!ini_section_names(path, names, sizeof names))
        return;
    for (p = names; *p; p += strlen(p) + 1) {
        char label[INI_LABEL_CAP], entry[INI_MATCH_CAP + 32];
        char provider[16], match[INI_MATCH_CAP];

        if (!ini_device_profile_section(p, "analog", label, sizeof label))
            continue;
        GetPrivateProfileStringA("devices", label, "", entry, sizeof entry,
                                 path);
        ini_strip_comment(entry);
        if (!entry[0])
            LOGW("io: [%s] holds settings for a controller called \"%s\", but "
                 "nothing in [devices] says WHICH controller that is, so every "
                 "key in that section is ignored. Add the line that binds it: "
                 "under [devices], `%s = dinput:<part of the product name>`.",
                 p, label, label);
        else if (!ini_device_entry(entry, provider, sizeof provider,
                                   match, sizeof match))
            LOGW("io: [devices] %s = %s is not <provider>:<match>, dinput, "
                 "xinput or winmm, then a colon, then a piece of the product "
                 "name (or the device number on xinput and winmm). [%s] is "
                 "ignored until it parses.", label, entry, p);
    }
}

void io_config_load(const char *ini)
{
    char path[MAX_PATH];

    ini_resolve_path(ini, path, sizeof path);
    controls_load_ini(path);
    report_device_profiles(path);
}

void io_config_report(void)
{
    report_key_map();
}
