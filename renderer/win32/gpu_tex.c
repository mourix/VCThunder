/* gpu_tex.c -- the GPU backend's texture cache and samplers.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * A TMU's current texture as a GPU texture: found by its identity (base,
 * LODs, aspect, format and, for a paletted one, the palette's content),
 * refreshed only in the mip levels the game rewrote, and evicted oldest
 * first. The sixteen samplers live here because the texture's address mode
 * is decided with them. gpu.c draws with what this returns.
 */
#include "gpu_int.h"

#include <string.h>

#define GPU_TEX_CACHE 1024
#define GPU_TEX_HASH 2048
#define GPU_PALETTE_KEYS 256

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

static SDL_GPUSampler *s_samplers[16];
static tex_cache_t s_tex_cache[GPU_TEX_CACHE];
static int s_tex_hash[GPU_TEX_HASH];
static tex_cache_t *s_bound_tex[VGL_TMUS];
static palette_key_t s_palette_keys[GPU_PALETTE_KEYS];
static unsigned s_palette_key_count;
static unsigned s_palette_seen_ver[VGL_TMUS];
static uint64_t s_palette_current_key[VGL_TMUS];
static uint64_t s_age;
uint64_t s_tex_lookups, s_tex_probes;

/* At render scales above one, clamp proven non-tiling primitives so invented
 * sub-pixel samples cannot wrap across a texture edge. Preserve the game's
 * address mode for tiled primitives and all scale-one rendering. */
SDL_GPUSampler *sampler_for(const vgl_tmu_t *u, int no_tile)
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

SDL_GPUTexture *texture_for(int tmu)
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

void gpu_tex_release(void)
{
    int i;
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
}
