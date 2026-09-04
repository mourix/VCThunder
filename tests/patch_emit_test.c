/* patch_emit_test.c -- the code emitter, over bytes and then over a CPU.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * src/patch.c writes x86 at runtime, into an image that is about to be
 * overwritten by a 1999 game, and the failure mode of a wrong byte is a process
 * that vanishes with no fault and no log. The only other thing that exercises
 * it is a live run, which reports a wrong emitter as "the game did not
 * start".
 *
 * The emitter is a pure function over bytes and needs no game to test. Two
 * things about it are worth asserting rather than reading:
 *
 *   THE BOUNDS ARE A REFUSAL, NOT A REPORT. Every write is checked BEFORE it
 *   lands, because a check that fires once the buffer is already overrun is
 *   the very bug it exists to prevent. So the thing to prove is not that
 *   overflow is noticed: it is that the byte after the buffer is still what it
 *   was, and that a 4-byte immediate with 3 bytes left writes NOTHING rather
 *   than three quarters of itself.
 *
 *   A rel32 IS RESOLVED AGAINST WHERE THE BYTES WILL RUN, never against the
 *   scratch they are being written into. Several stubs are built in a local
 *   buffer and copied into the pool afterwards; emit_t carries that distinction
 *   in the type, and this is where it is checked.
 *
 * And then the part no byte comparison can reach: the emitted code is put in
 * the executable pool and CALLED. patch_emit_sqrt() and thunk_count_only() are
 * the two smallest things patch.c builds, and both are complete programs.
 */
#include "../src/patch.c"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- what including patch.c needs -------------------------------------- */
shim_config_t g_cfg;
log_level_t g_log_level = LOG_ERR;

/* An emitter refusal is reported through LOGE, so a test of the refusals has
 * to be able to see them. Captured rather than printed: several cases here are
 * SUPPOSED to log, and a test whose passing output is a page of errors is a
 * test nobody reads. */
static char last_log[512];
static int  log_count;

void log_printf(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    log_count++;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
}

static void log_reset(void) { log_count = 0; last_log[0] = '\0'; }

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* ---- bounds ------------------------------------------------------------ */

/* The buffer is written with a guard byte past its end. Nothing the emitter
 * does may change it -- which is a different and stronger claim than "overflow
 * was set". */
#define GUARD 0x5A

static void test_bounds_refuse_before_the_write(void)
{
    unsigned char buf[9];
    emit_t e;

    /* Exactly full is not overflow. */
    memset(buf, 0, sizeof buf);
    buf[8] = GUARD;
    emit_init(&e, buf, 8, buf);
    emit32(&e, 0x11111111u);
    emit32(&e, 0x22222222u);
    check(!e.overflow, "eight bytes into an eight-byte buffer is not overflow");
    check(e.i == 8, "and it emitted all eight");
    check(buf[8] == GUARD, "and wrote nothing past the end");

    /* One byte more is. */
    emit8(&e, 0xCC);
    check(e.overflow, "the ninth byte overflows");
    check(e.i == 8, "and the count does not move");
    check(buf[8] == GUARD, "and the guard byte is untouched");

    /* A 4-byte immediate with 3 bytes left: all or nothing. A partial write
     * here is the worst case in this file -- it produces a stub that
     * disassembles correctly up to the point where it does not. */
    memset(buf, 0, sizeof buf);
    buf[8] = GUARD;
    emit_init(&e, buf, 8, buf);
    emit_bytes(&e, "abcde", 5);
    emit32(&e, 0xDEADBEEFu);
    check(e.overflow, "four bytes into three refuses");
    check(e.i == 5, "and emits none of them");
    check(buf[5] == 0 && buf[6] == 0 && buf[7] == 0,
          "and leaves the three bytes it could have filled alone");

    /* Once overflowed, everything after is refused too: the sequence is void,
     * not merely truncated. */
    memset(buf, 0, sizeof buf);
    emit_init(&e, buf, 8, buf);
    emit_bytes(&e, "123456789", 9);
    check(e.overflow && e.i == 0, "an oversized first write emits nothing");
    emit8(&e, 0xCC);
    check(e.i == 0, "and a write that WOULD fit is still refused after");
}

/* ---- rel32 against `at`, not against `buf` ----------------------------- */
static void test_rel32_is_relative_to_where_it_runs(void)
{
    unsigned char buf[16];
    /* A plausible pool address, deliberately far from `buf`. */
    const unsigned char *at = (const unsigned char *)0x30000000u;
    const void *target = (const void *)0x30001000u;
    emit_t e;
    int32_t got;
    int32_t want;

    emit_init(&e, buf, sizeof buf, at);
    emit8(&e, 0xE9);                    /* jmp rel32 */
    emit_rel32(&e, target);
    check(e.i == 5 && !e.overflow, "a jmp rel32 is five bytes");

    /* The displacement is from the END of the instruction. */
    want = (int32_t)((intptr_t)target - ((intptr_t)at + 5));
    memcpy(&got, buf + 1, 4);
    check(got == want, "rel32 is resolved against `at`");

    /* And the same sequence built to run where it sits gets a different
     * displacement, which is the whole point of carrying `at` separately. */
    emit_init(&e, buf, sizeof buf, buf);
    emit8(&e, 0xE9);
    emit_rel32(&e, target);
    memcpy(&got, buf + 1, 4);
    check(got != want, "the same bytes at a different `at` differ");
    check(got == (int32_t)((intptr_t)target - ((intptr_t)buf + 5)),
          "and are right for where they would run");
}

/* ---- little-endian immediates ------------------------------------------ */
static void test_immediates(void)
{
    unsigned char buf[16];
    emit_t e;

    emit_init(&e, buf, sizeof buf, buf);
    emit32(&e, 0x44332211u);
    check(buf[0] == 0x11 && buf[1] == 0x22 && buf[2] == 0x33 && buf[3] == 0x44,
          "emit32 is little-endian");

    emit_init(&e, buf, sizeof buf, buf);
    emit_abs(&e, (const void *)0x00CAFE00u);
    check(buf[0] == 0x00 && buf[1] == 0xFE && buf[2] == 0xCA && buf[3] == 0x00,
          "emit_abs writes the address itself, not a displacement");
}

/* ---- short branches ---------------------------------------------------- */
static void test_branch_and_land(void)
{
    unsigned char buf[512];
    emit_t e;
    size_t fixup;
    unsigned i;

    /* A jump over three bytes. */
    emit_init(&e, buf, sizeof buf, buf);
    fixup = emit_branch8(&e, 0xEB);
    emit8(&e, 0x90); emit8(&e, 0x90); emit8(&e, 0x90);
    emit_land(&e, fixup);
    check(buf[0] == 0xEB && buf[1] == 3, "a short jump lands three bytes on");
    check(!e.overflow, "and does not overflow");

    /* 127 is the last displacement a short jump can carry. */
    emit_init(&e, buf, sizeof buf, buf);
    fixup = emit_branch8(&e, 0x74);
    for (i = 0; i < 127; i++)
        emit8(&e, 0x90);
    emit_land(&e, fixup);
    check(!e.overflow && buf[1] == 127, "127 is reachable");

    /* 128 is not, and is REFUSED rather than truncated to -128. */
    emit_init(&e, buf, sizeof buf, buf);
    fixup = emit_branch8(&e, 0x74);
    for (i = 0; i < 128; i++)
        emit8(&e, 0x90);
    emit_land(&e, fixup);
    check(e.overflow, "128 is out of a short jump's reach and is refused");

    /* A fixup that was never emitted is a no-op, not a write. */
    emit_init(&e, buf, sizeof buf, buf);
    emit8(&e, 0x90);
    emit_land(&e, 4);
    check(!e.overflow && e.i == 1, "landing on an index past the end does nothing");
}

/* ---- the two questions emit_ok answers --------------------------------- */
static void test_emit_ok(void)
{
    unsigned char buf[64];
    emit_t e;

    emit_init(&e, buf, sizeof buf, buf);
    emit_bytes(&e, buf, 32);
    log_reset();
    check(emit_ok(&e, 32, "test", "fits"), "32 bytes into a 32-byte allocation");
    check(log_count == 0, "and says nothing");

    log_reset();
    check(!emit_ok(&e, 31, "test", "too big"),
          "32 bytes into a 31-byte allocation is refused");
    check(log_count == 1 && strstr(last_log, "31") != NULL,
          "and the refusal names the allocation");

    emit_init(&e, buf, 8, buf);
    emit_bytes(&e, buf, 16);
    log_reset();
    check(!emit_ok(&e, 1024, "test", "overran"),
          "an overrun is refused however large the allocation");
    check(log_count == 1 && strstr(last_log, "overran") != NULL,
          "and the refusal says so");
}

/* ---- the [diagnostics] switch inside the emitter ----------------------- */
static void test_lfence_is_configured(void)
{
    unsigned char buf[16];
    emit_t e;

    g_cfg.time_fence = 0;
    emit_init(&e, buf, sizeof buf, buf);
    emit_lfence(&e);
    check(e.i == 0, "time_fence false emits no lfence");

    g_cfg.time_fence = 1;
    emit_init(&e, buf, sizeof buf, buf);
    emit_lfence(&e);
    check(e.i == 3 && buf[0] == 0x0F && buf[1] == 0xAE && buf[2] == 0xE8,
          "time_fence true emits 0F AE E8");
    g_cfg.time_fence = 0;
}

/* ---- every primitive refuses address 0 --------------------------------- */
static void test_refuse_null(void)
{
    /* An absent anchor IS 0 -- 38 of Offroad's are -- so these are reached in
     * ordinary operation by an installer that forgot to game_require(). They
     * must name the caller and return, not fault on the null page. */
    log_reset();
    patch_jmp(0, (const void *)0x30000000u);
    check(log_count == 1, "patch_jmp(0) refuses and logs");

    log_reset();
    check(!patch_call(0, (const void *)0x30000000u), "patch_call(0) returns 0");
    check(log_count == 1, "and logs");

    log_reset();
    patch_ret_imm(0, 0);
    check(log_count == 1, "patch_ret_imm(0) refuses and logs");
}

/* ---- and now run what it emitted --------------------------------------
 *
 * Everything above compares bytes to bytes I worked out by hand, which cannot
 * catch a sequence that is wrong in the same way twice. These two put the
 * emitted code in the executable pool and call it. */
static uint32_t s_calls;

static int __cdecl add_three(int a, int b, int c) { return a + b + c; }

static void test_emitted_code_runs(void)
{
    float (__cdecl *sqrt_fn)(float);
    int (__cdecl *counted)(int, int, int);
    void *p;

    patch_init();

    p = patch_emit_sqrt();
    check(p != NULL, "the sqrt stub is emitted");
    if (p) {
        sqrt_fn = (float (__cdecl *)(float))p;
        check(sqrt_fn(16.0f) == 4.0f, "and computes sqrt(16) = 4");
        check(sqrt_fn(2.25f) == 1.5f, "and sqrt(2.25) = 1.5");
    }

    s_calls = 0;
    p = thunk_count_only((const void *)add_three, &s_calls);
    check(p != NULL, "the counting thunk is emitted");
    if (p) {
        counted = (int (__cdecl *)(int, int, int))p;
        check(counted(1, 2, 3) == 6,
              "and the arguments and the return value pass through it");
        check(counted(10, 20, 30) == 60, "twice");
        check(s_calls == 2, "and it counted both calls");
    }

    patch_shutdown();
}

/* The pool is finite and its exhaustion is a refusal, not a wrap. */
static void test_pool_exhaustion(void)
{
    unsigned i;
    int refused = 0;

    patch_init();
    for (i = 0; i < POOL_SIZE / 16u + 2u; i++)
        if (!pool_alloc(16)) {
            refused = 1;
            break;
        }
    check(refused, "the thunk pool refuses once it is full");
    log_reset();
    check(pool_alloc(16) == NULL, "and keeps refusing");
    check(log_count == 1 && strstr(last_log, "exhausted") != NULL,
          "and says which pool");
    patch_shutdown();
}

int main(void)
{
    test_bounds_refuse_before_the_write();
    test_rel32_is_relative_to_where_it_runs();
    test_immediates();
    test_branch_and_land();
    test_emit_ok();
    test_lfence_is_configured();
    test_refuse_null();
    test_emitted_code_runs();
    test_pool_exhaustion();

    if (failures) {
        fprintf(stderr, "patch emitter tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("patch emitter tests: all passed");
    return 0;
}
