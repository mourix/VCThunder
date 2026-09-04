/* ini_doc.h -- vcthunder.ini as a document, not as a set of values.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The launcher is the only thing here that WRITES the user's ini, and the file
 * it writes is a file a person has been editing by hand: comments, ordering,
 * blank lines, and the CRLF or LF the file already had. So it does not parse
 * the ini into values and print them back out -- that would silently rewrite
 * everything a user had put there. It holds the file as its own lines and
 * changes one line at a time.
 *
 * That is why this is a THIRD reader of one file and not a duplicate of the
 * other two. What it does not carry is the value grammar: a comment delimiter,
 * a boolean, whitespace, are src/ini.h's and shared with config.c, io_config.c
 * and dinput.c.
 *
 * It is separate from launcher.c because it is the half with no Win32 window
 * in it and the only half under test: tests/ini_doc_test.c builds THIS file
 * and nothing else, so a passing test means the document model round-trips,
 * rather than meaning a GUI translation unit compiled.
 */
#ifndef VCT_INI_DOC_H
#define VCT_INI_DOC_H

#include <stddef.h>

#include "vcthunder.h"

/* The longest value the launcher will hold, edit or write. Also the width of
 * its own per-setting buffers: one number, so a value that fits in the editor
 * cannot fail to fit in the document. */
#define INI_VALUE_CAP 512

typedef struct {
    char **line;                /* each with its own terminator, as read     */
    size_t count, capacity;
    char newline[3];            /* what THIS file uses; new lines get it too */
    char path[MAX_PATH];
} ini_doc_t;

/* Read `path` into `doc`. A file that is not there is success with no lines:
 * a first run has no ini yet and must still be able to save one. Any other
 * failure is 0, and the doc is left empty. */
int  ini_doc_load(ini_doc_t *doc, const char *path);
void ini_doc_free(ini_doc_t *doc);

/* The LAST assignment of `key` in `section` wins, which is what Windows'
 * own profile API does and therefore what the loader sees. */
int  ini_doc_get(const ini_doc_t *doc, const char *section, const char *key,
                 char *out, size_t out_size);

/* Rewrite the value in place, keeping the line's spacing and its trailing
 * comment. A key that is not in the file is inserted at the end of its
 * section; a section that is not in the file is appended. */
int  ini_doc_set(ini_doc_t *doc, const char *section, const char *key,
                 const char *value);

/* COMMENT the key out rather than deleting the line: what the user wrote
 * stays visible, and stops being read. */
int  ini_doc_remove(ini_doc_t *doc, const char *section, const char *key);

/* Write through a temporary and MoveFileEx, keeping a .bak. On failure
 * nothing is touched and `why` says what Windows refused. */
int  ini_doc_save(ini_doc_t *doc, char *why, size_t why_size);

/* Is this line a [section] header, and which? Exposed because the launcher
 * scans the document for title-scoped sections it does not itself manage. */
int  ini_doc_section(const char *line, char *out, size_t out_size);

#endif
