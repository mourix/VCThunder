/* layout.h -- where the bezel art and the picture land in the client.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Pure arithmetic, no state, so tests/bezel_test.c links it on its own. */
#ifndef VCGLIDE_LAYOUT_H
#define VCGLIDE_LAYOUT_H

typedef struct {
    int x, y, w, h;
} vgl_rect_t;

/* The art is fitted into the client keeping its own shape (`canvas`), and the
 * picture is placed in the art's hole keeping ITS shape (pic_w:pic_h):
 *   cover != 0  the picture grows until it covers the hole on both axes, and
 *               what passes the hole lies under the art's rim;
 *   cover == 0  the picture spans the hole's width, centred on its height.
 * The art's own pixels are never stretched apart from each other. */
void vgl_bezel_layout(int client_w, int client_h,
                      int art_w, int art_h,
                      int hole_x, int hole_y, int hole_w, int hole_h,
                      int pic_w, int pic_h, int cover,
                      vgl_rect_t *canvas, vgl_rect_t *picture);

/* The picture fitted into the client keeping pic_w:pic_h, centred: where it
 * goes when bezel art is set but cannot be drawn, so the window has the art's
 * shape and nothing frames the picture. */
void vgl_fit_layout(int client_w, int client_h, int pic_w, int pic_h,
                    vgl_rect_t *picture);

#endif /* VCGLIDE_LAYOUT_H */
