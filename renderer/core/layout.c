/* layout.c -- where the bezel art and the picture land in the client.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#include "layout.h"

#include <math.h>
#include <stdint.h>

void vgl_fit_layout(int client_w, int client_h, int pic_w, int pic_h,
                    vgl_rect_t *picture)
{
    picture->x = picture->y = 0;
    picture->w = client_w;
    picture->h = client_h;
    if (client_w <= 0 || client_h <= 0 || pic_w <= 0 || pic_h <= 0)
        return;
    if ((int64_t)client_w * pic_h > (int64_t)client_h * pic_w)
        picture->w = (int)((int64_t)client_h * pic_w / pic_h);
    else
        picture->h = (int)((int64_t)client_w * pic_h / pic_w);
    picture->x = (client_w - picture->w) / 2;
    picture->y = (client_h - picture->h) / 2;
}

void vgl_bezel_layout(int client_w, int client_h,
                      int art_w, int art_h,
                      int hole_x, int hole_y, int hole_w, int hole_h,
                      int pic_w, int pic_h, int cover,
                      vgl_rect_t *canvas, vgl_rect_t *picture)
{
    double s, sx, sy, hx0, hy0, hx1, hy1, cx, cy, aspect, pw, ph;

    canvas->x = canvas->y = 0;
    canvas->w = client_w;
    canvas->h = client_h;
    *picture = *canvas;
    if (client_w <= 0 || client_h <= 0 || art_w <= 0 || art_h <= 0 ||
        hole_w <= 0 || hole_h <= 0 || pic_w <= 0 || pic_h <= 0)
        return;

    s = (double)client_w / art_w;
    if ((double)client_h / art_h < s)
        s = (double)client_h / art_h;
    canvas->w = (int)floor(art_w * s + 0.5);
    canvas->h = (int)floor(art_h * s + 0.5);
    if (canvas->w > client_w) canvas->w = client_w;
    if (canvas->h > client_h) canvas->h = client_h;
    canvas->x = (client_w - canvas->w) / 2;
    canvas->y = (client_h - canvas->h) / 2;

    /* The hole where the drawn art puts it, so through the canvas's own
     * rounded scale on each axis. */
    sx = (double)canvas->w / art_w;
    sy = (double)canvas->h / art_h;
    hx0 = canvas->x + hole_x * sx;
    hx1 = canvas->x + (hole_x + hole_w) * sx;
    hy0 = canvas->y + hole_y * sy;
    hy1 = canvas->y + (hole_y + hole_h) * sy;
    cx = (hx0 + hx1) * 0.5;
    cy = (hy0 + hy1) * 0.5;

    aspect = (double)pic_w / pic_h;
    pw = hx1 - hx0;
    ph = pw / aspect;
    if (cover && ph < hy1 - hy0) {
        ph = hy1 - hy0;
        pw = ph * aspect;
    }
    if (cover) {
        /* Outward on every edge, so no hole pixel is left uncovered. */
        int x0 = (int)floor(cx - pw * 0.5), x1 = (int)ceil(cx + pw * 0.5);
        int y0 = (int)floor(cy - ph * 0.5), y1 = (int)ceil(cy + ph * 0.5);
        picture->x = x0; picture->w = x1 - x0;
        picture->y = y0; picture->h = y1 - y0;
    } else {
        int x0 = (int)floor(hx0 + 0.5), x1 = (int)floor(hx1 + 0.5);
        int y0 = (int)floor(cy - ph * 0.5 + 0.5);
        int y1 = (int)floor(cy + ph * 0.5 + 0.5);
        picture->x = x0; picture->w = x1 - x0;
        picture->y = y0; picture->h = y1 - y0;
    }
}
