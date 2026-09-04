/* ini_doc.c -- vcthunder.ini as a document, not as a set of values.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * See src/ini_doc.h. Nothing in this file draws, and nothing in it knows what
 * a setting is.
 */
#include "ini_doc.h"
#include "ini.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static int doc_reserve(ini_doc_t *doc, size_t need)
{
    char **p;
    size_t cap;

    if (need <= doc->capacity)
        return 1;
    cap = doc->capacity ? doc->capacity * 2 : 64;
    while (cap < need)
        cap *= 2;
    p = (char **)realloc(doc->line, cap * sizeof *p);
    if (!p)
        return 0;
    doc->line = p;
    doc->capacity = cap;
    return 1;
}

static int doc_push(ini_doc_t *doc, char *line)
{
    if (!doc_reserve(doc, doc->count + 1))
        return 0;
    doc->line[doc->count++] = line;
    return 1;
}

static int doc_insert(ini_doc_t *doc, size_t at, char *line)
{
    if (!doc_reserve(doc, doc->count + 1))
        return 0;
    memmove(&doc->line[at + 1], &doc->line[at],
            (doc->count - at) * sizeof *doc->line);
    doc->line[at] = line;
    doc->count++;
    return 1;
}

void ini_doc_free(ini_doc_t *doc)
{
    size_t i;
    for (i = 0; i < doc->count; i++)
        free(doc->line[i]);
    free(doc->line);
    memset(doc, 0, sizeof *doc);
}

int ini_doc_load(ini_doc_t *doc, const char *path)
{
    FILE *fp;
    char *data;
    long size;
    size_t at = 0;

    memset(doc, 0, sizeof *doc);
    snprintf(doc->path, sizeof doc->path, "%s", path);
    snprintf(doc->newline, sizeof doc->newline, "\r\n");
    fp = fopen(path, "rb");
    if (!fp)
        return errno == ENOENT;
    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return 0;
    }
    data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(fp);
        return 0;
    }
    if (size && fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    data[size] = '\0';
    while (at < (size_t)size) {
        size_t end = at;
        while (end < (size_t)size && data[end] != '\n')
            end++;
        if (end < (size_t)size)
            end++;
        if (!doc_push(doc, xstrndup(data + at, end - at))) {
            free(data);
            ini_doc_free(doc);
            return 0;
        }
        if (end - at >= 2 && data[end - 2] == '\r')
            snprintf(doc->newline, sizeof doc->newline, "\r\n");
        else if (end > at && data[end - 1] == '\n')
            snprintf(doc->newline, sizeof doc->newline, "\n");
        at = end;
    }
    free(data);
    return 1;
}

int ini_doc_section(const char *line, char *out, size_t out_size)
{
    char copy[256], *p, *end;
    snprintf(copy, sizeof copy, "%s", line);
    p = ini_trim_left(copy);
    if (*p != '[' || !(end = strchr(p + 1, ']')))
        return 0;
    *end = '\0';
    snprintf(out, out_size, "%s", p + 1);
    ini_trim_right(out);
    return 1;
}

static int doc_key_value(const char *line, char *key, size_t key_size,
                           char *value, size_t value_size)
{
    char copy[1024], *p, *eq;

    snprintf(copy, sizeof copy, "%s", line);
    p = ini_trim_left(copy);
    if (!*p || *p == ';' || *p == '#' || *p == '[')
        return 0;
    eq = strchr(p, '=');
    if (!eq)
        return 0;
    *eq = '\0';
    ini_trim_right(p);
    snprintf(key, key_size, "%s", p);
    p = ini_trim_left(eq + 1);
    ini_strip_comment(p);
    snprintf(value, value_size, "%s", p);
    return 1;
}

int ini_doc_get(const ini_doc_t *doc, const char *section, const char *key,
                   char *out, size_t out_size)
{
    char current[96] = "", found[INI_VALUE_CAP] = "";
    size_t i;
    int have = 0;

    for (i = 0; i < doc->count; i++) {
        char name[96], k[96], value[INI_VALUE_CAP];
        if (ini_doc_section(doc->line[i], name, sizeof name)) {
            snprintf(current, sizeof current, "%s", name);
            continue;
        }
        if (_stricmp(current, section) ||
            !doc_key_value(doc->line[i], k, sizeof k, value, sizeof value) ||
            _stricmp(k, key))
            continue;
        snprintf(found, sizeof found, "%s", value);
        have = 1;
    }
    if (have)
        snprintf(out, out_size, "%s", found);
    return have;
}

static size_t line_eol(const char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        n--;
    return n;
}

static char *replace_value(const char *line, const char *value)
{
    const char *eq = strchr(line, '=');
    const char *start, *suffix, *p;
    size_t prefix, end = line_eol(line), line_size = strlen(line), n;
    char *out;

    if (!eq)
        return NULL;
    start = eq + 1;
    while ((size_t)(start - line) < end && (*start == ' ' || *start == '\t'))
        start++;
    suffix = line + end;
    p = ini_comment_start(start);
    if (p && (size_t)(p - line) < end)
        suffix = p;
    while (suffix > start && (suffix[-1] == ' ' || suffix[-1] == '\t'))
        suffix--;
    prefix = (size_t)(start - line);
    n = prefix + strlen(value) + line_size - (size_t)(suffix - line) + 1;
    out = (char *)malloc(n);
    if (!out)
        return NULL;
    memcpy(out, line, prefix);
    memcpy(out + prefix, value, strlen(value));
    memcpy(out + prefix + strlen(value), suffix,
           line_size - (size_t)(suffix - line));
    out[n - 1] = '\0';
    return out;
}

int ini_doc_set(ini_doc_t *doc, const char *section, const char *key,
                   const char *value)
{
    char current[96] = "";
    size_t i, last = (size_t)-1, section_end = (size_t)-1;

    for (i = 0; i < doc->count; i++) {
        char name[96], k[96], old[INI_VALUE_CAP];
        if (ini_doc_section(doc->line[i], name, sizeof name)) {
            if (!_stricmp(current, section) && section_end == (size_t)-1)
                section_end = i;
            snprintf(current, sizeof current, "%s", name);
            continue;
        }
        if (!_stricmp(current, section)) {
            section_end = i + 1;
            if (doc_key_value(doc->line[i], k, sizeof k, old, sizeof old) &&
                !_stricmp(k, key))
                last = i;
        }
    }
    if (last != (size_t)-1) {
        char *changed = replace_value(doc->line[last], value);
        if (!changed)
            return 0;
        free(doc->line[last]);
        doc->line[last] = changed;
        return 1;
    }
    {
        char text[1024];
        char *added;
        if (section_end != (size_t)-1) {
            snprintf(text, sizeof text, "%s = %s%s", key, value, doc->newline);
            added = _strdup(text);
            return added && doc_insert(doc, section_end, added);
        }
        if (doc->count && line_eol(doc->line[doc->count - 1]) != 0) {
            if (!doc_push(doc, _strdup(doc->newline)))
                return 0;
        }
        snprintf(text, sizeof text, "[%s]%s", section, doc->newline);
        if (!doc_push(doc, _strdup(text)))
            return 0;
        snprintf(text, sizeof text, "%s = %s%s", key, value, doc->newline);
        return doc_push(doc, _strdup(text));
    }
}

int ini_doc_remove(ini_doc_t *doc, const char *section, const char *key)
{
    char current[96] = "";
    size_t i;

    for (i = 0; i < doc->count; i++) {
        char name[96], k[96], value[INI_VALUE_CAP];
        if (ini_doc_section(doc->line[i], name, sizeof name)) {
            snprintf(current, sizeof current, "%s", name);
            continue;
        }
        if (!_stricmp(current, section) &&
            doc_key_value(doc->line[i], k, sizeof k, value, sizeof value) &&
            !_stricmp(k, key)) {
            size_t n = strlen(doc->line[i]) + 3;
            char *commented = (char *)malloc(n);
            if (!commented)
                return 0;
            snprintf(commented, n, "; %s", doc->line[i]);
            free(doc->line[i]);
            doc->line[i] = commented;
        }
    }
    return 1;
}

int ini_doc_save(ini_doc_t *doc, char *why, size_t why_size)
{
    char tmp[MAX_PATH], bak[MAX_PATH];
    HANDLE file;
    size_t i;
    DWORD wrote;

    /* %.*s, not %s: doc->path is itself MAX_PATH, so a full-length path leaves
       no room for the suffix, and a truncation that dropped ".tmp" would make
       tmp name the REAL file, so the atomic write would clobber in place the
       file it is meant to be protecting. Bound the path; keep the suffix. */
    snprintf(tmp, sizeof tmp, "%.*s.tmp",
             (int)(sizeof tmp - sizeof ".tmp"), doc->path);
    snprintf(bak, sizeof bak, "%.*s.bak",
             (int)(sizeof bak - sizeof ".bak"), doc->path);
    file = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        snprintf(why, why_size, "Cannot create %s (Windows error %lu).",
                 tmp, GetLastError());
        return 0;
    }
    for (i = 0; i < doc->count; i++) {
        DWORD want = (DWORD)strlen(doc->line[i]);
        if (!WriteFile(file, doc->line[i], want, &wrote, NULL) || wrote != want) {
            snprintf(why, why_size, "Cannot write %s (Windows error %lu).",
                     tmp, GetLastError());
            CloseHandle(file);
            DeleteFileA(tmp);
            return 0;
        }
    }
    if (!FlushFileBuffers(file)) {
        snprintf(why, why_size, "Cannot flush %s (Windows error %lu).",
                 tmp, GetLastError());
        CloseHandle(file);
        DeleteFileA(tmp);
        return 0;
    }
    CloseHandle(file);
    if (GetFileAttributesA(doc->path) != INVALID_FILE_ATTRIBUTES &&
        !CopyFileA(doc->path, bak, FALSE)) {
        snprintf(why, why_size, "Cannot back up %s (Windows error %lu).",
                 doc->path, GetLastError());
        DeleteFileA(tmp);
        return 0;
    }
    if (!MoveFileExA(tmp, doc->path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        snprintf(why, why_size, "Cannot replace %s (Windows error %lu).",
                 doc->path, GetLastError());
        DeleteFileA(tmp);
        return 0;
    }
    return 1;
}

