/* di_role.h -- which controller steering, gas and brake are each read from.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The default device holds the three role keys; a value with a [devices]
 * label, `dinput_throttle_axis = pedals:z`, reads that controller's axis. A
 * role's invert, deadzone and steering range come from the controller it is
 * on; the pedal mode stays with the default device. No DirectInput here, so
 * tests/di_role_test.c drives it with no device attached.
 */
#ifndef VCT_DI_ROLE_H
#define VCT_DI_ROLE_H

#include "ini.h"

/* DIJOYSTATE2's axis members, in c_dfDIJoystick2's order, named as a driver's
 * control panel names them. */
typedef enum {
    DI_AXIS_NONE = 0,
    DI_AXIS_X, DI_AXIS_Y, DI_AXIS_Z,
    DI_AXIS_RX, DI_AXIS_RY, DI_AXIS_RZ,
    DI_AXIS_SLIDER0, DI_AXIS_SLIDER1,
    /* XInput's six, on an XInput slot, named as the xinput_* keys name
     * them. `rx`/`ry` mean these or DirectInput's by the slot's provider. */
    DI_AXIS_LX, DI_AXIS_LY, DI_AXIS_XRX, DI_AXIS_XRY, DI_AXIS_LT, DI_AXIS_RT,
    DI_AXIS_COUNT
} di_axis_t;

/* 1 and the axis for a name the provider has, `none` included; 0 otherwise.
 * `xinput` picks XInput's six instead of DIJOYSTATE2's eight. */
int di_axis_parse(const char *s, int xinput, di_axis_t *out);
const char *di_axis_name(di_axis_t a);

typedef enum {
    DI_ROLE_STEER = 0,
    DI_ROLE_THROTTLE,
    DI_ROLE_BRAKE,
    DI_ROLE_COUNT
} di_role_id_t;

/* Each role's keys, spelled once. `what` is the role's name in a sentence. */
typedef struct {
    const char *axis_key, *axis_default, *invert_key, *what;
    int         invert_default;
} di_role_desc_t;
extern const di_role_desc_t di_role_desc[DI_ROLE_COUNT];

typedef struct {
    di_axis_t axis;
    char      label[INI_LABEL_CAP];  /* as written; "" is the default device */
    /* The slot it is read from; -1 when its label opens nothing, and then it
     * reads released. */
    int       slot;
    int       invert;
    unsigned  deadzone_percent;
    unsigned  range_percent;         /* steering only; 100 for a pedal */
} di_role_t;

typedef struct {
    di_role_t role[DI_ROLE_COUNT];
    /* As configured, after the one refusal below; see di_roles_separate()
     * for what is actually read. */
    int       pedals_separate;
} di_roles_t;

/* Load the role keys and their facts.
 *
 *   slot_of(label)   the slot a [devices] label is open in, or -1
 *   scope_of(slot)   the label that slot's profile is read under, "" for none
 *   is_xinput(slot)  whether that slot is an XInput pad (XInput's axis names)
 *
 * The role keys and dinput_pedal_mode come from scope_of(0); each role's
 * facts from scope_of(its slot). Combined pedals on two controllers are
 * refused with a warning and read as separate. */
void di_roles_load(di_roles_t *r, const char *path,
                   int (*slot_of)(const char *label),
                   const char *(*scope_of)(unsigned slot),
                   int (*is_xinput)(unsigned slot));

/* Whether `combined` has to be refused: the brake is on another controller
 * than the throttle. Shared with the launcher's preview. */
int di_roles_split_combined(const di_roles_t *r);

/* Whether the pedals are read as two axes: configured separate and a brake
 * axis present. A lone trigger is always one pedal, never a centred axis. */
int di_roles_separate(const di_roles_t *r);

/* One slot's reading this poll: each axis -1..+1, 0 where the device has
 * none. */
typedef struct {
    int   answered;                  /* it returned a state this poll */
    float unit[DI_AXIS_COUNT];
} di_slot_reading_t;

/* Steering -1..+1 and drive -1 (full brake) .. +1 (full throttle). A role
 * whose slot did not answer reads released, not the raw 0 a failed read
 * leaves (a separate pedal maps (x + 1) / 2, so 0 is half throttle). */
void di_roles_sample(const di_roles_t *r, const di_slot_reading_t *slot,
                     unsigned slot_count, float *steer, float *drive);

#endif /* VCT_DI_ROLE_H */
