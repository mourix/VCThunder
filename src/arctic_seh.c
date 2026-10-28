/* arctic_seh.c -- the frame-based SEH handler the game's images import.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * WHY THIS EXISTS. snow.exe, goose.dll and gobj.dll were built by Intel C 4.5,
 * which put each function's SEH scope table in .data1, a WRITABLE section.
 * Today's msvcrt _except_handler3 will not run a scope table that lives in
 * writable memory, so every __try in the game is dead: the boot census's first
 * run died of goose's SetThreadName exception, which its own __except should
 * have caught. This file is the same
 * contract without that check. It is bound in place of
 * MSVCRT!_except_handler3 for those three images only.
 *
 * THE CONTRACT, from the published description of Visual C++'s frame-based SEH
 * (M. Pietrek, "A Crash Course on the Depths of Win32 Structured Exception
 * Handling", Microsoft Systems Journal, January 1997) and RtlUnwind's
 * documented signature. Nothing here is copied from a runtime library.
 *
 *   EstablisherFrame R, pushed by the function's prologue:
 *     R+0x00 previous registration   R+0x04 handler (an import thunk to us)
 *     R+0x08 scope table             R+0x0C try level (-1: outside any __try)
 *     R+0x10 the function's saved EBP: its frame pointer is R+0x10
 *   Frame-relative: [ebp-0x14] receives the EXCEPTION_POINTERS the filter
 *   reads; [ebp-0x18] holds the ESP the __except body restores first.
 *   Scope entry: { enclosing level, filter or NULL for __finally, body }.
 *
 *   Dispatch: walk the levels from R's current one outwards. A filter returning
 *   <0 continues execution, 0 keeps searching, >0 takes the handler: unwind
 *   every frame above R (RtlUnwind), run the __finally blocks inside R between
 *   its current level and this one, set R's level to the enclosing one, and
 *   enter the __except body with EBP = R+0x10. It never returns.
 *   Unwind (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND): run R's __finally
 *   blocks down to level -1 and keep searching.
 *
 * NOT DONE: msvcrt's local unwind registers a frame of its own so a fault
 * inside a __finally block during an unwind is reported against it. A fault
 * there is reported against the next frame out instead.
 */
#include "vcthunder.h"
#include "arctic.h"

#include <stdint.h>

#define EXC_UNWINDING   0x2     /* EXCEPTION_UNWINDING */
#define EXC_EXIT_UNWIND 0x4     /* EXCEPTION_EXIT_UNWIND */

typedef struct {
    int   enclosing;
    void *filter;               /* NULL: a __finally block   */
    void *body;
} eh3_scope_t;

typedef struct eh3_reg {
    struct eh3_reg *prev;
    void           *handler;
    eh3_scope_t    *scope;
    int             level;
} eh3_reg_t;

/* What the handler did, for the log: a run that never raised an exception
 * through these frames has not tested them. */
static volatile LONG s_searched, s_continued, s_handled, s_unwound;

void arctic_seh_report(void)
{
    LOGI("arctic: SEH: %ld dispatches, %ld continued, %ld handled by an __except"
         " body, %ld unwinds", s_searched, s_continued, s_handled, s_unwound);
}

static char *frame_ebp(eh3_reg_t *r)
{
    return (char *)r + 0x10;
}

/* Call a filter or a __finally block: code of the protected function, run on
 * its frame pointer, returning in EAX. Callee-saved registers are kept
 * whatever the funclet does with them. */
static int call_funclet(void *fn, char *ebp)
{
    int r;

    __asm__ volatile(
        "pushl %%ebp\n\t"
        "pushl %%ebx\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "movl  %%edx, %%ebp\n\t"
        "call  *%%ecx\n\t"
        "popl  %%edi\n\t"
        "popl  %%esi\n\t"
        "popl  %%ebx\n\t"
        "popl  %%ebp\n\t"
        : "=a"(r), "+c"(fn), "+d"(ebp)
        :
        : "memory", "cc");
    return r;
}

/* RtlUnwind every frame above `frame`. RtlUnwind resumes at the label with the
 * stack as it was after its four arguments were popped, which is why the
 * registers it may not preserve are saved on the stack around the call. */
void arctic_global_unwind(eh3_reg_t *frame);
__asm__(
    ".text\n"
    ".globl _arctic_global_unwind\n"
    "_arctic_global_unwind:\n\t"
    "pushl %ebp\n\t"
    "movl  %esp, %ebp\n\t"
    "pushl %ebx\n\t"
    "pushl %esi\n\t"
    "pushl %edi\n\t"
    "pushl %ebp\n\t"
    "pushl $0\n\t"                   /* ReturnValue                         */
    "pushl $0\n\t"                   /* ExceptionRecord: STATUS_UNWIND      */
    "pushl $1f\n\t"                  /* TargetIp                            */
    "pushl 8(%ebp)\n\t"              /* TargetFrame                         */
    "call  _RtlUnwind@16\n"
    "1:\n\t"
    "popl  %ebp\n\t"
    "popl  %edi\n\t"
    "popl  %esi\n\t"
    "popl  %ebx\n\t"
    "movl  %ebp, %esp\n\t"
    "popl  %ebp\n\t"
    "ret\n");

static void local_unwind(eh3_reg_t *r, int stop)
{
    while (r->level != -1 && r->level != stop) {
        eh3_scope_t *e = &r->scope[r->level];

        r->level = e->enclosing;
        if (!e->filter)
            call_funclet(e->body, frame_ebp(r));
    }
}

static __attribute__((noreturn)) void enter_except_body(void *body, char *ebp)
{
    __asm__ volatile(
        "movl %%edx, %%ebp\n\t"
        "jmp  *%%eax\n\t"
        :
        : "a"(body), "d"(ebp)
        : "memory");
    __builtin_unreachable();
}

int __cdecl arctic_except_handler3(void *rec_, void *frame, void *ctx, void *disp)
{
    EXCEPTION_RECORD *rec = rec_;
    eh3_reg_t *r = frame;
    EXCEPTION_POINTERS ep;
    int level;

    (void)disp;
    if (rec->ExceptionFlags & (EXC_UNWINDING | EXC_EXIT_UNWIND)) {
        InterlockedIncrement(&s_unwound);
        local_unwind(r, -1);
        return ExceptionContinueSearch;
    }

    InterlockedIncrement(&s_searched);
    ep.ExceptionRecord = rec;
    ep.ContextRecord = ctx;
    /* The filters read it while this function is still on the stack; no
     * __except body may read it (only a filter may ask for the exception), so
     * the pointer outliving `ep` is never followed. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
    *(EXCEPTION_POINTERS **)(frame_ebp(r) - 0x14) = &ep;
#pragma GCC diagnostic pop

    for (level = r->level; level != -1; level = r->scope[level].enclosing) {
        eh3_scope_t *e = &r->scope[level];
        int verdict;

        if (!e->filter)
            continue;
        verdict = call_funclet(e->filter, frame_ebp(r));
        if (verdict < 0) {
            InterlockedIncrement(&s_continued);
            return ExceptionContinueExecution;
        }
        if (verdict > 0) {
            InterlockedIncrement(&s_handled);
            arctic_global_unwind(r);
            local_unwind(r, level);
            r->level = e->enclosing;
            enter_except_body(e->body, frame_ebp(r));
        }
    }
    return ExceptionContinueSearch;
}
