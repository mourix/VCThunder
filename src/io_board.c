/* io_board.c -- a host reading becomes a board frame, and the transport for it.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Two wire formats (Hydro's Diego, Offroad's MagicBus) and the force-feedback
 * direction that shares their send buffer. The synthesised board is one
 * TRANSPORT among the several M6 will need; see the io_transport_t comment in
 * src/io.h.
 */
#include "vcthunder.h"
#include "io.h"

#include <stdio.h>
#include <string.h>

/* These two unnamed .bss bytes are the protocol handshake fields. They are the
 * only intentional non-generated addresses in shim/src: the HT-Diego
 * decompilation identified them, and xref analysis verified that only the
 * send/receive protocol functions touch them. Echoing the game's current bits
 * makes the emulated board self-synchronising. */
#define DG_PC_INIT_BIT  0x00603951u   /* bit 6: initialise, echoed by board */
#define DG_PC_BIT4      0x00603934u   /* bit 4: liveness toggle, echoed    */

#define s_cfg (*io_config())

/* The cabinet DIP byte the operator set, raw and active-low. */
static unsigned char s_dips = 0xFF;

/* Live input, packed into the next board payload. */
static unsigned char s_steer = 0x80;
static unsigned char s_throttle = 0x80;
static unsigned s_buttons;               /* BTN_* set, 1 = pressed        */
static unsigned char s_coin1, s_coin2;
static int s_coin1_held, s_coin2_held;
/* The gear the paddles have selected, 1..3, or 0 for neutral. */
static unsigned s_gear;

/* Bin commanded force by wheel position to distinguish input-range faults from
 * the game's own force curve. */
#define FFB_BINS 8
static unsigned s_ffb_samples, s_ffb_nonzero;
static int s_ffb_min = 127, s_ffb_max = -128;
static unsigned s_steer_min = 255, s_steer_max;
static unsigned s_ffb_bin_n[FFB_BINS];
static unsigned long s_ffb_bin_sum[FFB_BINS];
static DWORD s_ffb_reported;
#define s_ffb_trace (io_config_ffb_trace())

static unsigned char rd8(unsigned a)
{
    return *(volatile unsigned char *)(uintptr_t)a;
}

/* --- INI parsing ------------------------------------------------------- */
static int pad_down(const host_state_t *host, unsigned ordinal)
{
    return ordinal >= 1 && ordinal <= 32 &&
           (host->buttons & (1u << (ordinal - 1))) != 0;
}

static unsigned char encode_steering(float v)
{
    int n;
    v = io_clamp_unit(v);
    n = v < 0.0f ? 128 + (int)(v * 128.0f)
                 : 128 + (int)(v * 127.0f);
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    return (unsigned char)n;
}

static unsigned char encode_throttle(float v)
{
    int n = (int)(128.0f + io_clamp_unit(v) * 127.0f);
    if (n < 1) n = 1;
    if (n > 254) n = 254;
    return (unsigned char)n;
}

/* Convert momentary paddle edges into Offroad's maintained three-position
 * shifter state. The latch starts in neutral and remains inactive when the
 * paddle bindings are absent. */
static unsigned sequential_shift(const host_state_t *host)
{
    static int up_held, down_held;
    int up, down;

    if (!s_cfg.pad_shift_up && !s_cfg.pad_shift_down)
        return 0;
    up = pad_down(host, s_cfg.pad_shift_up);
    down = pad_down(host, s_cfg.pad_shift_down);
    if (up && !up_held && s_gear < 3)
        s_gear++;
    if (down && !down_held && s_gear > 0)
        s_gear--;
    up_held = up;
    down_held = down;

    switch (s_gear) {
    case 1: return BTN_SHIFT1;
    case 2: return BTN_SHIFT2;
    case 3: return BTN_SHIFT3;
    default: return 0;               /* neutral is the absence of a line */
    }
}

/* Coin slots are 3-bit counters that loop 1..7 and never return to zero after
 * first use. The game counts transitions, so one credit is one rising edge. */
static void coin_edge(int pressed, int *held, unsigned char *counter, int slot)
{
    if (pressed && !*held) {
        *counter = (unsigned char)(*counter >= 7 ? 1 : *counter + 1);
        LOGI("io: coin %d inserted (counter now %u)", slot, *counter);
    }
    *held = pressed;
}

static void sample_input(void)
{
    host_state_t host;
    int left, right, up, down;
    static int last_steer = -1, last_throttle = -1;
    static unsigned last_buttons = ~0u;

    io_host_refresh_active();

    io_host_sample(&host);
    s_steer = host.present ? encode_steering(host.steer) : 0x80;
    s_throttle = host.present ? encode_throttle(host.drive) : 0x80;

    /* MAME analogue fields have digital increment/decrement inputs. Keyboard
     * arrows and controller POV/D-pad take full-lock priority over the axis. */
    left = io_host_key_down(s_cfg.steer_left) || (s_cfg.pov_digital && host.left);
    right = io_host_key_down(s_cfg.steer_right) || (s_cfg.pov_digital && host.right);
    up = io_host_key_down(s_cfg.throttle) || (s_cfg.pov_digital && host.up);
    down = io_host_key_down(s_cfg.brake) || (s_cfg.pov_digital && host.down);
    if (left != right)
        s_steer = left ? 0x00 : 0xFF;
    if (up != down)
        s_throttle = up ? 0xFE : 0x01;

    /* Logical buttons only, from src/bindings.def. Which physical line each one
     * lands on is the BOARD's business, not the host's; see
     * build_payload_diego/_mb.
     *
     * A row whose `button` is BTN_NONE contributes nothing here and the whole
     * test folds away: the coin slots are counters (coin_edge, below) and the
     * shifter paddles drive a latch (sequential_shift). */
    s_buttons = 0;
#define BINDING(key, hydro, offroad, pad_default, button, hl, ol)             \
    if ((button) != BTN_NONE &&                                               \
        (io_host_key_down(s_cfg.key) || pad_down(&host, s_cfg.pad_##key)))    \
        s_buttons |= (button);
#define BINDING_AXIS(key, hydro, offroad, hl, ol)     /* the axes, above */
#define BINDING_PAD(key, pad_default, hl, ol)         /* the latch, below */
#include "bindings.def"
#undef BINDING
#undef BINDING_AXIS
#undef BINDING_PAD
    s_buttons |= sequential_shift(&host);

    coin_edge(io_host_key_down(s_cfg.coin1) || pad_down(&host, s_cfg.pad_coin1),
              &s_coin1_held, &s_coin1, 1);
    coin_edge(io_host_key_down(s_cfg.coin2) || pad_down(&host, s_cfg.pad_coin2),
              &s_coin2_held, &s_coin2, 2);

    if (last_steer != s_steer || last_throttle != s_throttle ||
        last_buttons != s_buttons) {
        LOGT("io: host state steer=%3u throttle=%3u buttons=0x%03X",
             s_steer, s_throttle, s_buttons);
        last_steer = s_steer;
        last_throttle = s_throttle;
        last_buttons = s_buttons;
    }
}

/* Read the signed force command from board DAC channel 0. Both titles encode
 * force as clamp(force * -128); configuration controls only motor direction. */
static void update_force_feedback(void)
{
    int raw;

    if (!G->io_dac_out)
        return;
    raw = (int)(signed char)rd8(G->io_dac_out);
    di_set_force(-(float)raw / 128.0f);

    if (!s_ffb_trace)
        return;
    s_ffb_samples++;
    if (raw)
        s_ffb_nonzero++;
    if (raw < s_ffb_min) s_ffb_min = raw;
    if (raw > s_ffb_max) s_ffb_max = raw;
    if (s_steer < s_steer_min) s_steer_min = s_steer;
    if (s_steer > s_steer_max) s_steer_max = s_steer;
    {
        unsigned bin = (unsigned)s_steer * FFB_BINS / 256u;
        s_ffb_bin_n[bin]++;
        s_ffb_bin_sum[bin] += (unsigned long)(raw < 0 ? -raw : raw);
    }
    {
        DWORD now = GetTickCount();
        if (!s_ffb_reported)
            s_ffb_reported = now;
        else if ((DWORD)(now - s_ffb_reported) >= 5000) {
            char curve[128];
            unsigned i;
            int n = 0;

            /* A window of all zeroes is the answer to "is the wheel broken or
             * is the game not asking?", which is the first question every FFB
             * report needs and the only one a screenshot cannot settle. */
            LOGI("io: ffb window %u samples, %u nonzero (%u%%), dac %d..%d, "
                 "steer %u..%u, peak sent to DirectInput %ld of 10000",
                 s_ffb_samples, s_ffb_nonzero,
                 s_ffb_samples ? s_ffb_nonzero * 100 / s_ffb_samples : 0,
                 s_ffb_min, s_ffb_max, s_steer_min, s_steer_max,
                 di_peak_force());
            curve[0] = '\0';
            for (i = 0; i < FFB_BINS; i++)
                n += snprintf(curve + n, sizeof curve - (size_t)n, "%s%lu",
                              i ? " " : "",
                              s_ffb_bin_n[i] ? s_ffb_bin_sum[i] / s_ffb_bin_n[i]
                                             : 0ul);
            /* Left lock to right lock in eight steps. The middle two bins are
             * the wheel around centre, which is where a dead band is felt. */
            LOGI("io: ffb curve  mean |force| by wheel position L->R: %s",
                 curve);
            s_ffb_samples = s_ffb_nonzero = 0;
            s_ffb_min = 127;
            s_ffb_max = -128;
            s_steer_min = 255;
            s_steer_max = 0;
            memset(s_ffb_bin_n, 0, sizeof s_ffb_bin_n);
            memset(s_ffb_bin_sum, 0, sizeof s_ffb_bin_sum);
            s_ffb_reported = now;
        }
    }
}

/* --- packet synthesis ------------------------------------------------- */

static void build_payload_diego(unsigned char *p)
{
    unsigned sw;

    memset(p, 0, IO_PACKET_MAX);
    p[0] = (unsigned char)((rd8(DG_PC_INIT_BIT) & 0x40) |
                           (rd8(DG_PC_BIT4) & 0x10));
    p[1] = 0;
    p[2] = (unsigned char)((s_coin1 & 7) | ((s_coin2 & 7) << 3));
    p[3] = s_steer;
    p[4] = s_throttle;
    p[5] = s_dips;
    /* Hydro's single switch byte. It has no separate cabinet Start switch(
     * Boost starts and confirms), so both feed line 0. */
    sw = 0;
    if (s_buttons & (BTN_START | BTN_BOOST)) sw |= 0x01;
    if (s_buttons & BTN_VIEW1)               sw |= 0x02;
    if (s_buttons & BTN_VIEW2)               sw |= 0x04;
    if (s_buttons & BTN_VIEW3)               sw |= 0x08;
    if (s_buttons & BTN_SERVICE)             sw |= 0x10;
    if (s_buttons & BTN_VOL_DN)              sw |= 0x20;
    if (s_buttons & BTN_VOL_UP)              sw |= 0x40;
    if (s_buttons & BTN_TEST)                sw |= 0x80;
    p[6] = (unsigned char)~sw;            /* physical lines are active-low */
    p[7] = 0;
}

/* Build Offroad's 16-byte MagicBus frame. Bytes 1-2 carry five packed 3-bit
 * coin counters; byte 4 carries service switches; bytes 5-8 carry ADC values;
 * bytes 9-14 carry gameplay switches. Neutral is represented by releasing all
 * three maintained gear lines. */
static void build_payload_mb(unsigned char *p)
{
    unsigned c0 = s_coin1 & 7, c1 = s_coin2 & 7;
    unsigned gas, brake, sw0, sw1;

    memset(p, 0, IO_PACKET_MAX);

    /* Echo the three handshake toggles back unchanged. Each one is compared
     * against the value the game last transmitted, and equality is what retires
     * the pending request; sending anything else leaves eeprom reads pending
     * forever. Reading them back out of the game's own state is the only way to
     * be right without modelling the board's side of the exchange. */
    p[0] = (unsigned char)((rd8(G->io_ack_write) & 0x10) |
                           (rd8(G->io_ack_security) & 0x20) |
                           (rd8(G->io_ack_read) & 0x40));

    p[1] = (unsigned char)(((c1 >> 2) & 1));      /* c2 bit2 lives here       */
    p[2] = (unsigned char)((c0 & 7) | ((c1 & 3) << 6));
    p[3] = 0;                                     /* eeprom read data         */

    /* One combined drive axis on the host becomes separate gas and brake
     * pedals, split around centre. Keyboard full-lock (0xFE / 0x01) therefore
     * still reaches full gas and full brake. */
    gas   = s_throttle > 0x80 ? (unsigned)(s_throttle - 0x80) * 2 : 0;
    brake = s_throttle < 0x80 ? (unsigned)(0x80 - s_throttle) * 2 : 0;
    if (gas > 0xFF)   gas = 0xFF;
    if (brake > 0xFF) brake = 0xFF;

    sw0 = 0;
    if (s_buttons & BTN_SERVICE)  sw0 |= 0x10;    /* CREDIT                    */
    if (s_buttons & BTN_VOL_DN)   sw0 |= 0x20;
    if (s_buttons & BTN_VOL_UP)   sw0 |= 0x40;
    if (s_buttons & BTN_TEST)     sw0 |= 0x80;

    sw1 = 0;
    if (s_buttons & BTN_VIEW1)    sw1 |= 0x01;    /* SLAM CAM                  */
    if (s_buttons & BTN_VIEW2)    sw1 |= 0x02;    /* CHASE CAM                 */
    if (s_buttons & BTN_VIEW3)    sw1 |= 0x04;    /* CHOPPER CAM               */
    if (s_buttons & BTN_START)    sw1 |= 0x08;
    if (s_buttons & BTN_BOOST)    sw1 |= 0x10;    /* NITRO                     */
    if (s_buttons & BTN_SHIFT1)   sw1 |= 0x20;
    if (s_buttons & BTN_SHIFT2)   sw1 |= 0x40;
    if (s_buttons & BTN_SHIFT3)   sw1 |= 0x80;

    p[4] = (unsigned char)~sw0;                   /* switch byte 0, active-low */
    p[5] = s_steer;                               /* ADC0 */
    p[6] = (unsigned char)gas;                    /* ADC1 */
    p[7] = (unsigned char)brake;                  /* ADC2 */
    p[8] = 0x80;                                  /* ADC3 */
    memset(p + 9, 0xFF, 6);                       /* switch bytes 1..6, idle   */
    p[9] = (unsigned char)~sw1;                   /* switch byte 1, active-low */
    p[15] = 0;
}

static void build_payload(unsigned char *p)
{
    if (io_board_is_magicbus())
        build_payload_mb(p);
    else
        build_payload_diego(p);
}
void io_board_sample_input(void)
{
    sample_input();
}

/* ---- the transports ----------------------------------------------------- */

/* `sim`, the board this project has always emulated: no hardware, no link,
 * frames synthesised from host input and the game's own handshake bits echoed
 * back so each request retires. Its transmit direction is the DAC cell, which
 * is the only Tx cell anything reads. */
static int sim_open(unsigned dips)
{
    s_dips = (unsigned char)dips;
    return 1;
}

/* Builds only: it does NOT sample.
 *
 * The poll thread samples host input at its own cadence (~60 Hz) whether or
 * not it is the thread that delivers frames, and the frame boundary delivers
 * at the game's frame rate (~30 Hz). Folding the sample into the build here
 * would have halved the input rate on every title whose frame boundary owns
 * dispatch, which is both of them. */
static unsigned sim_receive(unsigned char *frame, unsigned max)
{
    unsigned len = io_board_is_magicbus() ? 16u : 8u;

    if (max < len)
        return 0;
    build_payload(frame);
    return len;
}

static void sim_transmit(void)
{
    update_force_feedback();
}

static void sim_close(void)
{
}

static const io_transport_t k_transport_sim = {
    "sim", sim_open, sim_receive, sim_transmit, sim_close
};

static const io_transport_t *const k_transports[] = {
    &k_transport_sim,
    NULL
};

const io_transport_t *io_transport_find(const char *name)
{
    unsigned i;

    if (!name || !*name)
        name = "sim";
    for (i = 0; k_transports[i]; i++)
        if (_stricmp(k_transports[i]->name, name) == 0)
            return k_transports[i];
    return NULL;
}
