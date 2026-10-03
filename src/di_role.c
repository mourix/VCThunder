/* di_role.c -- which controller steering, gas and brake are each read from.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See di_role.h.
 */
#include "vcthunder.h"
#include "io.h"
#include "di_role.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *name; di_axis_t axis; } axis_name_t;

static const axis_name_t k_di_axis[] = {
    { "none", DI_AXIS_NONE },
    { "x", DI_AXIS_X }, { "y", DI_AXIS_Y }, { "z", DI_AXIS_Z },
    { "rx", DI_AXIS_RX }, { "ry", DI_AXIS_RY }, { "rz", DI_AXIS_RZ },
    { "slider", DI_AXIS_SLIDER0 }, { "slider0", DI_AXIS_SLIDER0 },
    { "slider1", DI_AXIS_SLIDER1 },
};
static const axis_name_t k_xi_axis[] = {
    { "none", DI_AXIS_NONE },
    { "lx", DI_AXIS_LX }, { "ly", DI_AXIS_LY },
    { "rx", DI_AXIS_XRX }, { "ry", DI_AXIS_XRY },
    { "lt", DI_AXIS_LT }, { "rt", DI_AXIS_RT },
};

int di_axis_parse(const char *s, int xinput, di_axis_t *out)
{
    const axis_name_t *t = xinput ? k_xi_axis : k_di_axis;
    unsigned n = xinput ? sizeof k_xi_axis / sizeof k_xi_axis[0]
                        : sizeof k_di_axis / sizeof k_di_axis[0], i;

    for (i = 0; i < n; i++)
        if (!_stricmp(s, t[i].name)) {
            *out = t[i].axis;
            return 1;
        }
    *out = DI_AXIS_NONE;
    return 0;
}

const char *di_axis_name(di_axis_t a)
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
    case DI_AXIS_LX:      return "lx";
    case DI_AXIS_LY:      return "ly";
    case DI_AXIS_XRX:     return "rx";
    case DI_AXIS_XRY:     return "ry";
    case DI_AXIS_LT:      return "lt";
    case DI_AXIS_RT:      return "rt";
    default:              return "none";
    }
}

/* A wheel's pedal rests at its axis maximum, so both pedals default to
 * inverted; steering does not. */
const di_role_desc_t di_role_desc[DI_ROLE_COUNT] = {
    { "dinput_steering_axis", "x",  "dinput_steering_invert", "steering",    0 },
    { "dinput_throttle_axis", "y",  "dinput_throttle_invert", "accelerator", 1 },
    { "dinput_brake_axis",    "rz", "dinput_brake_invert",    "brake",       1 },
};

/* An XInput trigger rests at its minimum (xi_axes.h): not inverted by default,
 * and one pedal on its own, as io_host.c reads it. */
static int is_trigger(di_axis_t a)
{
    return a == DI_AXIS_LT || a == DI_AXIS_RT;
}

static int scoped_bool(const char *path, const char *label, const char *key,
                       int fallback)
{
    char text[32];

    ini_read_device_scoped(path, "analog", label, key,
                           fallback ? "true" : "false", text, sizeof text);
    return ini_bool_or(text, fallback);
}

static unsigned scoped_percent(const char *path, const char *label,
                               const char *key, unsigned fallback,
                               unsigned cap)
{
    char text[32], def[32];
    unsigned n;

    snprintf(def, sizeof def, "%u", fallback);
    ini_read_device_scoped(path, "analog", label, key, def, text, sizeof text);
    n = ini_uint_or(text, 0, UINT_MAX, fallback);
    return n > cap ? cap : n;
}

/* One controller or two. An unresolved role compares by label. */
static int same_device(const di_role_t *a, const di_role_t *b)
{
    if (a->slot >= 0 && b->slot >= 0)
        return a->slot == b->slot;
    return a->slot == b->slot && !_stricmp(a->label, b->label);
}

void di_roles_load(di_roles_t *r, const char *path,
                   int (*slot_of)(const char *label),
                   const char *(*scope_of)(unsigned slot),
                   int (*is_xinput)(unsigned slot))
{
    const char *def = scope_of(0);
    char text[MAX_PATH], axis[16];
    unsigned i;
    int ok;

    memset(r, 0, sizeof *r);
    for (i = 0; i < DI_ROLE_COUNT; i++) {
        di_role_t *role = &r->role[i];
        const char *scope;

        ini_read_device_scoped(path, "analog", def, di_role_desc[i].axis_key,
                               di_role_desc[i].axis_default, text, sizeof text);
        /* No label is the default device; a label that opens nothing is
         * -1, never 0, so the role is never read from the wrong device. The
         * slot decides whose names the axis is in. */
        role->axis = DI_AXIS_NONE;
        role->slot = 0;
        ok = 0;
        if (ini_parse_axis_ref(text, role->label, sizeof role->label,
                               axis, sizeof axis)) {
            role->slot = role->label[0] ? slot_of(role->label) : 0;
            ok = role->slot >= 0 ?
                 di_axis_parse(axis, is_xinput((unsigned)role->slot),
                               &role->axis) :
                 di_axis_parse(axis, 0, &role->axis) ||
                 di_axis_parse(axis, 1, &role->axis);
        }
        if (!ok) {
            LOGW("io: [analog] %s = %s is not an axis. An axis is x, y, z, "
                 "rx, ry, rz, slider0, slider1 or none on a DirectInput "
                 "controller, lx, ly, rx, ry, lt, rt or none on an XInput "
                 "pad, after a [devices] label and a colon when it is on "
                 "another controller (twcs:z). Reading it as none.",
                 di_role_desc[i].axis_key, text);
            role->axis = DI_AXIS_NONE;
            role->label[0] = '\0';
            role->slot = 0;
        }
        scope = role->slot >= 0 ? scope_of((unsigned)role->slot) : role->label;
        role->invert = scoped_bool(path, scope, di_role_desc[i].invert_key,
                                   di_role_desc[i].invert_default &&
                                   !is_trigger(role->axis));
        /* Zero: a wheel's centre is mechanical, and a thumbstick's 12%
         * would eat a fifth of the lock. The key is for a worn pot. */
        role->deadzone_percent = scoped_percent(path, scope,
                                                "dinput_deadzone_percent", 0, 49);
        role->range_percent = 100;
        if (i == DI_ROLE_STEER) {
            /* Physical travel scaled onto the cabinet's full lock; the
             * wheel driver's own rotation limit is better where it exists. */
            role->range_percent = scoped_percent(
                path, scope, "dinput_steering_range_percent", 100, 100);
            if (role->range_percent < 5)
                role->range_percent = 5;
        }
    }

    ini_read_device_scoped(path, "analog", def, "dinput_pedal_mode",
                           "separate", text, sizeof text);
    r->pedals_separate = _stricmp(text, "combined") != 0;
    if (di_roles_split_combined(r)) {
        LOGW("io: [analog] dinput_pedal_mode = combined reads both pedals "
             "from one axis, but dinput_brake_axis names a brake on another "
             "controller than dinput_throttle_axis does. Reading them as "
             "separate pedals; set dinput_pedal_mode = separate to say so.");
        r->pedals_separate = 1;
    }
}

int di_roles_split_combined(const di_roles_t *r)
{
    return !r->pedals_separate &&
           r->role[DI_ROLE_BRAKE].axis != DI_AXIS_NONE &&
           !same_device(&r->role[DI_ROLE_THROTTLE], &r->role[DI_ROLE_BRAKE]);
}

int di_roles_separate(const di_roles_t *r)
{
    return is_trigger(r->role[DI_ROLE_THROTTLE].axis) ||
           (r->pedals_separate && r->role[DI_ROLE_BRAKE].axis != DI_AXIS_NONE);
}

/* The role's axis, -1..+1, and whether it was read at all. */
static int role_unit(const di_role_t *role, const di_slot_reading_t *slot,
                     unsigned slot_count, float *v)
{
    *v = 0.0f;
    if (role->axis == DI_AXIS_NONE || role->slot < 0 ||
        (unsigned)role->slot >= slot_count ||
        !slot[role->slot].answered)
        return 0;
    *v = slot[role->slot].unit[role->axis];
    return 1;
}

/* A separate pedal: -1..+1 onto 0..1, then io_pedal() inverts and deadzones.
 * Unread is released, whatever the inversion. */
static float pedal(const di_role_t *role, const di_slot_reading_t *slot,
                   unsigned slot_count)
{
    float v;

    if (!role_unit(role, slot, slot_count, &v))
        return 0.0f;
    return io_pedal((v + 1.0f) * 0.5f, role->invert,
                    (float)role->deadzone_percent / 100.0f);
}

void di_roles_sample(const di_roles_t *r, const di_slot_reading_t *slot,
                     unsigned slot_count, float *steer, float *drive)
{
    const di_role_t *s = &r->role[DI_ROLE_STEER];
    const di_role_t *t = &r->role[DI_ROLE_THROTTLE];
    float v;

    *steer = 0.0f;
    if (role_unit(s, slot, slot_count, &v)) {
        /* Scale to the configured travel before the deadzone, so the
         * deadzone is a fraction of the game's range. */
        v *= 100.0f / (float)s->range_percent;
        v = io_clamp_unit(v);
        v = io_deadzone_centred(v, (float)s->deadzone_percent / 100.0f);
        *steer = s->invert ? -v : v;
    }

    if (di_roles_separate(r)) {
        *drive = pedal(t, slot, slot_count) -
                 pedal(&r->role[DI_ROLE_BRAKE], slot, slot_count);
    } else {
        /* Combined, or only one pedal axis: one axis from full brake to
         * full throttle. */
        *drive = 0.0f;
        if (role_unit(t, slot, slot_count, &v)) {
            v = io_deadzone_centred(v, (float)t->deadzone_percent / 100.0f);
            *drive = t->invert ? -v : v;
        }
    }
    *drive = io_clamp_unit(*drive);
}
