/* glide_int.h -- the seam between glide_bind.c and its two subsystems.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Three files, one subject. glide_bind.c binds the Glide entry points and owns
 * the frame-pacing, stall and call-mix reporting; two subsystems that are
 * genuinely separable from it sit beside it and reach it only through here:
 *
 *   widescreen.c   the Hor+ engine; the seam origin, the widened frustum,
 *                  the six title-scoped repair hooks, the primitive shift and
 *                  the margin census. It needs one number from the binding
 *                  (the visible raster width) and the binding needs four
 *                  answers from it, all below.
 *   watchdog.c     the hang watchdog and its stack walk. Self-contained: it
 *                  owns a thread and reads one timestamp.
 *
 * THE REPORTING STAYS WITH THE BINDING, and a fourth file for it would be a
 * mistake. The frame-timing, stall and Glide call-mix reports exist to report
 * the BINDING'S OWN per-entry-point counters and timers: glide_bind() hands
 * their addresses to the thunks it builds, and the reports read the same
 * arrays. Separating them buys an interface of a dozen accessors over shared
 * arrays, which is a WIDER seam than the file boundary it would replace and no
 * clearer. A subsystem earns its own file by being separable, not by being
 * large.
 *
 * Not a public interface: src/vcthunder.h carries those. This is private to
 * the three files, which is why it is not in it.
 */
#ifndef VCT_GLIDE_INT_H
#define VCT_GLIDE_INT_H

#include "vcthunder.h"

/* grLfbLock's GrLock_t, low nibble. The published value; both the binding
 * and the widescreen engine's LFB repair test it. */
#define GR_LFB_READ_ONLY  0x00

/* Read an optional game global without dereferencing an absent profile anchor.
 * `0` is a real answer from the generator and means the title does not have
 * this one, so it can never be followed. Both files read profile globals. */
static inline int rd_opt(uint32_t va, int absent)
{
    return va ? *(const int *)(uintptr_t)va : absent;
}

/* ---- what widescreen.c needs from the binding --------------------------- */

/* The VISIBLE raster width the surface was opened with: not the Glide
 * surface, which may be wider. Its reports name it and its origin is computed
 * against it. */
int  glide_view_width(void);

/* ---- what the binding needs from widescreen.c --------------------------- */

/* Decide whether the mode allows Hor+ at all and install the title-scoped
 * repairs. Call before binding the primitives: ws_claim_primitive() only
 * claims once this has armed. */
void ws_arm(void);
/* Recompute the seam origin for a newly opened surface. `native_width` is the
 * raster the game would have had, `raster_width` the one it is getting; a
 * raster no wider than the native one leaves the origin inert. */
void ws_settle(int native_width, int raster_width);
/* Offer one Glide entry point to the widescreen engine. Returns nonzero when
 * it has taken it over: the same shape as texstate_claim(). */
int  ws_claim_primitive(const char *name, void *real, uint32_t va);
/* Per swap: retire the margin census window. */
void ws_frame_boundary(void);

/* The seam origin in pixels, 0 when Hor+ is off or inert. */
float ws_origin(void);
/* The byte offset a READ_ONLY LFB lock should be displaced by, or 0.
 *
 * Hydro projects a lens flare in the game's native field and then the
 * primitive seam moves it by the origin; its occlusion test is a READ_ONLY
 * lock that samples that projected x directly, so this one scoped caller is
 * shown the displayed pixel. Every other read lock, and grLfbReadRegion, stays
 * raw: capture and conformance coordinates deliberately name the provider
 * surface. `stride` bounds the displacement; the answer is 0 if it would not
 * fit. Reports itself once, the first time it is non-zero. */
unsigned ws_lfb_read_offset(int lock_type, unsigned stride);

/* ---- watchdog.c --------------------------------------------------------- */

/* The heartbeat, from the buffer swap, before the provider is called. Also
 * what identifies the render thread on the first swap. */
void wd_note_swap(uint64_t swap_qpc);

#endif /* VCT_GLIDE_INT_H */
