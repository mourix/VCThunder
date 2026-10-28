/* glide_int.h -- the seam between glide_bind.c and its two subsystems.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * glide_bind.c binds the Glide entry points and owns the frame-pacing, stall and
 * call-mix reporting; widescreen.c (the Hor+ engine) and watchdog.c (the hang
 * watchdog and its stack walk) reach it only through here. The reporting stays
 * with the binding because it reads the binding's own counters and timers:
 * splitting it out would buy a dozen accessors over shared state, a WIDER seam
 * than the file boundary it replaced. Private to the three; src/vcthunder.h
 * carries the public interface. */
#ifndef VCT_GLIDE_INT_H
#define VCT_GLIDE_INT_H

#include "vcthunder.h"

/* grLfbLock's GrLock_t, low nibble. The published value; both the binding
 * and the widescreen engine's LFB repair test it. */
#define GR_LFB_READ_ONLY  0x00

/* Read an optional game global without dereferencing an absent anchor: `0` is a
 * real answer from the generator and can never be followed. */
static inline int rd_opt(uint32_t va, int absent)
{
    return va ? *(const int *)(uintptr_t)va : absent;
}

/* ---- what widescreen.c needs from the binding --------------------------- */

/* The VISIBLE raster width the surface was opened with: not the Glide
 * surface, which may be wider. Its reports name it and its origin is computed
 * against it. */
int  glide_view_width(void);
/* Per-entry call counts through the forwarding thunks, and a provider export. */
const uint32_t *glide_call_counts(void);
void *glide_provider_proc(const char *decorated);
/* Have the provider black both side strips at the next swap, even where the
 * frame drew into them. A no-op on a provider without vglBlackMargins. */
void glide_black_margins(void);

/* ---- the geometry, for a title that opens its own surface (Arctic) ------ */

/* The nearest integer 16:9 width for a raster height. */
int  glide_widescreen_width(int height);
/* The smallest legal surface enum containing a raster, else `requested`, and
 * that enum's size. */
int  glide_surface_for_raster(int requested, int raster_w, int raster_h,
                              int *surface_w, int *surface_h);
/* Hand the provider the visible raster and the native field centred in it,
 * before the surface is opened. */
void glide_publish_field(int raster_w, int raster_h, int native_w);

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
/* The engine's scope state: what each hook's leave step puts back. A draw
 * abandoned by a longjmp (drawreplay.c) skips those steps, so it saves this
 * before the draw and restores it after. */
typedef struct {
    float dx;
    int ortho_depth, lensflare_depth, fade_depth;
    int hud_text_depth, hud_text_drawing, hud_text_pending;
    int hud_pending, hud_bias;
    uint32_t hud_key, hud_ra;
    const char *hud_name;
    const game_hud_t *hud_overlay;
} ws_scopes_t;
void ws_scopes_save(ws_scopes_t *s);
void ws_scopes_restore(const ws_scopes_t *s);
/* The byte offset a READ_ONLY LFB lock should be displaced by, or 0.
 *
 * Hydro projects a lens flare in the game's native field and the primitive seam
 * then moves it by the origin; its occlusion test is a READ_ONLY lock sampling
 * that projected x, so this one scoped caller is shown the displayed pixel.
 * Every other read lock and grLfbReadRegion stays raw, capture and
 * conformance coordinates name the provider surface. `stride` bounds the
 * displacement. Reports itself once. */
unsigned ws_lfb_read_offset(int lock_type, unsigned stride);

/* ---- watchdog.c --------------------------------------------------------- */

/* The heartbeat, from the buffer swap, before the provider is called. Also
 * what identifies the render thread on the first swap. */
void wd_note_swap(uint64_t swap_qpc);

#endif /* VCT_GLIDE_INT_H */
