/* io_board.c -- a host reading becomes a board frame, and the transport for it.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Two wire formats (Hydro's Diego, Offroad's MagicBus) and the force-feedback
 * direction that shares their send buffer. The synthesised board is one
 * TRANSPORT among several; see the io_transport_t comment in src/io.h.
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
/* ANY OF THE DEVICES THE BINDING NAMES, which is io.h's io_pad_down(). It was
 * io_button_down() on one mask and one ordinal until Phase 4; the accessor
 * moved rather than this call site, so the arithmetic stays in one place. */
static int pad_down(const host_state_t *host, const io_pad_bind_t *bind)
{
    return io_pad_down(host->buttons, bind);
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
 * paddle bindings are absent. On a select screen the paddles are the menu's
 * (menu_prev/menu_next share their buttons), so the latch follows them
 * without moving: a gear chosen there would be the gear the race starts in,
 * and a paddle still held when the screen goes is not a new press. */
static unsigned sequential_shift(const host_state_t *host)
{
    int frozen = menu_wheel_screen_up();
    static int up_held, down_held;
    int up, down;

    if (!io_pad_bound(&s_cfg.pad_shift_up) &&
        !io_pad_bound(&s_cfg.pad_shift_down))
        return 0;
    up = pad_down(host, &s_cfg.pad_shift_up);
    down = pad_down(host, &s_cfg.pad_shift_down);
    if (up && !up_held && s_gear < 3 && !frozen)
        s_gear++;
    if (down && !down_held && s_gear > 0 && !frozen)
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

/* The select screens (menu_wheel.c): the keys, the
 * hat, the D-pad and a pad's stick step them one item a press, and a wheel
 * turned away from where it was at the press takes them back. A wheel's hat
 * steps them whatever pov_digital says; that rule protects the race, which
 * this never touches. While the keys own a screen the wheel gets no force,
 * or the game's menu spring would turn it into a takeover. */
static volatile int s_menu_mute;

static unsigned char menu_steer(const host_state_t *host, unsigned char byte)
{
    int mute;

    byte = (unsigned char)menu_wheel_steer(host, byte, &mute);
    s_menu_mute = mute;
    return byte;
}

/* EACH FIELD IS PUBLISHED WITH ONE STORE. This runs on the poll thread and
 * the frame is built on the game's, so a field written twice, or cleared and
 * then filled in, can be sent half made: a held key's full lock in the middle
 * of a select screen's step, or a held button released for one frame. Each is
 * built in a local and stored once at the end. */
static void sample_input(void)
{
    host_state_t host;
    int left, right, up, down, pov;
    unsigned char steer, throttle;
    unsigned buttons;
    static int last_steer = -1, last_throttle = -1;
    static unsigned last_buttons = ~0u;

    io_host_refresh_active();

    io_host_sample(&host);
    steer = host.present ? encode_steering(host.steer) : 0x80;
    throttle = host.present ? encode_throttle(host.drive) : 0x80;

    /* MAME analogue fields have digital increment/decrement inputs. Keyboard
     * arrows and controller POV/D-pad take full-lock priority over the axis. */
    pov = io_config_pov_digital();
    left = io_key_down(&s_cfg.steer_left) || (pov && host.left);
    right = io_key_down(&s_cfg.steer_right) || (pov && host.right);
    up = io_key_down(&s_cfg.throttle) || (pov && host.up);
    down = io_key_down(&s_cfg.brake) || (pov && host.down);
    if (left != right)
        steer = left ? 0x00 : 0xFF;
    if (up != down)
        throttle = up ? 0xFE : 0x01;
    steer = menu_steer(&host, steer);

    /* Logical buttons only, from src/bindings.def; which physical line each
     * lands on is the BOARD's business. A row whose `button` is BTN_NONE folds
     * away here: coin slots are counters (coin_edge) and shifter paddles drive
     * a latch (sequential_shift). */
    buttons = 0;
#define BINDING(key, hydro, offroad, arctic, pad_hydro, pad_offroad,       \
                pad_arctic, button, hl, ol, al)                               \
    if ((button) != BTN_NONE &&                                               \
        (io_key_down(&s_cfg.key) || pad_down(&host, &s_cfg.pad_##key))) \
        buttons |= (button);
#define BINDING_AXIS(key, hydro, offroad, arctic, hl, ol, al) /* the axes, above */
#define BINDING_PAD(key, pad_hydro, pad_offroad, pad_arctic, hl, ol, al) \
    /* the latch, below */
#include "bindings.def"
#undef BINDING
#undef BINDING_AXIS
#undef BINDING_PAD
    buttons |= sequential_shift(&host);
    s_steer = steer;
    s_throttle = throttle;
    s_buttons = buttons;

    coin_edge(io_key_down(&s_cfg.coin1) || pad_down(&host, &s_cfg.pad_coin1),
              &s_coin1_held, &s_coin1, 1);
    coin_edge(io_key_down(&s_cfg.coin2) || pad_down(&host, &s_cfg.pad_coin2),
              &s_coin2_held, &s_coin2, 2);

    if (last_steer != s_steer || last_throttle != s_throttle ||
        last_buttons != s_buttons) {
        /* THE ONE LINE THAT SAYS WHAT THE CABINET ACTUALLY RECEIVED. Behind
         * `log_level = trace` it shares a level with stubs.c's PER-GLIDE-CALL
         * tracer, which is 1.6 million lines and 64 MB on one race, of which
         * the wanted ones are 0.07%.
         *
         * So it answers to `input_trace` at INFO, on change only. An instrument
         * you have to RACE with cannot live behind a global verbosity level. */
        log_printf(io_config_input_trace() ? LOG_INFO : LOG_TRACE,
                   "io: host state steer=%3u throttle=%3u buttons=0x%03X",
                   s_steer, s_throttle, s_buttons);
        last_steer = s_steer;
        last_throttle = s_throttle;
        last_buttons = s_buttons;
    }
    menu_wheel_trace(s_steer);
}

/* --- the wheel's queue ---------------------------------------------------
 *
 * A pass that owes two control ticks runs both at once, but the board sent
 * each tick's force on its own timer, so the wheel gets the first now and the
 * rest at their stamps. The timer wakes the poll thread (diego.c poll_wait()),
 * which calls sim_deliver(). The lock serialises di_set_force() between that
 * thread and the game's. */
#define WHEEL_QUEUE 4u

typedef struct {
    float  force;
    double when_ms;
} wheel_cmd_t;

static CRITICAL_SECTION s_wheel_lock;
static int s_wheel_lock_ready;
static HANDLE s_wheel_timer;
static wheel_cmd_t s_wheel_q[WHEEL_QUEUE];
static unsigned s_wheel_head, s_wheel_n;

/* The spacing of what was actually sent, for the ffb_trace window: the only
 * evidence the queue delivers at the board's cadence, wheel or no wheel. */
static unsigned s_wsent;
static double s_wlast, s_wgap_min, s_wgap_max, s_wgap_sum;

/* Lock held, or no lock yet. */
static void wheel_send(float force)
{
    double now = io_clock_ms();

    if (s_wsent || s_wlast) {
        double gap = now - s_wlast;

        if (!s_wsent || gap < s_wgap_min) s_wgap_min = gap;
        if (!s_wsent || gap > s_wgap_max) s_wgap_max = gap;
        s_wgap_sum += gap;
        s_wsent++;
    }
    s_wlast = now;
    di_set_force(force);
}

/* Lock held. */
static void wheel_pop_send(void)
{
    wheel_send(s_wheel_q[s_wheel_head].force);
    s_wheel_head = (s_wheel_head + 1u) % WHEEL_QUEUE;
    s_wheel_n--;
}

/* Lock held. */
static void wheel_arm(void)
{
    LARGE_INTEGER due;
    double left;

    if (!s_wheel_n || !s_wheel_timer)
        return;
    left = s_wheel_q[s_wheel_head].when_ms - io_clock_ms();
    /* Relative, in 100 ns units; never 0, which would mean "absolute 0". */
    due.QuadPart = left > 0.0 ? -(LONGLONG)(left * 10000.0) : -1;
    if (!due.QuadPart)
        due.QuadPart = -1;
    SetWaitableTimer(s_wheel_timer, &due, 0, NULL, NULL, FALSE);
}

static void wheel_submit(float force, double when_ms)
{
    if (!s_wheel_lock_ready) {
        wheel_send(force);
        return;
    }
    EnterCriticalSection(&s_wheel_lock);
    if (!s_wheel_timer || when_ms <= io_clock_ms() + 0.5) {
        /* Due now: anything still queued is older, so it goes first. */
        while (s_wheel_n)
            wheel_pop_send();
        wheel_send(force);
    } else {
        if (s_wheel_n == WHEEL_QUEUE)
            wheel_pop_send();
        s_wheel_q[(s_wheel_head + s_wheel_n) % WHEEL_QUEUE].force = force;
        s_wheel_q[(s_wheel_head + s_wheel_n) % WHEEL_QUEUE].when_ms = when_ms;
        s_wheel_n++;
        wheel_arm();
    }
    LeaveCriticalSection(&s_wheel_lock);
}

/* [diagnostics] ffb_record: one CSV row per control tick, the force, the
 * channels the pad model consumed, the motors it commanded and the BOOST
 * button, so the model can be replayed offline against a real race. */
static FILE *s_rec;
static unsigned s_rec_rows;
static int s_rec_failed;

static void record_tick(double when_ms, float force)
{
    const char *path = io_config_ffb_record();
    ffb_event_state_t ev;
    unsigned lo, hi;
    int events;

    if (!path || s_rec_failed)
        return;
    if (!s_rec) {
        s_rec = fopen(path, "w");
        if (!s_rec) {
            s_rec_failed = 1;
            LOGW("io: ffb_record: cannot open '%s'", path);
            return;
        }
        fprintf(s_rec, "t_ms,force,events,have,impact,texture,body,engine,"
                       "lo,hi,boost_btn,boost_known,boosting\n");
        LOGI("io: ffb_record: writing every control tick to '%s'", path);
    }
    events = rumble_record_state(&ev, &lo, &hi);
    fprintf(s_rec, "%.2f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.4f,%u,%u,%d,%d,%d\n",
            when_ms, (double)force, events, ev.have, (double)ev.impact,
            (double)ev.texture, (double)ev.body, (double)ev.engine, lo, hi,
            (s_buttons & BTN_BOOST) ? 1 : 0, ev.boost_known, ev.boosting);
    if (++s_rec_rows % 64u == 0)
        fflush(s_rec);
}

/* Read the signed force command from board DAC channel 0. Both titles encode
 * force as clamp(force * -128); configuration controls only motor direction.
 * Runs once per control tick, right after the callback that wrote it. */
static void update_force_feedback(double when_ms)
{
    int raw;

    if (!G->io_dac_out)
        return;
    raw = (int)(signed char)rd8(G->io_dac_out);
    wheel_submit(s_menu_mute ? 0.0f : -(float)raw / 128.0f, when_ms);
    /* THE UNITS FIRST, THEN THE PAD. These were the other way round, so
     * rumble.c consumed each frame's channels one frame before they were
     * filled: every impact arrived late and the first frame of every event was
     * simply lost. The pad reads what this tick measured, not what the last
     * one did. Both run now, while the cells hold this tick, and take their
     * intervals from the stamp. */
    ffb_events_poll(-(float)raw / 128.0f, when_ms);
    /* The same command, described to a pad's two motors instead of driven into
     * a wheel's one. Inert unless [ffb] rumble is set, and never reaches a
     * wheel; rumble.c's header says why an invented effect is allowed here at
     * all and nowhere else. */
    rumble_set_force(-(float)raw / 128.0f, when_ms,
                     (s_buttons & BTN_BOOST) != 0);
    record_tick(when_ms, -(float)raw / 128.0f);

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
            /* Under the lock: the poll thread sends forces too, and both
             * di_peak_force() and the spacing are read-and-reset. */
            if (s_wheel_lock_ready)
                EnterCriticalSection(&s_wheel_lock);
            LOGI("io: ffb window %u samples, %u nonzero (%u%%), dac %d..%d, "
                 "steer %u..%u, peak sent to DirectInput %ld of 10000",
                 s_ffb_samples, s_ffb_nonzero,
                 s_ffb_samples ? s_ffb_nonzero * 100 / s_ffb_samples : 0,
                 s_ffb_min, s_ffb_max, s_steer_min, s_steer_max,
                 di_peak_force());
            LOGI("io: ffb window wheel: %u commands, %.1f..%.1f ms apart, "
                 "mean %.1f ms", s_wsent, s_wsent ? s_wgap_min : 0.0,
                 s_wsent ? s_wgap_max : 0.0,
                 s_wsent ? s_wgap_sum / s_wsent : 0.0);
            s_wsent = 0;
            s_wgap_sum = 0.0;
            if (s_wheel_lock_ready)
                LeaveCriticalSection(&s_wheel_lock);
            /* The pad's half of the same question, and a separate line rather
             * than a column on the one above: only one of the two devices can
             * be attached, so a census that reported both would print a 0 that
             * means "not this run" next to one that means "the game asked for
             * nothing". */
            if (rumble_active()) {
                /* 240 left ten characters of margin once the census gained
                 * its sink clause. A truncated diagnostic is a diagnostic
                 * that lies about the end of its own sentence. */
                char pad[320];
                rumble_census(pad, sizeof pad);
                LOGI("io: ffb window pad: %s", pad);
            }
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
    if (!s_wheel_lock_ready) {
        InitializeCriticalSection(&s_wheel_lock);
        s_wheel_lock_ready = 1;
    }
    /* High resolution where Windows has it: the plain timer waits at the
     * system timer's 15.6 ms grain, which is a whole tick late at 16 ms. */
    s_wheel_timer = CreateWaitableTimerExW(NULL, NULL,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    if (s_wheel_timer) {
        LOGI("io: wheel force timer: high resolution");
    } else {
        DWORD err = GetLastError();

        s_wheel_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        LOGW("io: wheel force timer: no high-resolution timer (error %lu); %s",
             (unsigned long)err, s_wheel_timer ?
                 "plain waitable timer, at the system timer's grain" :
                 "none, so every tick's force is sent as it is computed");
    }
    return 1;
}

/* Builds only: it does NOT sample. The poll thread samples at its own ~60 Hz
 * whether or not it delivers frames, and frames are delivered at the control
 * callback's rate (Hydro ~30 Hz), so folding the sample in here would halve the
 * input rate wherever the frame boundary owns dispatch, which is both titles. */
static unsigned sim_receive(unsigned char *frame, unsigned max)
{
    unsigned len = io_board_is_magicbus() ? 16u : 8u;

    if (max < len)
        return 0;
    build_payload(frame);
    return len;
}

static void sim_transmit(double when_ms)
{
    update_force_feedback(when_ms);
}

static HANDLE sim_wake(void)
{
    return s_wheel_timer;
}

/* On the poll thread, when the timer fires. */
static void sim_deliver(void)
{
    double now;

    if (!s_wheel_lock_ready)
        return;
    EnterCriticalSection(&s_wheel_lock);
    now = io_clock_ms();
    while (s_wheel_n && s_wheel_q[s_wheel_head].when_ms <= now + 0.5)
        wheel_pop_send();
    wheel_arm();
    LeaveCriticalSection(&s_wheel_lock);
}

static void sim_close(void)
{
    if (s_rec) {
        fclose(s_rec);
        s_rec = NULL;
    }
    /* shim_exit() can close this under a live game thread; a transmit after
     * it must not reopen the file with "w" and truncate the recording. */
    s_rec_failed = 1;
    if (!s_wheel_lock_ready)
        return;
    EnterCriticalSection(&s_wheel_lock);
    s_wheel_n = 0;
    if (s_wheel_timer) {
        CancelWaitableTimer(s_wheel_timer);
        CloseHandle(s_wheel_timer);
        s_wheel_timer = NULL;
    }
    LeaveCriticalSection(&s_wheel_lock);
}

static const io_transport_t k_transport_sim = {
    "sim", sim_open, sim_receive, sim_transmit, sim_wake, sim_deliver,
    sim_close
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
