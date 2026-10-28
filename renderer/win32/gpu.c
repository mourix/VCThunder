/* gpu.c -- SDL3 GPU raster and presentation backend.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The portable core remains the authority for state and memory semantics.
 * Preload SDL and driver modules before the host maps the game image.
 */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/vgl.h"
#include "../core/vcglide.h"
#include "gpu_shader.inc"

#define GPU_TEX_CACHE 1024
#define GPU_TEX_HASH 2048
#define GPU_PALETTE_KEYS 256
#define GPU_PIPE_CACHE 128
/* Project-owned batching capacity; see the matching measured rationale in
 * gpu_shader.hlsl. Each triangle contributes twelve float4 vertex vectors. */
#define GPU_BATCH_TRIANGLES 64
#define GPU_BATCH_VECTORS   (GPU_BATCH_TRIANGLES * 12)
/* Batches of one 2D run held until they are drawn into the run's two
 * targets; a full list is drawn and emptied, so this is a capacity only. */
#define GPU_RUN_BATCHES     32
#define GPU_HITCH_LOG_MS     25.0
/* SDL's D3D12 transfer contract requires 256-byte texture row pitches and
 * 512-byte transfer offsets to avoid an internal repack. One reusable 8 MiB
 * logical buffer holds the largest native LFB plus any texture chain; SDL's
 * documented `cycle` operation supplies another internal backing only while
 * an earlier command buffer still owns the current one. */
#define GPU_UPLOAD_ARENA_BYTES (8u * 1024u * 1024u)
#define GPU_UPLOAD_ROW_ALIGN   256u
#define GPU_UPLOAD_OFFSET_ALIGN 512u

typedef struct {
    int used, tmu;
    uint32_t base;
    int even_odd, small_lod, large_lod, aspect, format;
    unsigned mem_ver;
    uint64_t pal_key;
    uint64_t age;
    int hash_next;
    SDL_GPUTexture *texture;
} tex_cache_t;

typedef struct {
    uint64_t hash;
    uint32_t rgba[256];
} palette_key_t;

typedef struct {
    int used;
    uint64_t key;
    SDL_GPUGraphicsPipeline *pipeline;
} pipe_cache_t;

typedef union {
    uint32_t u[20][4];
    float f[20][4];
} ps_data_t;

/* A 2D batch (`is2d`) carries a second copy of its triangles at native
 * resolution, `vertex1`, sampled through `bind1`: the game's own address
 * modes, because the 2D layer is drawn exactly as 1x draws it. */
typedef struct {
    float vertex[GPU_BATCH_VECTORS][4];
    float vertex1[GPU_BATCH_VECTORS][4];
    ps_data_t ps;
    SDL_GPUTextureSamplerBinding bind[2], bind1[2];
    SDL_GPUGraphicsPipeline *pipe;
    unsigned triangles;
    int is2d;
    int buffer, clip_x0, clip_y0, clip_x1, clip_y1;
} draw_batch_t;

typedef struct {
    uint64_t version;
    int valid;
    int layerable;
    ps_data_t ps;
    SDL_GPUTextureSamplerBinding bind[2];
    SDL_GPUGraphicsPipeline *pipe, *layer_pipe;
} draw_state_t;

static SDL_GPUDevice *s_dev;
static SDL_Window *s_window;
static HWND s_hwnd;
static SDL_GPUShader *s_main_vs, *s_main_ps;
static SDL_GPUShader *s_copy_vs, *s_overlay_ps, *s_clear_ps, *s_depth_resolve_ps;
static SDL_GPUGraphicsPipeline *s_overlay_pipe, *s_clear_pipe, *s_depth_resolve_pipe;
static SDL_GPUTexture *s_white;
static SDL_GPUTexture *s_color[2], *s_depth;
static SDL_GPUTexture *s_native, *s_native_depth, *s_lfb_tex, *s_display;

/* ---- the 2D layer ----------------------------------------------------------
 * Above 1x, 2D is not drawn into the scaled target. Each colour buffer has a
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
static int s_texmag_catmull;    /* VCGLIDE_TEXMAG = catmull_rom */
static const char *s_swap_mode; /* the present mode the swapchain took */
static int s_layer_used[2];     /* the layer holds something */
static int s_layer_stale[2];    /* s_layer_up does not show it yet */
static int s_run;               /* a 2D run is open, on s_run_buffer */
static int s_run_buffer, s_run_ready, s_run_hi_ready;
static unsigned s_run_count;
static SDL_GPUSampler *s_samplers[16];
static SDL_GPUCommandBuffer *s_cmd;
static SDL_GPUTransferBuffer *s_upload_arena;
static Uint32 s_upload_offset;
static int s_upload_cycle, s_upload_used;
static SDL_GPURenderPass *s_pass;
static int s_pass_buffer = -1;
static draw_batch_t s_batch;
static draw_batch_t s_run_list[GPU_RUN_BATCHES];
static draw_state_t s_draw_state;
static SDL_GPUGraphicsPipeline *s_pass_pipe;
/* The bezel: its art, a mipmapped sampler, and two pipelines for the
 * swapchain's format, one laying the picture into it and one blending the art
 * over it. Made at each open that presents and released at its close. */
static SDL_GPUShader *s_bezel_ps;
static SDL_GPUGraphicsPipeline *s_bezel_pipe, *s_bezel_pic_pipe;
static SDL_GPUTexture *s_bezel_tex;
static SDL_GPUSampler *s_bezel_sampler;

/* The display's gamma table (vgl.h): a 256x1 lookup texture, uploaded again
 * whenever the game loads a table, and the copy of the display the pass
 * reads while it writes the display. */
static SDL_GPUShader *s_gamma_ps;
static SDL_GPUGraphicsPipeline *s_gamma_pipe;
static SDL_GPUTexture *s_gamma_lut, *s_gamma_src;
static unsigned s_gamma_gen;
static SDL_GPUTextureSamplerBinding s_pass_bind[2];
static int s_pass_bind_valid, s_pass_scissor_valid, s_surface_pushed;
static SDL_Rect s_pass_scissor;
static tex_cache_t s_tex_cache[GPU_TEX_CACHE];
static int s_tex_hash[GPU_TEX_HASH];
static tex_cache_t *s_bound_tex[VGL_TMUS];
static palette_key_t s_palette_keys[GPU_PALETTE_KEYS];
static unsigned s_palette_key_count;
static unsigned s_palette_seen_ver[VGL_TMUS];
static uint64_t s_palette_current_key[VGL_TMUS];
static pipe_cache_t s_pipe_cache[GPU_PIPE_CACHE];
static uint64_t s_age;
static int s_preload_tried, s_ready, s_open, s_present, s_quit_posted;
static int s_w, s_h, s_scale, s_iw, s_ih;
static int s_rx, s_ry, s_rw, s_rh;
static int s_vx0, s_vy0, s_vx1, s_vy1;
static int s_dirty[2];
static int s_depth_dirty;
static SDL_GPUTransferBuffer *s_download;
static int s_refresh_hz;
static LARGE_INTEGER s_clock_freq, s_next_present;
static LARGE_INTEGER s_fps_start;
static unsigned s_fps_frames;
static uint64_t s_tex_uploads, s_tex_upload_bytes;
static uint64_t s_lfb_uploads, s_lfb_syncs, s_fence_waits, s_frame_waits;
static uint64_t s_draw_batches, s_batched_triangles;
static uint64_t s_tex_lookups, s_tex_probes;
static uint64_t s_submissions;
static uint64_t s_report_batches, s_report_triangles, s_report_lookups;
static uint64_t s_report_submissions, s_report_uploads, s_report_upload_bytes;
static uint64_t s_frame_index;

/* ---- a lost device ------------------------------------------------------ */
/* DXGI_ERROR_DEVICE_HUNG does not clear: once the device is gone every call
 * returns it, so a failure here is a STATE and not an event. Treating each as
 * its own event writes gigabytes of identical lines while the screen holds one
 * frame, the counter still reads ~28 fps because swaps keep happening, and the
 * watchdog stays quiet because frames never stop.
 *
 * So there are three parts: DETECTION (a run of frames in which nothing could
 * be encoded), a BOUND on the logging, and a DECISION, which is that the host
 * is told through vglHealth and shuts down as it does for a closed window.
 * Recreating the device is not attempted: SDL_GPU has no path back, and a
 * cabinet that exits is one a kiosk restart recovers. */
#define GPU_LOSS_FRAMES   30u   /* consecutive failing frames: one second */
#define GPU_FAIL_LOG_MAX   3u   /* lines per site, for the life of the run */

enum {
    GF_COMMAND, GF_SUBMIT, GF_FENCE, GF_SWAPCHAIN, GF_TEXTURE, GF_UPLOAD,
    GF_PIPELINE, GF_RENDERPASS, GF_COPYPASS, GF_SAMPLER, GF_COUNT
};
static const char *const s_fail_what[GF_COUNT] = {
    "cannot acquire command buffer",
    "command submission failed",
    "fence wait failed",
    "swapchain acquisition failed",
    "texture creation failed",
    "texture upload failed",
    "graphics pipeline creation failed",
    "render pass creation failed",
    "copy pass creation failed",
    "sampler creation failed"
};
/* Only a failure the device itself owns votes on the verdict, because the
 * verdict shuts the cabinet down. An upload that did not fit our own arena is
 * ours: it can repeat every frame for a reason that is not a dead GPU.
 *
 * EVERYTHING ELSE HERE IS THE DEVICE'S, AND THE RENDER PASS IS THE ONE THAT
 * MATTERS: a device still handing out command buffers and swapchain images
 * while refusing render passes draws nothing and presents on schedule.
 *
 * GF_PIPELINE is hard because an exhausted pipeline CACHE returns before it
 * reaches gpu_failed(); the only failure arriving here is the driver refusing
 * to build one. */
static const unsigned char s_fail_hard[GF_COUNT] = {
    1, 1, 1, 1, 1, 0, 1, 1, 1, 1
};
static unsigned s_fail_logged[GF_COUNT];
static uint64_t s_fail_total[GF_COUNT];
static unsigned s_fail_frames;          /* consecutive frames carrying one */
static int      s_fail_in_frame;
static int      s_lost;                 /* latched: the device is gone */
static uint64_t s_fault_at;             /* injection; 0 = off */
static int      s_fault_pass_only;      /* fail only the render pass */
static char     s_fail_first[192];

/* Every SDL failure on the frame path routes here. Its cost does not grow with
 * the frame rate: the log budget is per site and for the whole run. */
static void gpu_failed(int site)
{
    const char *err = SDL_GetError();

    s_fail_total[site]++;
    if (s_fail_hard[site])
        s_fail_in_frame = 1;
    if (!s_fail_first[0] && err && *err)
        snprintf(s_fail_first, sizeof s_fail_first, "%s", err);
    if (s_lost || s_fail_logged[site] >= GPU_FAIL_LOG_MAX)
        return;
    s_fail_logged[site]++;
    vgl_log(0, "gpu: %s: %s%s", s_fail_what[site], err && *err ? err : "?",
            s_fail_logged[site] == GPU_FAIL_LOG_MAX
                ? "  [further failures at this site are counted, not logged]"
                : "");
}

/* VCGLIDE_FAULTDEVICE=N fails every command buffer, texture and RENDER PASS
 * from frame N, which is the shape the real fault had: this is the only way
 * the decision below gets exercised without hanging a GPU on purpose. The
 * render pass is in that list deliberately, a device that fails only there
 * still acquires command buffers and still presents, so it is the case the
 * detector has to catch and the one an injection that skipped it could not
 * prove. */
/* `site` is the GF_* the caller is about to attempt, so a `renderpass`
 * injection can leave everything else working. */
static int gpu_fault_injected_at(int site)
{
    if (!s_fault_at || s_frame_index < s_fault_at)
        return 0;
    if (s_fault_pass_only && site != GF_RENDERPASS)
        return 0;
    /* Say what this is in the log the operator will read. Without it the
     * failure quotes whatever SDL last set, which on this host is a missing
     * WinPix DLL from start-up: an injected fault would report a real error
     * from somewhere else entirely. */
    SDL_SetError("injected by VCGLIDE_FAULTDEVICE=%llu",
                 (unsigned long long)s_fault_at);
    return 1;
}


/* The frame boundary is where a run of failures becomes a verdict. */
static void device_frame_verdict(void)
{
    int i;

    if (s_lost)
        return;
    if (!s_fail_in_frame) {
        s_fail_frames = 0;
        return;
    }
    s_fail_in_frame = 0;
    if (++s_fail_frames < GPU_LOSS_FRAMES)
        return;
    s_lost = 1;
    vgl_log(0, "gpu: THE DEVICE IS GONE. Nothing could be encoded for %u "
               "consecutive frames, ending at frame %llu. First error: %s",
            GPU_LOSS_FRAMES, (unsigned long long)s_frame_index,
            s_fail_first[0] ? s_fail_first : "(none reported)");
    for (i = 0; i < GF_COUNT; i++)
        if (s_fail_total[i])
            vgl_log(0, "gpu:   %-40s %llu", s_fail_what[i],
                    (unsigned long long)s_fail_total[i]);
    vgl_log(0, "gpu: this state does not clear, so nothing further is "
               "attempted and nothing further is logged. The host is asked to "
               "shut down.");
}

/* Read by the host after every swap, through vglHealth. */
int vgl_gpu_lost(void)
{
    return s_lost;
}

static void flush_batch(void);
static void run_draw(void);
static void blit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src,
                 int sx, int sy, int sw, int sh, SDL_GPUTexture *dst,
                 int dx, int dy, int dw, int dh, SDL_GPULoadOp load,
                 SDL_GPUFilter filter);

static double elapsed_ms(LARGE_INTEGER begin, LARGE_INTEGER end)
{
    if (!s_clock_freq.QuadPart)
        return 0.0;
    return 1000.0 * (double)(end.QuadPart - begin.QuadPart) /
           (double)s_clock_freq.QuadPart;
}

static Uint32 align_up(Uint32 value, Uint32 alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t fbits(float f)
{
    union { float f; uint32_t u; } v;
    v.f = f;
    return v.u;
}

static float ubits(uint32_t u)
{
    union { uint32_t u; float f; } v;
    v.u = u;
    return v.f;
}

static SDL_GPUCompareOp compare_op(int op)
{
    static const SDL_GPUCompareOp map[] = {
        SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_LESS,
        SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL,
        SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS
    };
    return op >= 0 && op < 8 ? map[op] : SDL_GPU_COMPAREOP_ALWAYS;
}

static SDL_GPUBlendFactor blend_factor(int f)
{
    switch (f) {
    case VGL_BLEND_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
    case VGL_BLEND_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case VGL_BLEND_SRC_COLOR: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case VGL_BLEND_DST_ALPHA: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case VGL_BLEND_ONE: return SDL_GPU_BLENDFACTOR_ONE;
    case VGL_BLEND_ONE_MINUS_SRC_ALPHA:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case VGL_BLEND_ONE_MINUS_SRC_COLOR:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case VGL_BLEND_ONE_MINUS_DST_ALPHA:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case VGL_BLEND_DST_COLOR: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case VGL_BLEND_ONE_MINUS_DST_COLOR:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}

/* The last submitted command buffer, possibly still executing. Nothing may
 * be recorded or created until it has finished: doing so while the GPU still
 * executed the presented frame hung the D3D12 device at render_scale 7 and 8
 * as 3D began (DXGI_ERROR_DEVICE_HUNG, the next frame's first draw never
 * completing). Waiting was clean in every run, and the same binary without it
 * hung half to four fifths of replays. One frame in flight was NOT enough:
 * SDL waits for it at the next acquisition, after the next frame is recorded.
 * The wait is taken lazily, at the next frame's first device call, so the
 * game's own work between frames still overlaps the GPU. Measured by
 * replaying a capture of the hang at 8x, presented.
 *
 * The mechanism is not known, so the wait is not narrowed to the calls that
 * happen to come first today: every submission holds a fence, whichever way
 * its frame ended, and EVERY SDL CALL THAT TAKES THE DEVICE TAKES IT FROM
 * device(). No SDL call is handed s_dev from anywhere else. */
static SDL_GPUFence *s_frame_fence;

static void settle_frame(void)
{
    SDL_GPUFence *fence = s_frame_fence;

    if (!fence)
        return;
    s_frame_fence = NULL;
    /* A lost device finishes nothing, and asking it only adds failures
     * after the verdict. Counted only when the GPU is still busy, but WAITED
     * ON EVEN WHEN IT IS NOT: the wait is where SDL retires the finished
     * command buffer, and releasing a fence that was only queried crashed
     * SDL 3.4.10 in a later draw (D3D12_INTERNAL_BindGraphicsResources),
     * headless every run, where no swapchain acquisition retires it
     * instead. */
    if (!s_lost) {
        if (!SDL_QueryGPUFence(s_dev, fence))
            s_frame_waits++;
        if (!SDL_WaitForGPUFences(s_dev, true, &fence, 1))
            gpu_failed(GF_FENCE);
    }
    SDL_ReleaseGPUFence(s_dev, fence);
}

static SDL_GPUDevice *device(void)
{
    settle_frame();
    return s_dev;
}

static SDL_GPUTexture *make_texture(SDL_GPUTextureFormat format,
                                    SDL_GPUTextureUsageFlags usage,
                                    unsigned w, unsigned h, unsigned levels)
{
    SDL_GPUTextureCreateInfo ci;
    SDL_GPUTexture *texture;
    SDL_GPUDevice *dev;
    LARGE_INTEGER begin, end;
    memset(&ci, 0, sizeof ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = format;
    ci.usage = usage;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = levels;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (s_lost)
        return NULL;
    dev = device();     /* outside the timing: a frame wait is not creation */
    QueryPerformanceCounter(&begin);
    texture = gpu_fault_injected_at(GF_TEXTURE) ? NULL
                                                : SDL_CreateGPUTexture(dev, &ci);
    QueryPerformanceCounter(&end);
    if (!texture)
        gpu_failed(GF_TEXTURE);
    if (elapsed_ms(begin, end) >= GPU_HITCH_LOG_MS)
        vgl_log(0, "gpu: slow texture creation at frame %llu: %ux%u, "
                   "%u levels, %.3f ms",
                (unsigned long long)s_frame_index, w, h, levels,
                elapsed_ms(begin, end));
    return texture;
}

static void end_pass(void)
{
    if (s_pass) {
        SDL_EndGPURenderPass(s_pass);
        s_pass = NULL;
        s_pass_buffer = -1;
        s_pass_pipe = NULL;
        s_pass_bind_valid = 0;
        s_pass_scissor_valid = 0;
    }
}

static int submit(void)
{
    int ok = 1;
    LARGE_INTEGER begin, end;
    flush_batch();
    run_draw();
    end_pass();
    if (s_cmd) {
        QueryPerformanceCounter(&begin);
        /* Every submission holds its fence for settle_frame(), however the
         * frame ended: presented, minimised with no swapchain image,
         * headless, or closing. None is held already: s_cmd came from
         * command(), which settled before it existed. */
        s_frame_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(s_cmd);
        ok = s_frame_fence != NULL;
        QueryPerformanceCounter(&end);
        s_submissions++;
        /* A normal paced frame can wait 8-12 ms here.  Logging that wait on
         * every frame perturbs the workload being measured; reserve the
         * diagnostic for an actual visible hitch. */
        if (elapsed_ms(begin, end) >= GPU_HITCH_LOG_MS)
            vgl_log(0, "gpu: slow command submission at frame %llu: %.3f ms",
                    (unsigned long long)s_frame_index,
                    elapsed_ms(begin, end));
        if (!ok)
            gpu_failed(GF_SUBMIT);
        s_cmd = NULL;
        s_surface_pushed = 0;
        if (s_upload_used) {
            /* The encoded ranges now belong to this command buffer. The next
             * write starts from zero and asks SDL to cycle only if that
             * backing is still bound, retaining at most the frames in flight. */
            s_upload_offset = 0;
            s_upload_cycle = 1;
            s_upload_used = 0;
        }
    }
    return ok;
}

static int submit_wait(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUFence *fence;
    bool ok;

    s_fence_waits++;
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        gpu_failed(GF_SUBMIT);
        return 0;
    }
    ok = SDL_WaitForGPUFences(device(), true, &fence, 1);
    SDL_ReleaseGPUFence(device(), fence);
    if (!ok)
        gpu_failed(GF_FENCE);
    return ok ? 1 : 0;
}

static SDL_GPUCommandBuffer *command(void)
{
    if (s_lost)
        return NULL;
    if (!s_cmd) {
        s_cmd = gpu_fault_injected_at(GF_COMMAND)
                    ? NULL : SDL_AcquireGPUCommandBuffer(device());
        s_surface_pushed = 0;
    }
    if (!s_cmd)
        gpu_failed(GF_COMMAND);
    return s_cmd;
}

static int reserve_upload(Uint32 bytes, Uint32 *offset, uint8_t **map)
{
    SDL_GPUTransferBufferCreateInfo tci;
    Uint32 at;

    if (!bytes || bytes > GPU_UPLOAD_ARENA_BYTES)
        return 0;
    if (!s_upload_arena) {
        memset(&tci, 0, sizeof tci);
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tci.size = GPU_UPLOAD_ARENA_BYTES;
        s_upload_arena = SDL_CreateGPUTransferBuffer(device(), &tci);
        if (!s_upload_arena)
            return 0;
        s_upload_offset = 0;
        s_upload_cycle = s_upload_used = 0;
        vgl_log(0, "gpu: reusable %.0f MiB upload arena, %u-byte rows / "
                   "%u-byte offsets", GPU_UPLOAD_ARENA_BYTES / (1024.0 * 1024.0),
                GPU_UPLOAD_ROW_ALIGN, GPU_UPLOAD_OFFSET_ALIGN);
    }
    at = align_up(s_upload_offset, GPU_UPLOAD_OFFSET_ALIGN);
    if (at > GPU_UPLOAD_ARENA_BYTES - bytes) {
        at = 0;
        s_upload_cycle = 1;
    }
    *map = (uint8_t *)SDL_MapGPUTransferBuffer(
        device(), s_upload_arena, s_upload_cycle != 0);
    if (!*map)
        return 0;
    s_upload_cycle = 0;
    s_upload_used = 1;
    s_upload_offset = at + bytes;
    *offset = at;
    return 1;
}

static SDL_GPUShader *make_shader(ID3DBlob *blob, SDL_GPUShaderStage stage,
                                  unsigned samplers, unsigned uniforms)
{
    SDL_GPUShaderCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.code = (const Uint8 *)ID3D10Blob_GetBufferPointer(blob);
    ci.code_size = ID3D10Blob_GetBufferSize(blob);
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_DXBC;
    ci.stage = stage;
    ci.num_samplers = samplers;
    ci.num_uniform_buffers = uniforms;
    return SDL_CreateGPUShader(device(), &ci);
}

static ID3DBlob *compile_shader(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(vgl_gpu_shader_source,
                            sizeof vgl_gpu_shader_source - 1,
                            "renderer/win32/gpu_shader.hlsl", NULL, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &errors);
    if (FAILED(hr)) {
        const char *why = errors ?
            (const char *)ID3D10Blob_GetBufferPointer(errors) : "no detail";
        vgl_log(0, "gpu: HLSL %s/%s compilation failed (0x%08lx): %s",
                entry, target, (unsigned long)hr, why);
        if (errors) ID3D10Blob_Release(errors);
        if (code) ID3D10Blob_Release(code);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

static SDL_GPUGraphicsPipeline *fixed_pipeline(SDL_GPUShader *vs,
                                               SDL_GPUShader *ps,
                                               SDL_GPUTextureFormat format,
                                               int depth_write)
{
    SDL_GPUColorTargetDescription color;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    memset(&color, 0, sizeof color);
    memset(&ci, 0, sizeof ci);
    color.format = format;
    color.blend_state.enable_color_write_mask = true;
    color.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    ci.vertex_shader = vs;
    ci.fragment_shader = ps;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.target_info.color_target_descriptions = &color;
    ci.target_info.num_color_targets = 1;
    ci.target_info.has_depth_stencil_target = depth_write != 0;
    ci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    ci.depth_stencil_state.enable_depth_test = depth_write != 0;
    ci.depth_stencil_state.enable_depth_write = depth_write != 0;
    return SDL_CreateGPUGraphicsPipeline(device(), &ci);
}

/* A full-quad pass for the 2D layer: RGBA8, all four channels written
 * (`mask` 0 writes none), optionally a depth target written ALWAYS, and
 * optionally blending a premultiplied source over the destination. */
static SDL_GPUGraphicsPipeline *layer_pipeline(SDL_GPUShader *ps, int depth,
                                               int over, int mask)
{
    SDL_GPUColorTargetDescription color;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    memset(&color, 0, sizeof color);
    memset(&ci, 0, sizeof ci);
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color.blend_state.enable_color_write_mask = true;
    color.blend_state.color_write_mask = mask ?
        SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A : 0;
    if (over) {
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    ci.vertex_shader = s_copy_vs;
    ci.fragment_shader = ps;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.target_info.color_target_descriptions = &color;
    ci.target_info.num_color_targets = 1;
    ci.target_info.has_depth_stencil_target = depth != 0;
    ci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    ci.depth_stencil_state.enable_depth_test = depth != 0;
    ci.depth_stencil_state.enable_depth_write = depth != 0;
    return SDL_CreateGPUGraphicsPipeline(device(), &ci);
}

static int upload_rgba(SDL_GPUTexture *texture, unsigned level,
                       unsigned w, unsigned h, const void *pixels)
{
    SDL_GPUTransferBufferCreateInfo tci;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUTextureTransferInfo src;
    SDL_GPUTextureRegion dst;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *copy;
    void *map;
    unsigned bytes = w * h * 4u;

    memset(&tci, 0, sizeof tci);
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = bytes;
    tb = SDL_CreateGPUTransferBuffer(device(), &tci);
    if (!tb) return 0;
    map = SDL_MapGPUTransferBuffer(device(), tb, false);
    if (!map) { SDL_ReleaseGPUTransferBuffer(device(), tb); return 0; }
    memcpy(map, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(device(), tb);
    cmd = SDL_AcquireGPUCommandBuffer(device());
    if (!cmd) { SDL_ReleaseGPUTransferBuffer(device(), tb); return 0; }
    copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        gpu_failed(GF_COPYPASS);
        SDL_CancelGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device(), tb);
        return 0;
    }
    memset(&src, 0, sizeof src);
    src.transfer_buffer = tb;
    src.pixels_per_row = w;
    src.rows_per_layer = h;
    memset(&dst, 0, sizeof dst);
    dst.texture = texture; dst.mip_level = level;
    dst.w = w; dst.h = h; dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    if (!submit_wait(cmd)) {
        SDL_ReleaseGPUTransferBuffer(device(), tb);
        return 0;
    }
    SDL_ReleaseGPUTransferBuffer(device(), tb);
    return 1;
}

/* Record an upload in the current command buffer and return immediately.
 * Rows and offsets satisfy SDL's documented D3D12 alignment, and the shared
 * transfer arena is cycled at command-buffer boundaries rather than creating
 * and releasing one driver object for every upload. */
static int queue_rgba(SDL_GPUTexture *texture, unsigned level,
                      unsigned w, unsigned h, const void *pixels)
{
    SDL_GPUTextureTransferInfo src;
    SDL_GPUTextureRegion dst;
    SDL_GPUCopyPass *copy;
    uint8_t *map;
    Uint32 at, pitch = align_up(w * 4u, GPU_UPLOAD_ROW_ALIGN);
    Uint32 bytes = pitch * h;
    unsigned y;

    end_pass();
    if (!command()) return 0;
    if (!reserve_upload(bytes, &at, &map)) return 0;
    for (y = 0; y < h; y++)
        memcpy(map + at + (size_t)y * pitch,
               (const uint8_t *)pixels + (size_t)y * w * 4u, w * 4u);
    SDL_UnmapGPUTransferBuffer(device(), s_upload_arena);
    copy = SDL_BeginGPUCopyPass(s_cmd);
    if (!copy) {
        gpu_failed(GF_COPYPASS);
        return 0;
    }
    memset(&src, 0, sizeof src);
    src.transfer_buffer = s_upload_arena;
    src.offset = at;
    src.pixels_per_row = pitch / 4u; src.rows_per_layer = h;
    memset(&dst, 0, sizeof dst);
    dst.texture = texture; dst.mip_level = level;
    dst.w = w; dst.h = h; dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    return 1;
}

/* Export and encode every dirty mip in one arena map and one copy pass. Apart
 * from avoiding a CPU-side RGBA allocation/copy, grouping is important for
 * the small levels: ungrouped, Venice manufactures about 204 transfer objects
 * per frame from only one real texture-memory download. */
static int queue_texture_mips(SDL_GPUTexture *texture, int tmu,
                              uint32_t dirty_mips)
{
    const vgl_tmu_t *u = &vgl.tmu[tmu];
    SDL_GPUTextureTransferInfo src;
    SDL_GPUTextureRegion dst;
    SDL_GPUCopyPass *copy;
    Uint32 relative[VGL_MAX_LOD + 1] = { 0 };
    Uint32 pitches[VGL_MAX_LOD + 1] = { 0 };
    Uint32 total = 0, at;
    uint8_t *map;
    int lod;

    for (lod = 0; lod <= VGL_MAX_LOD; lod++) {
        if (!(dirty_mips & (1u << lod)) || u->lod_off[lod] < 0)
            continue;
        total = align_up(total, GPU_UPLOAD_OFFSET_ALIGN);
        relative[lod] = total;
        pitches[lod] = align_up((Uint32)u->lod_w[lod] * 4u,
                                GPU_UPLOAD_ROW_ALIGN);
        total += pitches[lod] * u->lod_h[lod];
    }
    if (!total)
        return 1;
    end_pass();
    if (!command() || !reserve_upload(total, &at, &map))
        return 0;
    for (lod = 0; lod <= VGL_MAX_LOD; lod++) {
        if (!(dirty_mips & (1u << lod)) || u->lod_off[lod] < 0)
            continue;
        if (!vgl_tex_export_rgba(tmu, lod, map + at + relative[lod],
                                 pitches[lod])) {
            SDL_UnmapGPUTransferBuffer(device(), s_upload_arena);
            return 0;
        }
    }
    SDL_UnmapGPUTransferBuffer(device(), s_upload_arena);
    copy = SDL_BeginGPUCopyPass(s_cmd);
    if (!copy) {
        gpu_failed(GF_COPYPASS);
        return 0;
    }
    for (lod = 0; lod <= VGL_MAX_LOD; lod++) {
        unsigned w, h;
        if (!(dirty_mips & (1u << lod)) || u->lod_off[lod] < 0)
            continue;
        w = u->lod_w[lod]; h = u->lod_h[lod];
        memset(&src, 0, sizeof src);
        src.transfer_buffer = s_upload_arena;
        src.offset = at + relative[lod];
        src.pixels_per_row = pitches[lod] / 4u;
        src.rows_per_layer = h;
        memset(&dst, 0, sizeof dst);
        dst.texture = texture; dst.mip_level = (unsigned)lod;
        dst.w = w; dst.h = h; dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        s_tex_uploads++;
        s_tex_upload_bytes += (uint64_t)w * h * 4u;
    }
    SDL_EndGPUCopyPass(copy);
    return 1;
}

int vgl_gpu_preload(void)
{
    ID3DBlob *mvs = NULL, *mps = NULL, *cvs = NULL;
    ID3DBlob *ops = NULL, *cps = NULL, *dps = NULL;
    ID3DBlob *vps = NULL, *ups = NULL, *lps = NULL, *lcs = NULL, *lds = NULL;
    ID3DBlob *xps = NULL, *crs = NULL, *eas = NULL, *rcs = NULL, *bzs = NULL;
    static const uint8_t white[4] = { 255, 255, 255, 255 };

    if (s_preload_tried)
        return s_ready;
    s_preload_tried = 1;
    QueryPerformanceFrequency(&s_clock_freq);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        vgl_log(0, "gpu: SDL3 initialization failed: %s", SDL_GetError());
        return 0;
    }
    mvs = compile_shader("main_vs", "vs_5_1");
    mps = compile_shader("main_ps", "ps_5_1");
    cvs = compile_shader("copy_vs", "vs_5_1");
    ops = compile_shader("overlay_ps", "ps_5_1");
    cps = compile_shader("clear_ps", "ps_5_1");
    dps = compile_shader("depth_resolve_ps", "ps_5_1");
    vps = compile_shader("over_ps", "ps_5_1");
    ups = compile_shader("upscale_bilinear_ps", "ps_5_1");
    lps = compile_shader("clear_layer_ps", "ps_5_1");
    lcs = compile_shader("clear_layer_color_ps", "ps_5_1");
    lds = compile_shader("layer_depth_ps", "ps_5_1");
    xps = compile_shader("compose_ps", "ps_5_1");
    crs = compile_shader("upscale_catmull_ps", "ps_5_1");
    eas = compile_shader("fsr_easu_ps", "ps_5_1");
    rcs = compile_shader("fsr_rcas_ps", "ps_5_1");
    bzs = compile_shader("bezel_ps", "ps_5_1");
    if (!mvs || !mps || !cvs || !ops || !cps || !dps || !vps || !ups ||
        !lps || !lcs || !lds || !xps || !crs || !eas || !rcs || !bzs)
        goto done;
    s_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXBC, false, "direct3d12");
    if (!s_dev) {
        vgl_log(0, "gpu: SDL3 could not create a Direct3D 12 device: %s",
                SDL_GetError());
        goto done;
    }
    s_main_vs = make_shader(mvs, SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    s_main_ps = make_shader(mps, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    s_copy_vs = make_shader(cvs, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    s_overlay_ps = make_shader(ops, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_clear_ps = make_shader(cps, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    s_depth_resolve_ps = make_shader(dps, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_over_ps = make_shader(vps, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_upscale_ps = make_shader(ups, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_clear_layer_ps = make_shader(lps, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    s_clear_layer_color_ps = make_shader(lcs, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                         0, 1);
    s_layer_depth_ps = make_shader(lds, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_compose_ps = make_shader(xps, SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    s_catmull_ps = make_shader(crs, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_easu_ps = make_shader(eas, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_rcas_ps = make_shader(rcs, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    s_bezel_ps = make_shader(bzs, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    if (!s_main_vs || !s_main_ps || !s_copy_vs || !s_overlay_ps || !s_clear_ps ||
        !s_depth_resolve_ps || !s_over_ps || !s_upscale_ps ||
        !s_clear_layer_ps || !s_clear_layer_color_ps || !s_layer_depth_ps ||
        !s_compose_ps || !s_catmull_ps || !s_easu_ps || !s_rcas_ps ||
        !s_bezel_ps) {
        vgl_log(0, "gpu: SDL3 shader creation failed: %s", SDL_GetError());
        goto done;
    }
    s_overlay_pipe = fixed_pipeline(s_copy_vs, s_overlay_ps,
                                    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 0);
    s_clear_pipe = fixed_pipeline(s_copy_vs, s_clear_ps,
                                  SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1);
    s_depth_resolve_pipe = fixed_pipeline(s_copy_vs, s_depth_resolve_ps,
                                          SDL_GPU_TEXTUREFORMAT_R16_UNORM, 0);
    s_over_pipe = layer_pipeline(s_over_ps, 0, 1, 1);
    s_upscale_pipe = layer_pipeline(s_upscale_ps, 0, 0, 1);
    s_clear_layer_pipe = layer_pipeline(s_clear_layer_ps, 1, 0, 1);
    s_clear_layer_color_pipe = layer_pipeline(s_clear_layer_color_ps, 0, 0, 1);
    s_layer_depth_pipe = layer_pipeline(s_layer_depth_ps, 1, 0, 0);
    s_lfb_layer_pipe = layer_pipeline(s_overlay_ps, 0, 0, 1);
    s_compose_pipe = layer_pipeline(s_compose_ps, 0, 1, 1);
    s_catmull_pipe = layer_pipeline(s_catmull_ps, 0, 0, 1);
    s_easu_pipe = layer_pipeline(s_easu_ps, 0, 0, 1);
    s_rcas_pipe = layer_pipeline(s_rcas_ps, 0, 0, 1);
    s_white = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                           SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, 1, 1);
    if (!s_overlay_pipe || !s_clear_pipe || !s_depth_resolve_pipe || !s_white ||
        !s_over_pipe || !s_upscale_pipe || !s_clear_layer_pipe ||
        !s_clear_layer_color_pipe || !s_layer_depth_pipe || !s_lfb_layer_pipe ||
        !s_compose_pipe || !s_catmull_pipe || !s_easu_pipe || !s_rcas_pipe ||
        !upload_rgba(s_white, 0, 1, 1, white)) {
        vgl_log(0, "gpu: fixed resource creation failed: %s", SDL_GetError());
        goto done;
    }
    {
        ID3DBlob *gms = compile_shader("gamma_ps", "ps_5_1");

        if (gms) {
            s_gamma_ps = make_shader(gms, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
            ID3D10Blob_Release(gms);
        }
        if (s_gamma_ps)
            s_gamma_pipe = layer_pipeline(s_gamma_ps, 0, 0, 1);
        s_gamma_lut = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                   SDL_GPU_TEXTUREUSAGE_SAMPLER, 256, 1, 1);
        if (!s_gamma_pipe || !s_gamma_lut) {
            vgl_log(0, "gpu: gamma pass creation failed: %s", SDL_GetError());
            goto done;
        }
    }
    s_ready = 1;
    vgl_log(0, "gpu: SDL %d.%d.%d, driver %s, D3D12/DXBC backend ready",
            SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
            SDL_GetGPUDeviceDriver(device()));

done:
    if (mvs) ID3D10Blob_Release(mvs);
    if (mps) ID3D10Blob_Release(mps);
    if (cvs) ID3D10Blob_Release(cvs);
    if (ops) ID3D10Blob_Release(ops);
    if (cps) ID3D10Blob_Release(cps);
    if (dps) ID3D10Blob_Release(dps);
    if (vps) ID3D10Blob_Release(vps);
    if (ups) ID3D10Blob_Release(ups);
    if (lps) ID3D10Blob_Release(lps);
    if (lcs) ID3D10Blob_Release(lcs);
    if (lds) ID3D10Blob_Release(lds);
    if (xps) ID3D10Blob_Release(xps);
    if (crs) ID3D10Blob_Release(crs);
    if (eas) ID3D10Blob_Release(eas);
    if (rcs) ID3D10Blob_Release(rcs);
    if (bzs) ID3D10Blob_Release(bzs);
    return s_ready;
}

static void release_surface_resources(void)
{
    int i;
    submit();
    if (s_dev) SDL_WaitForGPUIdle(device());
    for (i = 0; i < GPU_TEX_CACHE; i++) {
        if (s_tex_cache[i].texture)
            SDL_ReleaseGPUTexture(device(), s_tex_cache[i].texture);
        memset(&s_tex_cache[i], 0, sizeof s_tex_cache[i]);
    }
    memset(s_tex_hash, 0, sizeof s_tex_hash);
    memset(s_bound_tex, 0, sizeof s_bound_tex);
    memset(s_palette_keys, 0, sizeof s_palette_keys);
    s_palette_key_count = 0;
    memset(s_palette_seen_ver, 0, sizeof s_palette_seen_ver);
    memset(s_palette_current_key, 0, sizeof s_palette_current_key);
    if (s_upload_arena)
        SDL_ReleaseGPUTransferBuffer(device(), s_upload_arena);
    s_upload_arena = NULL;
    if (s_download)
        SDL_ReleaseGPUTransferBuffer(device(), s_download);
    s_download = NULL;
    s_upload_offset = 0;
    s_upload_cycle = s_upload_used = 0;
    for (i = 0; i < 2; i++) {
        if (s_color[i]) SDL_ReleaseGPUTexture(device(), s_color[i]);
        s_color[i] = NULL;
    }
    if (s_depth) SDL_ReleaseGPUTexture(device(), s_depth);
    if (s_native) SDL_ReleaseGPUTexture(device(), s_native);
    if (s_native_depth) SDL_ReleaseGPUTexture(device(), s_native_depth);
    if (s_lfb_tex) SDL_ReleaseGPUTexture(device(), s_lfb_tex);
    if (s_display) SDL_ReleaseGPUTexture(device(), s_display);
    s_depth = s_native = s_native_depth = s_lfb_tex = s_display = NULL;
    if (s_gamma_src) SDL_ReleaseGPUTexture(device(), s_gamma_src);
    s_gamma_src = NULL;
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

static int create_targets(int scale)
{
    SDL_GPUTransferBufferCreateInfo tci;
    unsigned usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
    s_iw = s_w * scale;
    s_ih = s_h * scale;
    s_color[0] = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                              usage, s_iw, s_ih, 1);
    s_color[1] = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                              usage, s_iw, s_ih, 1);
    /* Sampled as well as drawn into: a caller may lock the depth buffer. */
    s_depth = make_texture(SDL_GPU_TEXTUREFORMAT_D16_UNORM,
                           SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                           SDL_GPU_TEXTUREUSAGE_SAMPLER,
                           s_iw, s_ih, 1);
    s_native = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                            usage, s_w, s_h, 1);
    s_native_depth = make_texture(SDL_GPU_TEXTUREFORMAT_R16_UNORM,
                                  SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                                  s_w, s_h, 1);
    s_lfb_tex = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                             usage, s_w, s_h, 1);
    memset(&tci, 0, sizeof tci);
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tci.size = (Uint32)((size_t)s_w * s_h * 4);   /* the wider of the two */
    s_download = SDL_CreateGPUTransferBuffer(device(), &tci);
    s_depth_dirty = 1;
    if (scale > 1) {
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
    }
    return s_color[0] && s_color[1] && s_depth && s_native &&
           s_native_depth && s_lfb_tex && s_download;
}

static int clear_initial_targets(void)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device());
    int i;
    if (!cmd) return 0;
    for (i = 0; i < 2; i++) {
        SDL_GPUColorTargetInfo ct;
        SDL_GPUDepthStencilTargetInfo dt;
        SDL_GPURenderPass *pass;
        memset(&ct, 0, sizeof ct);
        memset(&dt, 0, sizeof dt);
        ct.texture = s_color[i]; ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE; ct.clear_color.a = 1.0f;
        dt.texture = s_depth; dt.load_op = SDL_GPU_LOADOP_CLEAR;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
        if (!pass) {
            gpu_failed(GF_RENDERPASS);
            /* A command buffer is submitted or cancelled, never dropped. */
            SDL_CancelGPUCommandBuffer(cmd);
            return 0;
        }
        SDL_EndGPURenderPass(pass);
    }
    return submit_wait(cmd);
}

static void bezel_close(void)
{
    if (s_bezel_tex) SDL_ReleaseGPUTexture(device(), s_bezel_tex);
    if (s_bezel_pipe) SDL_ReleaseGPUGraphicsPipeline(device(), s_bezel_pipe);
    if (s_bezel_pic_pipe)
        SDL_ReleaseGPUGraphicsPipeline(device(), s_bezel_pic_pipe);
    s_bezel_tex = NULL;
    s_bezel_pipe = s_bezel_pic_pipe = NULL;
}

/* Is the art ready to be drawn this open? */
static int bezel_ready(void)
{
    return s_bezel_pipe && s_bezel_pic_pipe && s_bezel_tex && s_bezel_sampler;
}

/* A full-quad pipeline into the swapchain sampling tex0: `blend` lays a
 * premultiplied source over what is there (ONE / ONE_MINUS_SRC_ALPHA),
 * otherwise it replaces it. Colour only, so the swapchain stays opaque. */
static SDL_GPUGraphicsPipeline *swapchain_pipeline(int blend)
{
    SDL_GPUColorTargetDescription color;
    SDL_GPUGraphicsPipelineCreateInfo ci;

    memset(&color, 0, sizeof color);
    memset(&ci, 0, sizeof ci);
    color.format = SDL_GetGPUSwapchainTextureFormat(device(), s_window);
    if (blend) {
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    color.blend_state.enable_color_write_mask = true;
    color.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    ci.vertex_shader = s_copy_vs;
    ci.fragment_shader = s_bezel_ps;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.target_info.color_target_descriptions = &color;
    ci.target_info.num_color_targets = 1;
    return SDL_CreateGPUGraphicsPipeline(device(), &ci);
}

/* Upload the host's art, every mip level in one transfer and one copy pass,
 * and build the two swapchain pipelines. A failure costs the bezel, never the
 * picture, and is not reported to the device-loss detector: nothing here is a
 * frame's work, and a refused upload is not evidence about the device. */
static void bezel_open(void)
{
    SDL_GPUTextureCreateInfo tci2;
    SDL_GPUSamplerCreateInfo si;
    SDL_GPUTransferBufferCreateInfo tci;
    SDL_GPUTransferBuffer *tb = NULL;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *copy;
    SDL_GPUFence *fence;
    Uint32 offset[VGL_BEZEL_LEVELS], pitch[VGL_BEZEL_LEVELS], total = 0;
    uint8_t *map;
    int k, y, ok;

    bezel_close();
    if (!vgl_bezel.levels || !s_window || s_lost)
        return;
    if (!s_bezel_sampler) {
        memset(&si, 0, sizeof si);
        si.min_filter = SDL_GPU_FILTER_LINEAR;
        si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.min_lod = 0.0f;
        si.max_lod = (float)(VGL_BEZEL_LEVELS - 1);
        s_bezel_sampler = SDL_CreateGPUSampler(device(), &si);
    }
    s_bezel_pipe = swapchain_pipeline(1);
    s_bezel_pic_pipe = swapchain_pipeline(0);
    memset(&tci2, 0, sizeof tci2);
    tci2.type = SDL_GPU_TEXTURETYPE_2D;
    tci2.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci2.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci2.width = (Uint32)vgl_bezel.w[0];
    tci2.height = (Uint32)vgl_bezel.h[0];
    tci2.layer_count_or_depth = 1;
    tci2.num_levels = (Uint32)vgl_bezel.levels;
    tci2.sample_count = SDL_GPU_SAMPLECOUNT_1;
    s_bezel_tex = SDL_CreateGPUTexture(device(), &tci2);
    if (!s_bezel_sampler || !s_bezel_pipe || !s_bezel_pic_pipe || !s_bezel_tex)
        goto fail;

    for (k = 0; k < vgl_bezel.levels; k++) {
        pitch[k] = align_up((Uint32)vgl_bezel.w[k] * 4u, GPU_UPLOAD_ROW_ALIGN);
        offset[k] = align_up(total, GPU_UPLOAD_OFFSET_ALIGN);
        total = offset[k] + pitch[k] * (Uint32)vgl_bezel.h[k];
    }
    memset(&tci, 0, sizeof tci);
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = total;
    tb = SDL_CreateGPUTransferBuffer(device(), &tci);
    if (!tb) goto fail;
    map = (uint8_t *)SDL_MapGPUTransferBuffer(device(), tb, false);
    if (!map) goto fail;
    for (k = 0; k < vgl_bezel.levels; k++)
        for (y = 0; y < vgl_bezel.h[k]; y++)
            memcpy(map + offset[k] + (size_t)y * pitch[k],
                   vgl_bezel.px[k] + (size_t)y * vgl_bezel.w[k] * 4,
                   (size_t)vgl_bezel.w[k] * 4);
    SDL_UnmapGPUTransferBuffer(device(), tb);
    cmd = SDL_AcquireGPUCommandBuffer(device());
    if (!cmd) goto fail;
    copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(cmd);
        goto fail;
    }
    for (k = 0; k < vgl_bezel.levels; k++) {
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        memset(&src, 0, sizeof src);
        src.transfer_buffer = tb;
        src.offset = offset[k];
        src.pixels_per_row = pitch[k] / 4u;
        src.rows_per_layer = (Uint32)vgl_bezel.h[k];
        memset(&dst, 0, sizeof dst);
        dst.texture = s_bezel_tex; dst.mip_level = (Uint32)k;
        dst.w = (Uint32)vgl_bezel.w[k]; dst.h = (Uint32)vgl_bezel.h[k]; dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) goto fail;
    ok = SDL_WaitForGPUFences(device(), true, &fence, 1);
    SDL_ReleaseGPUFence(device(), fence);
    if (!ok) goto fail;
    SDL_ReleaseGPUTransferBuffer(device(), tb);
    vgl_log(0, "gpu: bezel art uploaded, %dx%d, %d levels, %.1f MiB",
            vgl_bezel.w[0], vgl_bezel.h[0], vgl_bezel.levels,
            total / (1024.0 * 1024.0));
    return;

fail:
    vgl_log(0, "gpu: the bezel could not be set up (%s); the picture is "
               "presented without it", SDL_GetError());
    if (tb) SDL_ReleaseGPUTransferBuffer(device(), tb);
    bezel_close();
}

/* One quad of `src` across `r` of a cw x ch swapchain image. `r` may pass
 * the image's edges: the rasteriser clips it, so the mapping stays exact. */
static void swapchain_quad(SDL_GPURenderPass *pass, SDL_GPUGraphicsPipeline *pipe,
                           SDL_GPUTexture *src, Uint32 cw, Uint32 ch,
                           const vgl_rect_t *r)
{
    SDL_GPUTextureSamplerBinding bind;
    float vd[10][4];

    SDL_BindGPUGraphicsPipeline(pass, pipe);
    bind.texture = src;
    bind.sampler = s_bezel_sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &bind, 1);
    memset(vd, 0, sizeof vd);
    vd[0][0] = (float)r->x / (float)cw;
    vd[0][1] = (float)r->y / (float)ch;
    vd[0][2] = (float)(r->x + r->w) / (float)cw;
    vd[0][3] = (float)(r->y + r->h) / (float)ch;
    SDL_PushGPUVertexUniformData(s_cmd, 0, vd, sizeof vd);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
}

/* Under a bezel: clear, the picture across `pic` (it may pass the edges when
 * it covers the hole), then the art over it across `canvas`. One pass. */
static void bezel_present(SDL_GPUTexture *swap, Uint32 cw, Uint32 ch,
                          const vgl_rect_t *pic, const vgl_rect_t *canvas)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPURenderPass *pass;

    if (!cw || !ch)
        return;
    memset(&ct, 0, sizeof ct);
    ct.texture = swap;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color.a = 1.0f;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    pass = gpu_fault_injected_at(GF_RENDERPASS)
               ? NULL : SDL_BeginGPURenderPass(s_cmd, &ct, 1, NULL);
    if (!pass) {
        gpu_failed(GF_RENDERPASS);
        return;
    }
    if (pic->w > 0 && pic->h > 0)
        swapchain_quad(pass, s_bezel_pic_pipe, s_display, cw, ch, pic);
    swapchain_quad(pass, s_bezel_pipe, s_bezel_tex, cw, ch, canvas);
    SDL_EndGPURenderPass(pass);
}

static int create_window(void *caller_window, int window_scale)
{
    int cw = s_rw * window_scale, ch = s_rh * window_scale;

    if (caller_window && IsWindow((HWND)caller_window)) {
        SDL_PropertiesID p = SDL_CreateProperties();
        if (p) {
            SDL_SetPointerProperty(p, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER,
                                   caller_window);
            s_window = SDL_CreateWindowWithProperties(p);
            SDL_DestroyProperties(p);
        }
    }
    if (!s_window)
        s_window = SDL_CreateWindow("VCThunder", cw, ch, 0);
    if (!s_window) {
        vgl_log(0, "gpu: SDL window creation failed: %s", SDL_GetError());
        return 0;
    }
    s_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(s_window),
                                           SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                           NULL);
    {
        char title[96];
        vgl_window_title(title, sizeof title, 0.0);   /* no frame rate yet */
        SDL_SetWindowTitle(s_window, title);
    }
    if (!SDL_ClaimWindowForGPUDevice(device(), s_window)) {
        vgl_log(0, "gpu: SDL swapchain claim failed: %s", SDL_GetError());
        return 0;
    }
    /* A claimed swapchain defaults to VSYNC, which waits on the display and
     * can make the games miss their own deadline.
     * Mailbox is the other mode that never waits; it just does not tear. */
    {
        static const struct {
            SDL_GPUPresentMode mode;
            const char *name;
        } k_modes[] = {
            { SDL_GPU_PRESENTMODE_IMMEDIATE, "immediate" },
            { SDL_GPU_PRESENTMODE_MAILBOX,   "mailbox" },
        };
        char why[160] = "not supported";
        size_t i;

        s_swap_mode = "vsync";
        for (i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++) {
            if (!SDL_WindowSupportsGPUPresentMode(device(), s_window,
                                                  k_modes[i].mode))
                continue;
            if (SDL_SetGPUSwapchainParameters(device(), s_window,
                    SDL_GPU_SWAPCHAINCOMPOSITION_SDR, k_modes[i].mode)) {
                s_swap_mode = k_modes[i].name;
                break;
            }
            snprintf(why, sizeof why, "%s refused: %s", k_modes[i].name,
                     SDL_GetError());
        }
        if (i == sizeof k_modes / sizeof k_modes[0])
            vgl_log(0, "gpu: WARNING no present mode that never waits (%s); "
                    "presenting with vsync, which can slow the game", why);
    }
    /* Bounds nothing: settle_frame() finishes every submission before the
     * next is recorded, so one frame is in flight whatever this says. Three
     * is what the settle was measured with, and the only reason it stays. Do
     * not read one here as an alternative to the settle: one frame in flight
     * with no settle hung 2 replays of 2. */
    SDL_SetGPUAllowedFramesInFlight(device(), 3);
    SDL_ShowWindow(s_window);
    return 1;
}

static int recreate_display(void)
{
    unsigned usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                     SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture *next;
    if (!s_present && !vgl_cfg.display_dump[0]) return 1;
    submit();
    next = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, usage,
                        s_rw * s_scale, s_rh * s_scale, 1);
    if (!next) {
        vgl_log(0, "gpu: cannot allocate %dx%d display texture: %s",
                s_rw * s_scale, s_rh * s_scale, SDL_GetError());
        return 0;
    }
    if (s_display) {
        SDL_WaitForGPUIdle(device());
        SDL_ReleaseGPUTexture(device(), s_display);
    }
    s_display = next;
    if (s_gamma_src) {
        SDL_WaitForGPUIdle(device());
        SDL_ReleaseGPUTexture(device(), s_gamma_src);
        s_gamma_src = NULL;
    }
    return 1;
}

int vgl_gpu_open(void *caller_window, int w, int h, int raster_w, int raster_h,
                 int render_scale, int window_scale, int refresh_hz, int present)
{
    int scale;

    if (!s_ready || s_open) return 0;
    s_w = w; s_h = h; s_present = present;
    s_quit_posted = 0;
    s_fault_at = vgl_cfg.fault_device;
    s_fault_pass_only = _stricmp(vgl_cfg.fault_site, "renderpass") == 0;
    if (s_fault_at)
        vgl_log(0, "gpu: FAULT INJECTION is armed from frame %llu: %s. This is "
                   "what a hung device looks like from here",
                (unsigned long long)s_fault_at,
                s_fault_pass_only
                    ? "every RENDER PASS fails and nothing else does, so "
                      "command buffers and presentation keep working and only "
                      "the picture stops; the case the detector was blind to"
                    : "every command buffer, texture and render pass fails");
    s_refresh_hz = refresh_hz > 0 ? refresh_hz : 60;
    s_rx = 0; s_ry = 0;
    /* Already clamped to the surface by vgl_refresh_geometry(); the guards
     * stay because this is the only place that knows the two must agree, and
     * a zero here would size a texture. */
    s_rw = raster_w > 0 && raster_w <= w ? raster_w : w;
    s_rh = raster_h > 0 && raster_h <= h ? raster_h : h;
    s_vx0 = s_rx; s_vy0 = s_ry;
    s_vx1 = s_rx + s_rw; s_vy1 = s_ry + s_rh;
    /* Before the retry loop, not after it: release_surface_resources() below
     * calls submit(), which calls flush_batch(), and a batch left over from a
     * previous open would be encoded against textures this open has not made
     * yet. Close already drains it, so this has never fired; the ordering
     * should not depend on that. */
    memset(&s_batch, 0, sizeof s_batch);
    memset(&s_draw_state, 0, sizeof s_draw_state);
    s_magnifier = !strcmp(vgl_cfg.upscale_2d, "catmull_rom") ? MAGNIFY_CATMULL_ROM
                : !strcmp(vgl_cfg.upscale_2d, "fsr1") ? MAGNIFY_FSR1
                : MAGNIFY_BILINEAR;
    s_texmag_catmull = !strcmp(vgl_cfg.texmag, "catmull_rom");
    scale = render_scale;
    while (scale >= 1) {
        if (create_targets(scale)) break;
        release_surface_resources();
        vgl_log(0, "gpu: %dx internal targets unavailable; trying %dx",
                scale, scale - 1);
        scale--;
    }
    if (scale < 1) return 0;
    s_scale = scale;
    if (!clear_initial_targets()) {
        release_surface_resources();
        return 0;
    }
    if (present && !create_window(caller_window, window_scale)) {
        release_surface_resources();
        if (s_window) SDL_DestroyWindow(s_window);
        s_window = NULL;
        return 0;
    }
    if (!recreate_display()) {
        release_surface_resources();
        return 0;
    }
    if (present)
        bezel_open();
    memset(s_dirty, 0, sizeof s_dirty);
    memset(s_layer_used, 0, sizeof s_layer_used);
    s_layer_stale[0] = s_layer_stale[1] = 1;
    s_run = s_run_ready = s_run_hi_ready = 0;
    s_run_count = 0;
    s_pass_pipe = NULL;
    s_pass_bind_valid = s_pass_scissor_valid = s_surface_pushed = 0;
    s_tex_uploads = s_tex_upload_bytes = 0;
    s_lfb_uploads = s_lfb_syncs = s_fence_waits = s_frame_waits = 0;
    s_draw_batches = s_batched_triangles = 0;
    s_tex_lookups = s_tex_probes = 0;
    s_submissions = 0;
    s_report_batches = s_report_triangles = s_report_lookups = 0;
    s_report_submissions = s_report_uploads = s_report_upload_bytes = 0;
    s_frame_index = 0;
    s_open = 1;
    QueryPerformanceCounter(&s_fps_start);
    s_next_present = s_fps_start;
    s_fps_frames = 0;
    vgl_log(0, "gpu: SDL3 %s, internal target %dx%d (%dx), native LFB %dx%d, %s%s",
            SDL_GetGPUDeviceDriver(device()), s_iw, s_ih, s_scale, s_w, s_h,
            present ? s_swap_mode : "headless", present ? " swapchain" : "");
    if (s_scale > 1)
        vgl_log(0, "gpu: 2D layer magnified by %s, 3D textures by %s",
                vgl_cfg.upscale_2d, vgl_cfg.texmag);
    return 1;
}

void vgl_gpu_close(void)
{
    if (!s_open) return;
    vgl_log(0, "gpu: uploads %llu texture levels / %.2f MiB, %llu LFB; "
            "%llu LFB readbacks, %llu blocking fence waits, %llu frames "
            "waited for the GPU; %llu triangles "
            "in %llu draw batches; %llu texture lookups / %llu hash probes",
            (unsigned long long)s_tex_uploads,
            (double)s_tex_upload_bytes / (1024.0 * 1024.0),
            (unsigned long long)s_lfb_uploads,
            (unsigned long long)s_lfb_syncs,
            (unsigned long long)s_fence_waits,
            (unsigned long long)s_frame_waits,
            (unsigned long long)s_batched_triangles,
            (unsigned long long)s_draw_batches,
            (unsigned long long)s_tex_lookups,
            (unsigned long long)s_tex_probes);
    release_surface_resources();
    bezel_close();
    if (s_window) {
        SDL_ReleaseWindowFromGPUDevice(device(), s_window);
        SDL_DestroyWindow(s_window);
    }
    s_window = NULL; s_hwnd = NULL;
    s_open = s_present = 0;
}

void vgl_gpu_set_raster(int x, int y, int w, int h,
                        int valid_x0, int valid_y0,
                        int valid_x1, int valid_y1)
{
    int changed = w != s_rw || h != s_rh;
    valid_x0 -= x; valid_x1 -= x;
    valid_y0 -= y; valid_y1 -= y;
    if (valid_x0 < 0) valid_x0 = 0;
    if (valid_y0 < 0) valid_y0 = 0;
    if (valid_x1 > w) valid_x1 = w;
    if (valid_y1 > h) valid_y1 = h;
    s_rx = x; s_ry = y; s_rw = w; s_rh = h;
    s_vx0 = valid_x0; s_vy0 = valid_y0;
    s_vx1 = valid_x1; s_vy1 = valid_y1;
    if (s_open && changed)
        recreate_display();
}

/* At render scales above one, clamp proven non-tiling primitives so invented
 * sub-pixel samples cannot wrap across a texture edge. Preserve the game's
 * address mode for tiled primitives and all scale-one rendering. */
static SDL_GPUSampler *sampler_for(const vgl_tmu_t *u, int no_tile)
{
    SDL_GPUSamplerCreateInfo ci;
    unsigned key = (u->min_filter != VGL_TEXFILTER_POINT) |
                   ((u->mag_filter != VGL_TEXFILTER_POINT) << 1) |
                   ((u->clamp_s == VGL_TEXCLAMP_CLAMP || no_tile) << 2) |
                   ((u->clamp_t == VGL_TEXCLAMP_CLAMP || no_tile) << 3);

    if (s_samplers[key]) return s_samplers[key];
    memset(&ci, 0, sizeof ci);
    ci.min_filter = (key & 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mag_filter = (key & 2) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = (key & 4) ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
                                  : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    ci.address_mode_v = (key & 8) ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
                                  : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.min_lod = 0.0f; ci.max_lod = (float)VGL_MAX_LOD;
    s_samplers[key] = SDL_CreateGPUSampler(device(), &ci);
    /* A null sampler makes vgl_gpu_draw_triangle drop the primitive, which is
     * a picture that silently loses geometry. Sixteen of these exist for the
     * life of the device, so a failure here is the device's. */
    if (!s_samplers[key])
        gpu_failed(GF_SAMPLER);
    return s_samplers[key];
}

static int texture_identity_matches(const tex_cache_t *e, int tmu,
                                    const vgl_tmu_t *u, uint64_t pal_key)
{
    return e->used && e->tmu == tmu && e->base == u->base &&
           e->even_odd == u->even_odd && e->small_lod == u->small_lod &&
           e->large_lod == u->large_lod && e->aspect == u->aspect &&
           e->format == u->format && e->pal_key == pal_key;
}

static unsigned texture_hash_values(int tmu, uint32_t base, int even_odd,
                                    int small_lod, int large_lod,
                                    int aspect, int format, uint64_t pal_key)
{
    /* Project-chosen odd mixers distribute cache buckets only. Identity is
     * always checked field-for-field, so a hash collision changes speed and
     * cannot select different texture content. */
    uint32_t h = base * 0x9e3779b1u;
    h ^= (uint32_t)tmu * 0x85ebca6bu;
    h ^= (uint32_t)even_odd * 0xc2b2ae35u;
    h ^= (uint32_t)small_lod << 3;
    h ^= (uint32_t)large_lod << 7;
    h ^= (uint32_t)aspect << 11;
    h ^= (uint32_t)format << 16;
    h ^= (uint32_t)pal_key;
    h ^= (uint32_t)(pal_key >> 32) * 0x27d4eb2du;
    h ^= h >> 16;
    return h & (GPU_TEX_HASH - 1);
}

static unsigned texture_hash_key(int tmu, const vgl_tmu_t *u,
                                 uint64_t pal_key)
{
    return texture_hash_values(tmu, u->base, u->even_odd,
                               u->small_lod, u->large_lod,
                               u->aspect, u->format, pal_key);
}

static unsigned texture_entry_hash(const tex_cache_t *e)
{
    return texture_hash_values(e->tmu, e->base, e->even_odd,
                               e->small_lod, e->large_lod,
                               e->aspect, e->format, e->pal_key);
}

static tex_cache_t *texture_hash_find(int tmu, const vgl_tmu_t *u,
                                      uint64_t pal_key)
{
    int link = s_tex_hash[texture_hash_key(tmu, u, pal_key)];
    s_tex_lookups++;
    while (link) {
        tex_cache_t *e = &s_tex_cache[link - 1];
        s_tex_probes++;
        if (texture_identity_matches(e, tmu, u, pal_key))
            return e;
        link = e->hash_next;
    }
    return NULL;
}

static void texture_hash_insert(tex_cache_t *e)
{
    unsigned bucket = texture_entry_hash(e);
    int ix = (int)(e - s_tex_cache);
    e->hash_next = s_tex_hash[bucket];
    s_tex_hash[bucket] = ix + 1;
}

static void texture_hash_remove(tex_cache_t *e)
{
    unsigned bucket;
    int *link;
    int target;

    if (!e->used) return;
    bucket = texture_entry_hash(e);
    target = (int)(e - s_tex_cache) + 1;
    link = &s_tex_hash[bucket];
    while (*link) {
        tex_cache_t *cur = &s_tex_cache[*link - 1];
        if (*link == target) {
            *link = cur->hash_next;
            e->hash_next = 0;
            return;
        }
        link = &cur->hash_next;
    }
}

static void texture_unbind(tex_cache_t *e)
{
    int i;
    for (i = 0; i < VGL_TMUS; i++)
        if (s_bound_tex[i] == e)
            s_bound_tex[i] = NULL;
}

static uint64_t texture_palette_key(int tmu, const vgl_tmu_t *u)
{
    const uint8_t *p;
    /* A 64-bit FNV-1a is only a prefilter for the exact memcmp below; palette
     * content equality never rests on the hash. */
    uint64_t hash = 1469598103934665603ull;
    unsigned i;

    if ((u->format != VGL_TEXFMT_P_8 && u->format != VGL_TEXFMT_AP_88) ||
        !u->palette_valid)
        return 0;
    if (s_palette_seen_ver[tmu] == u->pal_ver)
        return s_palette_current_key[tmu];
    p = (const uint8_t *)u->palette;
    for (i = 0; i < sizeof u->palette; i++) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    for (i = 0; i < s_palette_key_count; i++) {
        if (s_palette_keys[i].hash == hash &&
            memcmp(s_palette_keys[i].rgba, u->palette,
                   sizeof u->palette) == 0) {
            s_palette_seen_ver[tmu] = u->pal_ver;
            s_palette_current_key[tmu] = i + 1;
            return i + 1;
        }
    }
    if (s_palette_key_count < GPU_PALETTE_KEYS) {
        i = s_palette_key_count++;
        s_palette_keys[i].hash = hash;
        memcpy(s_palette_keys[i].rgba, u->palette, sizeof u->palette);
        s_palette_current_key[tmu] = i + 1;
    } else {
        /* Exact interning is an optimisation, never a content authority. If a
         * title exceeds the measured 40 unique Offroad palettes and fills the
         * table, retain correctness with a generation-unique non-reused key. */
        s_palette_current_key[tmu] = (1ull << 63) |
            ((uint64_t)(unsigned)tmu << 32) | u->pal_ver;
    }
    s_palette_seen_ver[tmu] = u->pal_ver;
    return s_palette_current_key[tmu];
}

static SDL_GPUTexture *texture_for(int tmu)
{
    const vgl_tmu_t *u = &vgl.tmu[tmu];
    tex_cache_t *slot = NULL;
    uint64_t pal_key;
    uint32_t dirty_mips = ~0u;
    int i, new_identity;
    LARGE_INTEGER begin, end;

    if (!u->valid || u->lvl_first < 0) return s_white;
    pal_key = texture_palette_key(tmu, u);
    slot = s_bound_tex[tmu];
    if (!slot || !texture_identity_matches(slot, tmu, u, pal_key))
        slot = texture_hash_find(tmu, u, pal_key);
    if (slot && slot->mem_ver == u->mem_ver) {
        slot->age = ++s_age;
        s_bound_tex[tmu] = slot;
        return slot->texture;
    }
    if (slot) {
        dirty_mips = vgl_tex_dirty_mips(tmu, slot->mem_ver);
        if (!dirty_mips) {
            /* Writes elsewhere in this TMU do not invalidate this identity. */
            slot->mem_ver = u->mem_ver;
            slot->age = ++s_age;
            s_bound_tex[tmu] = slot;
            return slot->texture;
        }
    }
    if (!slot) {
        for (i = 0; i < GPU_TEX_CACHE; i++) {
            if (!s_tex_cache[i].used) {
                slot = &s_tex_cache[i];
                break;
            }
            if (!slot || s_tex_cache[i].age < slot->age)
                slot = &s_tex_cache[i];
        }
    }
    if (!slot) return s_white;

    QueryPerformanceCounter(&begin);

    /* A cache replacement or refresh may release or overwrite a texture used
     * by the pending CPU-side batch. Record that draw first; the following
    * copy then remains ordered after it in the same GPU command buffer. The
    * same holds for the 2D run's recorded batches. */
    flush_batch();
    run_draw();
    new_identity = !texture_identity_matches(slot, tmu, u, pal_key);
    if (new_identity)
        dirty_mips = ~0u;

    /* The texture is deliberately 256-wide (or its aspect-ratio counterpart)
     * with absolute Glide LOD numbers as GPU mip indices. Split even/odd chains
     * can then use SampleLevel without repacking either half. Uploads and the
     * draws which consume them stay ordered in one command buffer. Paletted
     * variants are cached by exact palette content: Offroad alternates 40
     * palettes across 158,094 downloads, so a generation counter would turn
     * every return to existing content into a full decode and upload. */
    if (new_identity) {
        texture_hash_remove(slot);
        texture_unbind(slot);
        if (slot->texture) SDL_ReleaseGPUTexture(device(), slot->texture);
        memset(slot, 0, sizeof *slot);
        slot->texture = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                     SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                     u->lod_w[0], u->lod_h[0],
                                     VGL_MAX_LOD + 1);
        if (!slot->texture) goto fail;
    }
    if (!queue_texture_mips(slot->texture, tmu, dirty_mips))
        goto fail;
    slot->used = 1; slot->tmu = tmu; slot->base = u->base;
    slot->even_odd = u->even_odd;
    slot->small_lod = u->small_lod; slot->large_lod = u->large_lod;
    slot->aspect = u->aspect; slot->format = u->format;
    slot->mem_ver = u->mem_ver;
    slot->pal_key = pal_key;
    slot->age = ++s_age;
    if (new_identity)
        texture_hash_insert(slot);
    s_bound_tex[tmu] = slot;
    QueryPerformanceCounter(&end);
    if (elapsed_ms(begin, end) >= GPU_HITCH_LOG_MS)
        vgl_log(0, "gpu: slow texture resolve at frame %llu: tmu %d base "
                   "0x%08lx lod %d..%d mask %d, %.3f ms",
                (unsigned long long)s_frame_index, tmu,
                (unsigned long)u->base, u->large_lod, u->small_lod,
                u->even_odd, elapsed_ms(begin, end));
    return slot->texture;

fail:
    texture_hash_remove(slot);
    texture_unbind(slot);
    if (slot->texture) SDL_ReleaseGPUTexture(device(), slot->texture);
    memset(slot, 0, sizeof *slot);
    gpu_failed(GF_UPLOAD);
    return s_white;
}

static uint64_t pipeline_key(int layer)
{
    uint64_t k = (uint64_t)(layer != 0) << 14;
    k |= (uint64_t)(vgl.depth_mode != VGL_DEPTH_DISABLE);
    k |= (uint64_t)(vgl.depth_func & 7) << 1;
    k |= (uint64_t)(vgl.depth_mask != 0) << 4;
    k |= (uint64_t)(vgl.blend_rgb_src & 15) << 5;
    k |= (uint64_t)(vgl.blend_rgb_dst & 15) << 9;
    k |= (uint64_t)(vgl.color_mask_rgb != 0) << 13;
    return k;
}

/* `layer` is the variant that draws into a 2D layer: the game's colour blend,
 * and coverage accumulated in alpha by ONE / the game's destination factor,
 * so the layer stays premultiplied (layerable() says which blends allow it). */
static SDL_GPUGraphicsPipeline *pipeline_for(int layer)
{
    uint64_t key = pipeline_key(layer);
    SDL_GPUColorTargetDescription color;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_GPUGraphicsPipeline *p;
    SDL_GPUDevice *dev;
    int i, slot = -1;
    LARGE_INTEGER begin, end;

    for (i = 0; i < GPU_PIPE_CACHE; i++) {
        if (s_pipe_cache[i].used && s_pipe_cache[i].key == key)
            return s_pipe_cache[i].pipeline;
        if (!s_pipe_cache[i].used && slot < 0) slot = i;
    }
    if (slot < 0) {
        /* Capped, because this returns the same answer on every draw-state
         * change for the rest of the run. It is also worth saying what it
         * COSTS: pipeline_for() returning NULL makes vgl_gpu_draw_triangle
         * drop the primitive, so from here the picture stops changing. That is
         * not the device's fault and does not vote on the loss verdict, so
         * this line is the only warning there will be. */
        VGL_LOG_CAPPED(3, "gpu: graphics pipeline cache exhausted (%d entries)"
                          "; no further primitive will be drawn",
                       GPU_PIPE_CACHE);
        return NULL;
    }
    memset(&color, 0, sizeof color);
    memset(&ci, 0, sizeof ci);
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color.blend_state.src_color_blendfactor = blend_factor(vgl.blend_rgb_src);
    color.blend_state.dst_color_blendfactor = blend_factor(vgl.blend_rgb_dst);
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = layer
        ? blend_factor(vgl.blend_rgb_dst) : SDL_GPU_BLENDFACTOR_ZERO;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.enable_blend = layer ||
        !(vgl.blend_rgb_src == VGL_BLEND_ONE &&
          vgl.blend_rgb_dst == VGL_BLEND_ZERO);
    color.blend_state.enable_color_write_mask = true;
    color.blend_state.color_write_mask = !vgl.color_mask_rgb ? 0 :
        SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B | (layer ? SDL_GPU_COLORCOMPONENT_A : 0);
    ci.vertex_shader = s_main_vs; ci.fragment_shader = s_main_ps;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.depth_stencil_state.compare_op = compare_op(vgl.depth_func);
    ci.depth_stencil_state.enable_depth_test =
        vgl.depth_mode != VGL_DEPTH_DISABLE;
    ci.depth_stencil_state.enable_depth_write =
        vgl.depth_mode != VGL_DEPTH_DISABLE && vgl.depth_mask;
    ci.target_info.color_target_descriptions = &color;
    ci.target_info.num_color_targets = 1;
    ci.target_info.has_depth_stencil_target = true;
    ci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    dev = device();     /* outside the timing: a frame wait is not creation */
    QueryPerformanceCounter(&begin);
    p = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    QueryPerformanceCounter(&end);
    if (!p) {
        /* Report the failure before any timing line, so a refused pipeline
         * is never logged as `created`. */
        gpu_failed(GF_PIPELINE);
        return NULL;
    }
    vgl_log(0, "gpu: pipeline key 0x%llx created at frame %llu in %.3f ms",
            (unsigned long long)key, (unsigned long long)s_frame_index,
            elapsed_ms(begin, end));
    s_pipe_cache[slot].used = 1;
    s_pipe_cache[slot].key = key;
    s_pipe_cache[slot].pipeline = p;
    return p;
}

static SDL_GPURenderPass *draw_pass_for(int buffer, int clip_x0, int clip_y0,
                                        int clip_x1, int clip_y1)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_Rect scissor;
    if (s_pass && s_pass_buffer != buffer)
        end_pass();
    if (!s_pass) {
        if (!command()) return NULL;
        memset(&ct, 0, sizeof ct); memset(&dt, 0, sizeof dt);
        ct.texture = s_color[buffer];
        ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
        dt.texture = s_depth;
        dt.load_op = SDL_GPU_LOADOP_LOAD; dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        s_pass = gpu_fault_injected_at(GF_RENDERPASS)
                     ? NULL : SDL_BeginGPURenderPass(s_cmd, &ct, 1, &dt);
        if (!s_pass) {
            gpu_failed(GF_RENDERPASS);
            return NULL;
        }
        s_pass_buffer = buffer;
        s_pass_pipe = NULL;
        s_pass_bind_valid = 0;
        s_pass_scissor_valid = 0;
    }
    scissor.x = clip_x0 * s_scale;
    scissor.y = clip_y0 * s_scale;
    scissor.w = (clip_x1 - clip_x0) * s_scale;
    scissor.h = (clip_y1 - clip_y0) * s_scale;
    if (!s_pass_scissor_valid ||
        memcmp(&s_pass_scissor, &scissor, sizeof scissor) != 0) {
        SDL_SetGPUScissor(s_pass, &scissor);
        s_pass_scissor = scissor;
        s_pass_scissor_valid = 1;
    }
    return s_pass;
}

static SDL_GPURenderPass *draw_pass(void)
{
    return draw_pass_for(vgl.draw_buffer, vgl.clip_x0, vgl.clip_y0,
                         vgl.clip_x1, vgl.clip_y1);
}

static void texture_uniform(ps_data_t *ps, int tmu, int base)
{
    const vgl_tmu_t *u = &vgl.tmu[tmu];
    unsigned mask = 0;
    int lod;
    for (lod = 0; lod <= VGL_MAX_LOD; lod++)
        if (u->lod_off[lod] >= 0) mask |= 1u << lod;
    ps->u[base + 0][0] = u->valid;
    ps->u[base + 0][1] = mask;
    ps->u[base + 0][2] = (unsigned)u->large_lod;
    ps->u[base + 0][3] = (unsigned)u->small_lod;
    ps->u[base + 1][0] = (unsigned)u->even_odd;
    ps->u[base + 1][1] = (unsigned)u->mipmap_mode;
    ps->u[base + 1][2] = u->lod_w[0];
    ps->u[base + 1][3] = u->lod_h[0];
    ps->u[base + 2][0] = fbits(u->lod_bias);
    /* Above 1x a bilinear texture may be magnified by Catmull-Rom instead.
     * Never under a chroma key: the key is an exact colour, and a resampled
     * neighbourhood both invents near-key colours and lands on the key. */
    ps->u[base + 2][1] = s_scale > 1 && s_texmag_catmull &&
                         u->mag_filter != VGL_TEXFILTER_POINT &&
                         !vgl.chroma_mode;
}

static void fill_ps(ps_data_t *ps, int need_tex, int need_tmu1)
{
    int i;
    memset(ps, 0, sizeof *ps);
    ps->u[0][0] = need_tex; ps->u[0][1] = need_tmu1;
    ps->u[0][2] = vgl.itrgb_lighting ? (unsigned)vgl_opt.itrgb : 0;
    ps->u[0][3] = (unsigned)vgl_opt.lodfrac;
    ps->u[1][0] = (unsigned)(vgl_opt.lodf + 1);
    ps->u[1][1] = (unsigned)vgl.alpha_func;
    ps->u[1][2] = (unsigned)vgl.alpha_ref;
    ps->u[1][3] = vgl.alpha_func != VGL_CMP_ALWAYS;
    ps->u[2][0] = vgl.cc.func; ps->u[2][1] = vgl.cc.factor;
    ps->u[2][2] = vgl.cc.local; ps->u[2][3] = vgl.cc.other;
    ps->u[3][0] = vgl.cc.invert; ps->u[3][1] = vgl.ac.func;
    ps->u[3][2] = vgl.ac.factor; ps->u[3][3] = vgl.ac.local;
    ps->u[4][0] = vgl.ac.other; ps->u[4][1] = vgl.ac.invert;
    ps->u[4][2] = vgl.tmu[0].c_func; ps->u[4][3] = vgl.tmu[0].c_factor;
    ps->u[5][0] = vgl.tmu[0].a_func; ps->u[5][1] = vgl.tmu[0].a_factor;
    ps->u[5][2] = vgl.tmu[0].c_invert; ps->u[5][3] = vgl.tmu[0].a_invert;
    ps->u[6][0] = vgl.constant_color; ps->u[6][1] = vgl.chroma_mode;
    ps->u[6][2] = vgl.chroma_value;
    ps->u[6][3] = (vgl.fog_mode & 7) == 2;
    ps->u[7][0] = vgl.fog_color;
    ps->u[7][1] = (unsigned)vgl.depth_bias;
    ps->u[7][2] = (unsigned)vgl_opt.lodclamp;
    ps->u[7][3] = (unsigned)vgl_opt.lodgrad;
    ps->u[18][0] = (unsigned)vgl_opt.forcelod;
    ps->u[18][2] = vgl.blend_rgb_src == VGL_BLEND_ONE &&
                   vgl.blend_rgb_dst == VGL_BLEND_ZERO;
    texture_uniform(ps, 0, 8); texture_uniform(ps, 1, 11);
    for (i = 0; i < 64; i++)
        ps->u[14 + i / 16][(i / 4) & 3] |=
            (uint32_t)vgl.fog_table[i] << ((i & 3) * 8);
}

static void flush_batch(void)
{
    SDL_GPURenderPass *pass;
    float surface[4] = { (float)s_iw, (float)s_ih, 0.0f, 0.0f };
    unsigned triangles = s_batch.triangles;

    if (!triangles)
        return;
    if (s_batch.is2d) {
        if (s_run_count == GPU_RUN_BATCHES)
            run_draw();
        s_run_list[s_run_count++] = s_batch;
        s_batch.triangles = 0;
        return;
    }
    pass = draw_pass_for(s_batch.buffer,
                         s_batch.clip_x0, s_batch.clip_y0,
                         s_batch.clip_x1, s_batch.clip_y1);
    if (pass) {
        if (s_pass_pipe != s_batch.pipe) {
            SDL_BindGPUGraphicsPipeline(pass, s_batch.pipe);
            s_pass_pipe = s_batch.pipe;
        }
        if (!s_pass_bind_valid ||
            memcmp(s_pass_bind, s_batch.bind, sizeof s_pass_bind) != 0) {
            SDL_BindGPUFragmentSamplers(pass, 0, s_batch.bind, 2);
            memcpy(s_pass_bind, s_batch.bind, sizeof s_pass_bind);
            s_pass_bind_valid = 1;
        }
        SDL_PushGPUVertexUniformData(
            s_cmd, 0, s_batch.vertex,
            (Uint32)((size_t)triangles * 12 * sizeof s_batch.vertex[0]));
        if (!s_surface_pushed) {
            SDL_PushGPUVertexUniformData(s_cmd, 1, surface, sizeof surface);
            s_surface_pushed = 1;
        }
        SDL_PushGPUFragmentUniformData(s_cmd, 0,
                                       &s_batch.ps, sizeof s_batch.ps);
        SDL_DrawGPUPrimitives(pass, triangles * 3, 1, 0, 0);
        s_draw_batches++;
        s_batched_triangles += triangles;
    }
    s_batch.triangles = 0;
}

/* ---- the 2D layer: runs, folds, magnification ---------------------------- */

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
static void layer_quadn(SDL_GPUTexture *dst, SDL_GPULoadOp load,
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
static void run_draw(void)
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
static void layer_compose(int b, SDL_GPUTexture *dst, int ox, int oy)
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
static void run_commit(void)
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
static void layer_clear(int x0, int y0, int x1, int y1, uint32_t depth)
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

/* A primitive cannot tile when all recovered LOD-0 coordinates remain inside
 * the texture rectangle. Degenerate inputs conservatively retain tiling. */
static int tri_no_tile(const float *const v[3], int tmu, const ps_data_t *ps)
{
    unsigned base = tmu == 0 ? 8u : 11u;
    float w = (float)ps->u[base + 1][2], h = (float)ps->u[base + 1][3];
    int so = tmu == 0 ? 9 : 12, i;

    if (!(w > 0.0f) || !(h > 0.0f))
        return 0;
    for (i = 0; i < 3; i++) {
        float oow = v[i][8], inv, s, t;

        if (!(oow > 0.0f))
            return 0;
        inv = 1.0f / oow;
        s = v[i][so] * inv;
        t = v[i][so + 1] * inv;
        if (!(s >= 0.0f) || !(s <= w) || !(t >= 0.0f) || !(t <= h))
            return 0;
    }
    return 1;
}

/* Glide puts a centre exactly on a horizontal edge inside for the edge with
 * the lower device y; D3D's top-left rule does the opposite once a lower-left
 * origin is flipped. Only a horizontal edge can tie over a whole row, so a
 * triangle with one lying exactly on a sample centre is moved down one step of
 * the rasteriser's 1/256 snap, and every other triangle is left alone. */
static float tie_nudge(const float *const v[3], int scale)
{
    int i;

    for (i = 0; i < 3; i++) {
        const float y = v[i][1], other = v[(i + 1) % 3][1];
        float sy, frac;

        if (y != other)
            continue;
        sy = (vgl.h - y) * scale;
        frac = sy - (float)(int)sy;
        if (frac == 0.5f || frac == -0.5f)
            return 1.0f / 256.0f;
    }
    return 0.0f;
}

/* The nudge is for coverage only. Every interpolant is a screen-linear plane
 * (w = 1), so adding its y-gradient times the nudge to all three vertices puts
 * each sample back on the value it had. */
static void tie_compensate(float vd[12][4], float nudge)
{
    static const unsigned char slot[9][2] = {
        { 0, 2 }, { 0, 3 }, { 1, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 },
        { 2, 0 }, { 2, 1 }, { 2, 2 }
    };
    double x1 = (double)vd[4][0] - vd[0][0], y1 = (double)vd[4][1] - vd[0][1];
    double x2 = (double)vd[8][0] - vd[0][0], y2 = (double)vd[8][1] - vd[0][1];
    double det = x1 * y2 - x2 * y1;
    int k, i;

    if (det == 0.0)
        return;
    for (k = 0; k < 9; k++) {
        int r = slot[k][0], c = slot[k][1];
        double a0 = vd[r][c], a1 = vd[4 + r][c], a2 = vd[8 + r][c];
        double dy = (x1 * (a2 - a0) - x2 * (a1 - a0)) / det * nudge;

        for (i = 0; i < 3; i++)
            vd[i * 4 + r][c] = (float)(vd[i * 4 + r][c] + dy);
    }
}

/* One triangle's vertex vectors for a target `scale` times the surface. */
static void build_vertices(float vd[12][4], const float *const v[3],
                           const ps_data_t *ps, int scale)
{
    float nudge = vgl.origin_lower_left ? tie_nudge(v, scale) : 0.0f;
    int i;

    memset(vd, 0, sizeof(float) * 12 * 4);
    for (i = 0; i < 3; i++) {
        vd[i * 4 + 0][0] = v[i][0] * scale;
        vd[i * 4 + 0][1] = vgl.origin_lower_left
            ? (vgl.h - v[i][1]) * scale + nudge
            : v[i][1] * scale;
        vd[i * 4 + 0][2] = v[i][3]; vd[i * 4 + 0][3] = v[i][4];
        vd[i * 4 + 1][0] = v[i][5]; vd[i * 4 + 1][1] = v[i][7];
        vd[i * 4 + 1][2] = v[i][8]; vd[i * 4 + 1][3] = v[i][9];
        vd[i * 4 + 2][0] = v[i][10]; vd[i * 4 + 2][1] = v[i][12];
        vd[i * 4 + 2][2] = v[i][13];
        vd[i * 4 + 3][0] = ubits(ps->u[6][0]);
        vd[i * 4 + 3][1] = ubits(ps->u[1][2]);
        vd[i * 4 + 3][2] = ubits(ps->u[7][1]);
    }
    if (nudge != 0.0f)
        tie_compensate(vd, nudge);
}

void vgl_gpu_draw_triangle(const float *a, const float *b, const float *c)
{
    const float *v[3] = { a, b, c };
    if (s_lost) return;
    float vd[12][4], vd1[12][4];
    ps_data_t ps_key;
    const ps_data_t *ps;
    SDL_GPUTextureSamplerBinding tri_bind[2], tri_bind1[2];
    const SDL_GPUTextureSamplerBinding *bind;
    SDL_GPUGraphicsPipeline *pipe;
    float area;
    int need_tex, need_tmu1, is2d;

    if (!s_open || !a || !b || !c) return;
    vgl.n_tris++;
    area = (b[0] - a[0]) * (c[1] - a[1]) -
           (c[0] - a[0]) * (b[1] - a[1]);
    if (area == 0.0f || !(area == area)) {
        vgl.n_tris_culled++;
        return;
    }
    if (!s_draw_state.valid ||
        s_draw_state.version != vgl.draw_state_version) {
        need_tex = vgl_needs_texture(&need_tmu1);
        s_draw_state.bind[0].texture = need_tex ? texture_for(0) : s_white;
        s_draw_state.bind[1].texture = need_tmu1 ? texture_for(1) : s_white;
        s_draw_state.pipe = pipeline_for(0);
        s_draw_state.layerable = s_scale > 1 &&
            ((vgl.blend_rgb_src == VGL_BLEND_SRC_ALPHA &&
              vgl.blend_rgb_dst == VGL_BLEND_ONE_MINUS_SRC_ALPHA) ||
             (vgl.blend_rgb_src == VGL_BLEND_ONE &&
              vgl.blend_rgb_dst == VGL_BLEND_ZERO));
        s_draw_state.layer_pipe = s_draw_state.layerable ? pipeline_for(1)
                                                         : NULL;
        fill_ps(&s_draw_state.ps, need_tex, need_tmu1);
        s_draw_state.version = vgl.draw_state_version;
        s_draw_state.valid = 1;
    }
    ps = &s_draw_state.ps;
    pipe = s_draw_state.pipe;

    /* The sampler is per PRIMITIVE, not per draw state, because whether a
     * primitive can tile is a property of its own coordinates. It still
     * travels in `bind`, which the batch key already compares, so a run of
     * triangles that answer alike stays one batch. */
    tri_bind[0] = s_draw_state.bind[0];
    tri_bind[1] = s_draw_state.bind[1];
    tri_bind[0].sampler = sampler_for(&vgl.tmu[0],
                              s_scale > 1 && tri_no_tile(v, 0, ps));
    tri_bind[1].sampler = sampler_for(&vgl.tmu[1],
                              s_scale > 1 && tri_no_tile(v, 1, ps));
    bind = tri_bind;
    if (!pipe || !bind[0].sampler || !bind[1].sampler) return;

    /* 2D (flat w) above 1x joins the open run, drawn at the render scale and
     * at 1x; anything else ends the run as scene and folds the layer. */
    is2d = s_draw_state.layerable && s_draw_state.layer_pipe &&
           a[8] == b[8] && b[8] == c[8];
    if (is2d) {
        if (s_run && s_run_buffer != vgl.draw_buffer)
            run_commit();
        if (!s_run) {
            s_run = 1;
            s_run_buffer = vgl.draw_buffer;
        }
        pipe = s_draw_state.layer_pipe;
        tri_bind1[0] = s_draw_state.bind[0];
        tri_bind1[1] = s_draw_state.bind[1];
        tri_bind1[0].sampler = sampler_for(&vgl.tmu[0], 0);
        tri_bind1[1].sampler = sampler_for(&vgl.tmu[1], 0);
        if (!tri_bind1[0].sampler || !tri_bind1[1].sampler) return;
        build_vertices(vd1, v, ps, 1);
    } else if (s_scale > 1 && (s_run || s_layer_used[vgl.draw_buffer])) {
        scene_begins(vgl.draw_buffer);
    }
    build_vertices(vd, v, ps, s_scale);
    ps_key = *ps;
    ps_key.u[6][0] = 0; /* constant colour travels with each vertex */
    ps_key.u[1][2] = 0; /* alpha reference travels with each vertex */
    ps_key.u[7][1] = 0; /* depth bias travels with each vertex */
    if (s_batch.triangles &&
        (s_batch.pipe != pipe ||
         memcmp(&s_batch.ps, &ps_key, sizeof ps_key) != 0 ||
         memcmp(s_batch.bind, bind, sizeof s_batch.bind) != 0 ||
         s_batch.is2d != is2d ||
         (is2d && memcmp(s_batch.bind1, tri_bind1,
                         sizeof s_batch.bind1) != 0) ||
         s_batch.buffer != vgl.draw_buffer ||
         s_batch.clip_x0 != vgl.clip_x0 ||
         s_batch.clip_y0 != vgl.clip_y0 ||
         s_batch.clip_x1 != vgl.clip_x1 ||
         s_batch.clip_y1 != vgl.clip_y1))
        flush_batch();
    if (!s_batch.triangles) {
        s_batch.pipe = pipe;
        s_batch.ps = ps_key;
        memcpy(s_batch.bind, bind, sizeof s_batch.bind);
        s_batch.is2d = is2d;
        if (is2d)
            memcpy(s_batch.bind1, tri_bind1, sizeof s_batch.bind1);
        s_batch.buffer = vgl.draw_buffer;
        s_batch.clip_x0 = vgl.clip_x0; s_batch.clip_y0 = vgl.clip_y0;
        s_batch.clip_x1 = vgl.clip_x1; s_batch.clip_y1 = vgl.clip_y1;
    }
    memcpy(s_batch.vertex + s_batch.triangles * 12, vd, sizeof vd);
    if (is2d)
        memcpy(s_batch.vertex1 + s_batch.triangles * 12, vd1, sizeof vd1);
    s_batch.triangles++;
    if (s_batch.triangles == GPU_BATCH_TRIANGLES)
        flush_batch();
    s_dirty[vgl.draw_buffer] = 1;
    s_depth_dirty = 1;
}

/* One clear rectangle in surface coordinates, on the buffer being drawn. */
static void clear_rect(int x0, int y0, int x1, int y1,
                       uint32_t color, uint32_t depth)
{
    float vd[10][4];
    ps_data_t ps;
    SDL_GPURenderPass *pass;
    if (!s_open || x0 >= x1 || y0 >= y1) return;
    flush_batch();
    if (s_scale > 1)
        layer_clear(x0, y0, x1, y1, depth);
    end_pass();
    pass = draw_pass();
    if (!pass) return;
    memset(vd, 0, sizeof vd); memset(&ps, 0, sizeof ps);
    vd[0][0] = (float)x0 / vgl.w;
    vd[0][1] = (float)y0 / vgl.h;
    vd[0][2] = (float)x1 / vgl.w;
    vd[0][3] = (float)y1 / vgl.h;
    ps.u[0][0] = color; ps.u[0][1] = depth;
    SDL_BindGPUGraphicsPipeline(pass, s_clear_pipe);
    SDL_PushGPUVertexUniformData(s_cmd, 0, vd, sizeof vd);
    SDL_PushGPUFragmentUniformData(s_cmd, 0, &ps, sizeof ps);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    s_dirty[vgl.draw_buffer] = 1;
    s_depth_dirty = 1;
}

void vgl_gpu_clear(uint32_t color, uint32_t depth)
{
    if (s_lost) return;
    clear_rect(vgl.clip_x0, vgl.clip_y0, vgl.clip_x1, vgl.clip_y1,
               color, depth);
}

void vgl_gpu_clear_margins(int x0, int x1, int y0, int y1, int field_x0,
                           int field_x1)
{
    if (s_lost) return;
    /* Black at the far plane, which is what an untouched strip means: nothing
     * is there, and nothing that draws later may be occluded by it. */
    clear_rect(x0, y0, field_x0 < x1 ? field_x0 : x1, y1, 0u, 0xffffu);
    clear_rect(field_x1 > x0 ? field_x1 : x0, y0, x1, y1, 0u, 0xffffu);
}

static void blit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src,
                 int sx, int sy, int sw, int sh, SDL_GPUTexture *dst,
                 int dx, int dy, int dw, int dh, SDL_GPULoadOp load,
                 SDL_GPUFilter filter)
{
    SDL_GPUBlitInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.source.texture = src; bi.source.x = sx; bi.source.y = sy;
    bi.source.w = sw; bi.source.h = sh;
    bi.destination.texture = dst; bi.destination.x = dx; bi.destination.y = dy;
    bi.destination.w = dw; bi.destination.h = dh;
    bi.load_op = load; bi.clear_color.a = 1.0f; bi.filter = filter;
    SDL_BlitGPUTexture(cmd, &bi);
}

/* The DAC's table over the finished display, after the 2D layer and the
 * margins: the display is copied out and drawn back through the table. */
static void gamma_apply(void)
{
    unsigned w = (unsigned)(s_rw * s_scale), h = (unsigned)(s_rh * s_scale);
    SDL_GPUTexture *src[2];
    ps_data_t ps;
    uint8_t rgba[256 * 4];
    int i;

    if (!vgl_gamma.on || !s_gamma_pipe || !s_display || s_lost)
        return;
    /* Released with the display it copies (recreate_display), so a size
     * change never frees it under the frame being recorded. */
    if (!s_gamma_src) {
        s_gamma_src = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                   SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                   SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1);
        if (!s_gamma_src) {
            VGL_LOG_CAPPED(3, "gpu: cannot allocate the %ux%u gamma copy: %s",
                           w, h, SDL_GetError());
            return;
        }
    }
    if (s_gamma_gen != vgl_gamma.gen) {
        for (i = 0; i < 256; i++) {
            rgba[i * 4 + 0] = vgl_gamma.lut[0][i];
            rgba[i * 4 + 1] = vgl_gamma.lut[1][i];
            rgba[i * 4 + 2] = vgl_gamma.lut[2][i];
            rgba[i * 4 + 3] = 255;
        }
        if (!queue_rgba(s_gamma_lut, 0, 256, 1, rgba))
            return;
        s_gamma_gen = vgl_gamma.gen;
    }
    end_pass();
    if (!command())
        return;
    blit(s_cmd, s_display, 0, 0, (int)w, (int)h, s_gamma_src, 0, 0, (int)w,
         (int)h, SDL_GPU_LOADOP_DONT_CARE, SDL_GPU_FILTER_NEAREST);
    memset(&ps, 0, sizeof ps);
    src[0] = s_gamma_src;
    src[1] = s_gamma_lut;
    layer_quadn(s_display, SDL_GPU_LOADOP_DONT_CARE, NULL, s_gamma_pipe, src, 2,
                &ps, NULL);
}

static void prepare_display(SDL_GPUCommandBuffer *cmd)
{
    int vx0 = s_vx0, vy0 = s_vy0, vx1 = s_vx1, vy1 = s_vy1;
    int rw = s_rw * s_scale, rh = s_rh * s_scale;
    if (vx0 >= vx1 || vy0 >= vy1) {
        vx0 = vy0 = 0; vx1 = s_rw; vy1 = s_rh;
    }
    /* First copy the honest raster and lay the magnified 2D layer over it,
     * then overwrite only undrawn margins with their nearest valid
     * row/column. The display seam is hidden without altering LFB readback
     * or conformance captures. */
    blit(cmd, s_color[0], s_rx * s_scale, s_ry * s_scale, rw, rh,
         s_display, 0, 0, rw, rh, SDL_GPU_LOADOP_DONT_CARE,
         SDL_GPU_FILTER_NEAREST);
    if (s_layer_used[0])
        layer_compose(0, s_display, s_rx * s_scale, s_ry * s_scale);
    if (vy0 > 0)
        blit(cmd, s_display, vx0 * s_scale, vy0 * s_scale,
             (vx1 - vx0) * s_scale, s_scale,
             s_display, vx0 * s_scale, 0, (vx1 - vx0) * s_scale,
             vy0 * s_scale, SDL_GPU_LOADOP_LOAD, SDL_GPU_FILTER_NEAREST);
    if (vy1 < s_rh)
        blit(cmd, s_display, vx0 * s_scale, (vy1 - 1) * s_scale,
             (vx1 - vx0) * s_scale, s_scale,
             s_display, vx0 * s_scale, vy1 * s_scale,
             (vx1 - vx0) * s_scale, (s_rh - vy1) * s_scale,
             SDL_GPU_LOADOP_LOAD, SDL_GPU_FILTER_NEAREST);
    if (vx0 > 0)
        blit(cmd, s_display, vx0 * s_scale, 0, s_scale, rh,
             s_display, 0, 0, vx0 * s_scale, rh,
             SDL_GPU_LOADOP_LOAD, SDL_GPU_FILTER_NEAREST);
    if (vx1 < s_rw)
        blit(cmd, s_display, (vx1 - 1) * s_scale, 0, s_scale, rh,
             s_display, vx1 * s_scale, 0, (s_rw - vx1) * s_scale, rh,
             SDL_GPU_LOADOP_LOAD, SDL_GPU_FILTER_NEAREST);
    gamma_apply();
}

static int borderless_fullscreen(void)
{
    LONG style = s_hwnd ? GetWindowLongA(s_hwnd, GWL_STYLE) : 0;
    return (style & WS_POPUP) != 0 && (style & WS_CAPTION) == 0;
}

/* Alt+Enter belongs to the host and changes the Win32 handle directly. SDL
 * normally learns the new client size from WM_SIZE, but an externally restored
 * HWND can briefly leave its cached pixel extent at the old monitor size. If
 * pumping did not reconcile the two, repeat the native size through SDL in
 * window coordinates and make that request synchronous before acquiring the
 * next swapchain image. This does not choose geometry: Win32 remains the source
 * of truth, including DPI and the host's saved window placement. */
static void sync_adopted_window_size(void)
{
    RECT r;
    int nw, nh, pw, ph, lw, lh, want_w, want_h;

    vgl_gpu_pump();
    if (s_quit_posted) return;
    if (!s_hwnd || !GetClientRect(s_hwnd, &r)) return;
    nw = r.right - r.left; nh = r.bottom - r.top;
    if (nw <= 0 || nh <= 0 ||
        !SDL_GetWindowSizeInPixels(s_window, &pw, &ph) ||
        (pw == nw && ph == nh))
        return;
    if (!SDL_GetWindowSize(s_window, &lw, &lh) ||
        pw <= 0 || ph <= 0 || lw <= 0 || lh <= 0)
        return;
    want_w = MulDiv(nw, lw, pw);
    want_h = MulDiv(nh, lh, ph);
    /* Capped: this runs from present_frame(), so it is once per FRAME, and a
     * failure does not reconcile the two sizes, so the next frame tries again
     * and fails again. The success line below is self-limiting, once the
     * sizes agree the early return above takes it. */
    if (want_w <= 0 || want_h <= 0 ||
        !SDL_SetWindowSize(s_window, want_w, want_h)) {
        VGL_LOG_CAPPED(3, "gpu: could not synchronize adopted window from SDL "
                          "%dx%d to Win32 %dx%d: %s", pw, ph, nw, nh,
                       SDL_GetError());
        return;
    }
    if (!SDL_SyncWindow(s_window))
        VGL_LOG_CAPPED(3, "gpu: adopted-window resize synchronization timed "
                          "out: %s", SDL_GetError());
    vgl_gpu_pump();
    vgl_log(0, "gpu: synchronized adopted window from SDL %dx%d to Win32 "
               "%dx%d", pw, ph, nw, nh);
}

static void present_frame(void)
{
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTexture *swap = NULL;
    Uint32 cw = 0, ch = 0;
    vgl_rect_t pic, canvas;
    LARGE_INTEGER begin, end;
    if (!s_present || !s_window) return;
    /* Keep the scene and its presentation in one command buffer. SDL's GPU
     * API is designed around one command buffer per frame; submitting the
     * offscreen scene first made D3D12 recycle its per-command-buffer upload
     * storage before the following present, serializing busy Hydro frames.
     * Taken before the window is touched, so a frame that drew nothing has
     * still settled the last one before SDL resizes anything. */
    cmd = command();
    /* The host changes the adopted HWND directly for Alt+Enter. Process its
     * WM_SIZE before asking SDL for a swapchain image, otherwise SDL can keep
     * presenting at the just-left fullscreen extent after Win32 has restored
     * the configured client size. */
    sync_adopted_window_size();
    if (s_quit_posted) { submit(); return; }
    QueryPerformanceCounter(&begin);
    if (!cmd || !SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_window,
                                                       &swap, &cw, &ch)) {
        if (cmd) {
            SDL_CancelGPUCommandBuffer(cmd);
            s_cmd = NULL;
            if (s_upload_used) {
                s_upload_offset = 0;
                s_upload_cycle = 1;
                s_upload_used = 0;
            }
        }
        gpu_failed(GF_SWAPCHAIN);
        return;
    }
    QueryPerformanceCounter(&end);
    if (elapsed_ms(begin, end) >= GPU_HITCH_LOG_MS)
        vgl_log(0, "gpu: slow swapchain acquisition at frame %llu: %.3f ms",
                (unsigned long long)s_frame_index,
                elapsed_ms(begin, end));
    if (!swap) { submit(); return; }
    prepare_display(cmd);
    if (vgl_present_layout(borderless_fullscreen(), s_rw, s_rh, (int)cw,
                           (int)ch, bezel_ready(), &pic, &canvas))
        bezel_present(swap, cw, ch, &pic, &canvas);
    else
        blit(cmd, s_display, 0, 0, s_rw * s_scale, s_rh * s_scale,
             swap, pic.x, pic.y, pic.w, pic.h, SDL_GPU_LOADOP_CLEAR,
             SDL_GPU_FILTER_LINEAR);
    submit();
}

/* VCGLIDE_DISPLAY_DUMP: the picture as displayed, at the render scale and
 * eight bits a channel, for the frames VCGLIDE_DISPLAY_FRAMES ("A:B" or
 * "A:B:STEP") names.
 * Every readback is native-size, so this is the only instrument that sees
 * what the scale and the 2D layer put on screen. Headless too. */
static void dump_display(void)
{
    static SDL_GPUTransferBuffer *tb;
    static Uint32 tb_bytes;
    static unsigned lo, hi, step;
    static int parsed;
    SDL_GPUTextureTransferInfo out;
    SDL_GPUTextureRegion src;
    SDL_GPUCopyPass *copy;
    unsigned frame = vgl.frames ? vgl.frames - 1u : 0u;
    unsigned w = (unsigned)(s_rw * s_scale), h = (unsigned)(s_rh * s_scale);
    Uint32 bytes = w * h * 4u;
    const uint8_t *map;
    char path[MAX_PATH];
    FILE *f;
    unsigned i;

    if (!vgl_cfg.display_dump[0] || !s_display || s_lost)
        return;
    if (!parsed) {
        parsed = 1;
        lo = 0; hi = ~0u; step = 1;
        if (vgl_cfg.display_frames[0])
            sscanf(vgl_cfg.display_frames, "%u:%u:%u", &lo, &hi, &step);
        if (!step)
            step = 1;
        CreateDirectoryA(vgl_cfg.display_dump, NULL);
    }
    if (frame < lo || frame >= hi || (frame - lo) % step)
        return;
    if (tb && tb_bytes < bytes) {
        SDL_ReleaseGPUTransferBuffer(device(), tb);
        tb = NULL;
    }
    if (!tb) {
        SDL_GPUTransferBufferCreateInfo tci;
        memset(&tci, 0, sizeof tci);
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tci.size = bytes;
        tb = SDL_CreateGPUTransferBuffer(device(), &tci);
        tb_bytes = bytes;
        if (!tb)
            return;
    }
    if (!command())
        return;
    prepare_display(s_cmd);
    copy = SDL_BeginGPUCopyPass(s_cmd);
    if (!copy) {
        gpu_failed(GF_COPYPASS);
        return;
    }
    memset(&src, 0, sizeof src);
    src.texture = s_display; src.w = w; src.h = h; src.d = 1;
    memset(&out, 0, sizeof out);
    out.transfer_buffer = tb; out.pixels_per_row = w; out.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(copy, &src, &out);
    SDL_EndGPUCopyPass(copy);
    submit();
    map = (const uint8_t *)SDL_MapGPUTransferBuffer(device(), tb, false);
    if (!map)
        return;
    snprintf(path, sizeof path, "%.*s\\disp%05u.ppm",
             (int)(sizeof path - sizeof "\\disp.ppm" - 10),
             vgl_cfg.display_dump, frame);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (i = 0; i < w * h; i++)
            fwrite(map + (size_t)i * 4u, 1, 3, f);
        fclose(f);
    } else {
        VGL_LOG_CAPPED(3, "gpu: cannot write %s", path);
    }
    SDL_UnmapGPUTransferBuffer(device(), tb);
}

static void pace(int interval)
{
    LARGE_INTEGER now;
    LONGLONG step, remain;
    if (interval <= 0 || !s_clock_freq.QuadPart) return;
    step = s_clock_freq.QuadPart * interval / s_refresh_hz;
    QueryPerformanceCounter(&now);
    if (s_next_present.QuadPart < now.QuadPart - step)
        s_next_present = now;
    s_next_present.QuadPart += step;
    for (;;) {
        QueryPerformanceCounter(&now);
        remain = s_next_present.QuadPart - now.QuadPart;
        if (remain <= 0) break;
        if (remain * 1000 / s_clock_freq.QuadPart > 1) Sleep(1);
        else SwitchToThread();
    }
}

void vgl_gpu_swap(int interval)
{
    SDL_GPUTexture *t;
    int d;
    if (!s_open) return;
    s_frame_index++;
    if (s_lost) {
        /* Latched. Keep the game's pacing and its message pump, and let the
         * host's next health check take it down. */
        pace(interval);
        vgl_gpu_pump();
        return;
    }
    /* The physical front/back handles are about to swap, so finish encoding
    * all draws against the old mapping without submitting them yet. */
    flush_batch();
    if (s_run)
        run_commit();
    end_pass();
    t = s_color[0]; s_color[0] = s_color[1]; s_color[1] = t;
    d = s_dirty[0]; s_dirty[0] = s_dirty[1]; s_dirty[1] = d;
    t = s_layer[0]; s_layer[0] = s_layer[1]; s_layer[1] = t;
    t = s_layer_up[0]; s_layer_up[0] = s_layer_up[1]; s_layer_up[1] = t;
    d = s_layer_used[0]; s_layer_used[0] = s_layer_used[1];
    s_layer_used[1] = d;
    d = s_layer_stale[0]; s_layer_stale[0] = s_layer_stale[1];
    s_layer_stale[1] = d;
    dump_display();
    if (s_present) present_frame();
    else submit();
    pace(interval);
    s_fps_frames++;
    if (s_frame_index % 300 == 0) {
        vgl_log(0, "gpu: last 300 frames: %.1f triangles, %.1f draw batches, "
                   "%.1f texture lookups, %.2f mip uploads / %.2f MiB, "
                   "%.2f submissions per frame",
                (double)(s_batched_triangles - s_report_triangles) / 300.0,
                (double)(s_draw_batches - s_report_batches) / 300.0,
                (double)(s_tex_lookups - s_report_lookups) / 300.0,
                (double)(s_tex_uploads - s_report_uploads) / 300.0,
                (double)(s_tex_upload_bytes - s_report_upload_bytes) /
                    (1024.0 * 1024.0),
                (double)(s_submissions - s_report_submissions) / 300.0);
        s_report_triangles = s_batched_triangles;
        s_report_batches = s_draw_batches;
        s_report_lookups = s_tex_lookups;
        s_report_submissions = s_submissions;
        s_report_uploads = s_tex_uploads;
        s_report_upload_bytes = s_tex_upload_bytes;
    }
    if (s_window && !s_quit_posted) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - s_fps_start.QuadPart >= s_clock_freq.QuadPart) {
            char title[96];
            double sec = (double)(now.QuadPart - s_fps_start.QuadPart) /
                         (double)s_clock_freq.QuadPart;
            /* The backend and the internal scale are in vcglide.log, where they
             * can be read without watching a title bar go by. The window says
             * which game it is and how fast it is running. */
            vgl_window_title(title, sizeof title, s_fps_frames / sec);
            SDL_SetWindowTitle(s_window, title);
            s_fps_start = now; s_fps_frames = 0;
        }
    }
    device_frame_verdict();
    vgl_gpu_pump();
}

/* Copy a native-size target into s_download, submit, wait and map it. Takes
 * cmd whatever happens; the caller unmaps on success. */
static const uint8_t *download_native(SDL_GPUCommandBuffer *cmd,
                                      SDL_GPUTexture *texture)
{
    SDL_GPUTextureTransferInfo out;
    SDL_GPUTextureRegion src;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        gpu_failed(GF_COPYPASS);
        SDL_CancelGPUCommandBuffer(cmd);
        return NULL;
    }
    memset(&src, 0, sizeof src); src.texture = texture;
    src.w = s_w; src.h = s_h; src.d = 1;
    memset(&out, 0, sizeof out); out.transfer_buffer = s_download;
    out.pixels_per_row = s_w; out.rows_per_layer = s_h;
    SDL_DownloadFromGPUTexture(copy, &src, &out);
    SDL_EndGPUCopyPass(copy);
    if (!submit_wait(cmd)) return NULL;
    return (const uint8_t *)SDL_MapGPUTransferBuffer(device(), s_download,
                                                     false);
}

int vgl_gpu_sync_color(int buffer, uint32_t *dst, int w, int h)
{
    SDL_GPUCommandBuffer *cmd;
    const uint8_t *map;
    int x, y;
    if (!s_open || s_lost || !dst || w != s_w || h != s_h ||
        buffer < 0 || buffer > 1)
        return 0;
    s_lfb_syncs++;
    if (s_run && s_run_buffer == buffer)
        run_commit();
    if (s_layer_used[buffer]) {
        /* The caller reads the picture: the target with the layer over it,
         * both at native resolution. */
        ps_data_t ps;

        flush_batch();
        run_draw();
        end_pass();
        if (!command()) return 0;
        blit(s_cmd, s_color[buffer], 0, 0, s_iw, s_ih, s_native,
             0, 0, s_w, s_h, SDL_GPU_LOADOP_DONT_CARE, SDL_GPU_FILTER_LINEAR);
        memset(&ps, 0, sizeof ps);
        layer_quad(s_native, SDL_GPU_LOADOP_LOAD, NULL, s_over_pipe,
                   s_layer[buffer], &ps, NULL);
        submit();
        cmd = SDL_AcquireGPUCommandBuffer(device());
        if (!cmd) return 0;
    } else {
        submit();
        cmd = SDL_AcquireGPUCommandBuffer(device());
        if (!cmd) return 0;
        blit(cmd, s_color[buffer], 0, 0, s_iw, s_ih, s_native,
             0, 0, s_w, s_h, SDL_GPU_LOADOP_DONT_CARE, SDL_GPU_FILTER_LINEAR);
    }
    map = download_native(cmd, s_native);
    if (!map) return 0;
    for (y = 0; y < s_h; y++) for (x = 0; x < s_w; x++) {
        const uint8_t *p = map + ((size_t)y * s_w + x) * 4;
        dst[(size_t)y * s_w + x] =
            ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    SDL_UnmapGPUTransferBuffer(device(), s_download);
    s_dirty[buffer] = 0;
    return 1;
}

/* The depth buffer read back for a lock, at native resolution and exact.
 * Hydro's lens flare decides visibility from one depth word: 0xffff, nothing
 * drawn there, shows the flare and anything else hides it. dst keeps the last
 * readback, so one with nothing drawn since is skipped. */
int vgl_gpu_sync_depth(uint16_t *dst, int w, int h)
{
    SDL_GPUTextureSamplerBinding bind;
    SDL_GPUColorTargetInfo ct;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPURenderPass *pass;
    float vd[10][4];
    ps_data_t ps;
    vgl_tmu_t point;
    const uint8_t *map;
    if (!s_open || s_lost || !dst || w != s_w || h != s_h)
        return 0;
    if (!s_depth_dirty)
        return 1;
    s_lfb_syncs++;
    submit();
    cmd = SDL_AcquireGPUCommandBuffer(device());
    if (!cmd) return 0;
    memset(&ct, 0, sizeof ct); ct.texture = s_native_depth;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE; ct.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!pass) {
        gpu_failed(GF_RENDERPASS);
        SDL_CancelGPUCommandBuffer(cmd);
        return 0;
    }
    memset(vd, 0, sizeof vd); memset(&ps, 0, sizeof ps);
    vd[0][2] = 1.0f; vd[0][3] = 1.0f;
    ps.u[0][0] = (unsigned)s_scale;
    memset(&point, 0, sizeof point);
    point.min_filter = VGL_TEXFILTER_POINT;
    point.mag_filter = VGL_TEXFILTER_POINT;
    bind.texture = s_depth; bind.sampler = sampler_for(&point, 1);
    SDL_BindGPUGraphicsPipeline(pass, s_depth_resolve_pipe);
    SDL_BindGPUFragmentSamplers(pass, 0, &bind, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, vd, sizeof vd);
    SDL_PushGPUFragmentUniformData(cmd, 0, &ps, sizeof ps);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    map = download_native(cmd, s_native_depth);
    if (!map) return 0;
    memcpy(dst, map, (size_t)s_w * s_h * 2);
    SDL_UnmapGPUTransferBuffer(device(), s_download);
    s_depth_dirty = 0;
    return 1;
}

void vgl_gpu_upload_lfb(int buffer, const uint32_t *rgba_masked, int w, int h)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding bind;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPURenderPass *pass;
    float vd[10][4];
    ps_data_t ps;
    SDL_GPUSampler *sampler;
    vgl_tmu_t point;
    if (!s_open || s_lost || !rgba_masked || w != s_w || h != s_h ||
        buffer < 0 || buffer > 1) return;
    flush_batch();
    if (s_scale > 1) {
        /* Above 1x a written LFB pixel is 2D: it goes over the layer. */
        if (s_run)
            run_commit();
        if (!queue_rgba(s_lfb_tex, 0, s_w, s_h, rgba_masked)) return;
        memset(&ps, 0, sizeof ps);
        layer_quad(s_layer[buffer], s_layer_used[buffer]
                       ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
                   NULL, s_lfb_layer_pipe, s_lfb_tex, &ps, NULL);
        s_layer_used[buffer] = 1;
        s_layer_stale[buffer] = 1;
        s_lfb_uploads++;
        s_dirty[buffer] = 0;
        return;
    }
    end_pass();
    if (!queue_rgba(s_lfb_tex, 0, s_w, s_h, rgba_masked)) return;
    cmd = command();
    if (!cmd) return;
    memset(&ct, 0, sizeof ct); ct.texture = s_color[buffer];
    ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!pass) {
        gpu_failed(GF_RENDERPASS);
        return;
    }
    memset(vd, 0, sizeof vd); memset(&ps, 0, sizeof ps);
    vd[0][2] = 1.0f; vd[0][3] = 1.0f;
    /* Point-sample and clamp LFB page overlays. Their alpha is a written-pixel
     * flag and must not be interpolated during render-scale magnification. */
    memset(&point, 0, sizeof point);
    point.min_filter = VGL_TEXFILTER_POINT;
    point.mag_filter = VGL_TEXFILTER_POINT;
    sampler = sampler_for(&point, 1);
    bind.texture = s_lfb_tex; bind.sampler = sampler;
    SDL_BindGPUGraphicsPipeline(pass, s_overlay_pipe);
    SDL_BindGPUFragmentSamplers(pass, 0, &bind, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, vd, sizeof vd);
    SDL_PushGPUFragmentUniformData(cmd, 0, &ps, sizeof ps);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    s_lfb_uploads++;
    s_dirty[buffer] = 0;
}

void vgl_gpu_pump(void)
{
    SDL_Event ev;
    if (s_quit_posted) return;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT ||
            ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            /* SDL_PollEvent pumps the Win32 queue when its own queue empties.
             * Reposting WM_QUIT and continuing the loop lets SDL consume that
             * quit, emit SDL_EVENT_QUIT, repost it, and repeat forever. Post
             * once, stop touching the closing swapchain, and return to the
             * host; its swap wrapper owns the orderly NVRAM/audio shutdown. */
            s_quit_posted = 1;
            PostQuitMessage(0);
            break;
        }
    }
}
