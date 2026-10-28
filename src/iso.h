/* iso.h -- files out of a CD image, read-only.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The C form of the project's Python ISO 9660 reader, which the maintainer
 * keeps as the oracle.
 * It reads through fat.h's image callbacks, so the source can be a CD CHD
 * that is never extracted, or a cue sheet's image on the host. The sector
 * layout -- raw 2352-byte sectors, 2448-byte CHD frames, or cooked 2048-byte
 * blocks -- is decided from the image's own bytes, never from a sheet's word
 * for it. ISO 9660 with Joliet names when the disc has them. A file this
 * reader cannot read faithfully is refused, never guessed at: one spread
 * over several extents, an interleaved one, a mode 2 form 2 sector. */
#ifndef VCT_ISO_H
#define VCT_ISO_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "fat.h"

typedef struct iso iso_t;

/* A file or a directory on the disc. */
typedef struct {
    uint32_t extent, size;
    int dir;
} iso_node_t;

/* 1 opened; 0 not a CD image (err empty); -1 a CD image this reader cannot
 * read, err saying why. */
int iso_open(const fat_image_t *img, iso_t **out, char *err, size_t errsz);
void iso_close(iso_t *v);
/* "raw mode 2, 2352-byte sectors" and the like, for narration. */
const char *iso_layout(const iso_t *v);

/* `path` ('/' or '\\' between components, UTF-8), matched without regard to
 * case. 1 found, 0 absent, -1 a damaged disc. */
int iso_lookup(iso_t *v, const char *path, iso_node_t *n, char *err,
               size_t errsz);

/* Every file under directory `dir`, depth first in disc order: its path
 * relative to `dir` ('\\' between components). A nonzero return from the
 * visitor stops the walk and is returned; -1 on a damaged disc. */
typedef int (*iso_visit_t)(void *ctx, const wchar_t *path,
                           const iso_node_t *n);
int iso_walk(iso_t *v, const iso_node_t *dir, iso_visit_t visit, void *ctx,
             char *err, size_t errsz);

/* A file's bytes, streamed to sink in order; the image is told the file's
 * range first. A nonzero return from sink stops it and is returned; -1 on a
 * damaged disc or a failed read. */
int iso_read_file(iso_t *v, const iso_node_t *n, fat_sink_t sink, void *ctx,
                  char *err, size_t errsz);

/* A CD image on the host as a fat_image_t: a cue sheet or a cdrdao toc (its
 * first FILE or DATAFILE, beside it), or the image itself (.bin, .iso).
 * 1 opened, 0 not something that names or is an image file. */
int iso_file_open(const wchar_t *path, fat_image_t *img, char *err,
                  size_t errsz);
void iso_file_close(fat_image_t *img);
/* 1 when the name says a CD image or its sheet: .cue, .toc, .iso, .bin. */
int iso_file_name(const wchar_t *path);

#endif
