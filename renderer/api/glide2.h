/* glide2.h -- minimal clean-room Glide 2.x interface subset.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Entry signatures come from game call sites; required structure offsets and
 * public enumerant values are represented without external implementation
 * declarations.
 */
#ifndef VCGLIDE_GLIDE2_H
#define VCGLIDE_GLIDE2_H

#include <stdint.h>

/* GrVertex uses the games' derived 60-byte array stride: nine vertex floats and
 * two texture-unit triples. Separate-pointer triangle calls do not use it. */
#define GR_VERTEX_BYTES     60

/* Required GrTexInfo offsets: smallLod, largeLod, aspectRatio, format, and data
 * occupy consecutive 32-bit fields. */
#define GR_TEXINFO_BYTES    20
#define GR_TEXINFO_SMALLLOD 0x00
#define GR_TEXINFO_LARGELOD 0x04
#define GR_TEXINFO_ASPECT   0x08
#define GR_TEXINFO_FORMAT   0x0c
#define GR_TEXINFO_DATA     0x10

/* evenOdd selects uploaded mip levels but does not describe the source buffer.
 * Split uploads share one full-chain data pointer. */
#define GR_MIPMAPLEVELMASK_EVEN 0x1
#define GR_MIPMAPLEVELMASK_ODD  0x2
#define GR_MIPMAPLEVELMASK_BOTH 0x3

/* Required GrLfbInfo offsets: size, pointer, stride, write mode, and origin. */
#define GR_LFBINFO_BYTES    20
#define GR_LFBINFO_SIZE     0x00
#define GR_LFBINFO_PTR      0x04
#define GR_LFBINFO_STRIDE   0x08
#define GR_LFBINFO_WRITE    0x0c
#define GR_LFBINFO_ORIGIN   0x10

/* ---- the enumerants we actually read ------------------------------------ */
#define GR_LFB_READ_ONLY    0x00
#define GR_LFB_WRITE_ONLY   0x01

#define GR_BUFFER_FRONTBUFFER 0x0
#define GR_BUFFER_BACKBUFFER  0x1

#define GR_ORIGIN_UPPER_LEFT  0x0
#define GR_ORIGIN_LOWER_LEFT  0x1

/* grTexDownloadTable's table type, and the byte count each one implies.
 * NCC tables are twelve 32-bit words; a palette is 256 of them. */
#define GR_TEXTABLE_NCC0      0x0
#define GR_TEXTABLE_NCC1      0x1
#define GR_TEXTABLE_PALETTE   0x2
#define GR_NCCTABLE_BYTES     (12 * 4)
#define GR_PALETTE_BYTES      (256 * 4)

/* The fog table is one byte per entry over the 64 W ranges the hardware
 * divides depth into (Glide Programming Guide, GR_FOG_TABLE_SIZE). */
#define GR_FOG_TABLE_BYTES    64

/* grSstVidMode receives a 72-byte caller-owned cabinet timing record. Captures
 * retain it for future native monitor output. */
#define GR_VIDTIMING_BYTES    72

/* grSstWinOpen's resolution enum, only as far as the surface dimensions we
 * need to size an LFB region. Indexed by the enum value; the host keeps the
 * same table in src/glide_bind.c for the window it opens. */
typedef struct { int width, height; } vgl_res_t;
extern const vgl_res_t vgl_resolutions[];
extern const unsigned  vgl_resolution_count;

#endif /* VCGLIDE_GLIDE2_H */
