/* arctic_keylok.c -- the key and its watchdog, answered at the boundary.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Ultimate Arctic Thunder's snow.exe wants two things the cabinet had and a
 * PC does not: a KEYLOK II on the parallel port, and PPMON.EXE watching the
 * game while it talks to it. Both are answered here, in the same place and
 * the same way as the cabinet's other missing hardware -- the drive identity
 * two boundaries up in arctic.c is the model.
 *
 *   the device   the client opens \\.\KeyDongle_<n>, then \\.\PARCLASS<n>,
 *                and drives it with two IOCTLs on the driver's device type:
 *                function 0 answers a version, function 4 carries one
 *                KFUNC(command, arg2, arg3, arg4). keylok.c is the key.
 *   the watchdog the game stores a marker holding its own address, passes
 *                the address to PPMON.EXE on its command line, and kills
 *                itself ten seconds later unless the marker has been zeroed.
 *                PPMON does that with WriteProcessMemory from outside. The
 *                game is in this process, so the host does it directly and
 *                never starts the program.
 *
 * `[diagnostics] dongle` picks: auto (a real key if one opens, else the
 * host's), virtual (always the host's), off (neither -- the control, which
 * leaves the game to fail exactly as it does on a PC with no key).
 *
 * The key's variable memory is NOT persisted. A real key's is, but the game
 * rewrites every word it reads during its own boot (snow.exe 0x43f7d0), so a
 * file would record nothing that is ever read back.
 */
#include "vcthunder.h"
#include "arctic.h"
#include "keylok.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The serial the key reports. The game reads it twice and compares it with
 * itself (snow.exe 0x4401f0 and 0x440938); nothing else looks at it, so the
 * only property that matters is that it does not change. */
#define KEYLOK_SERIAL 0x0056435Au      /* 'VCZ': this host's stand-in key   */

/* CTL_CODE's halves. The client builds (device type << 16) | (function << 2)
 * with METHOD_BUFFERED and FILE_ANY_ACCESS. */
#define IOCTL_DEVTYPE(c)  ((c) >> 16)
#define IOCTL_FUNCTION(c) (((c) >> 2) & 0xfffu)
#define IOCTL_METHOD(c)   ((c) & 3u)

enum { KL_FUNC_VERSION = 0, KL_FUNC_CALL = 4 };

/* The driver version each device type must report before the client will use
 * it (snow.exe 0x401561): below it the client closes the handle and moves on. */
#define KL_TYPE_KEYDONGLE 0x22u
#define KL_TYPE_PARCLASS  0x16u
#define KL_VER_KEYDONGLE  0x0501u
#define KL_VER_PARCLASS   0x0405u

static keylok_t s_key;
static HANDLE   s_virtual;      /* the handle the host answered an open with */
static HANDLE   s_real;         /* a real key's, when one was found          */
static HMODULE  s_snow;
static unsigned s_calls, s_cleared, s_refused_open;
static int      s_reported;

static int mode_is(const char *want)
{
    return !_stricmp(g_cfg.dongle[0] ? g_cfg.dongle : "auto", want);
}

void arctic_keylok_arm(HMODULE snow)
{
    s_snow = snow;
    keylok_reset(&s_key, KEYLOK_SERIAL);
}

/* ---- the device ---------------------------------------------------------- */

/* Nonzero with the device type when `n` is one of the key driver's names.
 * \\.\KeyDongle_<digit>, \\.\PARCLASS<digit> and the Windows 9x
 * \\.\PARCLASS.VXD are the three the client tries. */
static int key_device(LPCSTR n, unsigned *type)
{
    if (!n || strncmp(n, "\\\\.\\", 4))
        return 0;
    n += 4;
    if (!_strnicmp(n, "KeyDongle_", 10) && n[10] && !n[11]) {
        *type = KL_TYPE_KEYDONGLE;
        return 1;
    }
    if (!_strnicmp(n, "PARCLASS", 8) && (!_stricmp(n + 8, ".VXD") ||
                                         (n[8] && !n[9]))) {
        *type = KL_TYPE_PARCLASS;
        return 1;
    }
    return 0;
}

int arctic_keylok_open(LPCSTR n, DWORD access, DWORD share,
                       LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                       HANDLE tmpl, HANDLE *out)
{
    unsigned type;

    if (!key_device(n, &type))
        return 0;
    if (mode_is("off")) {
        if (!s_refused_open++)
            LOGI("arctic: [diagnostics] dongle = off: %s is not answered", n);
        SetLastError(ERROR_FILE_NOT_FOUND);
        *out = INVALID_HANDLE_VALUE;
        return 1;
    }
    /* A real key answers for itself: the driver is the user's and the device
     * is a real one, so nothing here stands in for anything. */
    if (mode_is("auto") && !s_real) {
        HANDLE h = CreateFileA(n, access, share, sa, disp, flags, tmpl);

        if (h != INVALID_HANDLE_VALUE) {
            s_real = h;
            LOGI("arctic: %s opened: a real key answers, not the host", n);
            *out = h;
            return 1;
        }
    }
    if (s_real) {               /* reopened after the client closed it      */
        HANDLE h = CreateFileA(n, access, share, sa, disp, flags, tmpl);

        if (h != INVALID_HANDLE_VALUE) {
            *out = h;
            return 1;
        }
    }
    if (!s_virtual) {
        s_virtual = CreateEventA(NULL, TRUE, FALSE, NULL);
        LOGI("arctic: %s answered by the host (a KEYLOK II, type 0x%02x)", n,
             type);
    }
    *out = s_virtual;
    return 1;
}

/* Nonzero when `h` is the host's key. The client closes the key after each
 * session; the handle stays open so its value is never reused for another
 * object while the client still holds it, and the next open returns it. */
int arctic_keylok_close(HANDLE h)
{
    if (!s_virtual || h != s_virtual)
        return 0;
    LOGT("arctic: key: the client closed the host's key; it stays open");
    return 1;
}

/* Nonzero when this IOCTL was the host's to answer; `*result` is then what
 * the game's DeviceIoControl returns. */
int arctic_keylok_ioctl(HANDLE h, DWORD code, LPVOID in, DWORD nin,
                        LPVOID out, DWORD nout, LPDWORD ret, BOOL *result)
{
    const uint32_t *a = in;
    uint32_t *o = out;
    unsigned type = IOCTL_DEVTYPE(code), func = IOCTL_FUNCTION(code);

    if (!s_virtual || h != s_virtual)
        return 0;
    *result = FALSE;
    if (nout < 8 || !o) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 1;
    }
    if (type != KL_TYPE_KEYDONGLE && type != KL_TYPE_PARCLASS) {
        LOGW("arctic: key: IOCTL 0x%08lX is not the key's", (unsigned long)code);
        SetLastError(ERROR_NOT_SUPPORTED);
        return 1;
    }
    o[1] = 0;                   /* the client passes this to SetLastError   */
    if (func == KL_FUNC_VERSION) {
        o[0] = type == KL_TYPE_KEYDONGLE ? KL_VER_KEYDONGLE : KL_VER_PARCLASS;
    } else if (func == KL_FUNC_CALL && a && nin >= 16) {
        o[0] = keylok_call(&s_key, a[0], a[1], a[2], a[3]);
        s_calls++;
        LOGT("arctic: key: KFUNC(%u, 0x%04X, 0x%04X, 0x%04X) -> 0x%08lX",
             a[0] & 0xffffu, a[1] & 0xffffu, a[2] & 0xffffu, a[3] & 0xffffu,
             (unsigned long)o[0]);
    } else {
        LOGW("arctic: key: function %u with %lu in / %lu out is not the "
             "client's", func, (unsigned long)nin, (unsigned long)nout);
        SetLastError(ERROR_NOT_SUPPORTED);
        return 1;
    }
    if (ret)
        *ret = 8;
    *result = TRUE;
    return 1;
}

/* ---- the watchdog -------------------------------------------------------- */

/* "<program> <pid> <address>" as the game's own sprintf built it. The address
 * is the marker's, which the game set to its own address, so it moves with
 * the image and is never a constant here. */
static int ppmon_marker(LPCSTR cmd, DWORD *pid, void **marker)
{
    const char *p;
    char *end;
    unsigned long long v;

    if (!cmd || !(p = strstr(cmd, "ppmon.exe")))
        return 0;
    p += 9;
    v = strtoull(p, &end, 10);
    if (end == p)
        return 0;
    *pid = (DWORD)v;
    p = end;
    v = strtoull(p, &end, 10);
    if (end == p)
        return 0;
    *marker = (void *)(uintptr_t)v;
    return 1;
}

/* The marker must be a writable dword of the game's own image: the address
 * comes off a command line, and nothing else may be written through it. */
static int marker_is_the_games(void *p)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!s_snow || !p || ((uintptr_t)p & 3))
        return 0;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.AllocationBase != (void *)s_snow)
        return 0;
    return !!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

/* The game starts PPMON.EXE and dies ten seconds later unless the marker is
 * zero. Nothing is started: the host clears the marker, which is the whole of
 * what PPMON does for a process it is watching. A handle pair still has to
 * come back, because the caller closes both. */
BOOL WINAPI arctic_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                  LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                                  DWORD flags, LPVOID env, LPCSTR dir,
                                  LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    DWORD pid = 0;
    void *marker = NULL;

    if (!mode_is("off") && ppmon_marker(app ? app : cmd, &pid, &marker) &&
        pi) {
        if (pid != GetCurrentProcessId()) {
            LOGW("arctic: key: the watchdog was asked to watch pid %lu, not "
                 "this one; not answered", (unsigned long)pid);
        } else if (!marker_is_the_games(marker)) {
            LOGW("arctic: key: the watchdog's marker %p is not the game's; "
                 "not cleared", marker);
        } else {
            *(volatile uint32_t *)marker = 0;
            s_cleared++;
            LOGI("arctic: key: the watchdog is the host's; marker %p cleared",
                 marker);
        }
        memset(pi, 0, sizeof *pi);
        pi->hProcess = CreateEventA(NULL, TRUE, FALSE, NULL);
        pi->hThread = CreateEventA(NULL, TRUE, FALSE, NULL);
        pi->dwProcessId = GetCurrentProcessId();
        pi->dwThreadId = GetCurrentThreadId();
        return TRUE;
    }
    return CreateProcessA(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}

void arctic_keylok_report(void)
{
    if (s_reported || (!s_calls && !s_virtual && !s_real))
        return;
    s_reported = 1;
    if (s_real) {
        LOGI("arctic: key: a real one answered; the host stood in for nothing");
        return;
    }
    LOGI("arctic: key: %u call%s, %u challenge%s answered, %u refused; "
         "watchdog cleared %u time%s", s_calls, s_calls == 1 ? "" : "s",
         s_key.answered, s_key.answered == 1 ? "" : "s", s_key.refused,
         s_cleared, s_cleared == 1 ? "" : "s");
}
