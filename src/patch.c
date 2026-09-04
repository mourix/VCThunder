/* patch.c -- runtime patch primitives: import slots, detours and thunks.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Everything that rewrites the game's resident code or builds code of our own
 * to hand it: an import slot, a jmp, a single call site, a cdecl -> stdcall
 * thunk, a prologue-relocating hook, and the counting and timing instruments
 * built out of the same parts. Glide thunks copy arguments because the game
 * caller and the provider callee each clean their own frame.
 *
 * Three rules bind every one of them.
 *
 * EVERY PRIMITIVE REFUSES ADDRESS 0. An absent anchor IS 0 -- 38 of Offroad's
 * are -- and an installer is supposed to game_require() before patching. These
 * are the backstop for the ones that do not, and they name the caller instead
 * of faulting inside a memcpy on the null page.
 *
 * EVERY BYTE GOES THROUGH THE EMITTER BELOW, which bounds-checks before it
 * writes rather than after, and which carries where the bytes will RUN
 * separately from where they are being assembled. Both matter: a sequence built
 * in a scratch buffer and copied into the pool has a different rel32 than the
 * same bytes assembled in place.
 *
 * THE POOL IS EXECUTABLE, FINITE AND NEVER FREED PIECEWISE. Exhaustion is a
 * refusal that names the pool, not a wrap.
 */
#include "vcthunder.h"

#include <string.h>

#define POOL_SIZE 0x20000u
/* Scratch deliberately exceeds the limit it is checked against; see the note
 * on patch_time_calls(), where a check that could only fire AFTER the overrun
 * cost a crash four bytes from the end of a stub that disassembled perfectly. */
#define THUNK_MAX     384
#define THUNK_SCRATCH 640

static unsigned char *s_pool;
static size_t s_used;

static unsigned char *pool_alloc(size_t n)
{
    unsigned char *p;

    n = (n + 15u) & ~(size_t)15u;              /* keep thunks 16-byte aligned */
    if (!s_pool || s_used + n > POOL_SIZE) {
        LOGE("thunk pool exhausted (%zu/%u bytes used)", s_used, POOL_SIZE);
        return NULL;
    }
    p = s_pool + s_used;
    s_used += n;
    return p;
}

void patch_init(void)
{
    s_pool = (unsigned char *)VirtualAlloc(NULL, POOL_SIZE,
                                           MEM_RESERVE | MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE);
    s_used = 0;
    if (!s_pool)
        LOGE("thunk pool VirtualAlloc failed, GetLastError=%lu", GetLastError());
}

void patch_shutdown(void)
{
    if (s_pool) {
        VirtualFree(s_pool, 0, MEM_RELEASE);
        s_pool = NULL;
    }
}

static void flush(void *at, size_t n)
{
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

/* ---- the emitter --------------------------------------------------------- */
/* Every stub in this file is hand-assembled. Spelling one out as
 * `buf[i++] = 0xNN` with a `memcpy(buf + i, &v, 4); i += 4;` after every
 * immediate is sixty chances to miss an `i += 4`, and the failure mode of one
 * missed increment is a stub that disassembles perfectly up to the point where
 * it does not -- in the file whose mistakes arrive as a process that vanishes
 * with no fault and no log.
 *
 * `emit_t` carries the three things those lines kept re-deriving, and the third
 * is the one that is easy to get wrong: WHERE THE BYTES WILL RUN is not where
 * they are being written. Several stubs are built in a scratch buffer and copied
 * into the pool afterwards, so a rel32 resolved against the scratch is resolved
 * against the wrong address. That distinction is carried by the type rather
 * than by a comment on one call site, and `emit_rel32` is the only thing that
 * computes a displacement.
 *
 * Every write is bounds-checked, so an overrun is REFUSED rather than detected
 * afterwards. That is what the scratch/limit pairs below were reaching for: the
 * old check could only fire after the buffer had already been overrun, which is
 * the very bug it existed to prevent. The margin stays, and is now belt to the
 * emitter's braces. */
typedef struct {
    unsigned char       *buf;       /* scratch being written into            */
    const unsigned char *at;        /* where buf[0] will be EXECUTED         */
    size_t               i;         /* bytes emitted so far                  */
    size_t               cap;       /* what buf holds                        */
    int                  overflow;  /* a write was refused; nothing is valid */
} emit_t;

static void emit_init(emit_t *e, unsigned char *buf, size_t cap, const void *at)
{
    e->buf = buf;
    e->at = (const unsigned char *)at;
    e->i = 0;
    e->cap = cap;
    e->overflow = 0;
}

static void emit_bytes(emit_t *e, const void *bytes, size_t n)
{
    if (e->overflow || e->i + n > e->cap) {
        e->overflow = 1;
        return;
    }
    memcpy(e->buf + e->i, bytes, n);
    e->i += n;
}

static void emit8(emit_t *e, unsigned char b)
{
    emit_bytes(e, &b, 1);
}

static void emit32(emit_t *e, uint32_t v)
{
    emit_bytes(e, &v, 4);
}

/* An absolute address as an immediate: a counter, a name, a timer field. */
static void emit_abs(emit_t *e, const void *p)
{
    emit32(e, (uint32_t)(uintptr_t)p);
}

/* The rel32 that follows the opcode just emitted, against `at` and never
 * against `buf`. See the note above. */
static void emit_rel32(emit_t *e, const void *target)
{
    int32_t rel = (int32_t)((intptr_t)target - ((intptr_t)(e->at + e->i) + 4));

    emit_bytes(e, &rel, 4);
}

/* `lfence`, at the six sites where [diagnostics] time_fence decides whether a
 * timestamp read may drift into the out-of-order window around it. */
static void emit_lfence(emit_t *e)
{
    if (g_cfg.time_fence) {
        emit8(e, 0x0F); emit8(e, 0xAE); emit8(e, 0xE8);
    }
}

/* A short jump (conditional, or EB) to a destination not yet emitted.
 * Returns the index of its displacement byte; give that back to emit_land()
 * once the target is reached. Displacements are inside the scratch and so do
 * not depend on `at`. */
static size_t emit_branch8(emit_t *e, unsigned char opcode)
{
    size_t fixup;

    emit8(e, opcode);
    fixup = e->i;
    emit8(e, 0x00);
    return fixup;
}

static void emit_land(emit_t *e, size_t fixup)
{
    size_t delta;

    if (e->overflow || fixup >= e->i)
        return;
    delta = e->i - (fixup + 1);
    if (delta > 127u) {                 /* a short jump cannot reach it */
        e->overflow = 1;
        return;
    }
    e->buf[fixup] = (unsigned char)delta;
}

/* Did the sequence fit, and does it fit the allocation it is going into? */
static int emit_ok(const emit_t *e, size_t limit, const char *what,
                   const char *name)
{
    if (e->overflow) {
        LOGE("%s: %s overran its %zu-byte scratch or could not reach a jump "
             "target; refusing", what, name ? name : "?", e->cap);
        return 0;
    }
    if (e->i > limit) {
        LOGE("%s: %s is %zu bytes, the allocation is %zu; refusing",
             what, name ? name : "?", e->i, limit);
        return 0;
    }
    return 1;
}

void patch_slot(uint32_t slot_va, const void *target)
{
    *(const void **)(uintptr_t)slot_va = target;
}

/* Every primitive here refuses address 0.
 *
 * An absent anchor IS 0 (38 of Offroad's are), and every installer is
 * supposed to game_require() before patching. This is the backstop for the ones
 * that do not, and it names the caller's address instead of faulting on the
 * null page inside a memcpy, which is what a missed check has looked like
 * every time so far. */
static int refuse_null(uint32_t va, const char *what)
{
    if (va)
        return 0;
    LOGE("%s: refusing address 0; an anchor this title does not have reached "
         "a patch primitive; the caller is missing a game_require()", what);
    return 1;
}

void patch_jmp(uint32_t va, const void *target)
{
    unsigned char *p = (unsigned char *)(uintptr_t)va;
    int32_t rel = (int32_t)((intptr_t)target - ((intptr_t)p + 5));

    if (refuse_null(va, "patch_jmp"))
        return;
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    flush(p, 5);
}

/* Redirect one verified E8 call site while leaving the target function and its
 * other callers unchanged. */
int patch_call(uint32_t site_va, const void *target)
{
    unsigned char *p = (unsigned char *)(uintptr_t)site_va;
    int32_t rel;

    if (refuse_null(site_va, "patch_call"))
        return 0;
    if (p[0] != 0xE8) {
        LOGE("patch_call: 0x%08X is 0x%02X, not a call (E8); refusing",
             site_va, p[0]);
        return 0;
    }
    rel = (int32_t)((intptr_t)target - ((intptr_t)p + 5));
    memcpy(p + 1, &rel, 4);
    flush(p, 5);
    return 1;
}

void patch_ret_imm(uint32_t va, uint32_t eax_value)
{
    unsigned char *p = (unsigned char *)(uintptr_t)va;

    if (refuse_null(va, "patch_ret_imm"))
        return;
    p[0] = 0xB8;                                /* mov eax, imm32 */
    memcpy(p + 1, &eax_value, 4);
    p[5] = 0xC3;                                /* ret            */
    flush(p, 6);
}

/* ---- logging stub ------------------------------------------------------- */

static void stub_hit(const char *name)
{
    LOGT("stub: %s", name);
}

void *stub_logging(const char *name, uint32_t eax_value)
{
    unsigned char *t = pool_alloc(24);
    emit_t e;

    if (!t)
        return NULL;
    emit_init(&e, t, 24, t);                    /* built where it runs        */

    emit8(&e, 0x68);                            /* push imm32 (name)          */
    emit_abs(&e, name);
    emit8(&e, 0xE8);                            /* call stub_hit (cdecl)      */
    emit_rel32(&e, (void *)stub_hit);
    emit8(&e, 0x83); emit8(&e, 0xC4); emit8(&e, 0x04);  /* add esp, 4         */
    emit8(&e, 0xB8);                            /* mov eax, imm32             */
    emit32(&e, eax_value);
    emit8(&e, 0xC3);                            /* ret  (cdecl: caller cleans)*/

    if (!emit_ok(&e, 24, "stub", name))
        return NULL;
    flush(t, e.i);
    return t;
}

/* ---- call-through hook -------------------------------------------------- */
/* Install a call-through detour using a per-title prologue descriptor. The
 * descriptor carries the complete instruction length and, when present, one
 * rel32 offset that must be rebased in the trampoline. */
void *hook_prepare_call_through(uint32_t va, uint32_t prologue)
{
    unsigned char *p = (unsigned char *)(uintptr_t)va;
    unsigned len   = prologue & 0xFFu;
    unsigned reloc = (prologue >> 8) & 0xFFu;
    unsigned char *t;
    emit_t e;

    if (refuse_null(va, "hook_call_through"))
        return NULL;
    if (len < 5 || len > 64) {
        LOGE("hook: prologue metadata 0x%X gives length %u at 0x%08X; "
             "refusing (regenerate generated/ with `make defs`)",
             prologue, len, va);
        return NULL;
    }
    if (reloc && reloc + 4 > len) {
        LOGE("hook: rel32 at +%u does not fit in a %u-byte prologue at 0x%08X",
             reloc, len, va);
        return NULL;
    }
    t = pool_alloc((size_t)len + 8);
    if (!t)
        return NULL;
    emit_init(&e, t, (size_t)len + 8, t);       /* built where it runs        */

    emit_bytes(&e, p, (size_t)len);             /* the prologue, verbatim     */
    if (reloc) {
        /* The one instruction that moved: the same absolute target, said from
         * the trampoline's address instead of the function's. */
        int32_t disp;

        memcpy(&disp, p + reloc, 4);
        disp = (int32_t)(((intptr_t)(p + reloc + 4) + disp)
                         - (intptr_t)(t + reloc + 4));
        memcpy(t + reloc, &disp, 4);
    }
    emit8(&e, 0xE9);                            /* jmp target+len             */
    emit_rel32(&e, p + len);

    if (!emit_ok(&e, (size_t)len + 8, "hook", NULL))
        return NULL;
    flush(t, e.i);
    return t;
}

void *hook_call_through(uint32_t va, const void *replacement, uint32_t prologue)
{
    void *trampoline = hook_prepare_call_through(va, prologue);

    if (trampoline)
        patch_jmp(va, replacement);
    return trampoline;
}

/* ---- cdecl -> stdcall thunk --------------------------------------------- */
/* Copy the cdecl argument block into a temporary stdcall frame. The provider
 * removes the copy, then the game caller removes its original arguments. */
/* Optional per-entry-point CYCLE accounting, on top of the call counter.
 *
 * Safe here in a way it was not for the game's own functions: these thunks have
 * a known target, known arity and a known convention, so the timing brackets a
 * plain `call` instead of displacing a return address. t0 is a single global
 * because Glide calls do not nest and are made only from the render thread. */
static uint64_t s_thunk_t0;

/* THE TAIL BOTH CYCLE-ACCOUNTING STUBS SHARE.
 *
 * On entry edx:eax holds the call's duration and the flags are already saved by
 * the caller. A duration that overflows 32 bits is a preemption rather than a
 * call, so it is kept out of the minimum and out of the histogram.
 *
 * `a_hist` is 0 where there is no histogram, and that is not a preference:
 * `bsr` clobbers ecx, which patch_time_calls is still holding the nesting depth
 * in. The thunk has no such live register -- its callee has returned and only
 * edi/esi/ebp are popped after this -- so it passes one. */
static void emit_min_fast(emit_t *e, uint32_t a_min, uint32_t a_fast,
                          uint32_t fast_lim, uint32_t a_hist)
{
    size_t done_hi, done_fast, nomin;

    emit8(e, 0x09); emit8(e, 0xD2);             /* or edx,edx                 */
    done_hi = emit_branch8(e, 0x75);                /* jnz done                   */
    if (a_hist) {
        /* bucket = floor(log2(cycles)). bsr sets ZF when the source is 0. */
        size_t over;

        emit8(e, 0x0F); emit8(e, 0xBD); emit8(e, 0xC8);  /* bsr ecx,eax       */
        over = emit_branch8(e, 0x74);               /* jz over                    */
        emit8(e, 0xFF); emit8(e, 0x04); emit8(e, 0x8D);  /* inc [hist+ecx*4]  */
        emit32(e, a_hist);
        emit_land(e, over);
    }
    emit8(e, 0x3B); emit8(e, 0x05);             /* cmp eax,[min]              */
    emit32(e, a_min);
    nomin = emit_branch8(e, 0x73);                  /* jae nomin                  */
    emit8(e, 0xA3);                             /* mov [min],eax              */
    emit32(e, a_min);
    emit_land(e, nomin);
    emit8(e, 0x3D);                             /* cmp eax,FAST               */
    emit32(e, fast_lim);
    done_fast = emit_branch8(e, 0x73);              /* jae done                   */
    emit8(e, 0xFF); emit8(e, 0x05);             /* inc [fast]                 */
    emit32(e, a_fast);
    emit_land(e, done_hi);
    emit_land(e, done_fast);
}

void *thunk_cdecl_to_stdcall(const void *target, int argbytes, const char *name,
                             uint32_t *counter, struct glide_timer *timer,
                             glide_first_fn on_first, uint32_t cookie,
                             uint32_t *seen_flag)
{
    unsigned char  buf[THUNK_SCRATCH];
    unsigned char *t;
    emit_t e;
    uint32_t dwords;
    uint32_t a_t0    = (uint32_t)(uintptr_t)&s_thunk_t0;
    uint32_t a_t0_hi = a_t0 + 4u;
    uint32_t a_cyc   = (uint32_t)(uintptr_t)(timer ? &timer->cycles : NULL);
    uint32_t a_cyc_hi = a_cyc + 4u;
    uint32_t a_min   = (uint32_t)(uintptr_t)(timer ? &timer->min : NULL);
    uint32_t a_fast  = (uint32_t)(uintptr_t)(timer ? &timer->fast : NULL);
    uint32_t a_hist  = (uint32_t)(uintptr_t)(timer ? timer->hist : NULL);
    uint32_t fast_lim = GLIDE_FAST_CYCLES;
    const int cycles = timer != NULL;

    if (timer) {
        timer->min = 0xFFFFFFFFu;
        timer->fast = 0;
        memset(timer->hist, 0, sizeof timer->hist);
    }

    if (argbytes < 0 || (argbytes & 3) != 0) {
        LOGE("thunk: bad argbytes %d (must be a non-negative multiple of 4)",
             argbytes);
        return NULL;
    }
    t = pool_alloc(THUNK_MAX);
    if (!t)
        return NULL;
    /* Assembled in scratch and copied into the pool, so every rel32 below is
     * resolved against `t`, which is where these bytes will run. */
    emit_init(&e, buf, sizeof buf, t);
    dwords = (uint32_t)argbytes / 4u;

    /* Optional call counter: `inc dword [abs32]`, six bytes, no register
     * clobbered and only flags touched, which are caller-saved across a call
     * boundary anyway, so this is safe at function entry. Cheap enough to leave
     * enabled permanently: the per-frame Glide call mix is the measurement that
     * separates "the game submits too much" from "the wrapper is slow per call",
     * and it cannot be recovered after the fact. */
    if (counter) {
        emit8(&e, 0xFF); emit8(&e, 0x05);       /* inc dword [counter]        */
        emit_abs(&e, counter);
    }

    /* Optional trace prologue. Emitted before the frame is set up, which is
     * safe: `push imm32 / call / add esp,4` is esp-neutral, the game's cdecl
     * arguments are on the stack rather than in registers, and stub_hit only
     * clobbers caller-saved eax/ecx/edx. Baked in at build time rather than
     * branched at run time so the fast path stays a straight thunk. */
    if (name && g_log_level >= LOG_TRACE) {
        emit8(&e, 0x68);                          /* push imm32 (name)          */
        emit_abs(&e, name);
        emit8(&e, 0xE8);                          /* call stub_hit              */
        emit_rel32(&e, (void *)stub_hit);
        emit8(&e, 0x83); emit8(&e, 0xC4); emit8(&e, 0x04);   /* add esp, 4      */
    }

    emit8(&e, 0x55);                              /* push ebp                   */
    emit8(&e, 0x8B); emit8(&e, 0xEC);             /* mov  ebp, esp              */

    /* Capture arguments on the first call without assuming a signature beyond
     * the generated argument-byte count. */
    /* Preserve this one-shot gate: omitting its emitted byte sequence causes
     * deterministic unpack failure before any thunk executes. Repeated capture
     * is implemented by clearing seen_flag in the callback. */
    if (on_first && seen_flag) {
        uint32_t a_seen = (uint32_t)(uintptr_t)seen_flag;
        size_t skip;

        emit8(&e, 0x83); emit8(&e, 0x3D);         /* cmp dword [seen], 0        */
        emit32(&e, a_seen);
        emit8(&e, 0x00);
        skip = emit_branch8(&e, 0x75);                /* jne skip                   */
        emit8(&e, 0xC7); emit8(&e, 0x05);         /* mov dword [seen], 1        */
        emit32(&e, a_seen);
        emit32(&e, 1u);
        emit8(&e, 0x8D); emit8(&e, 0x45); emit8(&e, 0x08);   /* lea eax,[ebp+8] */
        emit8(&e, 0x50);                          /* push eax   (args)          */
        emit8(&e, 0x68);                          /* push imm32 (cookie)        */
        emit32(&e, cookie);
        emit8(&e, 0xE8);                          /* call on_first (cdecl)      */
        emit_rel32(&e, (void *)on_first);
        emit8(&e, 0x83); emit8(&e, 0xC4); emit8(&e, 0x08);   /* add esp, 8      */
        emit_land(&e, skip);
    } else if (on_first) {
        LOGE("thunk: %s; a callback without a seen_flag is refused; it "
             "breaks __pl_unpackrom. Re-arm the flag from the callback instead.",
             name ? name : "?");
    }

    emit8(&e, 0x56);                              /* push esi                   */
    emit8(&e, 0x57);                              /* push edi                   */

    if (argbytes > 0) {
        if (argbytes < 128) {
            emit8(&e, 0x83); emit8(&e, 0xEC);     /* sub esp, imm8              */
            emit8(&e, (unsigned char)argbytes);
        } else {
            emit8(&e, 0x81); emit8(&e, 0xEC);     /* sub esp, imm32             */
            emit32(&e, (uint32_t)argbytes);
        }
        emit8(&e, 0x8D); emit8(&e, 0x75); emit8(&e, 0x08);  /* lea esi,[ebp+8]  */
        emit8(&e, 0x8B); emit8(&e, 0xFC);         /* mov edi, esp               */
        emit8(&e, 0xB9);                          /* mov ecx, dwords            */
        emit32(&e, dwords);
        emit8(&e, 0xFC);                          /* cld                        */
        emit8(&e, 0xF3); emit8(&e, 0xA5);         /* rep movsd                  */
    }

    if (cycles) {
        /* Fence timestamp reads around short calls when requested. */
        emit_lfence(&e);
        emit8(&e, 0x0F); emit8(&e, 0x31);         /* rdtsc                      */
        emit_lfence(&e);
        emit8(&e, 0xA3);                          /* mov [t0], eax              */
        emit32(&e, a_t0);
        emit8(&e, 0x89); emit8(&e, 0x15);         /* mov [t0+4], edx            */
        emit32(&e, a_t0_hi);
    }

    emit8(&e, 0xE8);                              /* call target                */
    emit_rel32(&e, target);

    if (cycles) {
        /* eax/edx carry the return value and rdtsc destroys both, so both are
         * saved across the second read. Flags too: a zero-argument entry point
         * has no `add esp,N` after it to make them provably dead. */
        emit8(&e, 0x50);                          /* push eax                   */
        emit8(&e, 0x52);                          /* push edx                   */
        emit8(&e, 0x9C);                          /* pushfd                     */
        emit_lfence(&e);
        emit8(&e, 0x0F); emit8(&e, 0x31);         /* rdtsc                      */
        emit_lfence(&e);
        emit8(&e, 0x2B); emit8(&e, 0x05);         /* sub eax, [t0]              */
        emit32(&e, a_t0);
        emit8(&e, 0x1B); emit8(&e, 0x15);         /* sbb edx, [t0+4]            */
        emit32(&e, a_t0_hi);
        emit8(&e, 0x01); emit8(&e, 0x05);         /* add [cycles], eax          */
        emit32(&e, a_cyc);
        emit8(&e, 0x11); emit8(&e, 0x15);         /* adc [cycles+4], edx        */
        emit32(&e, a_cyc_hi);
        /* The distribution. edx:eax still holds the duration and the flags are
         * saved by the pushfd above, which is the contract emit_min_fast()
         * documents. */
        emit_min_fast(&e, a_min, a_fast, fast_lim, a_hist);
        emit8(&e, 0x9D);                          /* popfd                      */
        emit8(&e, 0x5A);                          /* pop edx                    */
        emit8(&e, 0x58);                          /* pop eax                    */
    }

    emit8(&e, 0x5F);                              /* pop edi                    */
    emit8(&e, 0x5E);                              /* pop esi                    */
    emit8(&e, 0x5D);                              /* pop ebp                    */
    emit8(&e, 0xC3);                              /* ret                        */

    if (!emit_ok(&e, THUNK_MAX, "thunk", name))
        return NULL;
    memcpy(t, buf, e.i);
    flush(t, e.i);
    return t;
}

/* Count calls without depending on function signature. The entry stub updates
 * a counter and jumps to a call-through trampoline without touching the stack
 * or registers. */
int patch_count_calls(uint32_t va, uint32_t *counter, int prologue,
                      const char *name)
{
    unsigned char *stub = pool_alloc(16);
    unsigned char *tramp;
    emit_t e;

    if (!stub)
        return 0;

    tramp = (unsigned char *)hook_call_through(va, stub, prologue);
    if (!tramp) {
        LOGW("count: could not hook %s @ 0x%08X", name, va);
        return 0;
    }

    emit_init(&e, stub, 16, stub);       /* built where it runs */
    emit8(&e, 0xFF); emit8(&e, 0x05);    /* inc dword [counter] */
    emit_abs(&e, counter);
    emit8(&e, 0xE9);                     /* jmp trampoline      */
    emit_rel32(&e, tramp);
    if (!emit_ok(&e, 16, "count", name))
        return 0;
    flush(stub, 16);

    LOGI("count: %s @ 0x%08X instrumented", name, va);
    return 1;
}

/* Count calls through a stdcall entry point, with nothing else changed.
 *
 * `inc [counter]` then `jmp target`: the callee's own `ret N` returns straight
 * to the game's caller, so the stack is untouched and the argument size does
 * not need to be known. Two instructions, no frame, no risk, which is what
 * makes it usable across all 123 Win32 entry points at once. */
void *thunk_count_only(const void *target, uint32_t *counter)
{
    unsigned char *t = pool_alloc(16);
    emit_t e;

    if (!t)
        return NULL;
    emit_init(&e, t, 16, t);                    /* built where it runs        */
    emit8(&e, 0xFF); emit8(&e, 0x05);           /* inc dword [counter]        */
    emit_abs(&e, counter);
    emit8(&e, 0xE9);                            /* jmp target                 */
    emit_rel32(&e, target);
    if (!emit_ok(&e, 16, "count", NULL))
        return NULL;
    flush(t, 16);
    return t;
}

/* Time calls of unknown signature by replacing each return address with an
 * epilogue thunk. The thunk preserves arguments, registers, flags, integer and
 * x87 return values, then returns to the saved caller. Storage is process-global,
 * so the instrumentation accepts only its configured thread. */
/* Emit into a bounded local buffer, validate the generated length, then copy
 * into the executable pool. */
#define STUB_MAX 192
/* Scratch is deliberately larger than the limit it is checked against. Both
 * sequences below are fixed-length (68 and 72 bytes, no loops, no
 * variable-length operands), so the check exists for the NEXT edit, and a
 * check that can only fire after it has already overrun is the very bug this
 * is here to prevent. The margin is what makes it a check rather than a
 * post-mortem. */
#define STUB_SCRATCH 384

/* Every timer installed, so patch_timer_set_owner() can hand them all to the
 * render thread once that thread has identified itself. */
#define MAX_TIMERS 16
static struct call_timer *s_timers[MAX_TIMERS];
static unsigned s_ntimers;

void patch_timer_reset(struct call_timer *t)
{
    t->cycles = 0;
    t->calls = 0;
    t->overflow = 0;
    t->fast = 0;
    t->min = 0xFFFFFFFFu;               /* not zero: zero would win every race */
}

void patch_timer_set_owner(uint32_t tid)
{
    unsigned i;

    for (i = 0; i < s_ntimers; i++)
        s_timers[i]->owner = tid;
}

void *patch_emit_sqrt(void)
{
    unsigned char *fn = pool_alloc(16);
    emit_t e;

    if (!fn)
        return NULL;
    emit_init(&e, fn, 16, fn);
    emit8(&e, 0xD9); emit8(&e, 0x44); emit8(&e, 0x24); emit8(&e, 0x04);
                                                            /* fld [esp+4]   */
    emit8(&e, 0xD9); emit8(&e, 0xFA);                       /* fsqrt         */
    emit8(&e, 0xC3);                                        /* ret           */
    if (!emit_ok(&e, 16, "sqrt", NULL))
        return NULL;
    memset(fn + e.i, 0x90, 16 - e.i);
    flush(fn, 16);
    return fn;
}

int patch_time_calls(uint32_t va, struct call_timer *t, int prologue,
                     const char *name)
{
    unsigned char  buf[STUB_SCRATCH];
    unsigned char *entry = pool_alloc(STUB_MAX);
    unsigned char *epi   = pool_alloc(STUB_MAX);
    unsigned char *tramp;
    emit_t   e;
    size_t   full, offthread, out, offout;
    uint32_t a_depth    = (uint32_t)(uintptr_t)&t->depth;
    uint32_t a_overflow = (uint32_t)(uintptr_t)&t->overflow;
    uint32_t a_calls    = (uint32_t)(uintptr_t)&t->calls;
    uint32_t a_cyc      = (uint32_t)(uintptr_t)&t->cycles;
    uint32_t a_cyc_hi   = a_cyc + 4u;
    uint32_t a_t0       = (uint32_t)(uintptr_t)t->t0;
    uint32_t a_t0_hi    = a_t0 + 4u;
    uint32_t a_ret      = (uint32_t)(uintptr_t)t->ret;
    uint32_t a_min      = (uint32_t)(uintptr_t)&t->min;
    uint32_t a_fast     = (uint32_t)(uintptr_t)&t->fast;
    uint32_t a_owner    = (uint32_t)(uintptr_t)&t->owner;
    uint32_t a_offthr   = (uint32_t)(uintptr_t)&t->offthread;
    uint32_t fast_lim   = CALL_TIMER_FAST;
    uint32_t epi32      = (uint32_t)(uintptr_t)epi;

    if (!entry || !epi)
        return 0;

    t->name = name;
    patch_timer_reset(t);
    /* The installing thread by default; glide_bind() re-claims all of them for
     * the render thread at the first buffer swap, which is the thread whose
     * frames these numbers describe. */
    t->owner = (uint32_t)GetCurrentThreadId();
    if (s_ntimers < MAX_TIMERS)
        s_timers[s_ntimers++] = t;
    else
        LOGW("time: timer registry full, '%s' will not follow the render "
             "thread", name);

    /* --- epilogue: entered by the target's own `ret` --------------------- */
    emit_init(&e, buf, sizeof buf, epi);
    emit8(&e, 0x68);                                  /* push imm32 (dead)   */
    emit32(&e, 0);
    emit8(&e, 0x50);                                  /* push eax            */
    emit8(&e, 0x52);                                  /* push edx            */
    emit8(&e, 0x51);                                  /* push ecx            */
    emit8(&e, 0x9C);                                  /* pushfd              */
    /* Fence both timestamp reads so short calls cannot overlap the measurements. */
    emit_lfence(&e);
    emit8(&e, 0x0F); emit8(&e, 0x31);                 /* rdtsc               */
    emit_lfence(&e);
    emit8(&e, 0x8B); emit8(&e, 0x0D);                 /* mov ecx,[depth]     */
    emit32(&e, a_depth);
    emit8(&e, 0x49);                                  /* dec ecx             */
    emit8(&e, 0x89); emit8(&e, 0x0D);                 /* mov [depth],ecx     */
    emit32(&e, a_depth);
    emit8(&e, 0x2B); emit8(&e, 0x04); emit8(&e, 0xCD);/* sub eax,[t0+ecx*8]  */
    emit32(&e, a_t0);
    emit8(&e, 0x1B); emit8(&e, 0x14); emit8(&e, 0xCD);/* sbb edx,[t0+ecx*8+4]*/
    emit32(&e, a_t0_hi);
    emit8(&e, 0x01); emit8(&e, 0x05);                 /* add [cycles],eax    */
    emit32(&e, a_cyc);
    emit8(&e, 0x11); emit8(&e, 0x15);                 /* adc [cycles+4],edx  */
    emit32(&e, a_cyc_hi);
    emit8(&e, 0xFF); emit8(&e, 0x05);                 /* inc [calls]         */
    emit32(&e, a_calls);

    /* Record the duration distribution. No histogram here, and no bsr with it:
     * ecx is holding the nesting depth the two lines below index with. */
    emit_min_fast(&e, a_min, a_fast, fast_lim, 0);

    emit8(&e, 0x8B); emit8(&e, 0x04); emit8(&e, 0x8D);/* mov eax,[ret+ecx*4] */
    emit32(&e, a_ret);
    emit8(&e, 0x89); emit8(&e, 0x44);                 /* mov [esp+16],eax    */
    emit8(&e, 0x24); emit8(&e, 0x10);                 /*   = the placeholder */
    emit8(&e, 0x9D);                                  /* popfd               */
    emit8(&e, 0x59);                                  /* pop ecx             */
    emit8(&e, 0x5A);                                  /* pop edx             */
    emit8(&e, 0x58);                                  /* pop eax             */
    emit8(&e, 0xC3);                                  /* ret -> real caller  */
    if (!emit_ok(&e, STUB_MAX, "time epilogue", name))
        return 0;
    memcpy(epi, buf, e.i);
    flush(epi, e.i);

    /* --- entry stub ------------------------------------------------------ */
    tramp = (unsigned char *)hook_call_through(va, entry, prologue);
    if (!tramp) {
        LOGW("time: could not hook %s @ 0x%08X", name, va);
        return 0;
    }

    /* `entry`, not `buf`: the emitter is told where these bytes will RUN, so
     * the trailing rel32 is resolved there while the short jumps below stay
     * relative to the scratch. */
    emit_init(&e, buf, sizeof buf, entry);
    emit8(&e, 0x60);                                /* pushad (32 bytes)   */
    /* Decline timing on other threads because the displaced-return stack is
     * process-global. fs:[0x24] supplies the Win32 thread ID without a call. */
    emit8(&e, 0x64); emit8(&e, 0xA1);             /* mov eax,fs:[0x24]   */
    emit32(&e, 0x24u);
    emit8(&e, 0x3B); emit8(&e, 0x05);             /* cmp eax,[owner]     */
    emit32(&e, a_owner);
    offthread = emit_branch8(&e, 0x75);               /* jne offthread       */
    emit_lfence(&e);
    emit8(&e, 0x0F); emit8(&e, 0x31);             /* rdtsc               */
    emit_lfence(&e);
    emit8(&e, 0x8B); emit8(&e, 0x0D);             /* mov ecx,[depth]     */
    emit32(&e, a_depth);
    emit8(&e, 0x83); emit8(&e, 0xF9);             /* cmp ecx, DEPTH      */
    emit8(&e, (unsigned char)CALL_TIMER_DEPTH);
    full = emit_branch8(&e, 0x73);                    /* jae full            */
    emit8(&e, 0x89); emit8(&e, 0x04); emit8(&e, 0xCD);  /* mov [t0+ecx*8],eax  */
    emit32(&e, a_t0);
    emit8(&e, 0x89); emit8(&e, 0x14); emit8(&e, 0xCD);  /* mov [t0+ecx*8+4],edx*/
    emit32(&e, a_t0_hi);
    emit8(&e, 0x8B); emit8(&e, 0x44);             /* mov eax,[esp+32]    */
    emit8(&e, 0x24); emit8(&e, 0x20);             /*   = return address  */
    emit8(&e, 0x89); emit8(&e, 0x04); emit8(&e, 0x8D);  /* mov [ret+ecx*4],eax */
    emit32(&e, a_ret);
    emit8(&e, 0xC7); emit8(&e, 0x44);             /* mov [esp+32],epi    */
    emit8(&e, 0x24); emit8(&e, 0x20);
    emit32(&e, epi32);
    emit8(&e, 0x41);                                /* inc ecx             */
    emit8(&e, 0x89); emit8(&e, 0x0D);             /* mov [depth],ecx     */
    emit32(&e, a_depth);
    out = emit_branch8(&e, 0xEB);                       /* jmp out             */
    emit_land(&e, offthread);
    emit8(&e, 0xFF); emit8(&e, 0x05);             /* inc [offthread]     */
    emit32(&e, a_offthr);
    offout = emit_branch8(&e, 0xEB);                    /* jmp out             */
    emit_land(&e, full);
    emit8(&e, 0xFF); emit8(&e, 0x05);             /* inc [overflow]      */
    emit32(&e, a_overflow);
    emit_land(&e, out);
    emit_land(&e, offout);
    emit8(&e, 0x61);                                /* popad               */
    emit8(&e, 0xE9);                                /* jmp trampoline      */
    emit_rel32(&e, tramp);
    if (!emit_ok(&e, STUB_MAX, "time entry stub", name))
        return 0;
    memcpy(entry, buf, e.i);
    flush(entry, e.i);

    LOGI("time: %s @ 0x%08X instrumented (entry %p, epilogue %p)",
         name, va, (void *)entry, (void *)epi);
    return 1;
}

/* Measure what the two stubs above cost, so the figures they produce can be
 * corrected rather than caveated. The target is six bytes we emit ourselves
 * (five nops and a ret), so nothing about a real function's prologue, alignment
 * or callees contaminates the baseline. Timed uninstrumented first, then
 * hooked, then differenced. */
static struct call_timer s_cal;
static void (__cdecl *s_cal_call)(void);

uint64_t patch_timer_overhead(void)
{
    struct call_timer *const pcal = &s_cal;
    unsigned char *fn = pool_alloc(16);
    void (__cdecl *call)(void);
    uint64_t t0, bare, hooked;
    unsigned n;
    const unsigned N = 200000u;

    if (!fn)
        return 0;
    memset(fn, 0x90, 6);                              /* nop x5              */
    fn[5] = 0xC3;                                     /* ret                 */
    flush(fn, 16);
    call = (void (__cdecl *)(void))fn;

    for (n = 0; n < 1000u; n++)                       /* warm i-cache/BTB    */
        call();
    t0 = diag_rdtsc();
    for (n = 0; n < N; n++)
        call();
    bare = diag_rdtsc() - t0;

    if (!patch_time_calls((uint32_t)(uintptr_t)fn, pcal, 5, "timer calibration"))
        return 0;
    s_cal_call = call;
    for (n = 0; n < 1000u; n++)
        call();
    patch_timer_reset(pcal);
    t0 = diag_rdtsc();
    for (n = 0; n < N; n++)
        call();
    hooked = diag_rdtsc() - t0;

    LOGI("time: calibration; bare call %.1f cycles, instrumented %.1f, "
         "overhead %.1f cycles/call (self-reported %.1f, min %u, %.0f%% under "
         "%u, depth residual %u)",
         (double)bare / N, (double)hooked / N,
         (double)(hooked - bare) / N,
         pcal->calls ? (double)pcal->cycles / pcal->calls : 0.0,
         pcal->min == 0xFFFFFFFFu ? 0u : pcal->min,
         pcal->calls ? 100.0 * pcal->fast / pcal->calls : 0.0,
         CALL_TIMER_FAST, pcal->depth);
    return (hooked - bare) / N;
}

/* Repeat calibration during gameplay on the render thread to separate
 * instrument overhead from call-site and working-set effects. */
uint64_t patch_timer_recalibrate(void)
{
    struct call_timer *const pcal = &s_cal;
    unsigned n;
    const unsigned N = 20000u;
    uint64_t c0, n0;

    if (!s_cal_call)
        return 0;
    for (n = 0; n < 200u; n++)
        s_cal_call();
    c0 = pcal->cycles;
    n0 = pcal->calls;
    for (n = 0; n < N; n++)
        s_cal_call();
    if (pcal->calls == n0)
        return 0;
    return (pcal->cycles - c0) / (pcal->calls - n0);
}
