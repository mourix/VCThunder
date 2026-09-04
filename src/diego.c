/* diego.c -- the game's comm layer, the frame boundary, and the poll thread.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * This file's half of the I/O boundary (src/io.h has the split): the five
 * hooks that replace the title's comm-layer API, the seam that runs the board's decoder
 * and the game's control callback ON THE GAME'S OWN THREAD, and the thread
 * that does it when that seam is not available. Nothing in this file knows a
 * key name, an axis, or a byte offset inside a board frame: it moves frames
 * between a transport and the game, and it decides which thread does it.
 */
#include "vcthunder.h"
#include "io.h"

#include <stdio.h>
#include <string.h>

/* How long the poll thread waits out one of the game's inline `cli` regions
 * before entering game code anyway. Only reachable on a title whose frame
 * boundary is not the seam; a late board packet is a late input, which is what
 * a masked UART interrupt was. */
#define IO_IRQ_WAIT_US  2000u

typedef void (__cdecl *diego_rx_fn)(unsigned char *payload);
typedef void (__cdecl *game_input_callback_fn)(void);

static diego_rx_fn s_rx;
/* Which board frame to move. Selected once at install from [controls] backend;
 * `sim` is the only one implemented, and adding another is now a matter of
 * writing it rather than of unpicking this file. */
static const io_transport_t *s_transport;
static HANDLE s_thread;
static HANDLE s_poll_start;
static HANDLE s_poll_idle;
static HANDLE s_poll_stop;
static volatile LONG s_stop;
static unsigned s_period_ms = 16;        /* board packet cadence, ~60 Hz */

/* The original comm-layer state restored by our five hooks. */
static volatile PVOID s_game_callback;
static volatile LONG s_comm_enabled;
static volatile LONG s_callback_period_ms = 33;
static volatile LONG s_callback_seen;

static unsigned char s_dips = 0xFF;      /* raw active-low board byte    */
/* The thread game_run() is called on: captured at install, which runs on it. */
static DWORD s_game_thread;
/* Nonzero once the frame boundary has been shown to BE that thread and has
 * taken ownership of the game's own code. Until then, and forever on a title
 * whose frame boundary is some other thread, the poll thread keeps it. */
static volatile LONG s_frame_dispatch;
static DWORD s_last_callback;

/* One board frame, from whichever transport is installed. Returns its length,
 * or 0 when there is nothing to deliver this cycle. */
static unsigned io_frame(unsigned char *frame)
{
    return s_transport ? s_transport->receive(frame, IO_PACKET_MAX) : 0;
}

int diego_preload(const char *ini)
{
    const controls_config_t *cfg;

    io_config_load(ini);
    cfg = io_config();
    if (g_cfg.io_backend[0] && !io_transport_find(g_cfg.io_backend))
        LOGW("io: backend = %s is not implemented; using simulation instead "
             "of the board on %s.",
             g_cfg.io_backend,
             g_cfg.io_port[0] ? g_cfg.io_port : "a serial port");
    io_host_preload();
    /* DirectInput is the only provider that opens a device at preload rather
     * than on the first sample (taking exclusive access and creating an
     * effect is not something to do on the poll thread), so it goes last,
     * after the two cheap ones have reported. */
    if (cfg->provider == PROVIDER_AUTO || cfg->provider == PROVIDER_DINPUT)
        di_preload(ini);
    else if (cfg->provider != PROVIDER_NONE)
        LOGI("io: [analog] provider = %s, so no wheel is opened and there is "
             "no force feedback", cfg->provider == PROVIDER_XINPUT ?
             "xinput" : "winmm");
    return 1;
}
/* --- original Diego comm API replacements ---------------------------- */

static int __cdecl shim_enable_comm(int enabled)
{
    LONG old = InterlockedExchange(&s_comm_enabled, enabled != 0);
    LOGT("io: game comm %s (previous=%ld)", enabled ? "enabled" : "disabled", old);
    return (int)old;
}

static unsigned __cdecl shim_get_cached_dips(void)
{
    /* The real comm init caches NOT(raw Diego byte). */
    return (unsigned)(unsigned char)~s_dips;
}

static unsigned __cdecl shim_get_callback_period(void)
{
    return (unsigned)InterlockedCompareExchange(&s_callback_period_ms, 0, 0);
}

static void __cdecl shim_set_callback_period(unsigned period_ms)
{
    InterlockedExchange(&s_callback_period_ms, (LONG)period_ms);
    LOGT("io: game requested a %u ms control callback", period_ms);
}

static void __cdecl shim_set_callback(game_input_callback_fn callback)
{
    InterlockedExchangePointer((PVOID volatile *)&s_game_callback, (PVOID)callback);
    InterlockedExchange(&s_callback_seen, 0);
    LOGI("io: game control callback registered @ %p", callback);
}

static void dispatch_game_callback(DWORD *last_tick)
{
    game_input_callback_fn callback;
    DWORD now = GetTickCount();
    LONG period = InterlockedCompareExchange(&s_callback_period_ms, 0, 0);

    if (!InterlockedCompareExchange(&s_comm_enabled, 0, 0)) {
        *last_tick = now;
        return;
    }
    callback = (game_input_callback_fn)InterlockedCompareExchangePointer(
        (PVOID volatile *)&s_game_callback, NULL, NULL);
    if (!callback || (DWORD)(now - *last_tick) < (DWORD)period)
        return;

    game_thread_seen("the game's control-driver callback");
    callback();
    *last_tick = now;
    if (!InterlockedExchange(&s_callback_seen, 1))
        LOGI("io: game control callback active; decoded inputs now reach gameplay");
}

/* Transfer ownership of game code with an acknowledgement, not a frame-sized
 * timing assumption. The poller resets s_poll_idle before its final flag check,
 * so this either sees it already outside the decoder/callback or waits until
 * the in-flight pass has returned. */
/* Long enough that a poll pass which is merely slow still wins it, short
 * enough that a poll thread stuck inside a driver costs one refusal instead of
 * the rest of the session. It was INFINITE: this runs on the render thread, at
 * the frame boundary, and a DirectInput call that never returns would have
 * hung the game with the watchdog able to describe it and not to break it.
 * Refusing here is not a failure: the poll thread keeps the dispatch it
 * already has, which is what every title did before this seam existed. */
#define IO_CLAIM_WAIT_MS 250u

static int claim_frame_dispatch(void)
{
    DWORD wait;

    InterlockedExchange(&s_frame_dispatch, 1);
    if (!s_poll_idle) {
        InterlockedExchange(&s_frame_dispatch, 0);
        LOGE("io: cannot claim the frame seam without the poll-idle event");
        return 0;
    }
    wait = WaitForSingleObject(s_poll_idle, IO_CLAIM_WAIT_MS);
    if (wait == WAIT_TIMEOUT) {
        InterlockedExchange(&s_frame_dispatch, 0);
        LOGW("io: the poll thread did not leave game code within %u ms, so the "
             "frame-boundary seam was NOT taken. The decoder and the game's "
             "control callback stay on the poll thread (which is where they "
             "have always run), and the healed cli/sti do not protect them. "
             "See diego_frame_boundary().", IO_CLAIM_WAIT_MS);
        return 0;
    }
    if (wait != WAIT_OBJECT_0) {
        InterlockedExchange(&s_frame_dispatch, 0);
        LOGE("io: waiting for the poll thread to leave game code failed (%lu)",
             wait);
        return 0;
    }
    return 1;
}

/* Execute packet decoding and registered callbacks on the game's logic thread
 * at the frame boundary. The poll thread touches only host input and the DAC
 * output byte, avoiding concurrent access to game state. */
void diego_frame_boundary(void)
{
    unsigned char payload[IO_PACKET_MAX];

    if (!s_rx)
        return;
    /* Verify the swap thread against the observed game-logic thread. Titles
     * without a logic-thread anchor fall back to the loader-thread check after
     * a bounded wait. */
    {
        static int checked;
        static unsigned waited;
        DWORD now_tid = GetCurrentThreadId();
        DWORD logic = game_thread_logic();

        if (!checked) {
            /* Wait for the printf only where there is one to wait for.
             * `G->printf_empty` is 0 in Hydro: its build kept a real printf,
             * so log.c never hooks anything and no amount of waiting will ever
             * name a logic thread. Waiting anyway cost that title four seconds
             * of the exposure this whole function exists to close, measured:
             * the seam moved from the first frame to 12.4 s in. Offroad prints
             * throughout boot and answers before the first frame. */
            if (!logic && G->printf_empty && waited < 120u) {
                waited++;
                return;
            }
            checked = 1;
            if (logic && now_tid == logic) {
                if (claim_frame_dispatch())
                    LOGI("io: board decoder and control callback run at the "
                         "frame boundary, on the thread the game's own code "
                         "runs on (%lu)%s", now_tid,
                         now_tid == s_game_thread ? "" :
                             ", which is NOT the thread game_run() was called "
                             "on (that one handed the game off and parked; the "
                             "30 s thread census reports its crossings)");
            } else if (logic) {
                LOGW("io: the frame boundary is thread %lu but the game's own "
                     "code runs on %lu: this title renders on a thread that "
                     "is not its main loop, so moving the board's decoder and "
                     "the game's control callback here would relocate the race "
                     "rather than remove it. They stay on the poll thread, and "
                     "the cli/sti critical sections main.c heals do not protect "
                     "them. See diego_frame_boundary().",
                     now_tid, logic);
            } else if (now_tid == s_game_thread) {
                if (claim_frame_dispatch())
                    LOGW("io: board decoder and control callback run at the "
                         "frame boundary on thread %lu, which is the thread "
                         "game_run() was called on. This title never reached "
                         "the game's own printf, so the census could not "
                         "confirm that is also where its logic runs: the "
                         "weaker test, stated as such.", now_tid);
            } else {
                LOGW("io: the frame boundary is thread %lu, the game was "
                     "started on %lu, and this title has no printf anchor to "
                     "say where its logic actually runs. Refusing the seam on "
                     "no evidence: the decoder and the callback stay on the "
                     "poll thread and the healed cli/sti do not protect them. "
                     "See diego_frame_boundary().",
                     now_tid, s_game_thread);
            }
            return;
        }
    }
    if (!InterlockedCompareExchange(&s_frame_dispatch, 0, 0))
        return;             /* this title's frame boundary is not the seam */
    if (!s_last_callback)
        s_last_callback = GetTickCount();
    if (io_frame(payload))
        s_rx(payload);
    dispatch_game_callback(&s_last_callback);
    /* AFTER the callback, not before: the callback is what runs the game's
     * control driver, and on Hydro it is the function that writes the DAC.
     * Reading first would deliver every force one frame late. */
    if (s_transport)
        s_transport->transmit();
}

static DWORD WINAPI poll_thread(LPVOID arg)
{
    (void)arg;
    if (WaitForSingleObject(s_poll_start, INFINITE) != WAIT_OBJECT_0)
        return 0;
    LOGI("io: poll thread running at %u ms", s_period_ms);
    while (!InterlockedCompareExchange(&s_stop, 0, 0)) {
        unsigned char payload[IO_PACKET_MAX];

        /* Sampled every cycle regardless of who delivers frames: this thread
         * runs at the board's cadence and the frame boundary runs at the
         * game's, and the cabinet's controls were read at the former. */
        io_board_sample_input();
        /* Only while the frame boundary has not claimed the game's code: at
         * startup, before the first frame, and for the whole run on a title
         * whose frame boundary turns out to be a different thread. Keeping the
         * old behaviour there is deliberate: it is what those titles have
         * always done, and swapping one cross-thread dispatch for another is
         * not a fix. */
        if (!InterlockedCompareExchange(&s_frame_dispatch, 0, 0)) {
            ResetEvent(s_poll_idle);
            /* Close the race with claim_frame_dispatch(): it may have set the
             * flag after the outer test but before this pass advertised itself
             * busy. In that ordering the poller does no game work and simply
             * acknowledges quiescence. */
            if (!InterlockedCompareExchange(&s_frame_dispatch, 0, 0) &&
                !InterlockedCompareExchange(&s_stop, 0, 0)) {
                unsigned len;

                if (!s_last_callback)
                    s_last_callback = GetTickCount();
                len = io_frame(payload);
                /* The fallback decoder executes game code as the UART interrupt
                 * and must honour the emulated interrupt-disable depth. */
                game_thread_seen("the board's packet decoder");
                game_irq_wait(IO_IRQ_WAIT_US);
                if (len)
                    s_rx(payload);
                dispatch_game_callback(&s_last_callback);
                if (s_transport)
                    s_transport->transmit();
            }
            SetEvent(s_poll_idle);
        }
        if (WaitForSingleObject(s_poll_stop, s_period_ms) == WAIT_OBJECT_0)
            break;
    }
    return 0;
}

/* --- install ---------------------------------------------------------- */


int diego_install(unsigned dips, unsigned period_ms)
{
    unsigned char payload[IO_PACKET_MAX];
    int ready = 1;

    /* Validate the complete mandatory patch group before allocating resources
     * or changing one byte of game code. io_get_period is deliberately optional
     * on Offroad; every other hook is part of both supported board contracts. */
    ready &= game_require("io", "a board packet decoder", G->io_receive);
    ready &= game_require("io", "_*_comm_EnableComm", G->io_enable_comm);
    ready &= game_require("io", "a DIP-switch reader", G->io_get_dips);
    ready &= game_require("io", "a callback-period setter", G->io_set_period);
    ready &= game_require("io", "a control-callback setter", G->io_set_callback);
    if (!ready)
        return 0;

    if (io_board_is_magicbus() &&
        (!game_require("io", "the board handshake toggles", G->io_ack_security) ||
         !game_require("io", "the board handshake toggles", G->io_ack_read)))
        return 0;

    /* Pick the transport before anything asks it for a frame. `sim` is the
     * fallback and the only one implemented; an unrecognised [controls]
     * backend was already reported by diego_preload(). */
    s_transport = io_transport_find(g_cfg.io_backend);
    if (!s_transport)
        s_transport = io_transport_find("sim");
    if (!s_transport || !s_transport->open(dips)) {
        LOGE("io: the %s transport would not open",
             s_transport ? s_transport->name : "requested");
        return 0;
    }

    s_dips = (unsigned char)dips;
    s_game_thread = GetCurrentThreadId();
    if (period_ms)
        s_period_ms = period_ms;
    s_rx = (diego_rx_fn)(uintptr_t)G->io_receive;

    s_poll_start = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_poll_idle  = CreateEventA(NULL, TRUE, TRUE, NULL);
    s_poll_stop  = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!s_poll_start || !s_poll_idle || !s_poll_stop) {
        LOGE("io: cannot create poll-thread events (%lu)", GetLastError());
        goto fail_resources;
    }
    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_frame_dispatch, 0);
    s_thread = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
    if (!s_thread) {
        LOGE("io: CreateThread failed %lu", GetLastError());
        goto fail_resources;
    }

    /* These entries were generic hardware stubs. Restore the original comm
     * semantics without restoring UART/thread hardware. */
    patch_jmp(G->io_enable_comm, shim_enable_comm);
    patch_jmp(G->io_get_dips, shim_get_cached_dips);
    /* Offroad has a setter but no getter: the cadence is write-only there, so
     * the shim keeps whatever the game last asked for and never reports it. */
    if (G->io_get_period)
        patch_jmp(G->io_get_period, shim_get_callback_period);
    patch_jmp(G->io_set_period, shim_set_callback_period);
    patch_jmp(G->io_set_callback, shim_set_callback);

    /* Seed the game's decoder before _main asks for its initial ADC values. */
    io_board_sample_input();
    if (io_frame(payload))
        s_rx(payload);

    SetEvent(s_poll_start);

    LOGI("io: %s board over the %s transport, decoder @ 0x%08X, %u-byte "
         "frame, DIPs raw=0x%02X, callback bridge installed",
         G->io_board, s_transport->name, G->io_receive,
         io_board_is_magicbus() ? 16u : 8u, s_dips);
    /* The motor is the one part of the board that is not synthesised, so say
     * once whether it is wired up and to what; an absent anchor here is a
     * board with no motor output, which neither supported title has. */
    if (!G->io_dac_out)
        LOGW("io: this profile has no DAC output anchor; force feedback is off");
    else if (di_present())
        LOGI("io: force feedback: DAC @ 0x%08X -> %s",
             G->io_dac_out, di_device_name());
    else
        LOGI("io: force feedback: DAC @ 0x%08X, no wheel attached",
             G->io_dac_out);
    io_config_report();
    return 1;

fail_resources:
    if (s_thread) {
        InterlockedExchange(&s_stop, 1);
        if (s_poll_start) SetEvent(s_poll_start);
        if (s_poll_stop) SetEvent(s_poll_stop);
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    if (s_poll_start) CloseHandle(s_poll_start);
    if (s_poll_idle) CloseHandle(s_poll_idle);
    if (s_poll_stop) CloseHandle(s_poll_stop);
    s_poll_start = s_poll_idle = s_poll_stop = NULL;
    s_rx = NULL;
    return 0;
}

void diego_shutdown(void)
{
    if (s_thread) {
        InterlockedExchange(&s_stop, 1);
        if (s_poll_stop) SetEvent(s_poll_stop);
        if (s_poll_start) SetEvent(s_poll_start);
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    /* After the poll thread has stopped and not before: the transport's
     * transmit direction runs on it, and di_set_force() releasing the effect
     * under a live caller is the one ordering the driver cannot be asked to
     * survive. */
    if (s_transport) {
        s_transport->close();
        s_transport = NULL;
    }
    di_close();
    if (s_poll_start) CloseHandle(s_poll_start);
    if (s_poll_idle) CloseHandle(s_poll_idle);
    if (s_poll_stop) CloseHandle(s_poll_stop);
    s_poll_start = s_poll_idle = s_poll_stop = NULL;
    s_rx = NULL;
    InterlockedExchange(&s_frame_dispatch, 0);
}
