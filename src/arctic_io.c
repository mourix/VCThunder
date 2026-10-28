/* arctic_io.c -- Arctic Thunder: the cabinet shell, inside the process.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * goose.dll talks to wincsm.exe, the cabinet's shell, a separate process that
 * owns the I/O board, the coins and the credits. Here wincsm is a thread and two blocks of
 * memory in this process, and goose finds them exactly as it found the shell:
 * psapi names a process "wincsm.exe", the registry's IOBlock value holds the
 * address of its two block pointers, and Read/WriteProcessMemory move the
 * bytes. Those calls stay real, on our own process; only the lookup is bound.
 *
 * Every layout below was read out of the binaries.
 *
 *   A (0x40, the game writes):  +0 the input handshake, +0xC the 12 output
 *     bytes goose pushes every 1 ms, +0x18 credits to subtract, +0x1C/+0x20
 *     clear units, +0x24 a service credit, +0x28 the coin lock, +0x2C ADC
 *     min/max enable.
 *   B (0x3E0, the game reads):  +8 the input record (0x330), +0x338 the
 *     pricing (13 words and the currency strings), +0x3CC the credits (5).
 *
 * The record is the I/O library's decoded format, one level above the wire:
 * switch words (active low), per-switch close and open counts the game
 * consumes as edges, and four ADC channels. goose copies it when A+0 is 0 and
 * then writes 1; the shell posts the next one and writes 0.
 */
#include "vcthunder.h"
#include "arctic.h"
#include "io.h"
#include "ini.h"

#include <stdio.h>
#include <string.h>

/* ---- the blocks ---------------------------------------------------------- */

#define A_HANDSHAKE   0x00
#define A_OUT         0x0C
#define A_SUBTRACT    0x18      /* s16 */
#define A_CLEAR_ALL   0x1C
#define A_CLEAR_BONUS 0x20
#define A_SERVICE     0x24
#define A_LOCK        0x28      /* u16: 1 requested, 2 acknowledged, 0 free */
#define A_SIZE        0x40

#define B_RECORD      0x008
#define B_PRICING     0x338
#define B_CREDITS     0x3CC
#define B_SIZE        0x3E0

/* Within the record. */
#define R_PLAYER      0x000     /* goose returns it inverted                */
#define R_COIN        0x004     /* the coin/dip word, the same              */
#define R_CLOSE       0x10C     /* u32[32] player, then u32[32] coin/dip    */
#define R_OPEN        0x20C     /* the same two arrays, for releases         */
#define R_ADC_SECOND  0x30C     /* u32[4], copied with the current values    */
#define R_ADC         0x31C     /* u32[4]                                     */
#define R_VARIANT     0x32C     /* 0: wheel motor, two lamps, shaker          */

/* The switch lines. Coin/dip word: diagdll.dll's switch table names bits 4..12
 * (CREDIT VOL- VOL+ TEST LCOIN RCOIN CCOIN ECOIN BILL), and its switch test
 * draws Start from bit 0. Player word: the same test draws Attack from bit 7. */
#define COIN_START    (1u << 0)
#define COIN_CREDIT   (1u << 4)
#define COIN_VOL_DN   (1u << 5)
#define COIN_VOL_UP   (1u << 6)
#define COIN_TEST     (1u << 7)
#define COIN_LCOIN    (1u << 8)
#define COIN_RCOIN    (1u << 9)
#define PLAYER_ATTACK (1u << 7)

/* The output block with variant 0 (goose io_write_*): wheel target, servo
 * gain, two lamps, wheel strength, two solenoid set-ups, shaker duty and the
 * solenoid enables (0x10 << n). */
#define O_WHEEL_TARGET 0
#define O_WHEEL_GAIN   1
#define O_LAMP0        2
#define O_WHEEL_FORCE  4
#define O_SOLENOID     6        /* 6..9 */
#define O_SHAKER       10
#define O_SOL_ENABLE   11

/* wincsm.exe's credit state is the block itself: credits, service credits,
 * then three unit counters. */
enum { C_CREDITS, C_SERVICE, C_UNITS, C_BONUS, C_REMAINDER, C_COUNT };

/* THE PRICING is the shell's, kept in the registry as one 0x358-byte record
 * (wincsm.exe get_pricing_table, 0x406240): 22 words, then three 256-byte
 * strings (the price's description, its name, its coin values). At start the
 * shell copies the first 13 words to B+0x338, where the game and diag read
 * them (coin_init_pricing, 0x401440). The operator's choice in diag's Pricing
 * menu is that record; with none, the shell writes its own default first,
 * the _us_pricing entry "USA 13" (0x40a4c0, chosen at 0x4062b3): units per
 * coin L/R/C/E/D 1 1 0 0 4, 1 unit per credit, no bonus, 4 credits to start
 * and to continue, 32 at most, 4 coins per bill, credit fractions shown. */
enum { P_UNITS_L, P_UNITS_R, P_UNITS_C, P_UNITS_E, P_UNITS_D,
       P_PER_CREDIT, P_PER_BONUS, P_MIN_UNITS, P_START, P_CONTINUE,
       P_MAX_CREDITS, P_PER_BILL, P_FRACTIONS, P_COUNT };
#define PRICING_KEY  "Software\\Midway Games\\WinCSM\\pricing table"
#define PRICING_SIZE 0x358
static const uint32_t k_default_words[22] = {
    1, 1, 0, 0, 4, 1, 0, 0, 4, 4, 32, 4, 1,     /* the 13 the game reads */
    1, 1, 0, 0, 4, 0, 0, 0, 0                   /* coin counts, currency */
};
static const char *const k_default_text[3] = {
    "1 CREDIT / 1 COIN", "USA 13", ".25\t.25\t-\t-\t$1.00"
};
static uint32_t s_pricing[P_COUNT];

static volatile BYTE s_a[A_SIZE] __attribute__((aligned(16)));
static volatile BYTE s_b[B_SIZE] __attribute__((aligned(16)));
/* What the IOBlock value points at: wincsm.exe's `ptrs`, which holds B FIRST
 * and A second (0x41D700, 0x41DAE0); goose stores the second as A. */
static uint32_t s_ptrs[2];

#define A32(off) (*(volatile LONG *)(s_a + (off)))
#define A16(off) (*(volatile SHORT *)(s_a + (off)))
#define B32(off) (*(volatile uint32_t *)(s_b + (off)))

/* ---- the lookup goose does (bound in arctic.c) --------------------------- */

#define FAKE_MODULE ((HMODULE)(uintptr_t)0x57494E43u)  /* "WINC" */

static BOOL WINAPI fake_EnumProcesses(DWORD *pids, DWORD cb, DWORD *needed)
{
    if (!pids || cb < sizeof(DWORD))
        return FALSE;
    pids[0] = GetCurrentProcessId();
    if (needed)
        *needed = sizeof(DWORD);
    return TRUE;
}

static BOOL WINAPI fake_EnumProcessModules(HANDLE h, HMODULE *mods, DWORD cb,
                                           DWORD *needed)
{
    (void)h;
    if (mods && cb >= sizeof(HMODULE))
        mods[0] = FAKE_MODULE;
    if (needed)
        *needed = sizeof(HMODULE);
    return TRUE;
}

static DWORD WINAPI fake_GetModuleBaseNameA(HANDLE h, HMODULE m, LPSTR out,
                                            DWORD cap)
{
    static const char name[] = "wincsm.exe";

    (void)h; (void)m;
    if (!out || cap < sizeof name)
        return 0;
    memcpy(out, name, sizeof name);
    LOGI("arctic: goose looked for the cabinet shell and found the host's");
    return sizeof name - 1;
}

void *arctic_io_psapi(const char *name)
{
    if (!name || (uintptr_t)name < 0x10000)
        return NULL;
    if (!strcmp(name, "EnumProcesses"))
        return (void *)fake_EnumProcesses;
    if (!strcmp(name, "EnumProcessModules"))
        return (void *)fake_EnumProcessModules;
    if (!strcmp(name, "GetModuleBaseNameA"))
        return (void *)fake_GetModuleBaseNameA;
    return NULL;
}

/* ---- the controls -------------------------------------------------------- */

static int s_wheel;             /* a force-feedback wheel takes the motor */
static int s_game;              /* the game's process: the pad gets its feel */

typedef struct {
    uint32_t player, coin;      /* pressed, active high */
    unsigned steer, gas;        /* 0..255, as the game reads the ADC     */
} cabinet_t;

static int pad_down(const host_state_t *h, const io_pad_bind_t *b)
{
    return h->present && io_pad_down(h->buttons, b);
}

static int down(const host_state_t *h, const io_key_bind_t *k,
                const io_pad_bind_t *p)
{
    return io_key_down(k) || pad_down(h, p);
}

static unsigned to_byte(float v)
{
    int b = (int)(v * 255.0f + 0.5f);

    return b < 0 ? 0u : b > 255 ? 255u : (unsigned)b;
}

/* The keys own a select screen (menu_wheel.c): the wheel gets no spring, or
 * the game's notch would move it into a takeover. Set by sample(), read by
 * outputs(), both on the shell thread. */
static int s_menu_mute;

/* snow.exe reads ADC 0 as (v - 127.5) / 127.5 and ADC 1 as v / 255, where v
 * is the 16-bit value shifted right by two. */
static void sample(cabinet_t *c)
{
    const controls_config_t *cfg = io_config();
    host_state_t h;
    float steer = 0.0f, gas = 0.0f;
    int pov, left, right, up;

    memset(c, 0, sizeof *c);
    io_host_refresh_active();
    memset(&h, 0, sizeof h);
    if (io_host_sample(&h) && h.present) {
        steer = io_clamp_unit(h.steer);
        gas = h.drive > 0.0f ? io_clamp_unit(h.drive) : 0.0f;
    }
    pov = io_config_pov_digital();
    left = io_key_down(&cfg->steer_left) || (pov && h.left);
    right = io_key_down(&cfg->steer_right) || (pov && h.right);
    up = io_key_down(&cfg->throttle) || (pov && h.up);
    if (left != right)
        steer = left ? -1.0f : 1.0f;
    if (up)
        gas = 1.0f;
    c->steer = menu_wheel_steer(&h, to_byte((steer + 1.0f) * 0.5f),
                                &s_menu_mute);
    c->gas = to_byte(gas);

    if (down(&h, &cfg->start1, &cfg->pad_start1))             c->coin |= COIN_START;
    if (down(&h, &cfg->service1, &cfg->pad_service1))         c->coin |= COIN_CREDIT;
    if (down(&h, &cfg->volume_down, &cfg->pad_volume_down))   c->coin |= COIN_VOL_DN;
    if (down(&h, &cfg->volume_up, &cfg->pad_volume_up))       c->coin |= COIN_VOL_UP;
    if (down(&h, &cfg->service_mode, &cfg->pad_service_mode)) c->coin |= COIN_TEST;
    if (down(&h, &cfg->coin1, &cfg->pad_coin1))               c->coin |= COIN_LCOIN;
    if (down(&h, &cfg->coin2, &cfg->pad_coin2))               c->coin |= COIN_RCOIN;
    if (down(&h, &cfg->boost, &cfg->pad_boost))               c->player |= PLAYER_ATTACK;
    menu_wheel_arctic_trace(c->steer);
}

/* ---- the shell ----------------------------------------------------------- */

/* wincsm.exe's game_state, as far as the game needs it: TEST while the game
 * runs sets GameKillEvent (goose then exits with 2) and the shell runs
 * diagnostics once the game is gone. Only the game's process has the event;
 * in the operator system's process TEST is a switch diag.exe reads. */
static HANDLE s_kill;
static volatile LONG s_test_requested;

HANDLE arctic_io_kill_event(void) { return s_kill; }
int arctic_io_test_requested(void) { return s_test_requested != 0; }

static volatile LONG s_run;
static HANDLE s_thread;
static cabinet_t s_now;
static uint32_t s_close[64], s_open[64];   /* edges not yet posted */
static BYTE s_out_seen[12];

static void add_credits(unsigned slot, uint32_t units)
{
    uint32_t total, max = s_pricing[P_MAX_CREDITS];

    if (!units)
        return;
    if (!s_pricing[P_PER_CREDIT])
        return;
    B32(B_CREDITS + slot * 4) += units / s_pricing[P_PER_CREDIT];
    total = B32(B_CREDITS + C_CREDITS * 4) + B32(B_CREDITS + C_SERVICE * 4);
    if (total > max)
        B32(B_CREDITS + slot * 4) -= total - max;
    LOGI("arctic: shell credits %u (+%u service)",
         (unsigned)B32(B_CREDITS + C_CREDITS * 4),
         (unsigned)B32(B_CREDITS + C_SERVICE * 4));
}

/* What wincsm.exe's coin code and I/O loop answer: the coin lock, the credits
 * the game spends (service credits first, as decrement_credits does), a
 * service credit, and the unit clears. The game waits on each of these in a
 * loop on its own process thread, so an unanswered one freezes it. */
static void serve(void)
{
    SHORT spend;

    if (A16(A_LOCK) == 1)
        A16(A_LOCK) = 2;
    spend = A16(A_SUBTRACT);
    if (spend) {
        int n = spend;
        uint32_t *svc = (uint32_t *)(s_b + B_CREDITS + C_SERVICE * 4);
        uint32_t *paid = (uint32_t *)(s_b + B_CREDITS + C_CREDITS * 4);

        while (n > 0 && *svc) { (*svc)--; n--; }
        while (n > 0 && *paid) { (*paid)--; n--; }
        A16(A_SUBTRACT) = 0;
        LOGI("arctic: the game spent %d credit%s; %u left (+%u service)",
             spend, spend == 1 ? "" : "s", (unsigned)*paid, (unsigned)*svc);
    }
    if (A32(A_SERVICE)) {
        add_credits(C_SERVICE, s_pricing[P_PER_CREDIT]);
        A32(A_SERVICE) = 0;
    }
    if (A32(A_CLEAR_ALL)) {
        B32(B_CREDITS + C_UNITS * 4) = B32(B_CREDITS + C_BONUS * 4) =
            B32(B_CREDITS + C_REMAINDER * 4) = 0;
        A32(A_CLEAR_ALL) = 0;
    }
    if (A32(A_CLEAR_BONUS)) {
        B32(B_CREDITS + C_BONUS * 4) = 0;
        A32(A_CLEAR_BONUS) = 0;
    }
}

static void edges(uint32_t was, uint32_t now, unsigned base)
{
    uint32_t pressed = now & ~was, released = was & ~now;
    unsigned i;

    for (i = 0; i < 32; i++) {
        if (pressed & (1u << i))
            s_close[base + i]++;
        if (released & (1u << i))
            s_open[base + i]++;
    }
}

static void post(void)
{
    static unsigned long posts;
    unsigned i;

    if (posts++ == 1)
        LOGI("arctic: goose is reading the board");
    if (io_config_input_trace())
        for (i = 0; i < 64; i++)
            if (s_close[i] || s_open[i])
                LOGI("arctic: posted %s bit %u: %u close, %u open",
                     i < 32 ? "player" : "coin", i & 31, (unsigned)s_close[i],
                     (unsigned)s_open[i]);

    B32(B_RECORD + R_PLAYER) = ~s_now.player;
    B32(B_RECORD + R_COIN) = ~s_now.coin;
    for (i = 0; i < 64; i++) {
        B32(B_RECORD + R_CLOSE + i * 4) = s_close[i];
        B32(B_RECORD + R_OPEN + i * 4) = s_open[i];
    }
    memset(s_close, 0, sizeof s_close);
    memset(s_open, 0, sizeof s_open);
    B32(B_RECORD + R_ADC + 0) = B32(B_RECORD + R_ADC_SECOND + 0) = s_now.steer << 2;
    B32(B_RECORD + R_ADC + 4) = B32(B_RECORD + R_ADC_SECOND + 4) = s_now.gas << 2;
    B32(B_RECORD + R_VARIANT) = 0;
    MemoryBarrier();
    InterlockedExchange((volatile LONG *)(s_a + A_HANDSHAKE), 0);
}

/* The motor is a position servo: the game commands where the wheel should be
 * (target byte, the pot's scale) and how hard to hold it (strength), with a
 * gain byte the game sets to 6. A wheel gets that as a spring; full stiffness
 * at the game's own gain. */
static void outputs(void)
{
    BYTE o[12];
    unsigned i;

    for (i = 0; i < 12; i++)
        o[i] = s_a[A_OUT + i];
    menu_wheel_arctic_servo(o[O_WHEEL_TARGET], o[O_WHEEL_FORCE]);
    if (s_wheel && s_menu_mute)
        di_set_spring(0.0f, 0.0f, 0.0f);
    else if (s_wheel)
        di_set_spring(((float)o[O_WHEEL_TARGET] - 127.5f) / 127.5f,
                      (float)o[O_WHEEL_GAIN] / 6.0f,
                      (float)o[O_WHEEL_FORCE] / 255.0f);
    if (s_game)
        arctic_ffb_poll(o[O_SHAKER]);
    if (memcmp(o, s_out_seen, sizeof o)) {
        static const char *const what[12] = {
            "wheel target", "wheel gain", "lamp 0", "lamp 1", "wheel strength",
            "byte 5", "solenoid 0a", "solenoid 1a", "solenoid 0b",
            "solenoid 1b", "seat shaker", "solenoid enables"
        };
        static BYTE ever[12];

        for (i = 0; i < 12; i++)
            if (o[i] && !ever[i]) {
                ever[i] = 1;
                LOGI("arctic: the game drives the %s (first value %u)",
                     what[i], o[i]);
            }
        LOGT("arctic: outputs wheel %3u/%u/%3u lamps %u %u solenoids %u %u %u"
             " %u enable 0x%02X shaker %u", o[0], o[1], o[4], o[2], o[3],
             o[6], o[7], o[8], o[9], o[11], o[10]);
        memcpy(s_out_seen, o, sizeof o);
    }
}

static DWORD WINAPI shell_thread(LPVOID p)
{
    DWORD last_sample = 0, last_feel = 0;
    LARGE_INTEGER due;
    HANDLE tick = CreateWaitableTimerExW(NULL, NULL,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);

    (void)p;
    /* wincsm.exe's I/O loop runs every millisecond. */
    due.QuadPart = -10000;
    if (tick && !SetWaitableTimer(tick, &due, 1, NULL, NULL, FALSE)) {
        CloseHandle(tick);
        tick = NULL;
    }
    while (s_run) {
        DWORD now = GetTickCount();

        if (now - last_sample >= 4) {
            cabinet_t c;

            last_sample = now;
            sample(&c);
            edges(s_now.player, c.player, 0);
            edges(s_now.coin, c.coin, 32);
            if ((c.coin & ~s_now.coin) & COIN_LCOIN)
                add_credits(C_CREDITS, s_pricing[P_UNITS_L]);
            if ((c.coin & ~s_now.coin) & COIN_RCOIN)
                add_credits(C_CREDITS, s_pricing[P_UNITS_R]);
            /* wincsm.exe's I/O loop (0x406ae9..0x406f5e): TEST while the
             * game runs sets GameKillEvent, once. */
            if (((c.coin & ~s_now.coin) & COIN_TEST) && s_kill &&
                !InterlockedExchange(&s_test_requested, 1)) {
                LOGI("arctic: TEST: entering test, as wincsm.exe does: the"
                     " game is told to exit and the operator system follows");
                SetEvent(s_kill);
            }
            if (c.player != s_now.player || c.coin != s_now.coin ||
                c.steer != s_now.steer || c.gas != s_now.gas)
                log_printf(io_config_input_trace() ? LOG_INFO : LOG_TRACE,
                           "arctic: cabinet steer=%3u gas=%3u player=0x%02X"
                           " coin=0x%03X", c.steer, c.gas,
                           (unsigned)c.player, (unsigned)c.coin);
            s_now = c;
        }
        serve();
        if (A32(A_HANDSHAKE))
            post();
        outputs();
        /* The pad's model at the rate the game's feedback thread runs. */
        if (s_game && now - last_feel >= 16) {
            last_feel = now;
            arctic_ffb_tick((s_now.player & PLAYER_ATTACK) != 0);
        }
        if (tick)
            WaitForSingleObject(tick, 20);
        else
            Sleep(1);
    }
    if (tick)
        CloseHandle(tick);
    return 0;
}

/* ---- the pricing --------------------------------------------------------- */

static void load_pricing(void)
{
    static BYTE rec[PRICING_SIZE];
    DWORD type = 0, len = sizeof rec, disp = 0;
    HKEY k;
    int i;

    if (arctic_RegCreateKeyExA(HKEY_LOCAL_MACHINE, PRICING_KEY, 0, NULL, 0,
                               KEY_ALL_ACCESS, NULL, &k, &disp)) {
        memcpy(s_pricing, k_default_words, sizeof s_pricing);
        LOGW("arctic: no pricing table in the cabinet registry; the shell's"
             " default for this run");
        return;
    }
    if (disp == REG_CREATED_NEW_KEY ||
        arctic_RegQueryValueExA(k, NULL, NULL, &type, rec, &len) ||
        len < P_COUNT * 4) {
        memset(rec, 0, sizeof rec);
        memcpy(rec, k_default_words, sizeof k_default_words);
        for (i = 0; i < 3; i++)
            snprintf((char *)rec + 0x58 + 0x100 * i, 0x100, "%s", k_default_text[i]);
        arctic_RegSetValueExA(k, NULL, 0, REG_BINARY, rec, sizeof rec);
        LOGI("arctic: the cabinet registry had no pricing; the shell wrote its"
             " default, %s", k_default_text[1]);
    }
    arctic_RegCloseKey(k);
    memcpy(s_pricing, rec, sizeof s_pricing);
    LOGI("arctic: pricing %.32s (%.32s): %u credit%s to start, %u to continue,"
         " L/R coin %u/%u units, %u per credit, %u at most",
         (const char *)rec + 0x158, (const char *)rec + 0x58,
         s_pricing[P_START], s_pricing[P_START] == 1 ? "" : "s",
         s_pricing[P_CONTINUE], s_pricing[P_UNITS_L], s_pricing[P_UNITS_R],
         s_pricing[P_PER_CREDIT], s_pricing[P_MAX_CREDITS]);
}

/* ---- bring-up ------------------------------------------------------------ */

void arctic_io_start(const char *ini, int game)
{
    const controls_config_t *cfg;
    char reg[MAX_PATH], rel[MAX_PATH];
    unsigned i;

    ini_set_title(AT->ids->settings);
    io_config_load(ini);
    cfg = io_config();
    io_host_preload();
    if (cfg->provider == PROVIDER_AUTO || cfg->provider == PROVIDER_DINPUT)
        di_preload(ini);
    io_config_bind_devices();
    io_config_report();

    s_wheel = di_publishes_ffb();
    LOGI("arctic: wheel motor %s",
         s_wheel ? "to the wheel as a spring" : "not driven (no force-feedback wheel)");
    /* The pad's model reads the game's own cells, which only the game's
     * process has mapped. rumble_preload() refuses a wheel itself. */
    s_game = game;
    if (game)
        arctic_ffb_start(ini);

    s_ptrs[0] = (uint32_t)(uintptr_t)s_b;
    s_ptrs[1] = (uint32_t)(uintptr_t)s_a;
    /* The cabinet's registry lives with its saves, by absolute path: the run
     * changes directory into the game folder afterwards. */
    CreateDirectoryA(g_cfg.save_dir, NULL);
    snprintf(rel, sizeof rel, "%s\\%s", g_cfg.save_dir, AT->ids->id);
    CreateDirectoryA(rel, NULL);
    snprintf(rel + strlen(rel), sizeof rel - strlen(rel), "\\registry.txt");
    if (!GetFullPathNameA(rel, sizeof reg, reg, NULL))
        snprintf(reg, sizeof reg, "%s", rel);
    arctic_reg_init(reg, (DWORD)(uintptr_t)s_ptrs);
    load_pricing();
    if (game)
        s_kill = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < P_COUNT; i++)
        B32(B_PRICING + i * 4) = s_pricing[i];
    sample(&s_now);
    A32(A_HANDSHAKE) = 1;
    post();
    s_run = 1;
    s_thread = CreateThread(NULL, 0, shell_thread, NULL, 0, NULL);
    LOGI("arctic: cabinet shell up: IOBlock at %p (A %p, B %p)", (void *)s_ptrs,
         (void *)s_a, (void *)s_b);
}

void arctic_io_stop(void)
{
    if (!s_run)
        return;
    s_run = 0;
    if (s_thread) {
        WaitForSingleObject(s_thread, 200);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    if (s_wheel)
        di_set_spring(0.0f, 0.0f, 0.0f);
    if (s_game)
        arctic_ffb_stop();
    di_close();
}
