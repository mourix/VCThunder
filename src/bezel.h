/* bezel.h -- bezel art: a PNG with a transparent screen hole.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The art is decoded once, premultiplied, and handed to the provider with the
 * rectangle of its screen hole (vglSetBezel). Nothing here knows a window or a
 * title, so tests/bezel_test.c drives it with no game. */
#ifndef VCT_BEZEL_H
#define VCT_BEZEL_H

#include <stddef.h>
#include <stdint.h>

#define BEZEL_MAX_SIDE 8192

typedef struct {
    uint8_t *rgba;              /* w*h*4 bytes, R G B A, premultiplied, top-down */
    int w, h;
    int hole_x, hole_y, hole_w, hole_h;     /* in art pixels */
} bezel_t;

/* Decode a PNG into straight (not premultiplied) RGBA8. Every colour type and
 * bit depth of the PNG specification, PLTE and tRNS; interlaced files are
 * refused. Returns 1, or 0 with the reason in `why`. *out is malloc'd. */
int bezel_png_decode(const uint8_t *data, size_t size, uint8_t **out,
                     int *w, int *h, char *why, size_t why_size);

/* The screen hole: the bounding box of the region of alpha < 128 that holds
 * the art's centre. 0 with a reason when the centre is opaque, the region is
 * too small to be a screen, or it reaches the art's edges so nothing frames
 * it (both sides, or three edges; top and bottom alone is a pillarbox frame). */
int bezel_find_hole(const uint8_t *rgba, int w, int h,
                    int *x, int *y, int *hw, int *hh,
                    char *why, size_t why_size);

/* The window's client for bezel art: the art's shape, sized so its hole is
 * raster_h * scale tall. The one copy of the rule: the host's window
 * (window.c) and the launcher's size list both come from it. */
void bezel_client_size(int art_w, int art_h, int hole_h, int raster_h,
                       int scale, int *w, int *h);

/* Read, decode, find the hole and premultiply. Returns 1, or 0 with a reason. */
int bezel_load(const char *path, bezel_t *out, char *why, size_t why_size);
void bezel_free(bezel_t *b);

#endif /* VCT_BEZEL_H */
