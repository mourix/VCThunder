/* arctic_reg.c -- Arctic Thunder: the cabinet's registry, never the user's.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * goose.dll and diag.exe keep their state in HKLM\Software\Midway Games: the
 * shell's IOBlock, the list of installed games, the pricing table, coin and
 * watchdog audits. Every Reg* call either
 * image makes is bound here. The tree is this host's: HKEY_LOCAL_MACHINE only,
 * and only under Software\Midway Games; anything else is refused and logged,
 * which is what keeps diag's Check Disk (Session Manager's BootExecute) away
 * from the machine. It persists in save/arctic/registry.txt, rewritten on
 * every change; IOBlock is volatile, answered from this process's shell.
 *
 * File format, one entry per line:  K <key>   then   V <type> <hex> <name>
 * for each of its values (name last, so it may hold spaces; "" is the default
 * value). Keys are stored as given and compared without case.
 */
#include "vcthunder.h"
#include "arctic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT        "Software\\Midway Games"
#define IOBLOCK     "Software\\Midway Games\\WinCSM\\IOBlock"
#define MAX_KEYS    256
#define MAX_VALUES  64
#define MAX_HANDLES 128
#define KEY_CAP     256
#define NAME_CAP    128
#define DATA_CAP    1024

typedef struct {
    char  name[NAME_CAP];
    DWORD type, len;
    BYTE  data[DATA_CAP];
} reg_value_t;

typedef struct {
    int         used, volatile_;
    char        path[KEY_CAP];          /* below HKLM, single backslashes */
    unsigned    nval;
    reg_value_t val[MAX_VALUES];
} reg_key_t;

static reg_key_t *s_keys;               /* MAX_KEYS, allocated on first use */
static int s_handle[MAX_HANDLES];       /* key index + 1; 0 is free         */
static char s_file[MAX_PATH];
static CRITICAL_SECTION s_lock;
static LONG s_init;
static DWORD s_ioblock;

/* A handle this module issued: a tag in the high bits nothing real carries. */
#define H_TAG  0x5EC00000u
#define H_MASK 0xFFFF0000u

static void lock(void)
{
    if (!InterlockedCompareExchange(&s_init, 1, 0)) {
        InitializeCriticalSection(&s_lock);
        s_keys = calloc(MAX_KEYS, sizeof *s_keys);
        InterlockedExchange(&s_init, 2);
    }
    while (s_init != 2)
        Sleep(0);
    EnterCriticalSection(&s_lock);
}

static void unlock(void) { LeaveCriticalSection(&s_lock); }

static int find(const char *path)
{
    int i;

    for (i = 0; s_keys && i < MAX_KEYS; i++)
        if (s_keys[i].used && !_stricmp(s_keys[i].path, path))
            return i;
    return -1;
}

static int add(const char *path)
{
    int i = find(path);

    if (i >= 0 || !s_keys)
        return i;
    for (i = 0; i < MAX_KEYS; i++)
        if (!s_keys[i].used) {
            memset(&s_keys[i], 0, sizeof s_keys[i]);
            s_keys[i].used = 1;
            snprintf(s_keys[i].path, KEY_CAP, "%s", path);
            return i;
        }
    LOGW("arctic: registry full (%d keys); [%s] not created", MAX_KEYS, path);
    return -1;
}

/* Every ancestor exists too, as RegCreateKeyEx leaves them. */
static int add_path(const char *path)
{
    char p[KEY_CAP];
    size_t i;

    snprintf(p, sizeof p, "%s", path);
    for (i = 0; p[i]; i++)
        if (p[i] == '\\') {
            p[i] = 0;
            add(p);
            p[i] = '\\';
        }
    return add(p);
}

static reg_value_t *value(reg_key_t *k, const char *name, int create)
{
    unsigned i;

    if (!name)
        name = "";
    for (i = 0; i < k->nval; i++)
        if (!_stricmp(k->val[i].name, name))
            return &k->val[i];
    if (!create || k->nval == MAX_VALUES)
        return NULL;
    memset(&k->val[k->nval], 0, sizeof k->val[0]);
    snprintf(k->val[k->nval].name, NAME_CAP, "%s", name);
    return &k->val[k->nval++];
}

/* ---- persistence --------------------------------------------------------- */

static void save(void)
{
    char tmp[MAX_PATH + 8];
    FILE *f;
    int i;

    if (!s_file[0])
        return;
    snprintf(tmp, sizeof tmp, "%s.new", s_file);
    f = fopen(tmp, "w");
    if (!f) {
        LOGW("arctic: cannot write %s", tmp);
        return;
    }
    for (i = 0; i < MAX_KEYS; i++) {
        const reg_key_t *k = &s_keys[i];
        unsigned v;

        if (!k->used || k->volatile_)
            continue;
        fprintf(f, "K %s\n", k->path);
        for (v = 0; v < k->nval; v++) {
            const reg_value_t *x = &k->val[v];
            DWORD b;

            fprintf(f, "V %lu ", (unsigned long)x->type);
            for (b = 0; b < x->len; b++)
                fprintf(f, "%02x", x->data[b]);
            fprintf(f, "%s %s\n", x->len ? "" : "-", x->name);
        }
    }
    fclose(f);
    if (!MoveFileExA(tmp, s_file, MOVEFILE_REPLACE_EXISTING))
        LOGW("arctic: cannot replace %s (error %lu)", s_file, GetLastError());
}

static void load(void)
{
    char line[2 * DATA_CAP + NAME_CAP + 32];
    FILE *f = fopen(s_file, "r");
    reg_key_t *k = NULL;
    unsigned nk = 0;

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (line[0] == 'K' && line[1] == ' ') {
            int i = add_path(line + 2);

            k = i >= 0 ? &s_keys[i] : NULL;
            nk++;
        } else if (k && line[0] == 'V' && line[1] == ' ') {
            char *p = line + 2, *hex, *name;
            reg_value_t *x;
            unsigned long type = strtoul(p, &p, 10);
            size_t h;

            if (*p != ' ')
                continue;
            hex = ++p;
            name = strchr(hex, ' ');
            if (!name)
                continue;
            *name++ = 0;
            x = value(k, name, 1);
            if (!x)
                continue;
            x->type = (DWORD)type;
            x->len = 0;
            for (h = 0; hex[h] && hex[h + 1] && x->len < DATA_CAP; h += 2) {
                unsigned b;

                if (sscanf(hex + h, "%2x", &b) != 1)
                    break;
                x->data[x->len++] = (BYTE)b;
            }
        }
    }
    fclose(f);
    LOGI("arctic: registry: %u key%s from %s", nk, nk == 1 ? "" : "s", s_file);
}

/* ---- the tree's handles -------------------------------------------------- */

static HKEY issue(int key)
{
    int h;

    for (h = 0; h < MAX_HANDLES; h++)
        if (!s_handle[h]) {
            s_handle[h] = key + 1;
            return (HKEY)(uintptr_t)(H_TAG | (unsigned)h);
        }
    LOGW("arctic: registry: out of handles");
    return NULL;
}

/* -1: not ours; -2: HKLM itself; otherwise a key index. */
static int resolve(HKEY h)
{
    uintptr_t v = (uintptr_t)h;

    if (h == HKEY_LOCAL_MACHINE)
        return -2;
    if ((v & H_MASK) == H_TAG && (v & ~H_MASK) < MAX_HANDLES &&
        s_handle[v & ~H_MASK])
        return s_handle[v & ~H_MASK] - 1;
    return -1;
}

/* The full path below HKLM of `sub` under `root`, or 0. */
static int join(HKEY root, const char *sub, char *out, size_t cap)
{
    int r = resolve(root);
    char tmp[KEY_CAP];
    size_t i, o = 0;

    if (r == -1)
        return 0;
    snprintf(tmp, sizeof tmp, "%s%s%s", r >= 0 ? s_keys[r].path : "",
             r >= 0 && sub && sub[0] ? "\\" : "", sub ? sub : "");
    for (i = 0; tmp[i] && o + 1 < cap; i++) {
        if (tmp[i] == '\\' && (o == 0 || out[o - 1] == '\\'))
            continue;
        out[o++] = tmp[i];
    }
    while (o && out[o - 1] == '\\')
        o--;
    out[o] = 0;
    return 1;
}

static int inside(const char *path)
{
    size_t n = strlen(ROOT);

    return !_strnicmp(path, ROOT, n) && (path[n] == 0 || path[n] == '\\');
}

static void refuse(const char *what, const char *path)
{
    static unsigned seen;

    if (seen++ < 32)
        LOGW("arctic: registry %s [HKLM\\%s] refused: the host keeps only"
             " HKLM\\" ROOT, what, path);
}

/* ---- the API ------------------------------------------------------------- */

void arctic_reg_init(const char *file, DWORD ioblock)
{
    int i;

    lock();
    snprintf(s_file, sizeof s_file, "%s", file ? file : "");
    s_ioblock = ioblock;
    if (s_file[0])
        load();
    /* What a cabinet with this one game installed holds. */
    add_path(ROOT "\\WinCSM\\Games\\" ARCTIC_CABINET_GAME);
    i = add_path(IOBLOCK);
    if (i >= 0) {
        reg_value_t *x = value(&s_keys[i], "", 1);

        s_keys[i].volatile_ = 1;
        if (x) {
            x->type = REG_DWORD;
            x->len = sizeof s_ioblock;
            memcpy(x->data, &s_ioblock, sizeof s_ioblock);
        }
    }
    unlock();
}

LONG WINAPI arctic_RegConnectRegistryA(LPCSTR machine, HKEY root, PHKEY out)
{
    if (!out)
        return ERROR_INVALID_PARAMETER;
    if (machine && machine[0] && strcmp(machine, ".") && strcmp(machine, "\\\\.")) {
        LOGI("arctic: registry on machine [%s] refused: there is one cabinet",
             machine);
        return ERROR_BAD_NETPATH;
    }
    if (root != HKEY_LOCAL_MACHINE)
        return ERROR_ACCESS_DENIED;
    *out = HKEY_LOCAL_MACHINE;
    return ERROR_SUCCESS;
}

LONG WINAPI arctic_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam,
                                 PHKEY out)
{
    char path[KEY_CAP];
    LONG r = ERROR_FILE_NOT_FOUND;
    int i;

    (void)opt; (void)sam;
    if (!out)
        return ERROR_INVALID_PARAMETER;
    lock();
    if (!join(root, sub, path, sizeof path)) {
        r = ERROR_INVALID_HANDLE;
    } else if (!inside(path)) {
        refuse("open", path);
        r = ERROR_ACCESS_DENIED;
    } else if ((i = find(path)) >= 0) {
        *out = issue(i);
        r = *out ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
    } else {
        LOGI("arctic: registry has no [HKLM\\%s]", path);
    }
    unlock();
    return r;
}

LONG WINAPI arctic_RegCreateKeyExA(HKEY root, LPCSTR sub, DWORD res, LPSTR cls,
                                   DWORD opt, REGSAM sam,
                                   LPSECURITY_ATTRIBUTES sa, PHKEY out,
                                   LPDWORD disp)
{
    char path[KEY_CAP];
    LONG r = ERROR_SUCCESS;
    int i, existed;

    (void)res; (void)cls; (void)opt; (void)sam; (void)sa;
    if (!out)
        return ERROR_INVALID_PARAMETER;
    lock();
    if (!join(root, sub, path, sizeof path)) {
        r = ERROR_INVALID_HANDLE;
    } else if (!inside(path)) {
        refuse("create", path);
        r = ERROR_ACCESS_DENIED;
    } else {
        existed = find(path) >= 0;
        i = add_path(path);
        if (i < 0) {
            r = ERROR_OUTOFMEMORY;
        } else {
            *out = issue(i);
            if (disp)
                *disp = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
            if (!existed) {
                LOGI("arctic: registry created [HKLM\\%s]", path);
                save();
            }
        }
    }
    unlock();
    return r;
}

LONG WINAPI arctic_RegCloseKey(HKEY h)
{
    uintptr_t v = (uintptr_t)h;

    if (h == HKEY_LOCAL_MACHINE)
        return ERROR_SUCCESS;
    if ((v & H_MASK) != H_TAG || (v & ~H_MASK) >= MAX_HANDLES)
        return ERROR_INVALID_HANDLE;
    lock();
    s_handle[v & ~H_MASK] = 0;
    unlock();
    return ERROR_SUCCESS;
}

LONG WINAPI arctic_RegFlushKey(HKEY h)
{
    (void)h;
    return ERROR_SUCCESS;           /* every change is already on disk */
}

LONG WINAPI arctic_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD reserved,
                                    LPDWORD type, LPBYTE data, LPDWORD len)
{
    const reg_value_t *x;
    LONG r = ERROR_SUCCESS;
    int k;

    (void)reserved;
    lock();
    k = resolve(h);
    if (k < 0) {
        unlock();
        return ERROR_INVALID_HANDLE;
    }
    x = value(&s_keys[k], name, 0);
    if (!x) {
        LOGI("arctic: registry [HKLM\\%s] has no value \"%s\"", s_keys[k].path,
             name ? name : "");
        unlock();
        return ERROR_FILE_NOT_FOUND;
    }
    if (type)
        *type = x->type;
    if (data) {
        if (!len || *len < x->len)
            r = ERROR_MORE_DATA;
        else
            memcpy(data, x->data, x->len);
    }
    if (len)
        *len = x->len;
    unlock();
    return r;
}

LONG WINAPI arctic_RegSetValueExA(HKEY h, LPCSTR name, DWORD reserved,
                                  DWORD type, const BYTE *data, DWORD len)
{
    reg_value_t *x;
    int k;

    (void)reserved;
    if (len > DATA_CAP)
        return ERROR_INVALID_PARAMETER;
    lock();
    k = resolve(h);
    if (k < 0 || s_keys[k].volatile_) {
        unlock();
        return k < 0 ? ERROR_INVALID_HANDLE : ERROR_ACCESS_DENIED;
    }
    x = value(&s_keys[k], name, 1);
    if (!x) {
        unlock();
        return ERROR_OUTOFMEMORY;
    }
    x->type = type;
    x->len = data ? len : 0;
    if (x->len)
        memcpy(x->data, data, x->len);
    save();
    unlock();
    return ERROR_SUCCESS;
}

/* Subkeys in creation order, which is the order a cabinet's install wrote
 * them in; RegEnumKey promises no order. */
LONG WINAPI arctic_RegEnumKeyA(HKEY h, DWORD index, LPSTR out, DWORD cap)
{
    char base[KEY_CAP];
    size_t n;
    DWORD seen = 0;
    int k, i;

    lock();
    k = resolve(h);
    if (k == -1) {
        unlock();
        return ERROR_INVALID_HANDLE;
    }
    snprintf(base, sizeof base, "%s", k >= 0 ? s_keys[k].path : "");
    n = strlen(base);
    for (i = 0; i < MAX_KEYS; i++) {
        const char *p = s_keys[i].path, *rest;

        if (!s_keys[i].used || (n && (_strnicmp(p, base, n) || p[n] != '\\')))
            continue;
        rest = n ? p + n + 1 : p;
        if (!rest[0] || strchr(rest, '\\'))
            continue;
        if (seen++ == index) {
            LONG r = strlen(rest) < cap ? ERROR_SUCCESS : ERROR_MORE_DATA;

            if (!r)
                memcpy(out, rest, strlen(rest) + 1);
            unlock();
            return r;
        }
    }
    unlock();
    return ERROR_NO_MORE_ITEMS;
}

LONG WINAPI arctic_RegDeleteKeyA(HKEY root, LPCSTR sub)
{
    char path[KEY_CAP] = "";
    size_t n;
    LONG r = ERROR_FILE_NOT_FOUND;
    int i;

    lock();
    if (!join(root, sub, path, sizeof path) || !inside(path)) {
        if (path[0])
            refuse("delete", path);
        unlock();
        return ERROR_ACCESS_DENIED;
    }
    n = strlen(path);
    for (i = 0; i < MAX_KEYS; i++)
        if (s_keys[i].used && !_strnicmp(s_keys[i].path, path, n) &&
            s_keys[i].path[n] == '\\') {
            unlock();
            return ERROR_ACCESS_DENIED;     /* RegDeleteKey: no subkeys */
        }
    i = find(path);
    if (i >= 0 && !s_keys[i].volatile_) {
        s_keys[i].used = 0;
        LOGI("arctic: registry deleted [HKLM\\%s]", path);
        save();
        r = ERROR_SUCCESS;
    }
    unlock();
    return r;
}
