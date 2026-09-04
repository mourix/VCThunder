/* capture.h -- self-describing .vglc call stream.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Records preserve ordered arguments and caller-owned payloads, use little-
 * endian fields, and are four-byte aligned.
 */
#ifndef VCGLIDE_CAPTURE_H
#define VCGLIDE_CAPTURE_H

#include <stdint.h>

#define VGLC_MAGIC      0x434C4756u   /* "VGLC" */
#define VGLC_VERSION    1u

/* File header, followed by `entry_count` entry descriptors, followed by the
 * record stream. `header_bytes` is the offset of the first record, so a
 * reader that does not care about a future header field can still skip to
 * the stream. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t entry_count;
    uint32_t vertex_bytes;      /* GR_VERTEX_BYTES the capture was taken with */
    uint32_t flags;
    uint32_t reserved[2];
    char     title[32];         /* the game, for a human reading the file */
    char     backend[64];       /* the provider that serviced the capture */
} vglc_header_t;

/* One per entry point, in the order this capture uses as its indices.
 * `name` follows, `namelen` bytes, then padding to a 4-byte boundary. */
typedef struct {
    uint16_t argbytes;
    uint8_t  payload;
    uint8_t  namelen;
} vglc_entry_t;

/* A record: an entry index, its argument bytes, then its blobs.
 *   idx  < entry_count   a call; args are argbytes[idx] long
 *   idx == VGLC_CONTROL  a control record; args are VGLC_CONTROL_ARGS long */
#define VGLC_CONTROL        0xFFFFu
#define VGLC_CONTROL_ARGS   16u

typedef struct {
    uint16_t idx;
    uint16_t blobs;
} vglc_record_t;

/* Control record kinds. args[0] is the kind; the rest are its own. */
enum {
    VGLC_FRAME = 1,     /* args[1] = frame number, at the START of the frame */
    VGLC_LFB   = 2,     /* a write-mode LFB region: args[1] = buffer,
                         * args[2] = strideInBytes, args[3] = origin.
                         * One blob: the region as the caller left it. */
    VGLC_END   = 3,     /* args[1] = records written, args[2] = frames */
};

/* A blob: `tag` says which argument it belongs to, or what it is.
 * Tags 0..15 are argument indices; the blob is what that pointer argument
 * pointed at. Tags above that are named. `size` is the payload size in bytes;
 * the payload is padded to 4. */
typedef struct {
    uint32_t tag;
    uint32_t size;
} vglc_blob_t;

#define VGLC_TAG_ARG(n)     ((uint32_t)(n))     /* the blob for argument n */
#define VGLC_TAG_TEXDATA    0x100u              /* GrTexInfo's own data */
#define VGLC_TAG_LFB        0x101u              /* an LFB write region */

/* Payload rules. Kept numerically stable: a capture stores these values. */
enum {
    PL_NONE = 0, PL_V2, PL_V3, PL_VLIST, PL_FOG, PL_TEXDOWN, PL_TEXINFO3,
    PL_TEXINFO1, PL_TEXPART, PL_TEXTABLE, PL_STATE_IN, PL_STATE_OUT,
    PL_HWCFG, PL_CALLBACK, PL_LFBINFO, PL_LFBUNLOCK, PL_WINOPEN, PL_SWAP,
    PL_VIDTIMING
};

#endif /* VCGLIDE_CAPTURE_H */
