/* gpu_int.h -- the GPU backend's internals, shared by its three files.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * gpu.c is the device, the frame and the draw path; gpu_tex.c the texture
 * cache; gpu_layer.c the 2D layer above 1x and its runs. Each owns its own
 * state. What is here is the frame state all three read and the calls between
 * them. Nothing outside renderer/win32 includes this.
 */
#ifndef VGL_GPU_INT_H
#define VGL_GPU_INT_H

#include <windows.h>
#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdint.h>

#include "../core/vgl.h"
#include "../core/vcglide.h"

/* Project-owned batching capacity; see the matching measured rationale in
 * gpu_shader.hlsl. Each triangle contributes twelve float4 vertex vectors. */
#define GPU_BATCH_TRIANGLES 64
#define GPU_BATCH_VECTORS   (GPU_BATCH_TRIANGLES * 12)
/* Batches of one 2D run held until they are drawn into the run's two
 * targets; a full list is drawn and emptied, so this is a capacity only. */
#define GPU_RUN_BATCHES     32
#define GPU_HITCH_LOG_MS     25.0

typedef union {
    uint32_t u[20][4];
    float f[20][4];
} ps_data_t;

/* A 2D batch (`is2d`) carries a second copy of its triangles at native
 * resolution, `vertex1`, sampled through `bind1`: the game's own address
 * modes, because the 2D layer is drawn exactly as 1x draws it.
 * The vertex arrays come last: a batch is copied as its header plus the
 * vectors its triangles use (batch_copy). `ps_gen` is the draw state's
 * generation that `ps` was last found equal to. */
typedef struct {
    ps_data_t ps;
    uint64_t ps_gen;
    SDL_GPUTextureSamplerBinding bind[2], bind1[2];
    SDL_GPUGraphicsPipeline *pipe;
    unsigned triangles;
    int is2d;
    int buffer, clip_x0, clip_y0, clip_x1, clip_y1;
    float vertex[GPU_BATCH_VECTORS][4];
    float vertex1[GPU_BATCH_VECTORS][4];
} draw_batch_t;

/* DXBC that Microsoft's compiler made at build time (gpu_shader_dxbc.h). It is
 * never compiled here: under Wine and Proton d3dcompiler_47 is another HLSL
 * compiler, and it miscompiles this shader. */
#define DXBC(entry) vgl_dxbc_##entry, sizeof vgl_dxbc_##entry

/* The SDL failures the lost-device detector counts (gpu.c). */
enum {
    GF_COMMAND, GF_SUBMIT, GF_FENCE, GF_SWAPCHAIN, GF_TEXTURE, GF_UPLOAD,
    GF_PIPELINE, GF_RENDERPASS, GF_COPYPASS, GF_SAMPLER, GF_COUNT
};

/* ---- the frame: gpu.c ---------------------------------------------------- */

extern int s_lost;                      /* the device is gone, latched      */
extern SDL_GPUCommandBuffer *s_cmd;     /* the frame's, or NULL             */
extern SDL_GPUTexture *s_white;         /* a 1x1 white texture              */
extern SDL_GPUTexture *s_color[2], *s_depth;
extern int s_w, s_h, s_scale, s_iw, s_ih;   /* native, scale, internal     */
extern int s_rx, s_ry, s_rw, s_rh;          /* the raster rectangle        */
extern int s_vx0, s_vy0, s_vx1, s_vy1;      /* its drawn part              */
extern int s_surface_pushed;
extern uint64_t s_draw_batches, s_batched_triangles;
extern uint64_t s_frame_index;

void gpu_failed(int site);
int  gpu_fault_injected_at(int site);
double elapsed_ms(LARGE_INTEGER begin, LARGE_INTEGER end);
/* Every SDL call that takes the device takes it from here: it settles the
 * previous submission first, so nothing is recorded while the GPU still
 * executes the last one. */
SDL_GPUDevice *device(void);
SDL_GPUCommandBuffer *command(void);
void end_pass(void);
void flush_batch(void);
SDL_GPUTexture *make_texture(SDL_GPUTextureFormat format,
                             SDL_GPUTextureUsageFlags usage,
                             unsigned w, unsigned h, unsigned levels);
int  queue_texture_mips(SDL_GPUTexture *texture, int tmu, uint32_t dirty_mips);
SDL_GPUShader *make_shader(const unsigned char *code, size_t size,
                           SDL_GPUShaderStage stage,
                           unsigned samplers, unsigned uniforms);
SDL_GPUGraphicsPipeline *layer_pipeline(SDL_GPUShader *ps, int depth,
                                        int over, int mask);
void blit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src,
          int sx, int sy, int sw, int sh, SDL_GPUTexture *dst,
          int dx, int dy, int dw, int dh, SDL_GPULoadOp load,
          SDL_GPUFilter filter);

/* ---- the texture cache: gpu_tex.c ---------------------------------------- */

extern uint64_t s_tex_lookups, s_tex_probes;

SDL_GPUSampler *sampler_for(const vgl_tmu_t *u, int no_tile);
SDL_GPUTexture *texture_for(int tmu);
/* Every cached texture released and the cache emptied. */
void gpu_tex_release(void);

/* ---- the 2D layer: gpu_layer.c ------------------------------------------- */

/* Its shaders and pipelines, once per device; 0 has logged why. */
int  layer_preload(SDL_GPUShader *overlay_ps);
/* [graphics] upscale_2d, read at each open. */
void layer_configure(void);
/* Its targets, made above 1x only, and released with the frame's. */
int  layer_create_targets(void);
void layer_release_targets(void);
/* At an open: both layers empty and stale, and no run open. */
void layer_forget(void);

/* A 2D batch joins the open run; a 2D triangle opens one on `buffer`. */
void run_add(const draw_batch_t *batch);
void run_join(int buffer);
void run_draw(void);
/* The open run is 2D after all: over its layer. On `buffer` only, or any. */
void run_commit(void);
void run_commit_on(int buffer);
/* A non-2D draw on `buffer`: an open run becomes scene, the layer folds. */
void scene_draws(int buffer);
void layer_clear(int x0, int y0, int x1, int y1, uint32_t depth);

int  layer_used(int buffer);
/* The front and back layers trade places with their colour buffers. */
void layer_swap(void);
void layer_compose(int b, SDL_GPUTexture *dst, int ox, int oy);
/* Buffer b's layer over dst, both at native resolution. */
void layer_over(SDL_GPUTexture *dst, int b);
/* An LFB write above 1x: the written pixels over buffer b's layer. */
void layer_lfb(int b, SDL_GPUTexture *lfb);
/* One quad into dst from up to three sources: also the gamma pass's. */
void layer_quadn(SDL_GPUTexture *dst, SDL_GPULoadOp load,
                 SDL_GPUTexture *depth, SDL_GPUGraphicsPipeline *pipe,
                 SDL_GPUTexture *const *src, unsigned nsrc,
                 const ps_data_t *ps, const float *r);

#endif /* VGL_GPU_INT_H */
