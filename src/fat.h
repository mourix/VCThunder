/* fat.h -- the partitions and FAT volumes of a cabinet's disk, read-only.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The C form of the Python ingest's partition and FAT readers, which the
 * maintainer keeps as the oracle. It reads through a callback, so the source
 * can be a CHD that is never extracted to an image. */
#ifndef VCT_FAT_H
#define VCT_FAT_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* The disk image: read n bytes at pos (1 = all of them), and, optionally, be
 * told which ranges are about to be read, in order. */
typedef struct {
    int (*read)(void *ctx, uint64_t pos, void *buf, size_t n);
    void (*plan)(void *ctx, const uint64_t *start, const uint32_t *len,
                 size_t n);
    void *ctx;
} fat_image_t;

/* One partition entry. type 0 means the image has no MBR and is tried whole. */
typedef struct {
    uint8_t type;
    uint32_t lba;
} fat_part_t;

#define FAT_MAX_PARTS 32

/* Every partition entry: the four primary slots, and the logical volumes
 * inside any extended one. Returns the count. */
int fat_partitions(const fat_image_t *img, fat_part_t *out, int max);
/* 1 for a partition type this reader understands, and its name. */
int fat_type_known(uint8_t type);
const char *fat_type_name(uint8_t type);

typedef struct fat fat_t;

int fat_open(const fat_image_t *img, uint32_t lba, fat_t **out,
             char *err, size_t errsz);
void fat_close(fat_t *v);
unsigned fat_bits(const fat_t *v);
const char *fat_label(const fat_t *v);

/* Every file in the volume, depth first in directory order: its path relative
 * to the volume root ('\\' between components), first cluster and size. A
 * nonzero return from the visitor stops the walk and is returned. Returns 0
 * when the walk completed, -1 on a damaged volume (err says how). */
typedef int (*fat_visit_t)(void *ctx, const wchar_t *path, uint32_t cluster,
                           uint32_t size);
int fat_walk(fat_t *v, fat_visit_t visit, void *ctx, char *err, size_t errsz);

/* One file's bytes, streamed to sink in order. Never holds the file whole.
 * A nonzero return from sink stops it and is returned; -1 on a damaged
 * volume or a failed read. */
typedef int (*fat_sink_t)(void *ctx, const void *buf, size_t n);
int fat_read_file(fat_t *v, uint32_t cluster, uint32_t size, fat_sink_t sink,
                  void *ctx, char *err, size_t errsz);

#endif
