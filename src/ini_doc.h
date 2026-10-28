/* ini_doc.h -- vcthunder.ini as a document, not as a set of values.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The launcher is the only thing that WRITES the user's ini, and the file it
 * writes is one a person edits by hand: comments, ordering, blank lines, and
 * whichever line ending it already had. So it holds the file as its own lines
 * and changes one line at a time rather than parsing and printing back. The
 * value grammar is src/ini.h's, shared with the other three readers.
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

/* Rewrite the value in place, keeping the line's spacing and trailing comment.
 * A key not in the file is inserted at the end of its section; a section not in
 * the file is appended. ini_doc_get() on the result answers exactly `value`,
 * less surrounding blanks; see replace_value(). */
int  ini_doc_set(ini_doc_t *doc, const char *section, const char *key,
                 const char *value);

/* The n-th DISTINCT key of a section, in file order. The document model's one
 * enumeration, and it exists for [devices], whose KEYS are the data: a label
 * is chosen by the user and cannot be looked up by name. */
int  ini_doc_key_index(const ini_doc_t *doc, const char *section,
                       unsigned index, char *out, size_t out_size);

/* COMMENT the key out rather than deleting the line: what the user wrote
 * stays visible, and stops being read. */
int  ini_doc_remove(ini_doc_t *doc, const char *section, const char *key);

/* DELETE the key's lines outright, for undoing a line the launcher itself
 * inserted this session; a line the user wrote is ini_doc_remove()'s. With
 * `empty_section`, a section this leaves with nothing in it goes too, header
 * and the blank line ini_doc_set() put before it. */
int  ini_doc_drop(ini_doc_t *doc, const char *section, const char *key,
                  int empty_section);

/* Does the document have a [section] header, keys or not? */
int  ini_doc_has_section(const ini_doc_t *doc, const char *section);

/* Would saving `a` change what `b` says? Line endings, blank lines and
 * sections with nothing in them are not content. */
int  ini_doc_same_content(const ini_doc_t *a, const ini_doc_t *b);

/* Write through a temporary and MoveFileEx, keeping a .bak. On failure
 * nothing is touched and `why` says what Windows refused. */
int  ini_doc_save(ini_doc_t *doc, char *why, size_t why_size);

/* Is this line a [section] header, and which? Exposed because the launcher
 * scans the document for title-scoped sections it does not itself manage. */
int  ini_doc_section(const char *line, char *out, size_t out_size);

/* ...and is it a `key = value` assignment, and which key? Blank lines and
 * comments are 0. Exposed for the same scan: a key this build does not know
 * is silently ignored by every reader of this file (an unknown key HAS to be,
 * since the binding maps live in the same file), so the launcher is the only
 * place that can say a line does nothing. */
int  ini_doc_line_key(const char *line, char *out, size_t out_size);

#endif
