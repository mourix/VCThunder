/* png.h -- shared RGB565 PNG output for the provider and replay harness.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#ifndef VCGLIDE_PNG_H
#define VCGLIDE_PNG_H

#include <stdint.h>

/* `stride_px` is the source pitch in PIXELS, which is not always `w`.
 * Returns 0 on any failure. */
int png_write565(const char *path, const uint16_t *px, unsigned w, unsigned h,
                 unsigned stride_px);

#endif /* VCGLIDE_PNG_H */
