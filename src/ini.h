/* ini.h -- the value grammar of vcthunder.ini, in one place.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Four files read this one file, through three different mechanisms: config.c
 * parses it line by line into shim_config_t, io_config.c and dinput.c ask
 * Win32's profile API for one key at a time, and launcher.c holds it as a
 * line-preserving document (src/ini_doc.h) so a save keeps the user's comments.
 * Those mechanisms are genuinely three things and stay three things.
 *
 * What is NOT three things is what a value MEANS. A boolean, a comment
 * delimiter, an integer and the `[<title>.<section>]` precedence rule are one
 * answer each, and this file is where that answer is. NOTHING ELSE MAY CARRY A
 * SECOND SPELLING OF ANY OF THEM: three genuinely different readers are exactly
 * the excuse that makes four copies of one predicate look like four different
 * things, and copies of a predicate drift silently, because each one is
 * plausible on its own.
 *
 * Nothing here logs. config.c's parse runs before the log file is open --
 * that is why it retains notes and reports them later -- so a helper that
 * called LOGW() could not be shared with it.
 */
#ifndef VCT_INI_H
#define VCT_INI_H

#include <stddef.h>

#include "vcthunder.h"

/* ---- whitespace --------------------------------------------------------- */

/* trim_left does NOT eat \r or \n: the launcher's document model keeps each
 * line's own terminator and needs to find it again when it writes the file
 * back. Everywhere else the terminator is gone before these are called. */
char *ini_trim_left(char *s);
void  ini_trim_right(char *s);
char *ini_trim(char *s);            /* both; returns the new start */

/* ---- comments ----------------------------------------------------------- */

/* An inline comment starts at a ; or # that is at the start of the text or
 * preceded by whitespace, so a value may contain either character: a Windows
 * path with a # in it, or a key name spelled "#". Returns NULL when there
 * is none. */
const char *ini_comment_start(const char *text);

/* Truncate at the inline comment, then right-trim. */
void  ini_strip_comment(char *text);

/* ---- values ------------------------------------------------------------- */

/* Each returns 1 when the text is a valid value and 0 when it is not; on 0 the
 * out parameter is untouched, so a caller may seed it with the previous layer
 * and let a bad value leave that in force. */
int ini_parse_bool(const char *v, int *out);
int ini_parse_uint(const char *v, unsigned lo, unsigned hi, unsigned *out);
int ini_parse_int(const char *v, int lo, int hi, int *out);

/* The forgiving forms, for the callers that have a default and no channel to
 * complain on. An out-of-range or unspellable value is the fallback. */
int      ini_bool_or(const char *v, int fallback);
unsigned ini_uint_or(const char *v, unsigned lo, unsigned hi, unsigned fallback);

/* ---- reading one key ---------------------------------------------------- */

/* Read [<G->id>.<section>] <key>, falling back to [<section>] <key> and then
 * to `fallback`. The precedence rule for the sections config.c does not own,
 * written once: io_config.c and dinput.c both read [analog], [ffb] and the
 * binding sections through this and nothing else.
 *
 * `path` must be ABSOLUTE. The profile API resolves a bare filename against
 * the Windows directory, not the working directory, so a relative path here
 * reads a file that does not exist and every key silently takes its default.
 * ini_resolve_path() is what the callers use to be sure. */
void ini_read_scoped(const char *path, const char *section, const char *key,
                     const char *fallback, char *out, DWORD out_size);

void ini_resolve_path(const char *in, char *out, size_t out_size);

#endif
