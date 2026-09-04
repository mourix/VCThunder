/* log.c -- the shim's only diagnostic channel.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The game has no console under Windows: its boot messages go through _vtext,
 * a software cell buffer rendered by the graphics path, so nothing the game
 * says reaches a terminal. This is the host's only channel, and it writes
 * UNBUFFERED: a crash must not eat the last line before it.
 */
#include "vcthunder.h"
#include "resource.h"           /* VCT_VERSION: the one definition of it */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

log_level_t g_log_level = LOG_INFO;

static FILE *s_fp;
static DWORD s_t0;

static const char *const LEVEL[] = { "ERR ", "WARN", "INFO", "TRC " };

/* NOT __DATE__ and __TIME__.
 *
 * They would say which build wrote a log, and they would make every build of
 * identical source a different binary, which this project cannot afford: a
 * change is signed off by reporting both SHA-256s, and the renderer's control
 * is BUILT from the previous revision and compared. A wall-clock stamp in the file means a
 * hash can say which binary you shipped and can never say a binary is
 * unchanged. `make` prints each binary's hash instead, and it is a better
 * answer to the same question. Pass -DVCT_BUILD='"..."' to stamp one anyway. */
#ifndef VCT_BUILD
#define VCT_BUILD "identify this build by the SHA-256 make printed for it"
#endif

void log_open(const char *path, log_level_t level)
{
    g_log_level = level;
    s_t0 = GetTickCount();
    if (path && *path) {
        s_fp = fopen(path, "w");
        if (!s_fp)
            fprintf(stderr, "shim: cannot open log '%s'\n", path);
    }
    log_printf(LOG_INFO, "VCThunder %s: %s", VCT_VERSION, VCT_BUILD);
}

void log_close(void)
{
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
}

void log_printf(log_level_t level, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    if (level > g_log_level)
        return;

    n = snprintf(line, sizeof line, "[%7lu] %s ",
                 (unsigned long)(GetTickCount() - s_t0),
                 LEVEL[level <= LOG_TRACE ? (int)level : 3]);
    if (n < 0 || (size_t)n >= sizeof line)
        return;

    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line - 1)
        n = (int)sizeof line - 2;       /* truncated; still one whole line */

    /* The newline goes in the SAME write as the text.
     * fputs-then-fputc is two calls, and the CRT locks each one separately, so
     * two threads logging at once interleave between them, which puts one
     * thread's line inside another's, with no newline in between. Every thread
     * in this process logs: the game's, the poll thread, the DCS tick, the
     * watchdog and the mixer. Observed 2026-08-19 in a dry-run A/B, where it
     * looked at first like a missing line. */
    line[n] = '\n';
    line[n + 1] = '\0';
    fputs(line, stderr);
    if (s_fp) {
        fputs(line, s_fp);
        fflush(s_fp);           /* unbuffered on purpose; see header comment */
    }
}

/* ---- the game's own printf ---------------------------------------------- */
/* Redirect a title's compiled-out printf entry to the host log. Buffer partial
 * cdecl variadic writes until newline; titles without the anchor are unchanged. */
static CRITICAL_SECTION s_gp_lock;
static char s_gp_line[512];
static size_t s_gp_len;

static void gp_flush(void)
{
    if (s_gp_len) {
        s_gp_line[s_gp_len] = '\0';
        log_printf(LOG_INFO, "game: %s", s_gp_line);
        s_gp_len = 0;
    }
}

static int __cdecl game_printf(const char *fmt, ...)
{
    char text[512];
    va_list ap;
    int n;
    const char *p;

    if (!fmt)
        return 0;
    /* The game's own printf, so its caller is game code by construction, and
     * unlike the swap or the I/O callback it is reached from the game's logic
     * rather than from a boundary the shim chose. That makes it the census
     * entry that identifies the title's MAIN thread. Before the lock: a report
     * from here would otherwise nest one log call inside another. */
    game_thread_logic_seen("the game's own printf");
    va_start(ap, fmt);
    n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n < 0)
        return 0;

    EnterCriticalSection(&s_gp_lock);
    for (p = text; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            gp_flush();
            continue;
        }
        if (s_gp_len + 1 >= sizeof s_gp_line)
            gp_flush();
        s_gp_line[s_gp_len++] = *p;
    }
    /* A message with no newline is usually the whole message (the game logs
     * plenty of those), so do not sit on it waiting for one that never comes.
     * Anything still buffered is emitted by the next call or at shutdown. */
    LeaveCriticalSection(&s_gp_lock);
    return n;
}

int log_install_game_printf(void)
{
    if (!game_require("log", "__printfEmpty to reroute into this log",
                      G->printf_empty))
        return 0;
    InitializeCriticalSection(&s_gp_lock);
    patch_jmp(G->printf_empty, (void *)game_printf);
    LOGI("log: __printfEmpty @ 0x%08X now writes to this log; the game's own "
         "boot messages follow", G->printf_empty);
    return 1;
}

void log_flush_game_printf(void)
{
    if (G && G->printf_empty) {
        EnterCriticalSection(&s_gp_lock);
        gp_flush();
        LeaveCriticalSection(&s_gp_lock);
    }
}
