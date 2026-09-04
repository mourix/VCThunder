/* vcglide.h -- internal interface between the seam, the log and the capture.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#ifndef VCGLIDE_INTERNAL_H
#define VCGLIDE_INTERNAL_H

#include <stdint.h>
#include "../api/glide2.h"
#include "capture.h"

/* One index per entry, in entries.def order. */
enum {
#define VGL_ENTRY(name, argbytes, payload, state) VGL_IX_##name,
#include "../api/entries.def"
#undef VGL_ENTRY
    VGL_COUNT
};

/* Does the call change something the GPU backend resolves into a draw state?
 * One column of entries.def rather than a list of its own; see the note there. */
enum { ST_NONE = 0, ST_DRAW = 1 };

typedef struct {
    const char *name;
    uint16_t    argbytes;
    uint8_t     payload;
    uint8_t     state;          /* ST_NONE | ST_DRAW */
} vgl_entry_desc_t;

extern const vgl_entry_desc_t vgl_entries[VGL_COUNT];
extern unsigned  vgl_calls[VGL_COUNT];

/* Resolved configuration, all of it from the environment (see vcglide.c). */
typedef struct {
    char     log[260];
    char     capture[260];
    char     frames[260];       /* dump each frame here as a PNG; "" = off */
    char     title[32];
    char     renderer[16];      /* auto | gpu | cpu */
    char     aspect[16];        /* raster | 4:3 | 16:9 | stretch */
    unsigned capture_frames;
    int      verbose;
    int      present;           /* show what was rendered; a harness says no */
    int      scale;             /* integer window scale, from [graphics] window_scale */
    int      render_scale;      /* internal GPU target scale, 1..8 */
    int      threads;           /* rasteriser workers; 0 = one per core, 1 = off */
    int      raster_width;      /* refreshed from the host at each surface open */
    int      raster_height;
    int      lfb_width;         /* native 2D field width; 0 = no translation */
    int      margin_trace;      /* per-frame side-strip pattern census; 0 = off */
    unsigned fault_device;      /* DIAGNOSTIC: fail the GPU device from this
                                 * frame, to exercise the loss path. 0 = off */
    /* WHICH site the injection fails: "all" (default) or "renderpass".
     *
     * The blanket form fails command-buffer acquisition first, which returns
     * before a render pass is ever begun -- so it cannot exercise the one
     * failure the loss detector is otherwise blind to. A device that
     * refuses ONLY render passes still acquires command buffers and still
     * presents, which is what makes it the dangerous case: frames count up and
     * the picture holds. This selects it. */
    char     fault_site[16];
} vgl_config_t;

extern vgl_config_t vgl_cfg;

/* Set once vgl_init() has resolved the configuration and brought the graphics
 * runtime up. Nothing below may run before it is. */
extern int vgl_ready;

/* Non-zero while a capture is being written. Read on every call, so it is a
 * plain int rather than anything that needs a lock: it is set and cleared on
 * the one thread that makes Glide calls. */
extern int vgl_capturing;

void vgl_init(void);                /* idempotent; exported as vglInit */

/* vglHealth's answers. The host treats anything but OK as a reason to take the
 * orderly shutdown; the value says which fault it is reporting. */
#define VGL_HEALTH_OK           0
#define VGL_HEALTH_DEVICE_LOST  1

/* logging: level 0 = always, 1 = verbose */
void vgl_log(int level, const char *fmt, ...);

/* The same, with a per-site budget for the life of the process. Use it on any
 * path that can fail once per frame, per LFB lock or per texture download: an
 * unbounded line there has written 28.4 GB in one run. The
 * macro owns the counter so a call site cannot forget to make it static. */
void vgl_log_capped(unsigned *seen, unsigned max, const char *fmt, ...);
#define VGL_LOG_CAPPED(max, ...)                                              \
    do {                                                                      \
        static unsigned vgl_log_seen_;                                        \
        vgl_log_capped(&vgl_log_seen_, (max), __VA_ARGS__);                   \
    } while (0)

/* The window's name: `VCThunder - Hydro (30.0 fps)`. Both backends and the host
 * compose it from here so there is one spelling; fps <= 0 leaves it off. */
void vgl_window_title(char *buf, size_t n, double fps);

/* The capture side. vgl_cap_call() is handed the argument words exactly as
 * they arrived. The two LFB hooks bracket a lock: the region a caller writes
 * only exists between the lock and the unlock, so it is read at the unlock
 * and nowhere else. */
void vgl_cap_open(void);
void vgl_cap_close(const char *why);
void vgl_cap_call(unsigned ix, const uint32_t *args);
void vgl_cap_frame(unsigned frame);
void vgl_cap_lfb_locked(const uint32_t *lock_args, int lock_result);
void vgl_cap_lfb_pending(void);

/* One PNG of the frame just finished, read back out of our own framebuffer.
 * See vcglide.c. */
void vgl_dump_frame(unsigned frame);

/* The surface grSstWinOpen actually produced, which is what sizes an LFB
 * region. 0 until the window is open. */
extern int vgl_surface_w, vgl_surface_h;

/* Resolve the presentation destination from the one aspect setting shared by
 * the CPU and GPU presenters. Windowed clients are already the requested
 * shape; only borderless fullscreen needs an explicit fit. */
void vgl_destination_rect(int fullscreen, int raster_w, int raster_h,
                          int client_w, int client_h,
                          int *x, int *y, int *w, int *h);
void vgl_refresh_geometry(int surface_w, int surface_h);

#endif /* VCGLIDE_INTERNAL_H */
