/* io.h -- the cabinet I/O boundary, split four ways.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 *   io_config.c   the control map. The ini, the MAME key names, the axis map,
 *                 and the per-title defaults. Knows nothing about boards.
 *   io_host.c     host devices -> host_state_t. XInput, WinMM, DirectInput and
 *                 the keyboard, resolved against the map above into ONE
 *                 provider-neutral reading. Knows nothing about boards either.
 *   io_board.c    host_state_t -> a wire frame, in whichever of the two
 *                 formats the title's board uses, and the TRANSPORT that
 *                 frame arrives through.
 *   diego.c       the game's own comm-layer hooks, the frame-boundary seam and
 *                 the poll thread. Nothing in it knows a key name or a byte
 *                 offset in a packet.
 *
 * The direction of dependency is one way: config <- host <- board <- diego.
 */
#ifndef VCT_IO_H
#define VCT_IO_H

#include "vcthunder.h"

/* Longest board frame any supported title uses: Diego 8, MagicBus 16. One
 * buffer size for both, so the frame length is a property of the payload
 * builder and never of the caller. */
#define IO_PACKET_MAX   16

/* ---- the control map (io_config.c) -------------------------------------- */

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

typedef enum {
    PEDALS_COMBINED = 0,
    PEDALS_SEPARATE
} pedal_mode_t;

typedef struct {
    /* MAME keyboard defaults. */
    int coin1, coin2, start1, service1, service_mode;
    int steer_left, steer_right, throttle, brake;
    int boost, view_high, view_low, view_pilot;
    int volume_down, volume_up;
    int shift1, shift2, shift3;          /* Offroad's 3-speed shifter */

    input_provider_t provider;
    unsigned xinput_device, winmm_device;
    unsigned deadzone_percent;

    axis_id_t xi_steer, xi_throttle, xi_brake;
    int xi_steer_invert, xi_throttle_invert, xi_brake_invert;

    axis_id_t mm_steer, mm_throttle, mm_brake;
    pedal_mode_t mm_pedals;
    int mm_steer_invert, mm_throttle_invert, mm_brake_invert;

    /* Logical button numbers: XInput A/B/X/Y/LB/RB/Back/Start are 1..8; WinMM
     * and DirectInput both use their native one-based button ordinals, which on
     * a wheel are the numbers its own control panel shows. Zero means unbound,
     * and 32 is the ceiling because a board switch line is a bit in a mask. */
    unsigned pad_boost, pad_view_high, pad_view_low, pad_view_pilot;
    unsigned pad_coin1, pad_coin2, pad_start1;
    unsigned pad_service1, pad_service_mode, pad_volume_down, pad_volume_up;
    unsigned pad_shift1, pad_shift2, pad_shift3;
    /* Offroad's gate as a pair of paddles. See sequential_shift(). */
    unsigned pad_shift_up, pad_shift_down;
    /* A wheel's hat is at thumb height and a full-lock override on it is a
     * crash; a pad's D-pad IS the steering when there is no stick worth using.
     * So the override is a key, defaulting to the pad's behaviour. */
    int pov_digital;
} controls_config_t;

/* Read the map. `ini` is the pack's file; per-title sections win, through
 * ini_read_scoped()'s precedence rule. */
void io_config_load(const char *ini);
/* The loaded map. Never NULL: io_config_load() installs defaults first. */
const controls_config_t *io_config(void);
/* One block naming every resolved binding, at install. */
void io_config_report(void);
/* [diagnostics] ffb_trace: census the force the GAME commanded. Read with the
 * rest of the file here, consumed by the force feedback in io_board.c. */
int  io_config_ffb_trace(void);

/* Which of the two boards this title carries, from the profile rather than
 * from a cached copy: MagicBus has the three handshake toggles and Diego does
 * not. One spelling of the question, because the key defaults need it before
 * install and the payload builder needs it after. */
int io_board_is_magicbus(void);

/* ---- host devices (io_host.c) ------------------------------------------- */

typedef struct {
    int present;
    input_provider_t provider;
    float steer;                         /* -1 full left .. +1 full right */
    float drive;                         /* -1 reverse .. +1 throttle    */
    unsigned buttons;                    /* logical one-based ordinals    */
    int left, right, up, down;            /* POV/D-pad digital overrides   */
} host_state_t;

/* A host axis is nominally -1..+1 and nothing downstream may assume it.
 * Inline here because both sides of the seam clamp: io_host.c after a
 * deadzone, io_board.c before encoding to a board byte. */
static inline float io_clamp_unit(float v)
{
    if (v < -1.0f) return -1.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* ---- one deadzone, and one pedal law ------------------------------------
 *
 * There were four copies of this arithmetic -- dinput.c, io_host.c's XInput
 * and WinMM paths, and the launcher's live preview -- and they did not agree.
 *
 * A pedal RESTS AT ITS AXIS MAXIMUM on every wheel this was written against,
 * which is why both pedals default to inverted (dinput.c). The inversion has
 * to happen BEFORE the deadzone. Applied the other way round the band is eaten
 * at the FLOORED end and the resting end gets none: a 12% deadzone leaves a
 * released pedal reading 1 - dz(0.95) = 0.057 instead of 0, so it creeps.
 * dinput.c and the launcher had the order right; io_host.c's two providers had
 * it backwards, and because the LAUNCHER agreed with dinput.c its live preview
 * read correct while WinMM did not.
 *
 * Inline here because all four callers are on the input path and none of them
 * may reach for its own. */
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

/* Two pedals on their own axes, each 0..1 as the driver reports it, into one
 * -1 (full brake) .. +1 (full throttle) drive value. */
static inline float io_pedals_separate(float throttle, float brake,
                                       int throttle_invert, int brake_invert,
                                       float deadzone)
{
    if (throttle_invert) throttle = 1.0f - throttle;
    if (brake_invert)    brake    = 1.0f - brake;
    return io_deadzone_unipolar(throttle, deadzone) -
           io_deadzone_unipolar(brake, deadzone);
}

/* Load XInput and WinMM. Same ordering rule as the rest of the preloads: this
 * must run before game_map(), because LoadLibrary stops working afterwards. */
void io_host_preload(void);
/* Re-evaluate the [controls] input_background policy and log a transition. */
void io_host_refresh_active(void);
int  io_host_active(void);
int  io_host_key_down(int vk);
/* One reading from whichever provider answers, already resolved against the
 * axis map. Returns 0 when there is no analogue device. */
int  io_host_sample(host_state_t *out);

/* ---- the board and its transport (io_board.c) --------------------------- */

/* Logical cabinet buttons, named once so each board's packer can place them
 * where ITS wiring puts them. Hydro and Offroad do not agree on a single bit:
 * Offroad's byte 0 uses only its top four lines (credit, vol down, vol up,
 * test) and puts everything a player touches on switch byte 1, which was not
 * emulated at all until the layout below was read out of the operator menu's
 * own name table. See build_payload_mb(). */
enum {
    /* Not a switch line at all: the coin slots are 3-bit counters and the
     * shifter paddles drive a latch. src/bindings.def gives them this so that
     * every control has a `button` column and none has to be special-cased. */
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

/* WHERE A BOARD FRAME COMES FROM.
 *
 * The synthesised board is one implementation of this and was, until now, the
 * only conceivable one: it was not behind an interface, it *was* the code
 * path. M6 needs two more: a real Diego or MagicBus over its own link, and a
 * replayer for recorded cabinet traffic (roadmap §11.1 names the replayer as a
 * deliverable in its own right, because captured traffic from someone else's
 * cabinet would unblock most of M6 and is far cheaper than a board).
 *
 * Neither should have to touch the poll thread, the frame-boundary seam, the
 * game's comm-layer hooks or the host input map in order to exist. So they do
 * not: a transport supplies frames and consumes the game's outbound direction,
 * and `[controls] backend` picks one by name.
 */
typedef struct {
    const char *name;
    /* Bring the link up. `dips` is the cabinet DIP byte the operator set;
     * a real board reads its own and may ignore this. 0 on failure. */
    int      (*open)(unsigned dips);
    /* The next frame the board would have sent, into `frame`. Returns its
     * length in bytes, or 0 to produce nothing this cycle. */
    unsigned (*receive)(unsigned char *frame, unsigned max);
    /* The game's outbound direction, once per cycle, AFTER its control-driver
     * callback has run, which is when the DAC cell holds this frame's force.
     * The synthesised board reads that cell itself; a real link sends it. */
    void     (*transmit)(void);
    void     (*close)(void);
} io_transport_t;

/* Look a transport up by the name in `[controls] backend`, or NULL. */
const io_transport_t *io_transport_find(const char *name);

/* Sample host input into the board's logical cabinet state. Called by the
 * transport that synthesises frames; a transport reading a real board does not
 * need it, which is the point of the split. */
void io_board_sample_input(void);

#endif /* VCT_IO_H */
