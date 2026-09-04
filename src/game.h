/* game.h -- runtime per-title address and size table.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Zero denotes an absent anchor and must be refused. Runtime counts are bounded
 * by compile-time VCT_MAX ceilings validated by generation and again at
 * startup.
 */
#ifndef VCT_GAME_H
#define VCT_GAME_H

#include <stdint.h>

/* Ceilings for statically sized arrays. Generous on purpose: they cost 4-8 bytes
 * an entry and the alternative is a heap allocation for a table that never
 * changes. gen-gamedefs.py fails rather than emit a title that exceeds one. */
#define VCT_MAX_GLIDE       96      /* Hydro 69, Offroad 65; Glide 2.x has 127 */
#define VCT_MAX_IMP        160      /* Hydro 98 __imp__ slots                    */
#define VCT_MAX_W32         96      /* Hydro 25 direct stdcall entries           */
#define VCT_MAX_MESH_LOOPS  32      /* Hydro 13 instrumentable loop heads        */

typedef struct { uint32_t va; const char *name; int argbytes; } game_fn_t;
typedef struct { uint32_t slot; const char *name; } game_imp_t;
typedef struct { uint32_t va; const char *name; const char *group; } game_hw_t;
typedef struct { uint32_t va; int prologue; int body; } game_loop_t;

typedef struct game_profile {
    /* ---- identity ---- */
    const char *id;             /* "hydro" | "offroad": matches scripts/titles.py */
    const char *name;           /* "Hydro Thunder"                                  */
    const char *exe;            /* default image name inside the title's data dir   */
    const char *sha256;         /* the image these addresses were derived from      */
    uint32_t    file_size;      /* ...and its length, for a cheap up-front check    */
    const char *io_board;       /* "Diego" | "MagicBus": display name             */

    /* ---- image layout (PE headers + __pl_unpackrom's own stream literal) ---- */
    uint32_t image_base, code_file_off, code_size, code_va, real_code_end_va;
    uint32_t data_va, bss_va, stack_va, stack_size, span_end;

    /* ---- loader anchors ---- */
    uint32_t entry;                 /* __p_start                                  */
    uint32_t unpackrom;             /* __pl_unpackrom: expands DATA + .bss      */
    uint32_t ets_init_subsys;       /* __p_EtsInitSubsystems (replaced by us    */
    uint32_t ets_kernel_public_info; /* __p_EtsGetKernelPublicInfoPointer) ours */
    uint32_t crt_startup;           /* _mainCRTStartup: runs static ctors       */
    uint32_t main;                  /* _main                                      */
    uint32_t security_callback;     /* _security_Callback                         */

    /* ---- timebase ---- */
    uint32_t cputimestamp, queryhwcaps, hwcaps_data;

    /* ---- game loop and its phase histories ---- */
    uint32_t gameloop_target_fps, gameloop_target_fields, gameloop_odd_frame;
    uint32_t gameloop_target_frame_time;
    uint32_t gameloop_frame_seconds, gameloop_draw_seconds, gameloop_work_seconds;
    uint32_t gameloop_net_seconds, gameloop_win_seconds, gameloop_history_index;
    uint32_t gameloop_delta_frames;

    /* ---- geometry / diagnostics ---- */
    uint32_t mesh3d_drawgroup, mesh3d_testbound, mesh3d_determinelod;
    uint32_t mesh3d_vtx_hit, mesh3d_vtx_miss;
    uint32_t vec3_calcunit, xmath_sqrt;
    uint32_t water_module_init, waterfall_module_init, water_draw;

    /* ---- viewport / raster ---- */
    uint32_t viewport_fov, viewport_frustum_pixels, viewport_unclipped;
    uint32_t viewport_hres, viewport_vres;
    /* Widen frustum culling without changing the 2D viewport values. */
    uint32_t viewport_init, viewport_hres_f;
    uint32_t viewport_half_hres, viewport_half_hres_f;
    uint32_t viewport_frustum_limit, viewport_screen_limit, viewport_zscreen;
    /* Widescreen consumers that decide before the primitive-origin seam.
     * Each is present only in the title that owns it. */
    uint32_t ws_lensflare, ws_drawqueue;
    uint32_t ws_spark_cluster, ws_racer_tag;
    uint32_t ws_race_update, ws_flag_icon;
    /* Retain the cabinet raster while submitting full-screen fades and sky. */
    uint32_t screen_fade;
    /* Offroad's fade is not a leaf: it runs registered callbacks after its own
     * box, and the origin has to be back ON for those. 0 in Hydro. */
    uint32_t post_fade_callbacks;
    /* Hydro's 2D/HUD ortho path. Its geometry is authored one pixel oversize
     * and the cabinet's own clip trimmed it; under the widescreen origin that
     * overhang reaches the raster margin instead. 0 in Offroad. */
    uint32_t mesh3d_drawortho;
    uint32_t sky_viewport, sky_init;
    uint32_t init3dfx_init, init3dfx_hpixels, init3dfx_vpixels;
    uint32_t init3dfx_chipset, init3dfx_fb_bytes, init3dfx_fbi_rev;
    uint32_t init3dfx_tmu_count, init3dfx_tmu_rev, init3dfx_tmu_bytes;

    /* ---- Glide entry points the shim hooks by name ---- */
    uint32_t grsstvidmode, grsstwinopen, grbufferswap;

    /* ---- R2 asset archive ---- */
    uint32_t r2_find_name, r2_name_to_doid, r2_directory, r2_directory_count;

    /* ---- I/O board. Exactly one family is nonzero per title. ---- */
    uint32_t io_receive;            /* _diego_io_receive / _mb_receive             */
    uint32_t io_enable_comm, io_get_dips, io_get_period, io_set_period;
    uint32_t io_set_callback;
    /* Offroad's three handshake toggles; 0 on a board without them. The
     * synthesised packet echoes these back so each request retires and the
     * security exchange never arms. */
    uint32_t io_ack_write, io_ack_security, io_ack_read;
    /* The board's DAC channel 0: the signed byte the game writes its steering
     * force into, in the same send buffer the handshake toggles live in. Both
     * cabinets drive force feedback through this one cell and both scale it the
     * same way (`(int)(force * -128)` clamped to a signed byte), so the host
     * recovers -1..+1 without hooking anything the game does above it. Nonzero
     * in both supported titles; 0 would mean a board with no motor output. */
    uint32_t io_dac_out;

    /* ---- non-volatile storage (Hydro: _wrs_* raw sectors) ---- */
    uint32_t wrs_module_init, wrs_init, wrs_read_block, wrs_write_block;
    uint32_t wrs_enter_write, wrs_exit_write;

    /* ---- audio -----------------------------------------------------------
     * Hydro cuts at _audio_dspcmd_*. Offroad keeps its dsound/cookie layer and
     * replaces the 11-entry Audiolib YMF backend table instead. The table
     * address and count are derived together from _audiolib_ymf_ModuleOpen;
     * neither is a cross-title constant. */
    uint32_t ymf_init, ymf_shutdown, ymf_alloc_buffer, ymf_free_buffer, ymf_info;
    uint32_t dspcmd_init, dspcmd_play, dspcmd_stop, dspcmd_pitch, dspcmd_volume;
    uint32_t dspcmd_pan, dspcmd_master_vol, dspcmd_lr_atten;
    uint32_t ets_pic_enable, dcs_lock_depth, speaker_mode;
    uint32_t audiolib_ymf_jump_table, audiolib_jump_count;
    /* Offroad only: its development-system silent switch and the completion
     * query whose pass-through branch otherwise returns true forever. */
    uint32_t dsound_passthrough, dsound_is_playing;

    /* ETS's own ExitProcess entry point. The __imp__ slot does not cover direct
     * callers, and the CRT's exit path is one. */
    uint32_t ets_exit_process;

    /* The release build's compiled-out printf (Offroad's __printfEmpty). 0 on a
     * title that kept a real one. */
    uint32_t printf_empty;

    /* The arcade link. See net_link_disable() in win32_bind.c. */
    uint32_t network_enable, network_enabled;

    /* ether0, which is three functions and not a chipset. Both titles ask ETS
     * for the device by name, read its 0x3a byte configuration block for the
     * cabinet's own IPv4 address at +0x18, and write the block back with
     * 192.168.200.<unit + 10> in it. Nothing below Winsock is reached through
     * any of them. src/net_link.c answers all three. */
    uint32_t ets_tcp_get_device_handle, ets_tcp_get_device_cfg;
    uint32_t ets_tcp_configure_device;

    /* ---- the CODE checksum each title computes over itself -----------------
     * The link's compatibility gate: a peer whose value differs is marked
     * incompatible and its game traffic discarded. `cksum_table` is the
     * (start,end) pair list the summing loop walks, in DATA; the others are the
     * cells the answer ends up in. Complementary per title and 0 is the
     * ordinary answer for the other one: Hydro caches the sum for an accessor,
     * Offroad computes it a second time for the link. See src/cksum.c. */
    uint32_t cksum_table, cksum_result, cksum_getter;
    uint32_t cksum_link_table, cksum_link_result;
    /* The one function per title the shim answers in place of, so the value
     * that reaches the wire is the one an unmodified image would have computed
     * rather than the one this host's own patches produce. Hydro's is the
     * accessor, which is all of its readers; Offroad's is the unnamed second
     * copy of the loop, whose caller stores the result in __nMyExeChecksum.
     * Complementary, like the cells above. See cksum_install_shim(). */
    uint32_t cksum_getter_fn, cksum_link_fn;
    /* The other half of the same gate: an XOR over the R2 archive's own
     * directory, `^= entry[+0x3c] + entry[+0x00]`, accumulated as the archive
     * is opened. A pure function of the asset file, which this host neither
     * patches nor hooks, so unlike the code checksum it is expected to survive
     * this host untouched. `cksum_r2_link` is Offroad's memoised copy. */
    uint32_t cksum_r2_result, cksum_r2_link;

    /* ---- material / texture object field offsets ---- */
    uint32_t material_texture, texobj_start, texobj_evenodd, texobj_texinfo;

    /* ---- attract-sequence stage table (diagnostic; Hydro only) ---- */
    uint32_t attract_stage, attract_table, attract_stride, attract_stages;

    /* Call-through descriptor: len | (reloc_off << 8). Zero means absent or
     * not safely relocatable. */
    uint32_t hook_init3dfx, hook_queryhwcaps, hook_viewport_init;
    uint32_t hook_screen_fade;
    uint32_t hook_post_fade, hook_drawortho;
    uint32_t hook_sky_viewport, hook_sky_init;
    uint32_t hook_ws_lensflare, hook_ws_drawqueue;
    uint32_t hook_ws_spark_cluster, hook_ws_racer_tag;
    uint32_t hook_ws_race_update, hook_ws_flag_icon;
    uint32_t hook_ymf_alloc, hook_ymf_free;
    uint32_t hook_r2_find_name, hook_r2_name_to_doid;
    /* Call-timer targets. Per-title, for the reason in gen-gamedefs.py: Hydro's
     * _mesh3d_DetermineLod prologue is 5 bytes and Offroad's is 10. */
    uint32_t hook_time_sqrt, hook_time_calcunit, hook_time_testbound;
    uint32_t hook_time_lod, hook_time_drawgroup;

    /* ---- tables ---- */
    const game_fn_t   *glide;   unsigned glide_count;
    const game_imp_t  *imp;     unsigned imp_count;
    const game_imp_t  *w32;     unsigned w32_count;
    const game_hw_t   *hw;      unsigned hw_count;
    const game_loop_t *loop;    unsigned loop_count;
    const uint32_t    *arrow_material; unsigned arrow_material_count;
    const uint32_t    *func_va; unsigned func_count;
} game_profile_t;

/* The selected title. Set once by game_select() before anything is mapped. */
extern const game_profile_t *g_game;
#define G (g_game)

/* True when the title has this anchor at all. Reads as a sentence at the call
 * site (`if (!GAME_HAS(dspcmd_play))`), which is the point: the check should
 * be as short as the patch it guards, or it will be skipped. */
#define GAME_HAS(field) (G->field != 0u)

/* Every profile linked into this build, NULL-terminated. */
extern const game_profile_t *const g_profiles[];

/* Pick a profile by id, or NULL. */
const game_profile_t *game_find(const char *id);

/* Install the profile and validate it against this build's ceilings. Returns 0
 * and logs the offending table if the profile does not fit. */
int game_select(const game_profile_t *p);

/* Fill in the title's data directory and image path under data_root, accepting
 * either the multi-title layout (data/<id>/<exe>) or the flat one every pack
 * built before this refactor uses (data/<exe>). Returns 0 and says where it
 * looked if neither exists. */
int game_resolve_paths(const game_profile_t *p, const char *data_root,
                       char *data_dir, size_t data_sz, char *exe, size_t exe_sz);

/* Returns nonzero when `va` is present. When it is 0, logs one line naming the
 * subsystem, the title and the missing anchor, and returns 0, so an installer
 * reads `if (!game_require("audio", "_audio_dspcmd_Play", G->dspcmd_play))
 * return 0;` and cannot accidentally patch the null page. */
int game_require(const char *subsystem, const char *what, uint32_t va);

#endif /* VCT_GAME_H */
