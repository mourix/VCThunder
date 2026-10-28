/* chd.h -- read a MAME CHD v5 without MAME.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The C form of the project's Python CHD reader, which the maintainer keeps as
 * its oracle: the same container, the same four codecs, the same checks, and
 * the same standing -- written from the container's own structure, no MAME or
 * libchdr source read. Decoding runs on worker threads a planned window ahead
 * of the reader. NO GAME MATERIAL IS IN THIS FILE. */
#ifndef VCT_CHD_H
#define VCT_CHD_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct chd chd_t;

/* CHD_UNSUPPORTED is a file this reader declines (not v5, has a parent, an
 * unknown codec); CHD_ERROR is a file that is wrong, or a read that failed. */
enum { CHD_OK = 0, CHD_UNSUPPORTED = 1, CHD_ERROR = 2 };

/* jobs = decoder threads; 0 picks one per hardware thread, capped. */
int chd_open(const wchar_t *path, unsigned jobs, chd_t **out,
             char *err, size_t errsz);
void chd_close(chd_t *c);

uint64_t chd_size(const chd_t *c);
uint32_t chd_hunk_bytes(const chd_t *c);
unsigned chd_jobs(const chd_t *c);
uint64_t chd_hunks_decoded(const chd_t *c);
/* The header's SHA-1 of the whole decoded image: the oracle for a full read. */
void chd_raw_sha1(const chd_t *c, uint8_t out[20]);
/* The header's SHA-1 of the image and its metadata: the one MAME names a disk
 * by. */
void chd_sha1(const chd_t *c, uint8_t out[20]);

/* n bytes of the decoded image at pos, clamped to its end; *got says how many.
 * On failure chd_error() says why, and the image is not usable. */
int chd_read(chd_t *c, uint64_t pos, void *buf, size_t n, size_t *got);
const char *chd_error(const chd_t *c);

/* The byte ranges about to be read, in the order they will be read. A file's
 * cluster chain is known before a byte of it is copied, and handing it over is
 * what lets the decode run ahead on every core instead of one hunk at a time.
 * The arrays are copied; a new plan replaces the old one. */
void chd_plan(chd_t *c, const uint64_t *start, const uint32_t *len, size_t n);

/* The pieces, for tests. */
uint16_t chd_crc16(const void *data, size_t n, uint16_t seed);
int chd_huff_decode(const uint8_t *raw, size_t rawlen, uint8_t *out,
                    size_t outlen);
int chd_flac_decode(const uint8_t *raw, size_t rawlen, uint8_t *out,
                    size_t outlen);
/* A CD sector's sync and ECMA-130 P/Q parity, rebuilt in place. */
void chd_cd_rebuild(uint8_t *sector);
/* One cdlz/cdzl/cdfl hunk of `frames` 2448-byte frames into out. */
int chd_cd_decode(const char codec[4], const uint8_t *raw, size_t rawlen,
                  uint8_t *out, unsigned frames);

#endif
