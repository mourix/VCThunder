/* arctic_fs.c -- Arctic Thunder: where the cabinet's files are in this pack.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A cabinet keeps everything under \Midway Games: the game's folder, the
 * operator system's (Diagnostics) and Exception Logs. This pack keeps the
 * install package read-only in data/<id> and data/<id>-diagnostics, and
 * everything either program writes in save/<id>: the game's CMOS, tables and
 * .fmt files, the operator system's logs, the exception logs. The rule is
 * fs_map.c's, the ACCESS MODE and never a list of names, applied per folder
 * (fs_overlay_path()): a write goes to save/, a read comes from save/ once
 * something has been written there and from data/ otherwise, and a listing is
 * the two merged.
 *
 * Both processes bind their file calls here: the game (snow.exe, goose.dll,
 * gobj.dll) and the operator system (diag.exe, diagdll.dll). A path names a
 * folder in either of two ways: the cabinet's ([X:]\Midway Games\<folder>\...,
 * the same behind \\.\ or \\<this cabinet>\), or a host path that resolves
 * into one of the pack's folders, which is what a relative open from the
 * current directory (the game's folder, or the operator system's) is. Any
 * other \Midway Games path is refused, and so is another cabinet's; anything
 * else is the machine's and passes through, logged once.
 */
#include "vcthunder.h"
#include "arctic.h"

#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_GAME, F_DIAG, F_EXC, F_COUNT };

static struct {
    const char *cabinet;            /* its name under \Midway Games */
    char data[MAX_PATH];            /* the package's copy; "" for none */
    char save[MAX_PATH];
} s_f[F_COUNT];

static int s_ready;

void arctic_fs_init(const char *game_data)
{
    char rel[MAX_PATH], save[MAX_PATH];

    s_f[F_GAME].cabinet = ARCTIC_CABINET_GAME;
    s_f[F_DIAG].cabinet = "Diagnostics";
    s_f[F_EXC].cabinet = "Exception Logs";
    snprintf(s_f[F_GAME].data, MAX_PATH, "%s", game_data);
    snprintf(rel, sizeof rel, "%s\\%s-diagnostics", g_cfg.data_dir,
             AT->ids->id);
    if (!GetFullPathNameA(rel, MAX_PATH, s_f[F_DIAG].data, NULL))
        snprintf(s_f[F_DIAG].data, MAX_PATH, "%s", rel);
    s_f[F_EXC].data[0] = 0;

    CreateDirectoryA(g_cfg.save_dir, NULL);
    snprintf(rel, sizeof rel, "%s\\%s", g_cfg.save_dir, AT->ids->id);
    if (!GetFullPathNameA(rel, sizeof save, save, NULL))
        snprintf(save, sizeof save, "%s", rel);
    CreateDirectoryA(save, NULL);
    snprintf(s_f[F_GAME].save, MAX_PATH, "%s", save);
    snprintf(s_f[F_DIAG].save, MAX_PATH, "%s\\Diagnostics", save);
    snprintf(s_f[F_EXC].save, MAX_PATH, "%s\\Exception Logs", save);
    s_ready = 1;
    LOGI("arctic: files: '%s' and '%s' read-only, every write to '%s'",
         s_f[F_GAME].data, s_f[F_DIAG].data, save);
}

const char *arctic_fs_diag_dir(void) { return s_f[F_DIAG].data; }

/* ---- naming a folder --------------------------------------------------------- */

static int under(const char *path, const char *root, const char **rest)
{
    size_t n = strlen(root);

    if (!n || _strnicmp(path, root, n) || (path[n] && path[n] != '\\' &&
                                           path[n] != '/'))
        return 0;
    *rest = path + n;
    return 1;
}

/* The pack folder `in` names, and the rest of the path after it ("" or
 * "\..."). F_COUNT: not a cabinet folder, pass it through. -1: refused, with
 * the error set and logged. -2: \Midway Games itself, listed (rest is the
 * pattern). */
static int folder_of(const char *in, const char **rest, char *abs, size_t cap,
                     const char *why)
{
    static const char root[] = "\\Midway Games";
    const char *p = in, *name;
    size_t n;
    int i, best;

    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
        const char *m = p + 2, *e = m + strcspn(m, "\\/");

        if (!(e - m == 1 && m[0] == '.')) {
            char me[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD len = sizeof me;

            if (!GetComputerNameA(me, &len) || (size_t)(e - m) != len ||
                _strnicmp(m, me, len)) {
                LOGI("arctic: %s [%s] refused: another cabinet", why, in);
                SetLastError(ERROR_BAD_NETPATH);
                return -1;
            }
        }
        p = e;
    } else if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':' &&
               (p[2] == '\\' || p[2] == '/')) {
        p += 2;
    }
    if ((p[0] == '\\' || p[0] == '/') && !_strnicmp(p, root, sizeof root - 1) &&
        (!p[sizeof root - 1] || p[sizeof root - 1] == '\\' ||
         p[sizeof root - 1] == '/')) {
        name = p + sizeof root - 1;
        if (*name)
            name++;
        n = strcspn(name, "\\/");
        if (!name[n] && (!n || strpbrk(name, "*?"))) {
            *rest = name;
            return -2;
        }
        for (i = 0; i < F_COUNT; i++)
            if (strlen(s_f[i].cabinet) == n &&
                !_strnicmp(name, s_f[i].cabinet, n)) {
                *rest = name + n;
                return i;
            }
        LOGI("arctic: %s [%s] refused: no such cabinet folder here", why, in);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return -1;
    }
    /* A host path, relative or not, that lands in one of the pack's folders. */
    if (!GetFullPathNameA(in, (DWORD)cap, abs, NULL))
        return F_COUNT;
    /* The longest root wins: save/<id>\Diagnostics is also under save/<id>. */
    best = F_COUNT;
    for (i = 0; i < F_COUNT; i++) {
        const char *r;

        if ((under(abs, s_f[i].save, &r) ||
             (s_f[i].data[0] && under(abs, s_f[i].data, &r))) &&
            (best == F_COUNT || r > *rest)) {
            best = i;
            *rest = r;
        }
    }
    return best;
}

/* Make every directory on the way to `path`'s last component. */
static void make_parents(const char *path)
{
    char buf[MAX_PATH], *p;

    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf + 3; *p; p++)
        if (*p == '\\' || *p == '/') {
            char c = *p;

            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = c;
        }
}

/* Paths outside the pack, said once each: the game's reach into the machine. */
static void passing(const char *in, const char *why)
{
    static char seen[32][MAX_PATH];
    static LONG nseen;
    LONG i, n = nseen;

    for (i = 0; i < n && i < 32; i++)
        if (!_stricmp(seen[i], in))
            return;
    if (n < 32) {
        snprintf(seen[n], MAX_PATH, "%s", in);
        InterlockedIncrement(&nseen);
    }
    LOGI("arctic: %s [%s] is the machine's, not the cabinet's: passed through",
         why, in);
}

/* Where `in` lives for `mode`, in `out`. Returns the path to use (`out`, or
 * `in` itself for the machine's), or NULL when refused. */
const char *arctic_fs_route(const char *in, char *out, size_t cap, int mode,
                            const char *why)
{
    char abs[MAX_PATH];
    const char *rest = "";
    int f;

    if (!in || !in[0] || !s_ready)
        return in;
    f = folder_of(in, &rest, abs, sizeof abs, why);
    if (f == -2) {
        LOGI("arctic: %s [%s] refused: \\Midway Games is listed, not opened",
             why, in);
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    if (f < 0)
        return NULL;
    if (f == F_COUNT) {
        passing(in, why);
        return in;
    }
    while (*rest == '\\' || *rest == '/')
        rest++;
    if (!*rest) {
        /* The folder itself: the package's for a read, ours for a write. */
        const char *dir = mode == FS_READ && s_f[f].data[0] &&
                          GetFileAttributesA(s_f[f].data) != INVALID_FILE_ATTRIBUTES
                          ? s_f[f].data : s_f[f].save;
        if (mode != FS_READ)
            CreateDirectoryA(s_f[f].save, NULL);
        snprintf(out, cap, "%s", dir);
    } else if (!s_f[f].data[0]) {
        if (snprintf(out, cap, "%s\\%s", s_f[f].save, rest) >= (int)cap)
            return in;
        if (mode == FS_WRITE)
            make_parents(out);
    } else {
        /* The folders first: a write seeds the save copy from the package. */
        if (mode == FS_WRITE &&
            snprintf(out, cap, "%s\\%s", s_f[f].save, rest) < (int)cap)
            make_parents(out);
        if (!fs_overlay_path(s_f[f].data, s_f[f].save, rest, out, cap,
                             (fs_access_t)mode)) {
            LOGW("arctic: %s [%s]: path too long, passing through", why, in);
            return in;
        }
    }
    LOGT("arctic: %s [%s] -> [%s] (%s)", why, in, out,
         mode == FS_WRITE ? "write" : mode == FS_DELETE ? "delete" : "read");
    return out;
}

/* ---- listings ------------------------------------------------------------------ */

/* A listing this module made: \Midway Games itself, or a pack folder's
 * package and save copies merged, save first and a name only once. Its
 * handle is the pointer tagged with bit 0, which no real handle carries
 * (Win32 and msvcrt find handles are multiples of 4). */
typedef struct {
    unsigned next, count, cap;
    WIN32_FIND_DATAA *e;
} listing_t;

static listing_t *listing_of(intptr_t h)
{
    return (h & 3) == 1 ? (listing_t *)(h & ~(intptr_t)3) : NULL;
}

static int wild(const char *pat, const char *s)
{
    if (!*pat)
        return !*s;
    if (*pat == '*')
        return wild(pat + 1, s) || (*s && wild(pat, s + 1));
    if (!*s)
        return 0;
    if (*pat == '?' || (*pat | 0x20) == (*s | 0x20))
        return wild(pat + 1, s + 1);
    return 0;
}

static int add(listing_t *l, const WIN32_FIND_DATAA *d)
{
    unsigned i;

    for (i = 0; i < l->count; i++)
        if (!_stricmp(l->e[i].cFileName, d->cFileName))
            return 1;
    if (l->count == l->cap) {
        unsigned cap = l->cap ? l->cap * 2 : 32;
        WIN32_FIND_DATAA *e = realloc(l->e, cap * sizeof *e);
        if (!e)
            return 0;
        l->e = e;
        l->cap = cap;
    }
    l->e[l->count++] = *d;
    return 1;
}

static void gather(listing_t *l, const char *pattern)
{
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pattern, &d);

    if (h == INVALID_HANDLE_VALUE)
        return;
    do
        add(l, &d);
    while (FindNextFileA(h, &d));
    FindClose(h);
}

/* \Midway Games itself: the cabinet folders this pack keeps, each with its
 * real folder's attributes and times. diag walks it to find what is
 * installed (Software Versions, among others). */
static void gather_root(listing_t *l, const char *pat)
{
    char p[MAX_PATH];
    int i;

    /* "*.*" matches a name without a dot, as FindFirstFile has it. */
    snprintf(p, sizeof p, "%s", !strcmp(pat, "*.*") || !*pat ? "*" : pat);
    for (i = 0; i < F_COUNT; i++) {
        const char *dir = s_f[i].data[0] ? s_f[i].data : s_f[i].save;
        WIN32_FIND_DATAA d;
        HANDLE h;

        if (!wild(p, s_f[i].cabinet))
            continue;
        if (!s_f[i].data[0])
            CreateDirectoryA(dir, NULL);
        h = FindFirstFileA(dir, &d);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        FindClose(h);
        snprintf(d.cFileName, sizeof d.cFileName, "%s", s_f[i].cabinet);
        d.cAlternateFileName[0] = 0;
        add(l, &d);
    }
}

/* A listing of `pattern`, or 0 when it is the machine's (*real is then
 * the routed path to list) or refused (*real NULL). */
static listing_t *list(const char *pattern, const char **real)
{
    char abs[MAX_PATH], sub[MAX_PATH], pat[MAX_PATH];
    const char *rest = "", *mask;
    listing_t *l;
    int f;

    *real = pattern;
    if (!pattern || !s_ready)
        return NULL;
    f = folder_of(pattern, &rest, abs, sizeof abs, "list");
    if (f == -1) {
        *real = NULL;
        return NULL;
    }
    if (f == F_COUNT) {
        passing(pattern, "list");
        return NULL;
    }
    if (!(l = calloc(1, sizeof *l))) {
        *real = NULL;
        return NULL;
    }
    if (f == -2) {
        gather_root(l, rest);
        LOGT("arctic: listed \\Midway Games (%s): %u folders", pattern, l->count);
        return l;
    }
    /* <folder><sub>\<mask>: the same sub-path and mask in both copies. */
    while (*rest == '\\' || *rest == '/')
        rest++;
    mask = strrchr(rest, '\\');
    if (!mask || (strrchr(rest, '/') > mask))
        mask = strrchr(rest, '/');
    snprintf(sub, sizeof sub, "%.*s", mask ? (int)(mask - rest) : 0, rest);
    mask = mask ? mask + 1 : rest;
    if (!*mask)
        mask = "*";
    snprintf(pat, sizeof pat, "%s%s%s\\%s", s_f[f].save, *sub ? "\\" : "", sub,
             mask);
    gather(l, pat);
    if (s_f[f].data[0]) {
        snprintf(pat, sizeof pat, "%s%s%s\\%s", s_f[f].data, *sub ? "\\" : "",
                 sub, mask);
        gather(l, pat);
    }
    LOGT("arctic: listed [%s]: %u entr%s", pattern, l->count,
         l->count == 1 ? "y" : "ies");
    return l;
}

/* ---- the boundaries ---------------------------------------------------------- */

static HANDLE WINAPI b_CreateFileA(LPCSTR n, DWORD a, DWORD s,
                                   LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f,
                                   HANDLE t)
{
    char buf[MAX_PATH];
    HANDLE h;
    /* A create, or any write access, is the game's file; as fs_map.c. */
    int writing = (a & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
                        FILE_APPEND_DATA)) ||
                  c == CREATE_NEW || c == CREATE_ALWAYS || c == OPEN_ALWAYS ||
                  c == TRUNCATE_EXISTING;

    if (arctic_keylok_open(n, a, s, sa, c, f, t, &h))
        return h;
    if (arctic_device_open(n, &h))
        return h;
    if (!(n = arctic_fs_route(n, buf, sizeof buf, writing ? FS_WRITE : FS_READ,
                              "open")))
        return INVALID_HANDLE_VALUE;
    return CreateFileA(n, a, s, sa, c, f, t);
}

static BOOL WINAPI b_CloseHandle(HANDLE h)
{
    if (arctic_keylok_close(h))
        return TRUE;
    return CloseHandle(h);
}

static HANDLE WINAPI b_FindFirstFileA(LPCSTR n, LPWIN32_FIND_DATAA d)
{
    const char *real;
    listing_t *l = list(n, &real);

    if (!l)
        return real ? FindFirstFileA(real, d) : INVALID_HANDLE_VALUE;
    if (!l->count) {
        free(l->e);
        free(l);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    *d = l->e[0];
    l->next = 1;
    return (HANDLE)((intptr_t)l | 1);
}

static BOOL WINAPI b_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA d)
{
    listing_t *l = listing_of((intptr_t)h);

    if (!l)
        return FindNextFileA(h, d);
    if (l->next >= l->count) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    *d = l->e[l->next++];
    return TRUE;
}

static BOOL WINAPI b_FindClose(HANDLE h)
{
    listing_t *l = listing_of((intptr_t)h);

    if (!l)
        return FindClose(h);
    free(l->e);
    free(l);
    return TRUE;
}

/* msvcrt.dll's own _findfirst family, whose record is the layout the game was
 * built against (this toolchain's _findfirst is a macro for another export):
 * attrib, three 32-bit times, a 32-bit size, the name. */
typedef struct {
    unsigned attrib;
    int32_t time_create, time_access, time_write;
    uint32_t size;
    char name[260];
} finddata_t;

typedef intptr_t (__cdecl *findfirst_fn)(const char *, void *);
typedef int (__cdecl *findnext_fn)(intptr_t, void *);
typedef int (__cdecl *findclose_fn)(intptr_t);

static void *crt(const char *name)
{
    return (void *)GetProcAddress(GetModuleHandleA("msvcrt.dll"), name);
}

static int32_t time32(FILETIME ft)
{
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return t < 116444736000000000ull ? -1
         : (int32_t)((t - 116444736000000000ull) / 10000000ull);
}

static void to_finddata(const WIN32_FIND_DATAA *w, void *out)
{
    finddata_t *d = out;

    d->attrib = w->dwFileAttributes == FILE_ATTRIBUTE_NORMAL ? 0
              : w->dwFileAttributes;
    d->time_create = time32(w->ftCreationTime);
    d->time_access = time32(w->ftLastAccessTime);
    d->time_write = time32(w->ftLastWriteTime);
    d->size = w->nFileSizeLow;
    snprintf(d->name, sizeof d->name, "%s", w->cFileName);
}

static intptr_t __cdecl b_findfirst(const char *n, void *d)
{
    const char *real;
    listing_t *l = list(n, &real);
    findfirst_fn f = (findfirst_fn)crt("_findfirst");

    if (!l) {
        if (!real || !f) {
            errno = ENOENT;
            return -1;
        }
        return f(real, d);
    }
    if (!l->count) {
        free(l->e);
        free(l);
        errno = ENOENT;
        return -1;
    }
    to_finddata(&l->e[0], d);
    l->next = 1;
    return (intptr_t)l | 1;
}

static int __cdecl b_findnext(intptr_t h, void *d)
{
    listing_t *l = listing_of(h);
    findnext_fn f = (findnext_fn)crt("_findnext");

    if (!l)
        return f ? f(h, d) : -1;
    if (l->next >= l->count) {
        errno = ENOENT;
        return -1;
    }
    to_finddata(&l->e[l->next++], d);
    return 0;
}

static int __cdecl b_findclose(intptr_t h)
{
    listing_t *l = listing_of(h);
    findclose_fn f = (findclose_fn)crt("_findclose");

    if (!l)
        return f ? f(h) : -1;
    free(l->e);
    free(l);
    return 0;
}

static DWORD WINAPI b_GetFileAttributesA(LPCSTR n)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_READ, "stat")))
        return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesA(n);
}

/* Attributes are a write: the copy in save/ gets them, seeded from the
 * package's on first use, as any update in place. */
static BOOL WINAPI b_SetFileAttributesA(LPCSTR n, DWORD a)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_WRITE, "set attributes")))
        return FALSE;
    return SetFileAttributesA(n, a);
}

static BOOL WINAPI b_CreateDirectoryA(LPCSTR n, LPSECURITY_ATTRIBUTES sa)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_WRITE, "mkdir")))
        return FALSE;
    return CreateDirectoryA(n, sa);
}

static BOOL WINAPI b_DeleteFileA(LPCSTR n)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_DELETE, "delete")))
        return FALSE;
    return DeleteFileA(n);
}

static int __cdecl b_unlink(const char *n)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_DELETE, "delete"))) {
        errno = EACCES;
        return -1;
    }
    return _unlink(n);
}

static BOOL WINAPI b_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail)
{
    char a[MAX_PATH], b[MAX_PATH];

    if (!(from = arctic_fs_route(from, a, sizeof a, FS_READ, "copy from")) ||
        !(to = arctic_fs_route(to, b, sizeof b, FS_WRITE, "copy to")))
        return FALSE;
    LOGI("arctic: copied %s to %s", from, to);
    return CopyFileA(from, to, fail);
}

/* The current directory stays the package's folder: every path that lands
 * in it is routed, so relative opens need nothing else. */
static BOOL WINAPI b_SetCurrentDirectoryA(LPCSTR n)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_READ, "chdir")))
        return FALSE;
    return SetCurrentDirectoryA(n);
}

/* Whether an fopen mode writes, read as msvcrt reads it: the first letter
 * decides, then '+' among the flags that follow, and the first character that
 * is not a flag ends the mode. snow.exe's script loader opens its .txt files
 * "ra", which is a plain read: any 'a' anywhere is not the rule. */
static int mode_writes(const char *m)
{
    if (!m || (*m != 'r' && *m != 'w' && *m != 'a'))
        return 0;
    if (*m != 'r')
        return 1;
    for (m++; *m && strchr("+tbcnSRTD", *m); m++)
        if (*m == '+')
            return 1;
    return 0;
}

static FILE *__cdecl b_fopen(const char *n, const char *mode)
{
    char buf[MAX_PATH], why[32];
    int writing = mode_writes(mode);

    snprintf(why, sizeof why, "fopen \"%s\"", mode ? mode : "");
    if (!(n = arctic_fs_route(n, buf, sizeof buf, writing ? FS_WRITE : FS_READ,
                              why))) {
        errno = EACCES;
        return NULL;
    }
    return fopen(n, mode);
}

/* Software Versions reads each installed file's version resource. */
static DWORD WINAPI b_GetFileVersionInfoSizeA(LPCSTR n, LPDWORD h)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_READ, "version of")))
        return 0;
    return GetFileVersionInfoSizeA(n, h);
}

static BOOL WINAPI b_GetFileVersionInfoA(LPCSTR n, DWORD h, DWORD len, LPVOID d)
{
    char buf[MAX_PATH];

    if (!(n = arctic_fs_route(n, buf, sizeof buf, FS_READ, "version of")))
        return FALSE;
    return GetFileVersionInfoA(n, h, len, d);
}

static const struct {
    const char *dll, *name;
    void *fn;
} k_bound[] = {
    { "KERNEL32.dll", "CreateFileA",          (void *)b_CreateFileA },
    { "KERNEL32.dll", "CloseHandle",          (void *)b_CloseHandle },
    { "KERNEL32.dll", "FindFirstFileA",       (void *)b_FindFirstFileA },
    { "KERNEL32.dll", "FindNextFileA",        (void *)b_FindNextFileA },
    { "KERNEL32.dll", "FindClose",            (void *)b_FindClose },
    { "KERNEL32.dll", "GetFileAttributesA",   (void *)b_GetFileAttributesA },
    { "KERNEL32.dll", "SetFileAttributesA",   (void *)b_SetFileAttributesA },
    { "KERNEL32.dll", "CreateDirectoryA",     (void *)b_CreateDirectoryA },
    { "KERNEL32.dll", "DeleteFileA",          (void *)b_DeleteFileA },
    { "KERNEL32.dll", "CopyFileA",            (void *)b_CopyFileA },
    { "KERNEL32.dll", "SetCurrentDirectoryA", (void *)b_SetCurrentDirectoryA },
    { "VERSION.dll",  "GetFileVersionInfoSizeA", (void *)b_GetFileVersionInfoSizeA },
    { "VERSION.dll",  "GetFileVersionInfoA",  (void *)b_GetFileVersionInfoA },
    { "MSVCRT.dll",   "fopen",                (void *)b_fopen },
    { "MSVCRT.dll",   "_unlink",              (void *)b_unlink },
    { "MSVCRT.dll",   "_findfirst",           (void *)b_findfirst },
    { "MSVCRT.dll",   "_findnext",            (void *)b_findnext },
    { "MSVCRT.dll",   "_findclose",           (void *)b_findclose },
};

void *arctic_fs_bound(const char *dll, const char *name)
{
    unsigned i;

    for (i = 0; name && i < sizeof k_bound / sizeof k_bound[0]; i++)
        if (!_stricmp(dll, k_bound[i].dll) && !strcmp(name, k_bound[i].name))
            return k_bound[i].fn;
    return NULL;
}
