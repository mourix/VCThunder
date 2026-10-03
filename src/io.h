/* io.h -- the cabinet I/O boundary, split four ways.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * io_config.c is the control map: the ini, the MAME key names, the axis map and
 * the per-title defaults. io_host.c turns host devices into ONE
 * provider-neutral host_state_t. io_board.c turns that into a wire frame in the
 * title board's own format and owns the transport. diego.c is the game's hooks,
 * the frame-boundary seam and the poll thread. The first two know nothing about
 * boards; the last knows no key name and no byte offset. Dependency runs one
 * way: config <- host <- board <- diego. */
#ifndef VCT_IO_H
#define VCT_IO_H

#include "vcthunder.h"
/* For INI_LABEL_CAP: a binding may name its device by its [devices] label. */
#include "ini.h"

/* Longest board frame: Diego 8, MagicBus 16. One size for both, so frame
 * length is the payload builder's property and never the caller's. */
#define IO_PACKET_MAX   16

/* Buttons this host can see. DIJOYSTATE2 carries 128; every reader goes
 * through io_button_down() rather than shifting a bit itself. */
#define IO_MAX_BUTTONS  128
#define IO_BUTTON_WORDS (IO_MAX_BUTTONS / 32)

/* Devices held open at once. Slot 0 is the default device: it owns the force
 * feedback, the POV hat and bare button numbers, and its profile names the
 * axis roles. The rest are read-only, non-exclusive, with no effects: their
 * buttons, and any axis a role names with their label (`twcs:z`). An extra
 * slot is a DirectInput controller or an XInput pad. */
#define IO_MAX_SLOTS    8

/* Devices one action may be bound to at once: `boost = 9, pad:1` is two. */
#define IO_MAX_PAD_REFS 4

/* ---- the control map (io_config.c) -------------------------------------- */

/* `auto` asks the opened device; the other two are the user overriding it. */
typedef enum {
    POV_DIGITAL_AUTO = 0,
    POV_DIGITAL_ON,
    POV_DIGITAL_OFF
} pov_digital_mode_t;

typedef enum {
    PROVIDER_NONE = 0,
    PROVIDER_AUTO,
    PROVIDER_XINPUT,
    PROVIDER_WINMM,
    PROVIDER_DINPUT
} input_provider_t;

typedef enum {
    AXIS_NONE = 0,
    AXIS_X, AXIS_Y, AXIS_Z, AXIS_R, AXIS_U, AXIS_V,
    AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY, AXIS_LT, AXIS_RT
} axis_id_t;

/* The three analogue roles every backend resolves an axis for; addressing by
 * role is what keeps one "this device has no such axis" for every provider. */
typedef enum {
    AXIS_ROLE_STEER,
    AXIS_ROLE_THROTTLE,
    AXIS_ROLE_BRAKE
} axis_role_t;

typedef enum {
    PEDALS_COMBINED = 0,
    PEDALS_SEPARATE
} pedal_mode_t;

/* ONE ELEMENT OF A [gamepad] VALUE, AS TYPED AND AS RESOLVED.
 *
 * The label travels into the runtime because it cannot be resolved at parse
 * time: io_config_load() runs before di_preload(), so no device is open yet.
 * io_config_bind_devices() fills `slot` and `resolved`; the label is kept
 * because a warning about an absent device has to name it.
 *
 * AN UNRESOLVED LABEL IS NOT SLOT 0: `shift1 = th8:1` with no th8 attached
 * reads as unbound, never as button 1 of the wheel. */
typedef struct {
    char          label[INI_LABEL_CAP];  /* "" is the primary device        */
    unsigned char slot;                  /* resolved slot; 0 is primary     */
    unsigned char resolved;              /* 0: the label matched nothing    */
    unsigned      ordinal;               /* 1-based; 0 is unbound           */
} io_pad_ref_t;

typedef struct {
    io_pad_ref_t ref[IO_MAX_PAD_REFS];
    unsigned     n;
} io_pad_bind_t;

/* The keyboard half is a list too, so the launcher's `Add` means the same
 * thing on both halves. `[digital] boost = LCTRL` is one element and reads as
 * it always did. Read through io_key_down()/io_key_bound(), never by hand. */
#define IO_MAX_KEY_REFS 4

typedef struct {
    int      vk[IO_MAX_KEY_REFS];
    unsigned n;
} io_key_bind_t;

typedef struct {
    /* MAME keyboard defaults. Each is a list; see io_key_bind_t. */
    io_key_bind_t coin1, coin2, start1, service1, service_mode;
    io_key_bind_t steer_left, steer_right, throttle, brake;
    io_key_bind_t boost, view_high, view_low, view_pilot;
    io_key_bind_t volume_down, volume_up;
    io_key_bind_t shift1, shift2, shift3;   /* Offroad's 3-speed shifter */

    input_provider_t provider;
    unsigned xinput_device, winmm_device;
    unsigned deadzone_percent;

    axis_id_t xi_steer, xi_throttle, xi_brake;
    int xi_steer_invert, xi_throttle_invert, xi_brake_invert;

    axis_id_t mm_steer, mm_throttle, mm_brake;
    pedal_mode_t mm_pedals;
    int mm_steer_invert, mm_throttle_invert, mm_brake_invert;

    /* Logical button numbers: XInput A/B/X/Y/LB/RB/Back/Start are 1..8;
     * WinMM and DirectInput use their native one-based ordinals, the numbers a
     * wheel's own control panel shows. 0 is unbound, IO_MAX_BUTTONS the
     * ceiling; 32 was not enough for a wheel reporting 92.
     * Each is a LIST; read through io_pad_down()/io_pad_bound(), never by
     * hand. */
    io_pad_bind_t pad_boost, pad_view_high, pad_view_low, pad_view_pilot;
    io_pad_bind_t pad_coin1, pad_coin2, pad_start1;
    io_pad_bind_t pad_service1, pad_service_mode;
    io_pad_bind_t pad_volume_down, pad_volume_up;
    io_pad_bind_t pad_shift1, pad_shift2, pad_shift3;
    /* Offroad's gate as a pair of paddles. See sequential_shift(). */
    io_pad_bind_t pad_shift_up, pad_shift_down;
    /* A wheel's hat is at thumb height and a full-lock override on it is a
     * crash; a pad's D-pad IS the steering. So it is a key, defaulting to
     * ASKING THE DEVICE. NEVER READ THIS FIELD; it is the setting, not the
     * answer; io_config_pov_digital() is, and it cannot resolve here because
     * no device is open when the file is read. */
    pov_digital_mode_t pov_digital;
} controls_config_t;

/* Read the map. `ini` is the pack's file; per-title sections win, through
 * ini_read_scoped()'s precedence rule. */
void io_config_load(const char *ini);

/* Does the POV hat / D-pad drive the analogue fields to full lock? THE ONE
 * READER of cfg->pov_digital: `auto` is unanswerable until a device is open,
 * so call this at sample time, never at load time. */
int io_config_pov_digital(void);

/* A MAME key NAME as the ini spells it -> its virtual-key code; 0 for NONE and
 * for anything unparsable. The door to KEY_NAMES, so the launcher never grows
 * a second copy of it. */
int io_config_key_code(const char *name);
/* The loaded map. Never NULL: io_config_load() installs defaults first. */
const controls_config_t *io_config(void);
/* One block naming every resolved binding, at install. */
void io_config_report(void);

/* BIND THE LABELS TO THE OPEN DEVICES: after di_preload(), before the first
 * sample. A label becomes a slot here, and one naming nothing attached is
 * reported here. Correct to call with no DirectInput at all; that is how an
 * unresolvable binding gets its warning. */
void io_config_bind_devices(void);
/* An axis id as the ini spells it ("rz", "lt", "none"); the reverse of the
 * parse, out of the same table. */
const char *io_config_axis_name(axis_id_t axis);
/* The resolved axis for one role of one provider, and the same field cleared.
 * XInput and WinMM only; DirectInput keeps its own map in dinput.c. */
/* [diagnostics] ffb_trace: census the force the GAME commanded, consumed by
 * io_board.c. */
axis_id_t io_config_axis(input_provider_t provider, axis_role_t role);
void io_config_drop_axis(input_provider_t provider, axis_role_t role);

/* [diagnostics] input_trace, read once here for all three backends. */
int  io_config_input_trace(void);

int  io_config_ffb_trace(void);
/* The [diagnostics] ffb_record path, or NULL when off. */
const char *io_config_ffb_record(void);
/* [diagnostics] ffb_events: trace the game's OWN force-feedback units instead
 * of the byte they are summed into. Consumed by ffb_events.c. */
int  io_config_ffb_events(void);
/* [diagnostics] mark1..mark4: what F5..F8 mean in this session's trace, so the
 * log annotates itself. NULL for an unbound key. */
const char *io_config_mark_label(unsigned i);

/* Which board this title carries, from the profile and not a cached copy:
 * MagicBus has the three handshake toggles and Diego does not. One spelling,
 * because the key defaults need it before install and the packer after. */
int io_board_is_magicbus(void);

/* ---- host devices (io_host.c) ------------------------------------------- */

typedef struct {
    int present;
    input_provider_t provider;
    float steer;                         /* -1 full left .. +1 full right */
    float drive;                         /* -1 reverse .. +1 throttle    */
    /* One bit per button, bit 0 == button 1, PER SLOT; slot 0 is primary.
      * Never index a word directly; io_button_down() reads, io_button_set()
      * writes, io_pad_down() chooses a slot, so widening costs one file.
      *
      * Only the primary contributes the POV below: a second device's hat
      * quietly steering is the D-pad-versus-wheel defect on a device nobody
      * is looking at. */
    uint32_t buttons[IO_MAX_SLOTS][IO_BUTTON_WORDS];
    int left, right, up, down;            /* POV/D-pad digital overrides   */
} host_state_t;

/* One-based; out of range is DOWN=false, not an index off the end. An ordinal
 * comes from the user's ini, where 0 means "unbound". */
static inline int io_button_down(const uint32_t *mask, unsigned ordinal)
{
    if (!ordinal || ordinal > IO_MAX_BUTTONS)
        return 0;
    return (mask[(ordinal - 1) >> 5] & (1u << ((ordinal - 1) & 31))) != 0;
}

/* Zero-based, because every device API numbers its own buttons from 0. */
static inline void io_button_set(uint32_t *mask, unsigned index)
{
    if (index < IO_MAX_BUTTONS)
        mask[index >> 5] |= 1u << (index & 31);
}

static inline int io_buttons_any(const uint32_t *mask)
{
    unsigned i;
    for (i = 0; i < IO_BUTTON_WORDS; i++)
        if (mask[i])
            return 1;
    return 0;
}

/* Is anything bound at all? `!cfg->pad_shift_up` is not a question a list
 * answers; the shifter latch asks this to stay out of the way. */
static inline int io_pad_bound(const io_pad_bind_t *bind)
{
    unsigned i;
    for (i = 0; i < bind->n && i < IO_MAX_PAD_REFS; i++)
        if (bind->ref[i].ordinal)
            return 1;
    return 0;
}

/* Declared here as well as below: the inline underneath calls it. */
int io_host_key_down(int vk);

/* DOWN ON ANY OF THE KEYS THE BINDING NAMES. A zero element is NONE. */
static inline int io_key_down(const io_key_bind_t *bind)
{
    unsigned i;

    for (i = 0; i < bind->n && i < IO_MAX_KEY_REFS; i++)
        if (bind->vk[i] && io_host_key_down(bind->vk[i]))
            return 1;
    return 0;
}

static inline int io_key_bound(const io_key_bind_t *bind)
{
    unsigned i;

    for (i = 0; i < bind->n && i < IO_MAX_KEY_REFS; i++)
        if (bind->vk[i])
            return 1;
    return 0;
}

/* DOWN ON ANY OF THE DEVICES THE BINDING NAMES. A reference whose label
 * resolved to nothing is SKIPPED, never folded onto slot 0 (io_pad_ref_t). */
static inline int io_pad_down(const uint32_t buttons[][IO_BUTTON_WORDS],
                              const io_pad_bind_t *bind)
{
    unsigned i;

    for (i = 0; i < bind->n && i < IO_MAX_PAD_REFS; i++) {
        const io_pad_ref_t *ref = &bind->ref[i];

        if (ref->label[0] && !ref->resolved)
            continue;
        if (ref->slot < IO_MAX_SLOTS &&
            io_button_down(buttons[ref->slot], ref->ordinal))
            return 1;
    }
    return 0;
}

/* THE TWO SPELLINGS OF THE READING'S WIDTH, HELD TO EACH OTHER. di_reading_t
 * is in vcthunder.h, included first, so it spells the numbers out; a widening
 * here and not there would silently truncate every slot above the first. */
typedef char io_reading_width_check[
    (sizeof ((di_reading_t *)0)->buttons ==
     sizeof(uint32_t) * IO_MAX_SLOTS * IO_BUTTON_WORDS) ? 1 : -1];

/* A host axis is nominally -1..+1 and nothing downstream may assume it. Both
 * sides of the seam clamp: io_host.c after a deadzone, io_board.c before
 * encoding. */
static inline float io_clamp_unit(float v)
{
    if (v < -1.0f) return -1.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* ---- one deadzone, and one pedal law ------------------------------------
 *
 * There were four copies of this arithmetic and they did not agree.
 *
 * A pedal RESTS AT ITS AXIS MAXIMUM on every wheel this was written against,
 * which is why both pedals default to inverted (dinput.c), and the INVERSION
 * HAS TO COME FIRST: the other way round the band is eaten at the floored end
 * and a released pedal creeps. Inline because all four callers are on the
 * input path and none may reach for its own. */
static inline float io_deadzone_centred(float v, float deadzone)
{
    float a = v < 0.0f ? -v : v;

    if (a <= deadzone)
        return 0.0f;
    a = (a - deadzone) / (1.0f - deadzone);
    return v < 0.0f ? -a : a;
}

static inline float io_deadzone_unipolar(float v, float deadzone)
{
    if (v <= deadzone)
        return 0.0f;
    return (v - deadzone) / (1.0f - deadzone);
}

/* One pedal on its own axis, 0..1 as reported, into how far it is pressed:
 * inverted first, then deadzoned. */
static inline float io_pedal(float v, int invert, float deadzone)
{
    if (invert)
        v = 1.0f - v;
    return io_deadzone_unipolar(v, deadzone);
}

/* Two pedals on their own axes, each 0..1 as the driver reports it, into one
 * -1 (full brake) .. +1 (full throttle) drive value. */
static inline float io_pedals_separate(float throttle, float brake,
                                       int throttle_invert, int brake_invert,
                                       float deadzone)
{
    return io_pedal(throttle, throttle_invert, deadzone) -
           io_pedal(brake, brake_invert, deadzone);
}

/* Load XInput and WinMM. Before game_map(), like every preload: LoadLibrary
 * stops working afterwards. */
void io_host_preload(void);
/* One XInput pad held as a slot: its six axes (xi_axes.h's units) and ten
 * buttons, bit 0 = button 1. 0 when XInput is not loaded or the pad does not
 * answer. */
int  io_host_xinput_read(unsigned user, float axis[6], unsigned *buttons);
/* Re-evaluate the [controls] input_background policy and log a transition. */
void io_host_refresh_active(void);
int  io_host_active(void);
int  io_host_key_down(int vk);
/* One reading from whichever provider answers, already resolved against the
 * axis map. Returns 0 when there is no analogue device. */
int  io_host_sample(host_state_t *out);
/* Drive the XInput pad's two motors, 0..65535 each (low, high). 0 when there
 * is no such pad, which includes every wheel: a DirectInput wheel takes the
 * game's real force and nothing derived may be added to it. */
int  io_host_rumble(unsigned low_motor, unsigned high_motor);

/* ---- the board and its transport (io_board.c) --------------------------- */

/* Logical cabinet buttons, named once so each board's packer places them where
 * ITS wiring puts them. The two titles agree on no single bit. */
enum {
    /* Not a switch line: coin slots are 3-bit counters and shifter paddles
     * drive a latch. bindings.def gives them this so every control has a
     * `button` column. */
    BTN_NONE    = 0u,
    BTN_START   = 1u << 0,
    BTN_BOOST   = 1u << 1,     /* Hydro: Boost.  Offroad: NITRO            */
    BTN_VIEW1   = 1u << 2,     /* Hydro: view hi. Offroad: SLAM CAM        */
    BTN_VIEW2   = 1u << 3,     /* Hydro: view lo. Offroad: CHASE CAM       */
    BTN_VIEW3   = 1u << 4,     /* Hydro: pilot.   Offroad: CHOPPER CAM     */
    BTN_SERVICE = 1u << 5,     /* service credit                            */
    BTN_VOL_DN  = 1u << 6,
    BTN_VOL_UP  = 1u << 7,
    BTN_TEST    = 1u << 8,
    BTN_SHIFT1  = 1u << 9,     /* Offroad only; no shift bit set == neutral */
    BTN_SHIFT2  = 1u << 10,
    BTN_SHIFT3  = 1u << 11
};

/* WHERE A BOARD FRAME COMES FROM. The synthesised board is one implementation;
 * a real Diego/MagicBus link and a replayer for captured cabinet traffic are
 * others. Neither should have to touch the poll thread, the
 * frame-boundary seam or the input map, so a transport supplies frames and
 * consumes the outbound direction, and `[controls] backend` names one. */
typedef struct {
    const char *name;
    /* Bring the link up. `dips` is the cabinet DIP byte the operator set;
     * a real board reads its own and may ignore this. 0 on failure. */
    int      (*open)(unsigned dips);
    /* The next frame the board would have sent, into `frame`. Returns its
     * length in bytes, or 0 to produce nothing this cycle. */
    unsigned (*receive)(unsigned char *frame, unsigned max);
    /* The outbound direction, once per control-driver callback and AFTER it
     * (once per pass while no callback runs); that is when the DAC cell holds
     * this tick's force. `when_ms` is the
     * tick's time on io_clock_ms(), which may be ahead of now: a pass that
     * owes two ticks runs both at once, and the second belongs one period
     * later. */
    void     (*transmit)(double when_ms);
    /* A handle the poll thread waits on, signalled when something transmit
     * queued falls due, and the call that sends it. Either may be NULL. */
    HANDLE   (*wake)(void);
    void     (*deliver)(void);
    void     (*close)(void);
} io_transport_t;

/* Milliseconds on QueryPerformanceCounter: the clock control ticks are
 * scheduled and stamped on. */
double io_clock_ms(void);

/* Look a transport up by the name in `[controls] backend`, or NULL. */
const io_transport_t *io_transport_find(const char *name);

/* Sample host input into the board's logical cabinet state. The synthesising
 * transport calls it; one reading a real board does not, which is the split. */
void io_board_sample_input(void);

/* ---- derived pad rumble (rumble.c) --------------------------------------
 *
 * The ONE invented effect in this project: a pad has no force axis, so the
 * DAC torque is described to its two motors. Off unless [ffb] rumble, and
 * never reaches a wheel. Read rumble.c's header first. */
void     rumble_preload(const char *ini);
int      rumble_active(void);
/* Say which source the pad ended up on. At INSTALL: the profile's anchors do
 * not exist at preload, so asking there reports "none" for every title. */
void     rumble_report_source(void);
/* The same force di_set_force() gets, -1..+1, once per control tick and
 * stamped like ffb_events_poll(), with whether BOOST is held: the pad's boost
 * path is attributed by the button. The steering POSITION is not wanted: the
 * spring is removed by high-passing, which works for a stick that is held as
 * well as a wheel that is swept. */
void     rumble_set_force(float force, double when_ms, int boost_held);
/* One line for the ffb_trace window; resets it. */
void     rumble_census(char *out, size_t size);
void     rumble_close(void);

/* ---- the game's own feedback events (ffb_events.c) -----------------------
 *
 * io_dac_out is the SUM of named units and a pad cannot use a sum; this reads
 * the units. Read ffb_events.c's header before building on it.
 *
 * At INSTALL, not preload: it names cells out of the runtime profile, and the
 * profile is not chosen until the image is mapped. */
void ffb_events_install(void);

/* WHAT THE PAD CONSUMES. Each title fills these from the cells it has:
 *
 *   Hydro    impact <- the COLLIDE flag and the two decaying impulses
 *   Offroad  impact <- _ffbnew_CurHitVal, taken at the source by the hook
 *            texture <- the four per-tire jolts, r = -0.42..-0.67 vs the force
 *            body    <- the suspension, alive but weak, so a slow bed only
 *
 * THE STEERING SPRING IS ABSENT FROM ALL OF THEM, which is the entire reason
 * for reading the units instead of the byte. */
typedef struct {
    /* TWO DIFFERENT NOTHINGS. `anchored`: does this profile carry the cells
     * at all. `have`: do they mean anything right now. No anchors means the
     * byte is the ONLY source, at full range; anchors whose path is down
     * (Offroad's menus) means the byte stands in for trimmed channels and must
     * be capped to match. Collapsing the two made every anchor-less title as
     * quiet as a menu. */
    int   anchored;
    int   have;
    float impact;               /* -1..+1, ONE-SHOT: consumed by reading */
    float texture;              /* 0..1, continuous                      */
    float body;                 /* 0..1, slow                            */
    /* 0..1: everything synthesised as fast oscillation, MOTOR, BOOSTER,
     * cavitation, nitro. None is stored in a cell, so this is taken off the
     * DAC byte high-passed at ~8 Hz: above anything an arm does to the
     * steering, below the rate these units run at. */
    float engine;
    /* Hydro's booster AVAILABILITY state, raw. Measured not to change while
     * boosting, so nothing drives a motor from it; the trace reports it. */
    int   boost;
    /* Whether boost is FIRING, from the game's own fuel or flag (game.h's
     * boost_* chain). `boost_known` is 0 when the profile has no chain, and the
     * pad then falls back to the button. */
    int   boost_known;
    int   boosting;
} ffb_event_state_t;

int  ffb_events_take(ffb_event_state_t *out);
/* The channels rumble.c's last step consumed and the motors it commanded,
 * and whether it took the events path. For [diagnostics] ffb_record. */
int  rumble_record_state(ffb_event_state_t *ev, unsigned *lo, unsigned *hi);
int  ffb_events_active(void);
/* One control tick, with the force commanded on it: every slot is correlated
 * against that, which identifies the real sources without a driver marking by
 * hand. `when_ms` is the tick's stamp on io_clock_ms(); intervals come from
 * it, because ticks owed by one pass run back to back. */
void ffb_events_poll(float force, double when_ms);

#endif /* VCT_IO_H */
