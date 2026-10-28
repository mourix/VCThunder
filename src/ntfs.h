/* ntfs.h -- files out of an NTFS volume, read-only.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The C form of the project's Python NTFS reader, which the maintainer keeps
 * as the oracle. It reads through fat.h's image callbacks, so the source can
 * be a CHD that is never extracted. No compression, no journal, no write: a
 * compressed or encrypted file is refused, never guessed at. */
#ifndef VCT_NTFS_H
#define VCT_NTFS_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "fat.h"

#define NTFS_MAX_VOLUMES 4
#define NTFS_ROOT 5u                /* the root directory's MFT record */

/* Byte offsets of the image's NTFS volumes: the image itself when sector 0 is
 * an NTFS boot sector (Arctic Thunder's disk has no partition table), else
 * each primary partition of NTFS's type. Returns the count. */
int ntfs_volumes(const fat_image_t *img, uint64_t *out, int max);

typedef struct ntfs ntfs_t;

int ntfs_open(const fat_image_t *img, uint64_t offset, ntfs_t **out,
              char *err, size_t errsz);
void ntfs_close(ntfs_t *v);
uint32_t ntfs_cluster_bytes(const ntfs_t *v);
uint64_t ntfs_record_count(const ntfs_t *v);

/* The MFT record of `path` ('/' or '\\' between components, UTF-8), matched
 * without regard to case. 1 found, 0 absent, -1 a damaged volume. */
int ntfs_lookup(ntfs_t *v, const char *path, uint64_t *rec, char *err,
                size_t errsz);

/* Every file under directory `dir`, depth first in index order: its path
 * relative to `dir` ('\\' between components) and its record. A nonzero
 * return from the visitor stops the walk and is returned; -1 on a damaged
 * volume. */
typedef int (*ntfs_visit_t)(void *ctx, const wchar_t *path, uint64_t rec);
int ntfs_walk(ntfs_t *v, uint64_t dir, ntfs_visit_t visit, void *ctx,
              char *err, size_t errsz);

/* A file's size, from its unnamed $DATA. 0 or -1 (damaged, or a stream this
 * reader refuses). */
int ntfs_file_size(ntfs_t *v, uint64_t rec, uint64_t *size, char *err,
                   size_t errsz);

/* A file's bytes, streamed to sink in order; the image is told the file's
 * ranges first. A nonzero return from sink stops it and is returned; -1 on a
 * damaged volume or a failed read. */
int ntfs_read_file(ntfs_t *v, uint64_t rec, fat_sink_t sink, void *ctx,
                   char *err, size_t errsz);

#endif
