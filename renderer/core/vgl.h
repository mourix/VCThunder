/* vgl.h -- portable C99 rasteriser state shared by the core implementation.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Platform presentation and threading are isolated behind vgl_present_* and
 * vgl_par_*. Unsupported state combinations are accepted and reported on first
 * use.
 */
#ifndef VCGLIDE_VGL_H
#define VCGLIDE_VGL_H

#include <stdint.h>

/* ---- Glide enumerants --------------------------------------------------- */
enum {                                  /* grDepthBufferMode */
    VGL_DEPTH_DISABLE = 0, VGL_DEPTH_Z = 1, VGL_DEPTH_W = 2,
    VGL_DEPTH_Z_BIAS = 3, VGL_DEPTH_W_BIAS = 4
};

enum {                                  /* grDepthBufferFunction, grAlphaTestFunction */
    VGL_CMP_NEVER = 0, VGL_CMP_LESS, VGL_CMP_EQUAL, VGL_CMP_LEQUAL,
    VGL_CMP_GREATER, VGL_CMP_NOTEQUAL, VGL_CMP_GEQUAL, VGL_CMP_ALWAYS
};

enum {                                  /* grAlphaBlendFunction */
    VGL_BLEND_ZERO = 0, VGL_BLEND_SRC_ALPHA, VGL_BLEND_SRC_COLOR,
    VGL_BLEND_DST_ALPHA, VGL_BLEND_ONE, VGL_BLEND_ONE_MINUS_SRC_ALPHA,
    VGL_BLEND_ONE_MINUS_SRC_COLOR, VGL_BLEND_ONE_MINUS_DST_ALPHA,
    VGL_BLEND_DST_COLOR = 8, VGL_BLEND_ONE_MINUS_DST_COLOR = 9
};

enum { VGL_DITHER_DISABLE = 0, VGL_DITHER_2x2 = 1, VGL_DITHER_4x4 = 2 };

/* The combine FUNCTION, shared by the colour, alpha and texture units. Each
 * one is `factor * A + B` for a choice of A and B; the table in raster.c is
 * written that way rather than as ten special cases. */
enum {
    VGL_CF_ZERO = 0,
    VGL_CF_LOCAL = 1,
    VGL_CF_LOCAL_ALPHA = 2,
    VGL_CF_SCALE_OTHER = 3,
    VGL_CF_SCALE_OTHER_ADD_LOCAL = 4,
    VGL_CF_SCALE_OTHER_ADD_LOCAL_ALPHA = 5,
    VGL_CF_SCALE_OTHER_MINUS_LOCAL = 6,
    VGL_CF_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL = 7,
    VGL_CF_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA = 8,
    VGL_CF_SCALE_MINUS_LOCAL_ADD_LOCAL = 9,
    VGL_CF_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA = 10
};

/* Combine-factor values 0x8..0xd invert 0x0..0x5. Values 0x4 and 0x5 are
 * interpreted by their colour, alpha, or texture combine unit. */
enum {
    VGL_FAC_ZERO = 0, VGL_FAC_LOCAL = 1, VGL_FAC_OTHER_ALPHA = 2,
    VGL_FAC_LOCAL_ALPHA = 3, VGL_FAC_TEXTURE_ALPHA = 4, VGL_FAC_TEXTURE_RGB = 5,
    VGL_FAC_DETAIL = 4, VGL_FAC_LOD_FRACTION = 5,
    VGL_FAC_ONE_MINUS = 8
};

enum { VGL_LOCAL_ITERATED = 0, VGL_LOCAL_CONSTANT = 1, VGL_LOCAL_DEPTH = 2 };
enum { VGL_OTHER_ITERATED = 0, VGL_OTHER_TEXTURE = 1, VGL_OTHER_CONSTANT = 2,
       VGL_OTHER_NONE = 3 };

enum { VGL_MIPMAP_DISABLE = 0, VGL_MIPMAP_NEAREST = 1,
       VGL_MIPMAP_NEAREST_DITHER = 2 };
enum { VGL_TEXFILTER_POINT = 0, VGL_TEXFILTER_BILINEAR = 1 };
enum { VGL_TEXCLAMP_WRAP = 0, VGL_TEXCLAMP_CLAMP = 1 };

enum {                                  /* GrTexInfo.format */
    VGL_TEXFMT_RGB_332 = 0, VGL_TEXFMT_YIQ_422, VGL_TEXFMT_ALPHA_8,
    VGL_TEXFMT_INTENSITY_8, VGL_TEXFMT_ALPHA_INTENSITY_44, VGL_TEXFMT_P_8,
    VGL_TEXFMT_RSVD0, VGL_TEXFMT_RSVD1,
    VGL_TEXFMT_ARGB_8332 = 8, VGL_TEXFMT_AYIQ_8422, VGL_TEXFMT_RGB_565,
    VGL_TEXFMT_ARGB_1555, VGL_TEXFMT_ARGB_4444, VGL_TEXFMT_ALPHA_INTENSITY_88,
    VGL_TEXFMT_AP_88
};

enum {                                  /* grLfbLock's writeMode */
    VGL_LFBWRITE_565 = 0, VGL_LFBWRITE_555 = 1, VGL_LFBWRITE_1555 = 2,
    VGL_LFBWRITE_RSVD = 3, VGL_LFBWRITE_888 = 4, VGL_LFBWRITE_8888 = 5,
    VGL_LFBWRITE_ANY = 0xff
};

#define VGL_BUFFER_FRONT   0
#define VGL_BUFFER_BACK    1
#define VGL_BUFFER_AUX     2
#define VGL_BUFFER_DEPTH   3

#define VGL_TMUS           2
#define VGL_MAX_LOD        8            /* LOD 0 = 256 texels, LOD 8 = 1 */

/* Parallelize triangles taller than the measured handoff threshold. */
#define VGL_PAR_MAX        16
#define VGL_PAR_MIN_ROWS   16

/* ---- texture units ------------------------------------------------------ */

typedef struct {
    /* The chain currently selected by grTexSource, as a decoded description.
     * `base` is a byte offset into this unit's texture memory; `valid` is 0
     * until a grTexSource has named a chain that fits. */
    uint32_t base;
    int      even_odd;                  /* the parity mask this chain holds */
    int      small_lod, large_lod;      /* small_lod is the SMALLEST map */
    int      aspect, format;
    int      valid;
    /* Byte offset of each level from `base`, and its dimensions. Levels
     * outside [large_lod, small_lod] or outside the parity are marked -1. */
    int32_t  lod_off[VGL_MAX_LOD + 1];
    uint16_t lod_w[VGL_MAX_LOD + 1], lod_h[VGL_MAX_LOD + 1];
    /* Which level answers for a real-valued LOD, without searching for it.
     * `lvl_le[f]` is the largest PRESENT level <= f and `lvl_gt[f]` the
     * smallest present level > f, both -1 when there is none. The nearest
     * present level to any lod in [f, f+1) is one of those two, so the scan
     * over the whole chain that select_level would otherwise run per sample
     *: twice per pixel, 1.3 billion pixels: becomes two array reads and a
     * comparison. Filled by vgl_tex_source alongside lod_off. */
    int8_t   lvl_le[VGL_MAX_LOD + 1], lvl_gt[VGL_MAX_LOD + 1];
    int8_t   lvl_first;                 /* sharpest level held, or -1 */
    uint8_t  bpp;                       /* format_bytes(format), 1 or 2 */

    /* A texel decoded by table lookup rather than by arithmetic. Every format
     * either both games use is separable into its two bytes, so one 256-entry
     * table per byte decodes it, and the tables are BUILT BY CALLING the
     * arithmetic decoder, so there is still exactly one implementation of what
     * a texel means. `dec_kind` says which composition rule holds, and it is
     * established by testing all 65,536 byte pairs against that decoder rather
     * than by reading the format table twice (tex.c). */
    uint32_t dec_lo[256], dec_hi[256];
    int      dec_kind;
    int      lut_fmt;                   /* format the tables hold, -1 = none */
    unsigned lut_pal_ver;               /* palette generation they were built from */
    unsigned pal_ver;                   /* bumped by every grTexDownloadTable */
    unsigned mem_ver;                   /* bumped by every texture-memory write */

    /* state */
    int      min_filter, mag_filter;
    int      mipmap_mode, lod_blend;
    int      clamp_s, clamp_t;
    float    lod_bias;
    int      c_func, c_factor, a_func, a_factor, c_invert, a_invert;
    uint32_t palette[256];              /* grTexDownloadTable, ARGB8888 */
    int      palette_valid;

    uint8_t *mem;                       /* this unit's texture memory */
    uint32_t mem_bytes;
    uint32_t min_addr, max_addr;
} vgl_tmu_t;

typedef struct {
    int func, factor, local, other, invert;
} vgl_combine_state_t;

/* Only state a Glide caller can save and later restore. Keeping this distinct
 * from vgl_dev_t prevents grGlideSetState from rolling back surface ownership,
 * per-frame bookkeeping, statistics, or cache generations. */
/* THE PIPELINE STATE, WRITTEN ONCE.
 *
 * Every member below lives in two structs -- vgl_dev_t, where the rasteriser
 * reads it, and vgl_pipeline_state_t, which is what grGlideGetState saves and
 * grGlideSetState puts back -- and is copied between them in both directions.
 * That was four hand-written lists over one set of fields, and the failure mode
 * of a member added to the device and forgotten in the other three is a state
 * the game saves, restores, and quietly does not get back: a wrong picture some
 * frames after the call that caused it, with nothing logged anywhere.
 *
 * So the list is written here, once, and the four expansions are generated from
 * it. Adding a member to the pipeline is now one line and cannot be half done.
 *
 * SCALAR copies by assignment; ARRAY needs a memcpy, which is the only reason
 * the two are distinguished. The order is the order both structs had, so no
 * layout changes. */
#define VGL_PIPELINE_MEMBERS(SCALAR, ARRAY)                                   \
    SCALAR(int,      origin_lower_left)                                       \
    SCALAR(int,      clip_x0)                                                 \
    SCALAR(int,      clip_y0)                                                 \
    SCALAR(int,      clip_x1)                                                 \
    SCALAR(int,      clip_y1)                                                 \
    /* ---- the pixel pipeline ---- */                                        \
    SCALAR(int,      depth_mode)                                              \
    SCALAR(int,      depth_func)                                              \
    SCALAR(int,      depth_mask)                                              \
    SCALAR(int,      depth_bias)                                              \
    SCALAR(int,      alpha_func)                                              \
    SCALAR(int,      alpha_ref)                                               \
    SCALAR(int,      blend_rgb_src)                                           \
    SCALAR(int,      blend_rgb_dst)                                           \
    SCALAR(int,      blend_a_src)                                             \
    SCALAR(int,      blend_a_dst)                                             \
    SCALAR(int,      dither)                                                  \
    SCALAR(int,      color_mask_rgb)                                          \
    SCALAR(int,      color_mask_a)                                            \
    SCALAR(int,      chroma_mode)                                             \
    SCALAR(uint32_t, chroma_value)                                            \
    SCALAR(int,      fog_mode)                                                \
    SCALAR(uint32_t, fog_color)                                               \
    ARRAY (uint8_t,  fog_table, 64)                                           \
    SCALAR(uint32_t, constant_color)              /* ARGB8888 */              \
    SCALAR(int,      cull_mode)                                               \
    SCALAR(int,      itrgb_lighting)                                          \
    SCALAR(uint32_t, stw_hint)                                                \
    SCALAR(uint32_t, lfb_const_alpha)                                         \
    SCALAR(uint32_t, lfb_const_depth)                                         \
    SCALAR(vgl_combine_state_t, cc)                                           \
    SCALAR(vgl_combine_state_t, ac)                                           \
    ARRAY (vgl_tmu_t, tmu, VGL_TMUS)

#define VGL_PIPE_DECL_SCALAR(type, name)        type name;
#define VGL_PIPE_DECL_ARRAY(type, name, n)      type name[n];

typedef struct {
    VGL_PIPELINE_MEMBERS(VGL_PIPE_DECL_SCALAR, VGL_PIPE_DECL_ARRAY)
} vgl_pipeline_state_t;

/* ---- the device --------------------------------------------------------- */

typedef struct {
    int       open;
    int       w, h;                     /* the SURFACE, 640x400 for both games */
    uint32_t *color[2];                 /* front, back; XRGB8888 in screen order */
    uint16_t *depth;
    int       draw_buffer;              /* which of color[] a draw lands in */
    int       frame_drawn;              /* back buffer touched since last swap */
    /* Track the native field inside a wide raster and whether the current frame
     * painted either side margin. */
    int       field_x0, field_x1;
    int       margin_drawn;
    int       margin_geom;              /* a PRIMITIVE reached a strip, not a clear */
    int       presenting;               /* a platform surface is attached */
    int       gpu;                      /* SDL GPU owns triangle rasterisation */
    /* The state grGlideGetState saves, in the struct the rasteriser reads it
     * from. One list, above; see VGL_PIPELINE_MEMBERS. */
    VGL_PIPELINE_MEMBERS(VGL_PIPE_DECL_SCALAR, VGL_PIPE_DECL_ARRAY)

    /* Monotonic invalidation for the GPU's resolved draw state. Glide games
     * repeat many triangles between state calls; resolving textures, samplers,
     * pipelines and packed uniforms for every one was pure CPU/driver work. */
    uint64_t draw_state_version;

    /* Statistics reported at shutdown. */
    uint64_t  n_tris, n_tris_culled, n_pixels, n_pixels_written, n_spans;
    uint64_t  n_lfb_writes;
    unsigned  frames;
} vgl_dev_t;

extern vgl_dev_t vgl;

/* Conformance-sensitive readings remain switches so the harness can reproduce
 * their A/B evidence. Set from the environment by the seam; the defaults are
 * the measured readings. */
typedef struct {
    int itrgb;      /* ITRGB diagnostic: ignore, documented select, zero gate,
                     * or inverted select. */
    int lodfrac;    /* LOD_FRACTION: 0 = measured plain fractional LOD,
                     * 1 = old unit-local/even-level diagnostic */
    int lodgrad;    /* LOD footprint: 1 = max x/y texture-coordinate
                     * derivative, 0 = old x-only diagnostic */
    int forcelod;   /* 1 = always sample the SHARPEST level a unit holds;
                     * N>1 requests diagnostic LOD N-1. Not a mode: it is the A/B
                     * that separates "the asset has no more detail" from
                     * "we are choosing a blurrier level than we need". */
    int lodclamp;   /* LOD_FRACTION input: 1 = the LOD the chain can serve,
                     * 0 = the raw footprint LOD. Below large_lod a surface is
                     * magnified and the fraction is 0; frac() of an unclamped
                     * negative LOD is a sawtooth, and it cross-fades a blurrier
                     * level in under magnification. */
    int lodf;       /* DIAGNOSTIC, -1 = off. Forces the LOD_FRACTION factor to
                     * a constant, which on the two-TMU trilinear path pins the
                     * cross-fade to one unit: it is the only way to ask "which
                     * of the two halves is carrying the wrong texture?" without
                     * reading a comparator. Off in every shipped path. */
} vgl_opt_t;

extern vgl_opt_t vgl_opt;

/* ---- device.c ----------------------------------------------------------- */

int  vgl_device_open(void *caller_window, int res_enum, int refresh_enum,
                     int origin, int color_buffers, int aux_buffers);
void vgl_device_close(void);
void vgl_device_shutdown(void);       /* close and forget session state */
void vgl_device_reset_state(void);
void vgl_disable_all_effects(void);
void vgl_buffer_clear(uint32_t color, uint32_t alpha, uint32_t depth);
void vgl_buffer_swap(int interval);
int  vgl_lfb_lock(uint32_t type, uint32_t buffer, uint32_t write_mode,
                  uint32_t origin, uint32_t pixel_pipeline, void *info);
int  vgl_lfb_unlock(uint32_t type, uint32_t buffer);
int  vgl_lfb_read_region(uint32_t buffer, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t stride, void *dst);
void vgl_state_get(void *blob);
void vgl_state_set(const void *blob);

/* ---- native.c ----------------------------------------------------------- */

/* The seam's whole interface to the rasteriser: an entry index and its
 * argument words. Returns what the caller will see in eax. */
int  vgl_native_call(unsigned ix, const uint32_t *args);

/* Platform presentation and threading seam. Validate caller_window before
 * treating a grSstWinOpen argument as a native window handle. */
int  vgl_present_open(void *caller_window, int w, int h, int scale,
                      int refresh_hz);
void vgl_present_raster(int x, int y, int w, int h,
                        int valid_x0, int valid_y0,
                        int valid_x1, int valid_y1);
void vgl_present_frame(const uint32_t *buf, int w, int h, int swap_interval);
void vgl_present_pump(void);
void vgl_present_close(void);

/* Optional accelerated raster backend. It consumes the same decoded device
 * state as the CPU reference path and owns only platform/GPU objects. `auto`
 * and `gpu` both try this seam; device.c falls back to the CPU renderer if
 * open fails, keeping the reference path reachable on every machine. */
int  vgl_gpu_preload(void);
/* `raster_w/h` are the VISIBLE raster inside the w*h surface, already resolved
 * by vgl_refresh_geometry() at the surface boundary. Passed in rather than
 * looked up: win32/ owns platform objects, core/ owns configuration, and this
 * backend must not re-read the same two environment variables through a
 * clamping rule of its own: that is one fact with two implementations. */
int  vgl_gpu_open(void *caller_window, int w, int h, int raster_w, int raster_h,
                  int render_scale, int window_scale, int refresh_hz,
                  int present);
void vgl_gpu_close(void);
void vgl_gpu_set_raster(int x, int y, int w, int h,
                        int valid_x0, int valid_y0,
                        int valid_x1, int valid_y1);
void vgl_gpu_clear(uint32_t color, uint32_t depth);
/* The two widescreen side strips only, in the buffer being drawn: the raster
 * is wider than the game's own screen and a frame that painted neither of them
 * would otherwise present whatever that buffer held two frames ago. */
void vgl_gpu_clear_margins(int x0, int x1, int y0, int y1, int field_x0,
                           int field_x1);
void vgl_gpu_draw_triangle(const float *a, const float *b, const float *c);
void vgl_gpu_swap(int interval);
int  vgl_gpu_sync_color(int buffer, uint32_t *dst, int w, int h);
void vgl_gpu_upload_lfb(int buffer, const uint32_t *rgba_masked, int w, int h);
void vgl_gpu_pump(void);
/* Non-zero once the GPU device has been declared gone: a state, not a failed
 * call. The host reads it after every swap through vglHealth (gpu.c). */
int  vgl_gpu_lost(void);

/* A fixed worker pool splits triangles by disjoint scanline rows and blocks
 * until completion. Primitive order and per-pixel access order remain unchanged. */
typedef void (*vgl_par_fn)(void *ctx, int worker, int y0, int y1);

int  vgl_par_open(int want);        /* 0 = one per core; returns the count */
void vgl_par_close(void);
void vgl_par_rows(vgl_par_fn fn, void *ctx, int y0, int y1);
int  vgl_par_workers(void);         /* 1 when threading is off */

/* ---- tex.c -------------------------------------------------------------- */

/* Distinguish texture-memory occupancy from caller-buffer extent. */
uint32_t vgl_tex_mem_required(int even_odd, const void *info);
uint32_t vgl_tex_chain_extent(const void *info);
uint32_t vgl_tex_level_bytes(int lod, int aspect, int format);

void vgl_tex_download(int tmu, uint32_t start, int even_odd, const void *info);
void vgl_tex_download_partial(int tmu, uint32_t start, int this_lod,
                              int large_lod, int aspect, int format,
                              int even_odd, const void *data, int from, int to);
void vgl_tex_source(int tmu, uint32_t start, int even_odd, const void *info);
void vgl_tex_table(int tmu, int type, const void *data);
void vgl_tex_init(void);
void vgl_tex_free(void);
int  vgl_tex_export_rgba(int tmu, int lod, uint8_t *dst, unsigned pitch);
/* Return the selected chain's mip levels whose texture-memory byte ranges
 * were written after `since_ver`. Bit N represents absolute Glide LOD N. */
uint32_t vgl_tex_dirty_mips(int tmu, unsigned since_ver);

/* Sample unit `t` at texture coordinates (s,tc) in texels of its LARGEST map,
 * at the given real-valued LOD. Returns ARGB8888 and, in *lod_frac, the
 * distance from the level this unit actually used to `lod`. The measured
 * default LOD_FRACTION is global frac(lod); this unit-local value remains for
 * option 1's reproducible diagnostic. */
/* Accumulate pixel counters per worker and reduce them after joining to avoid
 * races and hot-loop atomic updates. */
typedef struct {
    uint64_t pixels, written, spans;
    uint64_t lod[VGL_MAX_LOD + 3];      /* what the rasteriser asked for */
    uint64_t level[VGL_MAX_LOD + 1];    /* what the unit answered with */
} vgl_stats_t;

extern uint64_t vgl_split_parity[2];    /* split grTexSource by large_lod parity */

uint32_t vgl_tex_sample(int t, float s, float tc, float lod, int *lod_frac,
                        vgl_stats_t *st);

/* Bucketed by integer LOD: what the rasteriser asked for, and what level a
 * unit could answer with. Bucket 0 of the first holds everything at or below
 * LOD -1, so the two are offset by one. */
extern uint64_t vgl_lod_hist[VGL_MAX_LOD + 3];
extern uint64_t vgl_level_hist[VGL_MAX_LOD + 1];

/* ---- raster.c ----------------------------------------------------------- */

/* Fold the workers' counters into the device. Called where the counters are
 * read (the buffer swap and the shutdown report), and nowhere else: they
 * are per worker precisely so that the pixel loop never touches a shared one. */
void vgl_raster_collect(void);

void vgl_draw_triangle(const float *a, const float *b, const float *c);
void vgl_draw_vertex_list(int planar, int count, const float *verts);
void vgl_draw_line(const float *a, const float *b);

/* One 8-bit multiply, rounded, used everywhere in the combine units. */
static inline int vgl_mul8(int a, int b)
{
    int p = a * b;
    return (p + 128 + ((p + 128) >> 8)) >> 8;   /* == round(a*b/255) */
}

static inline int vgl_clamp8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* Narrow an 8-bit channel to max-valued code space with a 0..254 quantisation
 * offset. This is the inverse of the renderer's full-range code expansion. */
static inline int vgl_narrow8(int v, int max, int dith)
{
    if (v <= 0)   return 0;
    if (v >= 255) return max;
    /* Exact division by 255 for the full 16-bit numerator domain. */
    return (int)(((unsigned)(v * max + dith) * 32897u) >> 23);
}

/* floor, without libm; see the x87 note in raster.c and the Makefile. */
static inline int vgl_ifloor(float x)
{
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}

#endif /* VCGLIDE_VGL_H */
