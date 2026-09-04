/* ini.c -- the value grammar of vcthunder.ini, in one place.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See src/ini.h for what this is and, more importantly, what it is not: the
 * three readers over this file stay three readers. Only the grammar is shared.
 */
#include "ini.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *ini_trim_left(char *s)
{
    while (*s && isspace((unsigned char)*s) && *s != '\r' && *s != '\n')
        s++;
    return s;
}

void ini_trim_right(char *s)
{
    size_t n = strlen(s);

    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

char *ini_trim(char *s)
{
    s = ini_trim_left(s);
    ini_trim_right(s);
    return s;
}

const char *ini_comment_start(const char *text)
{
    const char *p;

    for (p = text; *p; p++)
        if ((*p == ';' || *p == '#') &&
            (p == text || isspace((unsigned char)p[-1])))
            return p;
    return NULL;
}

void ini_strip_comment(char *text)
{
    /* The one cast: the caller handed us a mutable string, so the position
     * ini_comment_start() found in it is mutable too. */
    char *p = (char *)ini_comment_start(text);

    if (p)
        *p = '\0';
    ini_trim_right(text);
}

int ini_parse_bool(const char *v, int *out)
{
    if (!v)
        return 0;
    if (_stricmp(v, "true") == 0 || _stricmp(v, "yes") == 0 ||
        _stricmp(v, "on") == 0 || strcmp(v, "1") == 0)
        *out = 1;
    else if (_stricmp(v, "false") == 0 || _stricmp(v, "no") == 0 ||
             _stricmp(v, "off") == 0 || strcmp(v, "0") == 0)
        *out = 0;
    else
        return 0;
    return 1;
}

/* strtoul accepts a leading '-' and wraps it, so "-1" reads as 4294967295 and
 * lands inside almost any range. Refuse the sign before parsing rather than
 * hoping the range catches it: io_config.c's own copy did not, and a negative
 * deadzone came out as an enormous one. */
int ini_parse_uint(const char *v, unsigned lo, unsigned hi, unsigned *out)
{
    char *end;
    unsigned long n;

    if (!v || !*v || *v == '-')
        return 0;
    errno = 0;
    n = strtoul(v, &end, 0);
    if (errno == ERANGE || end == v || *end != '\0' ||
        n < (unsigned long)lo || n > (unsigned long)hi)
        return 0;
    *out = (unsigned)n;
    return 1;
}

int ini_parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long n;

    if (!v || !*v)
        return 0;
    errno = 0;
    n = strtol(v, &end, 0);
    if (errno == ERANGE || end == v || *end != '\0' ||
        n < (long)lo || n > (long)hi)
        return 0;
    *out = (int)n;
    return 1;
}

int ini_bool_or(const char *v, int fallback)
{
    int n;

    return ini_parse_bool(v, &n) ? n : fallback;
}

unsigned ini_uint_or(const char *v, unsigned lo, unsigned hi, unsigned fallback)
{
    unsigned n;

    return ini_parse_uint(v, lo, hi, &n) ? n : fallback;
}

void ini_read_scoped(const char *path, const char *section, const char *key,
                     const char *fallback, char *out, DWORD out_size)
{
    char scoped[64];

    /* 1. this game's own scoped section */
    if (G && G->id[0]) {
        snprintf(scoped, sizeof scoped, "%s.%s", G->id, section);
        GetPrivateProfileStringA(scoped, key, "", out, out_size, path);
        ini_strip_comment(out);
        if (out[0])
            return;
    }
    /* 2. the plain section both cabinets share */
    GetPrivateProfileStringA(section, key, fallback, out, out_size, path);
    ini_strip_comment(out);
}

void ini_resolve_path(const char *in, char *out, size_t out_size)
{
    DWORD n = GetFullPathNameA(in, (DWORD)out_size, out, NULL);

    if (!n || n >= out_size)
        snprintf(out, out_size, "%s", in);
}
