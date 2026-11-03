/* gpu_layer.c -- the GPU backend's 2D layer, above 1x.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The layer, its runs and its magnifiers, and every target and pipeline they
 * use. gpu.c's draw path, swap, display, LFB write and readback reach it
 * through gpu_int.h; at 1x none of it is made and none of it runs.
 */
#include "gpu_int.h"
#include "gpu_shader_dxbc.h"

#include <string.h>

/* Above 1x, 2D is not drawn into the scaled target. Each colour buffer has a
 * native-resolution premultiplied layer (s_layer) holding the 2D on top of
 * its target, and the picture is target + layer magnified to the render
 * scale (s_layer_up, cached until the layer changes). 2D is a flat-w
 * triangle, as both titles draw it, an LFB write, or a clear.
 *
 * Whether a run of 2D is the layer or part of the scene is known only when
 * the run ends: a 3D draw after it makes it scene (Hydro's sky, Offroad's
 * horizon band and billboards). So a run is drawn twice as it goes, at the
 * render scale into s_run_hi with the scene's depth and at 1x into s_run_lo
 * with s_layer_depth, the scaled depth resolved to 1x when the run begins.
 * 3D next: s_run_hi is laid over the target. Anything else ends the run in
 * the layer: s_run_lo is laid over s_layer. Both blend the game's colour
 * and accumulate coverage in alpha, which is exact because over is
 * associative and both titles blend only SRC_ALPHA/ONE_MINUS_SRC_ALPHA (or
 * replace). Before scene drawing lands on a buffer whose layer holds
 * anything, the layer is folded into its target as it was displayed. */
static SDL_GPUTexture *s_layer[2], *s_layer_up[2];
static SDL_GPUTexture *s_native_lo, *s_native_up;   /* composed at 1x, magnified */
static SDL_GPUTexture *s_fsr_easu;                  /* FSR's first pass */
static SDL_GPUTexture *s_run_lo, *s_run_hi, *s_layer_depth;
static SDL_GPUShader *s_over_ps, *s_upscale_ps, *s_clear_layer_ps;
static SDL_GPUShader *s_clear_layer_color_ps, *s_layer_depth_ps;
static SDL_GPUShader *s_compose_ps, *s_catmull_ps, *s_easu_ps, *s_rcas_ps;
static SDL_GPUGraphicsPipeline *s_over_pipe, *s_upscale_pipe;
static SDL_GPUGraphicsPipeline *s_clear_layer_pipe, *s_clear_layer_color_pipe;
static SDL_GPUGraphicsPipeline *s_layer_depth_pipe, *s_lfb_layer_pipe;
static SDL_GPUGraphicsPipeline *s_compose_pipe, *s_catmull_pipe;
static SDL_GPUGraphicsPipeline *s_easu_pipe, *s_rcas_pipe;
enum { MAGNIFY_BILINEAR, MAGNIFY_CATMULL_ROM, MAGNIFY_FSR1 };
static int s_magnifier;         /* [graphics] upscale_2d */
static int s_layer_used[2];     /* the layer holds something */
static int s_layer_stale[2];    /* s_layer_up does not show it yet */
static int s_run;               /* a 2D run is open, on s_run_buffer */
static int s_run_buffer, s_run_ready, s_run_hi_ready;
static unsigned s_run_count;
static draw_batch_t s_run_list[GPU_RUN_BATCHES];

int layer_preload(SDL_GPUShader *overlay_ps)
{
#define PS SDL_GPU_SHADERSTAGE_FRAGMENT
    s_over_ps = make_shader(DXBC(over_ps), PS, 1, 1);
    s_upscale_ps = make_shader(DXBC(upscale_bilinear_ps), PS, 1, 1);
    s_clear_layer_ps = make_shader(DXBC(clear_layer_ps), PS, 0, 1);
    s_clear_layer_color_ps = make_shader(DXBC(clear_layer_color_ps), PS, 0, 1);
    s_layer_depth_ps = make_shader(DXBC(layer_depth_ps), PS, 1, 1);
    s_compose_ps = make_shader(DXBC(compose_ps), PS, 3, 1);
    s_catmull_ps = make_shader(DXBC(upscale_catmull_ps), PS, 1, 1);
    s_easu_ps = make_shader(DXBC(fsr_easu_ps), PS, 1, 1);
    s_rcas_ps = make_shader(DXBC(fsr_rcas_ps), PS, 1, 1);
#undef PS
    if (!s_over_ps || !s_upscale_ps || !s_clear_layer_ps ||
        !s_clear_layer_color_ps || !s_layer_depth_ps || !s_compose_ps ||
        !s_catmull_ps || !s_easu_ps || !s_rcas_ps) {
        vgl_log(0, "gpu: SDL3 shader creation failed: %s", SDL_GetError());
        return 0;
    }
    s_over_pipe = layer_pipeline(s_over_ps, 0, 1, 1);
    s_upscale_pipe = layer_pipeline(s_upscale_ps, 0, 0, 1);
    s_clear_layer_pipe = layer_pipeline(s_clear_layer_ps, 1, 0, 1);
    s_clear_layer_color_pipe = layer_pipeline(s_clear_layer_color_ps, 0, 0, 1);
    s_layer_depth_pipe = layer_pipeline(s_layer_depth_ps, 1, 0, 0);
    s_lfb_layer_pipe = layer_pipeline(overlay_ps, 0, 0, 1);
    s_compose_pipe = layer_pipeline(s_compose_ps, 0, 1, 1);
    s_catmull_pipe = layer_pipeline(s_catmull_ps, 0, 0, 1);
    s_easu_pipe = layer_pipeline(s_easu_ps, 0, 0, 1);
    s_rcas_pipe = layer_pipeline(s_rcas_ps, 0, 0, 1);
    if (!s_over_pipe || !s_upscale_pipe || !s_clear_layer_pipe ||
        !s_clear_layer_color_pipe || !s_layer_depth_pipe || !s_lfb_layer_pipe ||
        !s_compose_pipe || !s_catmull_pipe || !s_easu_pipe || !s_rcas_pipe) {
        vgl_log(0, "gpu: fixed resource creation failed: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

void layer_configure(void)
{
    s_magnifier = !strcmp(vgl_cfg.upscale_2d, "catmull_rom") ? MAGNIFY_CATMULL_ROM
                : !strcmp(vgl_cfg.upscale_2d, "fsr1") ? MAGNIFY_FSR1
                : MAGNIFY_BILINEAR;
}

int layer_create_targets(void)
{
    unsigned usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
    int i;

    for (i = 0; i < 2; i++) {
        s_layer[i] = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                  usage, s_w, s_h, 1);
        s_layer_up[i] = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                     usage, s_iw, s_ih, 1);
        if (!s_layer[i] || !s_layer_up[i])
            return 0;
    }
    s_run_lo = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                            usage, s_w, s_h, 1);
    s_run_hi = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                            usage, s_iw, s_ih, 1);
    s_layer_depth = make_texture(SDL_GPU_TEXTUREFORMAT_D16_UNORM,
                                 SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
                                 s_w, s_h, 1);
    s_native_lo = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               usage, s_w, s_h, 1);
    s_native_up = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               usage, s_iw, s_ih, 1);
    if (!s_run_lo || !s_run_hi || !s_layer_depth || !s_native_lo ||
        !s_native_up)
        return 0;
    if (s_magnifier == MAGNIFY_FSR1) {
        s_fsr_easu = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                  usage, s_iw, s_ih, 1);
        if (!s_fsr_easu)
            return 0;
    }
    return 1;
}

void layer_release_targets(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (s_layer[i]) SDL_ReleaseGPUTexture(device(), s_layer[i]);
        if (s_layer_up[i]) SDL_ReleaseGPUTexture(device(), s_layer_up[i]);
        s_layer[i] = s_layer_up[i] = NULL;
        s_layer_used[i] = 0;
        s_layer_stale[i] = 1;
    }
    if (s_native_lo) SDL_ReleaseGPUTexture(device(), s_native_lo);
    if (s_native_up) SDL_ReleaseGPUTexture(device(), s_native_up);
    if (s_fsr_easu) SDL_ReleaseGPUTexture(device(), s_fsr_easu);
    s_native_lo = s_native_up = s_fsr_easu = NULL;
    if (s_run_lo) SDL_ReleaseGPUTexture(device(), s_run_lo);
    if (s_run_hi) SDL_ReleaseGPUTexture(device(), s_run_hi);
    if (s_layer_depth) SDL_ReleaseGPUTexture(device(), s_layer_depth);
    s_run_lo = s_run_hi = s_layer_depth = NULL;
    s_run = s_run_ready = s_run_hi_ready = 0;
    s_run_count = 0;
}

void layer_forget(void)
{
    memset(s_layer_used, 0, sizeof s_layer_used);
    s_layer_stale[0] = s_layer_stale[1] = 1;
    s_run = s_run_ready = s_run_hi_ready = 0;
    s_run_count = 0;
}

int layer_used(int buffer)
{
    return s_layer_used[buffer];
}

static void batch_copy(draw_batch_t *dst, const draw_batch_t *src)
{
    size_t used = (size_t)src->triangles * 12 * sizeof src->vertex[0];

    memcpy(dst, src, offsetof(draw_batch_t, vertex));
    memcpy(dst->vertex, src->vertex, used);
    if (src->is2d)
        memcpy(dst->vertex1, src->vertex1, used);
}

void run_add(const draw_batch_t *batch)
{
    if (s_run_count == GPU_RUN_BATCHES)
        run_draw();
    batch_copy(&s_run_list[s_run_count++], batch);
}

void run_join(int buffer)
{
    if (s_run && s_run_buffer != buffer)
        run_commit();
    if (!s_run) {
        s_run = 1;
        s_run_buffer = buffer;
    }
}

static SDL_GPUSampler *point_sampler(void)
{
    vgl_tmu_t point;
    memset(&point, 0, sizeof point);
    point.min_filter = VGL_TEXFILTER_POINT;
    point.mag_filter = VGL_TEXFILTER_POINT;
    return sampler_for(&point, 1);
}

/* One quad over `r` (normalised x0, y0, x1, y1; NULL is the whole target)
 * into dst, optionally with a depth target and a source texture read texel
 * for texel. Its own render pass, in the frame's command buffer. */
void layer_quadn(SDL_GPUTexture *dst, SDL_GPULoadOp load,
                 SDL_GPUTexture *depth, SDL_GPUGraphicsPipeline *pipe,
                 SDL_GPUTexture *const *src, unsigned nsrc,
                 const ps_data_t *ps, const float *r)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_GPUTextureSamplerBinding bind[3];
    unsigned k;
    SDL_GPURenderPass *pass;
    float vd[10][4];

    if (s_lost || !dst || !pipe)
        return;
    end_pass();
    if (!command())
        return;
    memset(&ct, 0, sizeof ct);
    ct.texture = dst; ct.load_op = load; ct.store_op = SDL_GPU_STOREOP_STORE;
    memset(&dt, 0, sizeof dt);
    dt.texture = depth;
    dt.load_op = SDL_GPU_LOADOP_LOAD; dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    pass = gpu_fault_injected_at(GF_RENDERPASS)
               ? NULL : SDL_BeginGPURenderPass(s_cmd, &ct, 1,
                                               depth ? &dt : NULL);
    if (!pass) {
        gpu_failed(GF_RENDERPASS);
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipe);
    for (k = 0; k < nsrc && k < 3; k++) {
        bind[k].texture = src[k];
        bind[k].sampler = point_sampler();
    }
    if (nsrc)
        SDL_BindGPUFragmentSamplers(pass, 0, bind, nsrc < 3 ? nsrc : 3);
    memset(vd, 0, sizeof vd);
    vd[0][0] = r ? r[0] : 0.0f; vd[0][1] = r ? r[1] : 0.0f;
    vd[0][2] = r ? r[2] : 1.0f; vd[0][3] = r ? r[3] : 1.0f;
    SDL_PushGPUVertexUniformData(s_cmd, 0, vd, sizeof vd);
    SDL_PushGPUFragmentUniformData(s_cmd, 0, ps, sizeof *ps);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

static void layer_quad(SDL_GPUTexture *dst, SDL_GPULoadOp load,
                       SDL_GPUTexture *depth, SDL_GPUGraphicsPipeline *pipe,
                       SDL_GPUTexture *src, const ps_data_t *ps,
                       const float *r)
{
    layer_quadn(dst, load, depth, pipe, &src, src ? 1u : 0u, ps, r);
}

/* The recorded batches of the open run, into one of its two targets: `hi`
 * at the render scale against the scene's depth, else at 1x against the
 * layer's. */
static void run_draw_pass(int hi)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_GPURenderPass *pass;
    float surface[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int scale = hi ? s_scale : 1;
    unsigned i;

    memset(&ct, 0, sizeof ct);
    ct.texture = hi ? s_run_hi : s_run_lo;
    ct.load_op = hi && !s_run_hi_ready ? SDL_GPU_LOADOP_CLEAR
                                       : SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    memset(&dt, 0, sizeof dt);
    dt.texture = hi ? s_depth : s_layer_depth;
    dt.load_op = SDL_GPU_LOADOP_LOAD; dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    pass = gpu_fault_injected_at(GF_RENDERPASS)
               ? NULL : SDL_BeginGPURenderPass(s_cmd, &ct, 1, &dt);
    if (!pass) {
        gpu_failed(GF_RENDERPASS);
        return;
    }
    if (hi)
        s_run_hi_ready = 1;
    surface[0] = (float)(hi ? s_iw : s_w);
    surface[1] = (float)(hi ? s_ih : s_h);
    SDL_PushGPUVertexUniformData(s_cmd, 1, surface, sizeof surface);
    for (i = 0; i < s_run_count; i++) {
        const draw_batch_t *b = &s_run_list[i];
        SDL_Rect sc;

        sc.x = b->clip_x0 * scale; sc.y = b->clip_y0 * scale;
        sc.w = (b->clip_x1 - b->clip_x0) * scale;
        sc.h = (b->clip_y1 - b->clip_y0) * scale;
        SDL_SetGPUScissor(pass, &sc);
        SDL_BindGPUGraphicsPipeline(pass, b->pipe);
        SDL_BindGPUFragmentSamplers(pass, 0, hi ? b->bind : b->bind1, 2);
        SDL_PushGPUVertexUniformData(
            s_cmd, 0, hi ? b->vertex : b->vertex1,
            (Uint32)((size_t)b->triangles * 12 * sizeof b->vertex[0]));
        SDL_PushGPUFragmentUniformData(s_cmd, 0, &b->ps, sizeof b->ps);
        SDL_DrawGPUPrimitives(pass, b->triangles * 3, 1, 0, 0);
        if (hi) {
            s_draw_batches++;
            s_batched_triangles += b->triangles;
        }
    }
    SDL_EndGPURenderPass(pass);
}

/* Draw what the open run has recorded. The run's first draw clears its 1x
 * target and takes its depth from the scene's, as 1x would have it. */
void run_draw(void)
{
    ps_data_t ps;

    if (!s_run_count)
        return;
    end_pass();
    if (s_lost || !command()) {
        s_run_count = 0;
        return;
    }
    if (!s_run_ready) {
        memset(&ps, 0, sizeof ps);
        ps.u[0][0] = (unsigned)s_scale;
        layer_quad(s_run_lo, SDL_GPU_LOADOP_CLEAR, s_layer_depth,
                   s_layer_depth_pipe, s_depth, &ps, NULL);
        s_run_ready = 1;
        s_run_hi_ready = 0;
    }
    run_draw_pass(1);
    run_draw_pass(0);
    s_run_count = 0;
    s_surface_pushed = 0;
}

static void run_reset(void)
{
    s_run = s_run_ready = s_run_hi_ready = 0;
    s_run_count = 0;
}

/* A native picture magnified to the render scale: the one replaceable
 * pass, from the native grid to the scaled one, premultiplied in and out.
 * `opaque`: src's alpha is coverage, not part of the picture. */
static void magnify(SDL_GPUTexture *src, SDL_GPUTexture *dst, int opaque)
{
    ps_data_t ps;
    int x0 = s_vx0, y0 = s_vy0, x1 = s_vx1, y1 = s_vy1;

    if (x0 >= x1 || y0 >= y1) {
        x0 = y0 = 0; x1 = s_rw; y1 = s_rh;
    }
    memset(&ps, 0, sizeof ps);
    ps.u[0][0] = (unsigned)s_scale;
    ps.u[1][0] = (unsigned)(s_rx + x0); ps.u[1][1] = (unsigned)(s_ry + y0);
    ps.u[1][2] = (unsigned)(s_rx + x1); ps.u[1][3] = (unsigned)(s_ry + y1);
    ps.u[0][2] = opaque != 0;
    if (s_magnifier == MAGNIFY_FSR1) {
        layer_quad(s_fsr_easu, SDL_GPU_LOADOP_DONT_CARE, NULL, s_easu_pipe,
                   src, &ps, NULL);
        layer_quad(dst, SDL_GPU_LOADOP_DONT_CARE, NULL, s_rcas_pipe,
                   s_fsr_easu, &ps, NULL);
    } else {
        layer_quad(dst, SDL_GPU_LOADOP_DONT_CARE, NULL,
                   s_magnifier == MAGNIFY_CATMULL_ROM ? s_catmull_pipe
                                                      : s_upscale_pipe,
                   src, &ps, NULL);
    }
}

/* The layer magnified to the render scale, once per change. */
static void layer_upscale(int b)
{
    if (!s_layer_stale[b])
        return;
    magnify(s_layer[b], s_layer_up[b], 0);
    s_layer_stale[b] = 0;
}

/* The magnified layer of buffer b over dst, whose pixel (0, 0) is the
 * layer's (ox, oy) at the render scale. The display and a fold both. */
void layer_compose(int b, SDL_GPUTexture *dst, int ox, int oy)
{
    SDL_GPUTexture *src[3];
    ps_data_t ps;

    layer_upscale(b);
    /* The native picture: the target at native resolution, as a readback
     * takes it, with the layer over it; then magnified like the layer. */
    end_pass();
    if (!command())
        return;
    blit(s_cmd, s_color[b], 0, 0, s_iw, s_ih, s_native_lo, 0, 0, s_w, s_h,
         SDL_GPU_LOADOP_DONT_CARE, SDL_GPU_FILTER_LINEAR);
    memset(&ps, 0, sizeof ps);
    layer_quad(s_native_lo, SDL_GPU_LOADOP_LOAD, NULL, s_over_pipe,
               s_layer[b], &ps, NULL);
    magnify(s_native_lo, s_native_up, 1);
    ps.u[0][0] = (unsigned)ox;
    ps.u[0][1] = (unsigned)oy;
    ps.u[0][2] = (unsigned)s_scale;
    src[0] = s_layer_up[b]; src[1] = s_layer[b]; src[2] = s_native_up;
    layer_quadn(dst, SDL_GPU_LOADOP_LOAD, NULL, s_compose_pipe, src, 3,
                &ps, NULL);
}

/* Scene drawing is about to land on buffer b: what its layer shows becomes
 * part of the target, exactly as it was displayed. */
static void layer_fold(int b)
{
    if (!s_layer_used[b])
        return;
    layer_compose(b, s_color[b], 0, 0);
    s_layer_used[b] = 0;
    s_layer_stale[b] = 1;
}

/* The open run is 2D: its 1x drawing goes over its buffer's layer. */
void run_commit(void)
{
    ps_data_t ps;
    int b = s_run_buffer;

    if (!s_run)
        return;
    flush_batch();
    run_draw();
    if (s_run_ready) {
        memset(&ps, 0, sizeof ps);
        layer_quad(s_layer[b], s_layer_used[b] ? SDL_GPU_LOADOP_LOAD
                                               : SDL_GPU_LOADOP_CLEAR,
                   NULL, s_over_pipe, s_run_lo, &ps, NULL);
        s_layer_used[b] = 1;
        s_layer_stale[b] = 1;
    }
    run_reset();
}

/* The open run is scene: its scaled drawing goes over the target, after
 * whatever the layer held. */
static void run_cancel(void)
{
    ps_data_t ps;
    int b = s_run_buffer;

    if (!s_run)
        return;
    flush_batch();
    run_draw();
    layer_fold(b);
    if (s_run_ready) {
        memset(&ps, 0, sizeof ps);
        layer_quad(s_color[b], SDL_GPU_LOADOP_LOAD, NULL, s_over_pipe,
                   s_run_hi, &ps, NULL);
    }
    run_reset();
}

/* A non-2D draw on buffer b ends any run on it as scene and folds its layer. */
static void scene_begins(int b)
{
    flush_batch();
    if (s_run) {
        if (s_run_buffer == b)
            run_cancel();
        else
            run_commit();
    }
    layer_fold(b);
}

/* A clear writes the target as it always has; above it, the run and the
 * layer become transparent over the rectangle, so the clear shows. */
void layer_clear(int x0, int y0, int x1, int y1, uint32_t depth)
{
    int b = vgl.draw_buffer;
    int full = x0 <= 0 && y0 <= 0 && x1 >= vgl.w && y1 >= vgl.h;
    float r[4];
    ps_data_t ps;

    r[0] = (float)x0 / vgl.w; r[1] = (float)y0 / vgl.h;
    r[2] = (float)x1 / vgl.w; r[3] = (float)y1 / vgl.h;
    memset(&ps, 0, sizeof ps);
    ps.u[0][1] = depth;
    if (s_run && s_run_buffer == b) {
        if (full) {
            run_reset();
        } else {
            run_draw();
            if (s_run_ready) {
                layer_quad(s_run_lo, SDL_GPU_LOADOP_LOAD, s_layer_depth,
                           s_clear_layer_pipe, NULL, &ps, r);
                layer_quad(s_run_hi, s_run_hi_ready ? SDL_GPU_LOADOP_LOAD
                                                    : SDL_GPU_LOADOP_CLEAR,
                           NULL, s_clear_layer_color_pipe, NULL, &ps, r);
                s_run_hi_ready = 1;
            }
        }
    }
    if (s_layer_used[b]) {
        if (full)
            s_layer_used[b] = 0;
        else
            layer_quad(s_layer[b], SDL_GPU_LOADOP_LOAD, NULL,
                       s_clear_layer_color_pipe, NULL, &ps, r);
        s_layer_stale[b] = 1;
    }
}

void scene_draws(int buffer)
{
    if (s_run || s_layer_used[buffer])
        scene_begins(buffer);
}

void run_commit_on(int buffer)
{
    if (s_run && s_run_buffer == buffer)
        run_commit();
}

void layer_swap(void)
{
    SDL_GPUTexture *t;
    int d;

    t = s_layer[0]; s_layer[0] = s_layer[1]; s_layer[1] = t;
    t = s_layer_up[0]; s_layer_up[0] = s_layer_up[1]; s_layer_up[1] = t;
    d = s_layer_used[0]; s_layer_used[0] = s_layer_used[1];
    s_layer_used[1] = d;
    d = s_layer_stale[0]; s_layer_stale[0] = s_layer_stale[1];
    s_layer_stale[1] = d;
}

void layer_over(SDL_GPUTexture *dst, int b)
{
    ps_data_t ps;

    memset(&ps, 0, sizeof ps);
    layer_quad(dst, SDL_GPU_LOADOP_LOAD, NULL, s_over_pipe, s_layer[b], &ps,
               NULL);
}

void layer_lfb(int b, SDL_GPUTexture *lfb)
{
    ps_data_t ps;

    memset(&ps, 0, sizeof ps);
    layer_quad(s_layer[b], s_layer_used[b] ? SDL_GPU_LOADOP_LOAD
                                           : SDL_GPU_LOADOP_CLEAR,
               NULL, s_lfb_layer_pipe, lfb, &ps, NULL);
    s_layer_used[b] = 1;
    s_layer_stale[b] = 1;
}
