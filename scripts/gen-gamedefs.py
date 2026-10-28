#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""gen-gamedefs.py -- emit one game_profile_t per title, from the binary.

READ-ONLY with respect to the game; writes generated/<title>_profile.c. The
output is DATA, so VCThunder carries several titles in one image and chooses at
startup: image layout, named anchors resolved through a candidate list, the
Glide entry points with their argument bytes, the __imp__ slots, the Win32
entries with no slot, the hardware boundaries, and the offsets and call sites
found by scanning code patterns.

    python3 scripts/gen-gamedefs.py                  # every ETS title
    python3 scripts/gen-gamedefs.py --title offroad
    python3 scripts/gen-gamedefs.py --check          # verify only, write nothing
"""
import bisect
import os
import posixpath
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from titles import get, ets_titles, REPO, MAME_DISKS       # noqa: E402
import hashlib                                             # noqa: E402
from titles import win32_titles                            # noqa: E402
from glide2_abi import FALLBACK_ARG_BYTES                  # noqa: E402
from glide2_pushes import push_argbytes                    # noqa: E402

ENTRIES_DEF = os.path.join(REPO, "renderer", "api", "entries.def")


def provider_entries():
    """The entry names vcglide implements: renderer/api/entries.def, the ONE list."""
    out = set()
    for line in open(ENTRIES_DEF, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*VGL_ENTRY\(\s*([A-Za-z0-9_]+)", line)
        if m:
            out.add(m.group(1))
    if not out:
        sys.exit(f"no VGL_ENTRY rows in {ENTRIES_DEF}")
    return out

MESH3D_LOOP_SLOTS = 12
OUTDIR = os.path.join(REPO, "generated")

# Ceilings must match src/game.h. Duplicated deliberately: the
# generator refusing is a better failure than the shim overrunning an array.
MAX = {"glide": 96, "imp": 160, "w32": 96, "loop": 32, "hud": 16, "hudtext": 16}

# --- hardware boundaries the shim implements itself -----------------------
#
# Prefix -> (group, note). Group names are what src/stubs.c switches
# on, so they encode a judgement: everything that would execute a privileged
# instruction must be stubbed before _main can be walked at all, while TIME and
# CMOS are already portable and must NOT be stubbed (GetTickCount and rdtsc work
# natively; _cmos_driver_* is a plain RAM array). Getting that split wrong is the
# difference between a shim that reaches _main and one that faults in the first
# millisecond.
#
# Prefixes that match nothing in a given title are simply absent from its table.
HW = [
    ("_ymf_",          "YMF",   "Yamaha YMF740 DS-XG -> host mixer"),
    # NOT _audio_dspcmd_. It looks like it belongs here (it is the layer that
    # actually reaches the chip), but it executes no privileged instruction: it
    # writes a 32-slot voice command table in RAM that the YMF ISR drains. Adding
    # it makes stubs_install_m1() neuter the command layer as a "fallback", so a
    # pack with audio disabled loses the game's own cookie allocation and
    # generation counter instead of just running silent. audio_install() replaces
    # these deliberately, after the stubs, and is the only thing that should.
    ("_diego_io_",     "DIEGO", "Diego board serial protocol -> host input"),
    ("_diegoio_comm_", "DIEGO", "Diego comm/thread layer -> host input"),
    ("_mb_io_comm_",   "MBIO",  "MagicBus I/O board comm/thread layer -> host input"),
    ("_pci",           "PCI",   "PCI config space (in/out 0xCF8) -> constant stubs"),
    ("_wrs_",          "DISK",  "raw 512-byte sector I/O for the settings block"),
    ("_pcspeaker_",    "SPKR",  "out 0x61: the very first thing _main touches"),
    ("_phide_",        "IDE",   "IDE controller probe"),
    ("_watchdog_",     "WDOG",  "cabinet watchdog timer"),
    ("_uart_",         "UART",  "COM1 for the I/O board link"),
    # ETS hardware drivers reached by RAW PORT I/O rather than an int gate:
    # 8259 PIC, ISR priority, the PC/AT timer/screen/keyboard drivers, the IDT,
    # and PCI config space via 0xCF8. Every one faults under Windows.
    #
    # Curated by NAME, not derived by opcode scan, and that is deliberate: a byte
    # scan reports 120 hits and even a disassembly-based one reports 88, because
    # CODE carries font bitmaps and tables that decode as `out` (_vtext_font8x8
    # "contains" twelve). Names are unambiguous where opcodes are not. The
    # vectored exception handler in main.c reports any straggler with the exact
    # symbol-lookup command, so this list grows on evidence.
    ("_EtsPic",        "ETSHW", "8259 PIC mask/EOI: cli; in; out"),
    ("_EtsInp",        "ETSHW", "raw port read"),
    ("_EtsOutp",       "ETSHW", "raw port write"),
    ("_EtsSetInterruptFlag", "ETSHW", "cli/sti"),
    ("_EtsSetISRPriority",   "ETSHW", "interrupt priority"),
    ("_EtsClearISRPriority", "ETSHW", "interrupt priority"),
    ("_EtsCheckISRPriority", "ETSHW", "interrupt priority"),
    ("_EtsSetIDTHandler",    "ETSHW", "IDT"),
    ("_EtsSaveIDTHandler",   "ETSHW", "IDT"),
    ("_EtsRestoreIDTHandler", "ETSHW", "IDT"),
    ("_EtsMarkTimeSlice",    "ETSHW", "cli"),
    ("_EtsPcKey",      "ETSHW", "8042 keyboard controller"),
    ("_EtsPcCard",     "ETSHW", "PC Card / ATA"),
    ("_EtsPCI",        "ETSHW", "PCI config space via 0xCF8"),
    ("_EtsCustomGetTimerDriver",    "ETSHW", "PC/AT timer driver"),
    ("_EtsCustomGetScreenDriver",   "ETSHW", "VGA CRTC"),
    ("_EtsCustomGetKeyboardDriver", "ETSHW", "8042 + spawns a thread"),
    ("___Pc",          "ETSHW", "PC/AT timer, screen and keyboard ISRs"),
    ("_EtsTCPNe2k",    "ETSHW", "NE2000: 116 out, 26 in"),
    # ETS object NAMING. These attach a debug string to an ETS kernel object,
    # and every one of them dereferences the handle to find the 'sHND' magic at
    # offset 0. Under the shim the handles are real Win32 handles (small
    # integers), so the dereference reads unmapped low memory. Offroad faults
    # at _EtsSetDebugName+0x38 with a handle of 0x48C within milliseconds of
    # _main.
    #
    # Only the SETTERS are stubbed. They are diagnostic-only (there is no ETS
    # monitor attached to name objects for) and return void or a status, so a
    # stub loses nothing. The getters are left alone because a caller may
    # dereference what they return, and returning 0 would move the fault rather
    # than remove it.
    ("_EtsSetDebugName",        "ETSDBG", "names an ETS object; handles are Win32 now"),
    ("_EtsSetThreadDebugName",  "ETSDBG", "names an ETS thread object"),
    ("_EtsSetCritSecDebugName", "ETSDBG", "names an ETS critical section"),
    ("__p_EventSetDebugName",   "ETSDBG", "names an ETS event"),
    ("__p_MutexSetDebugName",   "ETSDBG", "names an ETS mutex"),
    ("__p_TimerSetDebugName",   "ETSDBG", "names an ETS timer"),
    ("__p_SetThreadDebugName",  "ETSDBG", "names an ETS thread"),
    ("_cmos_driver_",  "CMOS",  "plain RAM array (portable, do NOT stub"),
    # ETSGATE is filled in below by scanning, not by prefix) see find_int_gates().
    ("_timer_driver_", "TIME",  "GetTickCount-based (portable, throttle later"),
    ("_cpuTimeStamp",  "TIME",  "rdtsc) legal in user mode, scale later"),
]

# --- named anchors --------------------------------------------------------
#
# (field, [candidate symbols], note). The first candidate that exists in the
# title wins; none matching emits 0. Candidate lists are how one table serves
# titles that solve the same problem under different names: Hydro's I/O board
# is _diegoio_comm_*, Offroad's is _mb_io_comm_*, and the shim only cares which
# function enables the control callback.
# A PATTERN THAT DOES NOT MATCH EMITS 0 AND SAYS SO. That is the whole point: an
# anchor absent from a title is a fact about the title, and the shim's
# installers check for it. Guessing a plausible address instead would produce a
# shim that patches the wrong function and reports success.
ANCHORS = [
    ("entry",             ["__p_start"],              "PE entry; call this after patching"),
    ("unpackrom",         ["__pl_unpackrom"],         "expands DATA + .bss; keep"),
    ("ets_init_subsys",   ["__p_EtsInitSubsystems"],  "REPLACE with shim init"),
    # __pl_unpackrom refuses every record whose destination overlaps either of
    # two reserved (base, length) regions it reads out of the ETS kernel public
    # info block, and reports that refusal as return code 2. The block reaches
    # it through _EtsGetSystemInfo, which copies 0x100 bytes from this gate's
    # result and copies NOTHING when the gate returns 0: leaving the game to
    # test its records against uninitialised host stack.
    ("ets_kernel_public_info", ["__p_EtsGetKernelPublicInfoPointer"],
                          "int 0xFE ax=0x254A bx=5; must return a real 0x100-byte block"),
    ("crt_startup",       ["_mainCRTStartup"],        "keep: __cinit runs static ctors"),
    ("main",              ["_main"],                  "game entry"),
    ("security_callback", ["_security_Callback"],     "never armed if the board omits security bits"),
    ("grsstvidmode",      ["_grSstVidMode"],          "OEM arcade timing; no-op in a shim"),
    # --- timebase (src/timebase.c) ---
    ("cputimestamp",      ["_cpuTimeStamp"],          "rdtsc -> synthetic 333 MHz counter"),
    ("queryhwcaps",       ["_hardware_caps_driver_QueryHardwareCaps"],
                          "call-through hook; 6-byte relocatable prologue"),
    ("hwcaps_data",       ["_HardwareCaps_Data"],
                          "+0x0c = cpu Hz, +0x10 = 1.0f/Hz seconds-per-tick"),
    # --- game loop ---
    ("gameloop_target_fps",    ["_Gameloop_fTargetFPS"], "game-selected render cadence"),
    ("gameloop_target_fields", ["_Gameloop_fTargetFieldsPerFrame"],
                               "number of 60 Hz fields per rendered frame"),
    ("gameloop_odd_frame",     ["_Gameloop_bOddFrame"], "odd/even frame flag"),
    ("gameloop_target_frame_time", ["_Gameloop_fTargetFrameTime"],
                                   "the frame limiter's deadline in seconds"),
    ("gameloop_frame_seconds", ["_Gameloop_afFrameSeconds"], "50-sample frame history"),
    ("gameloop_draw_seconds",  ["_Gameloop_afGameDrawSecs"], "50-sample draw history"),
    ("gameloop_work_seconds",  ["_Gameloop_afGameWorkSecs"], "50-sample work history"),
    ("gameloop_net_seconds",   ["_Gameloop_afNetWaitSecs"],  "50-sample net/timer history"),
    ("gameloop_win_seconds",   ["_Gameloop_afWinWorkSecs"],  "50-sample platform history"),
    ("gameloop_history_index", ["_Gameloop_nNextFrameSecs"],
                               "ring index the five phase histories are written at"),
    ("gameloop_delta_frames",  ["_Gameloop_nDeltaFrames"],
                               "_glcount_Advance stores its argument here; the 1/30 s step count"),
    ("gameloop_setdrawfunc",   ["_gameloop_SetDrawFunc"],
                               "installs the loop's draw function; drawreplay wraps what it installs"),
    ("xfm_push",               ["_xfm_Push"],
                               "every object matrix reaches the transform stack here; drawinterp blends it"),
    ("xfm_initcamerarot",      ["_xfm_InitCameraRot"],
                               "camera position + three 16-bit angles; drawinterp blends them"),
    ("sky_work",               ["_sky_Work"],
                               "pans Hydro's screen-space sky from the camera's three angles once a tick"),
    ("hud_draw",               ["_hud_Draw"],
                               "the HUD; drawinterp draws it from the real state, not the blend"),
    ("xfm_stack",              ["_Xfm_aStack"],
                               "the transform stack; element 0 is the camera, read for cuts"),
    ("mesh3d_slidest",         ["_mesh3d_SlideST"],
                               "steps a mesh's texture coordinates; drawinterp never blends what it moves"),
    ("mvplayer_inmovie",       ["_mvplayer_inMovie"],
                               "Offroad: nonzero while an attract movie plays; drawinterp stands aside"),
    ("stream_file",            ["_stream_file"],
                               "Offroad: the movie stream's open file, set before the player's flag"),
    ("mvplayer_openmovie",     ["_mvplayer_OpenMovie"],
                               "Offroad: opens a movie; an in-between draw that reaches it is abandoned"),
    ("linklist_initpool",      ["_linklist_InitPool"],
                               "threads a free-list pool; drawinterp notes where the never-blended pools live"),
    # --- geometry census ---
    ("mesh3d_drawgroup",   ["_mesh3d_DrawGroup_NoFrustumTest"],
                           "largest sampled game path; counted per frame against grDrawTriangle"),
    ("mesh3d_testbound",   ["_mesh3d_TestBoundWithFrustum"],
                           "the cull that decides whether DrawGroup runs at all"),
    ("mesh3d_determinelod", ["_mesh3d_DetermineLod"], "detail selection"),
    ("vec3_calcunit",      ["_vec3_CalcUnit"],  "one normalise; 3-8 calls/frame, not 12%"),
    ("xmath_sqrt",         ["_xmath_sqrt"],
                           "fld/fsqrt/ret: the in-situ instrumentation-overhead standard"),
    # --- viewport / raster ---
    ("viewport_fov",       ["_Viewport_fov"], "frustum field of view"),
    ("viewport_frustum_pixels", ["_Viewport_afFrustumPixels"], "frustum extent in pixels"),
    ("viewport_unclipped", ["_Viewport_afUnclippedLimits"], "viewport limits before clipping"),
    ("viewport_hres",      ["_Viewport_hres"], "full-screen viewport width"),
    ("viewport_vres",      ["_Viewport_vres"], "full-screen viewport height"),
    # The widescreen origin. One hook on the sole
    # writer of the whole _Viewport_* block widens the CULLING while the members
    # the 2D layer reads are put back to the cabinet's own values, so no 2D
    # coordinate anywhere in either title has to be corrected.
    ("viewport_init",      ["_viewport_InitToFullScreen"],
                           "sole writer of the _Viewport_* block; the widescreen hook"),
    ("viewport_hres_f",    ["_Viewport_hres_f"],  "viewport width as a float"),
    ("viewport_half_hres", ["_Viewport_half_hres"],   "viewport width / 2"),
    ("viewport_half_hres_f", ["_Viewport_half_hres_f"],
                           "viewport width / 2 as a float; the 2D layer's centre"),
    ("viewport_frustum_limit", ["_Viewport_frustum_limit"],
                           "screen-space offsets; [0] is 1.0 - half_hres_f"),
    ("viewport_screen_limit", ["_Viewport_screen_limit"],
                           "the 2D screen rect: left, right, top, bottom"),
    ("viewport_zscreen",   ["_Viewport_zscreen"],
                           "half_hres_f / tan(fov/2); the projection scale"),
    # Every consumer of `_Viewport_*` that makes a clip/read/state decision
    # before the primitive-origin seam. The host gives each one a scoped
    # coordinate or native-FOV view. The absent title emits 0 for every member
    # it does not own.
    ("ws_lensflare",       ["_anim_air_DoLensFlare"],
                           "Hydro lens-flare LFB occlusion read"),
    ("ws_drawqueue",       ["_drawqueue_Draw"],
                           "Hydro projected winner-camera line queue"),
    ("ws_spark_cluster",   ["_spark_DrawCluster"],
                           "Offroad spark projection and native-field reject"),
    ("ws_racer_tag",       ["_racer_ShowRacerTag"],
                           "Offroad racer-tag projection and native-field clip"),
    ("ws_race_update",     ["_race_UpdateDynamics"],
                           "Offroad native-FOV identity comparison"),
    ("ws_flag_icon",       ["__hud_DrawFlagIcon"],
                           "Offroad nested viewport save and restore"),
    ("ws_select_draw",     ["_wpr_select_Draw"],
                           "Hydro track/boat select: native frame over 3D"),
    # The class the widescreen origin cannot serve by keeping the game native:
    # a draw that is meant to cover the WHOLE screen. It is defined by what a
    # function DOES with a viewport width, not by which member it reads: the
    # first version of this comment said "every function outside the viewport
    # that reads _Viewport_hres", missed the sky, and shipped a mode with a
    # black band down each margin. Members so far:
    #
    #   the full-screen fade    reads _Viewport_hres, both titles
    #   Hydro's sky             reads _Viewport_half_hres_f
    #
    # Both titles build the fade from `_Viewport_hres` with a literal 0 for the
    # left edge, so it comes out one native field wide and leaves the two side
    # strips untouched. The host runs it with a wide viewport and no origin.
    # Named per title: Hydro draws the fade itself, Offroad reaches
    # _gutil_DrawSolidBox through the camera.
    ("screen_fade",        ["_gutil_DrawBlackOrWhiteScreenFade", "_camera_DoFade"],
                           "full-screen fade; spans the raster, not the native field"),
    # AND THE FADE HOOK'S SCOPE IS PER-TITLE TOO. Hydro's fade is a LEAF: it
    # calls grDrawTriangle twice and returns, so the whole function is the
    # full-screen draw. Offroad's `_camera_DoFade` draws its box and then calls
    # `_camera_CallPostFadeCallbacks`, which is where the attract high-score
    # overlay is drawn, so running the whole of _camera_DoFade with the origin
    # switched off left that overlay at raster x = game x, one dx to the left of
    # everything else, in every frame (_camera_DoFade runs once per frame from
    # _gameloop_Start whether anything is fading or not).
    #
    # Hooking `_gutil_DrawSolidBox` instead is not the answer: it has four
    # callers in Offroad and the other three draw ordinary native-field boxes.
    # The host therefore hooks the callback runner as well and puts the origin
    # back for its duration. Absent in Hydro, whose fade has no callback tail.
    ("post_fade_callbacks", ["_camera_CallPostFadeCallbacks"],
                           "runs inside the fade; the origin must be ON for it"),
    # HYDRO'S SKY IS A SCREEN QUAD, NOT A DOME. `_sky_Init` and
    # `_sky_ChangeViewport` build one quad whose corners are
    # +/- _Viewport_half_hres_f about a LITERAL 256.0 (the cabinet's own
    # screen centre) into a static block that `_sky_Work` pans per frame and
    # `_sky_Draw` submits through grDrawPlanarPolygonVertexList. With
    # half_hres_f put back to the cabinet's 256 the backdrop is exactly one
    # native field wide, so the widened world is drawn into two side strips
    # with no sky behind them: black wherever terrain and water do not cover
    # it, which is 25.3% / 25.6% of the two margins.
    #
    # Run the two builders with the wide half-width and the literal 256.0
    # centre is exactly right: the quad comes out -dx .. N+dx in game
    # coordinates, which the seam origin maps onto the whole raster. Neither
    # function is called per frame (16 camera-init sites and one track load),
    # and `_sky_Work` reads the corners back out of the block rather than
    # re-deriving them, so widening them once holds.
    #
    # OFFROAD NEEDS NEITHER: its sky is real geometry (`__DrawDomeSky`,
    # `__SkyTestTriVsFrustum`) and follows the widened `rect_norm` by itself.
    # Both anchors are absent there and the hooks are skipped.
    # THE 2D LAYER IS AUTHORED ONE PIXEL OVERSIZE AND THE CABINET'S CLIP TRIMMED
    # IT. Hydro's ortho path submits x = -1..511, y = -1..399 and relies on the
    # game's own grClipWindow(0,0,511,399) to drop the overhang. The widescreen
    # mode substitutes the raster clip so the widened 3D is not cropped, and the
    # -1 column stops being trimmed: it lands one column inside the left margin,
    # at raster x=98, which is the whole of the flicker reported there. Measured
    # by the seam's margin census: on the track-select screen this is the ONLY
    # call site drawing outside the native field. The host clamps x into [0, N]
    # for the duration of this call, which is the one place the class can be
    # named: clamping at the seam unconditionally would squash the widened 3D.
    # Offroad has no ortho path and the anchor is 0 there.
    ("mesh3d_drawortho",   ["_mesh3d_DrawOrtho"],
                           "the 2D/HUD ortho path; authored 1px oversize"),
    ("sky_viewport",       ["_sky_ChangeViewport"],
                           "rebuilds the sky quad after every viewport init"),
    # Widescreen HUD at the edges (d_hud_elements and the comment above it).
    ("hud_text_draw",      ["_hud_text_Draw"],
                           "Hydro: every race-HUD string is printed inside it"),
    ("text_printstr",      ["_text_PrintStr"],
                           "appends one string's glyph records when buffered"),
    ("gametext_print",     ["__GameTextPrint"],
                           "Offroad: prints one gametext object at its own x"),
    ("gametext_width",     ["_text_gametext_CalcWidth"],
                           "Offroad: an object's width in pixels, as the HUD lays it out"),
    ("text_flush",         ["_text_Flush"],
                           "draws every buffered glyph; Offroad clips them to the screen limit"),
    ("hud_lap_texts",      ["__hud_apLapTimeText"],
                           "Offroad: pointer to the HUD's per-lap gametext objects"),
    ("hud_num_laps",       ["__hud_nNumLaps"],
                           "Offroad: how many of those there are"),
    ("sky_init",           ["_sky_Init"],
                           "builds the same sky quad at track load"),
    ("init3dfx_init",      ["_init3dfx_Init"],
                           "post-call hook normalizes unsupported OEM raster geometry"),
    ("init3dfx_hpixels",   ["_Init3dfx_nHorizontalPixels"], "game-selected render width"),
    ("init3dfx_vpixels",   ["_Init3dfx_nVerticalPixels"],   "game-selected render height"),
    ("init3dfx_chipset",   ["_Init3dfx_nChipsetCode"],      "Glide-reported chipset"),
    ("init3dfx_fb_bytes",  ["_Init3dfx_nFBIMemBytes"],      "framebuffer memory in bytes"),
    ("init3dfx_fbi_rev",   ["_Init3dfx_nFBIRev"],           "FBI revision"),
    ("init3dfx_tmu_count", ["_Init3dfx_nTMUCount"],         "texture-unit count"),
    ("init3dfx_tmu_rev",   ["_Init3dfx_anTMURev"],          "two-element TMU revision array"),
    ("init3dfx_tmu_bytes", ["_Init3dfx_anTMUMemBytes"],     "two-element TMU-memory array"),
    ("grsstwinopen",       ["_grSstWinOpen"], "game-selected resolution drives the client size"),
    ("grbufferswap",       ["_grBufferSwap"], "frame boundary; also the message pump"),
    # --- R2 asset archive ---
    ("r2_find_name",       ["_r2file_FindObjectNameInDir"], "linear R2 directory search"),
    ("r2_name_to_doid",    ["_r2file_NameToDoid"],          "linear R2 name-to-ID search"),
    # --- I/O board ---
    # The decode entry. Offroad's MagicBus is Diego's successor and exposes the
    # layers Hydro's build inlined. _mb_encrypt_receive is a PARITY CHECK on a
    # fixed 16-byte frame, not a cipher (it hands _mb_receive the raw buffer),
    # so feeding _mb_receive directly needs no encryption, no checksum and no
    # SLIP.
    ("io_receive",         ["_diego_io_receive", "_mb_receive"],
                           "we synthesise board packets and call this directly"),
    ("io_enable_comm",     ["_diegoio_comm_EnableComm", "_mb_io_comm_EnableComm"],
                           "preserve the game's control-callback enable gate"),
    ("io_get_dips",        ["_diegoio_comm_GetCachedDIPSwitches",
                            "_mb_io_comm_GetCachedDIPSwitches"],
                           "return the emulated board's cached active-high DIP byte"),
    ("io_get_period",      ["_diegoio_comm_GetCallbackPeriodInMilliseconds"],
                           "control-driver callback cadence getter"),
    ("io_set_period",      ["_diegoio_comm_SetCallbackPeriodInMilliseconds",
                            "_mb_io_comm_SetCallbackPeriodInMilliseconds"],
                           "control-driver callback cadence setter"),
    ("io_set_callback",    ["_diegoio_comm_SetCallbackFunction",
                            "_mb_io_comm_SetCallbackFunction"],
                           "register the periodic control-driver callback"),
    # --- performance diagnostics ---
    ("water_module_init",  ["_water_ModuleInit"],     "start of the water code region"),
    ("waterfall_module_init", ["_waterfall_ModuleInit"], "end of the water code region"),
    ("water_draw",         ["_water_Draw"],           "diagnostic-only subsystem bisection"),
    # --- non-volatile storage (src/nvram.c) ---
    ("wrs_module_init",    ["_wrs_ModuleInit"],           "-> 1"),
    ("wrs_init",           ["_wrs_Init"],                 "(a,b) -> nonzero = ok"),
    ("wrs_read_block",     ["_wrs_ReadCMOSBlock"],        "(dst, len)"),
    ("wrs_write_block",    ["_wrs_WriteCMOSBlockToDisk"], "(src, len) -> nonzero = ok"),
    ("wrs_enter_write",    ["_wrs_EnterFileSystemWriteMode"], "no-op"),
    ("wrs_exit_write",     ["_wrs_ExitFileSystemWriteMode"],  "no-op"),
    # --- audio (src/audio.c) ---
    #
    # _ymf_* is NOT Hydro's cut line; the layer that actually reaches the
    # chip is one lower. _audio_dspcmd_* writes a 32-slot voice command table the
    # YMF interrupt handler drains, so replacing THAT keeps the game's own cookie
    # allocation, two-bank voice generation counter and constant-power pan law.
    # Offroad has no such layer: its _ymf_* IS the driver, with DMA and slot
    # activation. Its independent Audiolib backend table is derived below, so
    # the titles retain their own upper audio stacks and meet at the host mixer.
    ("ymf_init",           ["_ymf_Init"],   "(dcsTimerCallback) -> 0 == ok"),
    ("ymf_shutdown",       ["_ymf_Shutdown"], "silence everything"),
    ("ymf_alloc_buffer",   ["_ymf_AllocateSoundBuffer"],
                           "(pcm,bytes,rate,loop,?,index): only the loop flag is ours"),
    ("ymf_free_buffer",    ["_ymf_DeallocateSoundBuffer"], "(index)"),
    ("ymf_info",           ["_YmfInfo"], "+0x04 is the IRQ line DCS masks"),
    ("dspcmd_init",        ["_audio_dspcmd_Init"], "constant-power sine table; must still run"),
    ("dspcmd_play",        ["_audio_dspcmd_Play"],
                           "(voice, pcm, rate, bytes, loopDefault): 16-bit signed mono"),
    ("dspcmd_stop",        ["_audio_dspcmd_Stop"],        "(voice)"),
    ("dspcmd_pitch",       ["_audio_dspcmd_ChangePitch"], "(voice, 0..255; 128 = unity)"),
    ("dspcmd_volume",      ["_audio_dspcmd_ChangeVolume"], "(voice, 0..255)"),
    ("dspcmd_pan",         ["_audio_dspcmd_ChangePan"],   "(voice, 0..255)"),
    ("dspcmd_master_vol",  ["_audio_dspcmd_SetMasterVolume"], "(0..255; 255 = unity)"),
    ("dspcmd_lr_atten",    ["_audio_dspcmd_SetLeftRightAttenuators"], "(float bits L, R)"),
    ("ets_pic_enable",     ["_EtsPicEnable"],
                           "(irq, enable): DCS masks the audio IRQ as a mutex"),
    # The ETS-side ExitProcess ENTRY, not its __imp__ slot.
    #
    # win32_bind rewrites the slot, which redirects every `call [__imp__X]`:
    # but a statically linked kernel also calls its own implementations
    # DIRECTLY, and those calls do not go through the table. ETS's exit path is
    # exactly that: the CRT calls _ExitProcess@4 by address, which reaches
    # _EtsCustomExitProcess and terminates the process without any override of
    # ours being consulted. Offroad exited with status 5 and an empty log for
    # three runs because of it.
    ("ets_exit_process", ["_ExitProcess@4"],
     "ETS's own ExitProcess entry; direct callers bypass the __imp__ slot"),
    # Offroad's release build compiles printf down to __printfEmpty: a bare
    # `ret` with 346 call sites, including EVERY failure message in _GameInit
    # and _init3dfx_Init. The binary is not silent about why it stops; it was
    # BUILT silent. Pointing that one entry at the log restores the game's own
    # boot narrative and costs nothing on a title that does not have it.
    ("printf_empty", ["__printfEmpty"],
     "the release build's compiled-out printf; rerouted into the shim log"),
    # Offroad's _GameInit REFUSES TO BOOT without sound: _dsound_InitDS returns
    # -3 when _audiolib_Init cannot find the YMF, and _GameInit turns that into
    # "return 0", which unwinds _main2 and ends the game thread. That is why
    # graphics could not be reached before audio. The binary carries its own
    # answer: _dsound_passThrough is read by all 17 _dsound_* entry points and
    # written by none of them; a development-system flag that keeps the cookie
    # bookkeeping and skips every _audiolib_* call. Setting it is the game's own
    # supported silent mode, not a patch around a check.
    ("dsound_passthrough", ["_dsound_passThrough"],
     "nonzero = run the game's own silent path instead of the YMF driver"),
    ("dsound_is_playing", ["_dsound_IsCookiePLaying"],
     "pass-through returns true forever; silent fallback replaces it with completion"),
    # The arcade link. _network_Reset gets the cabinet's IP from ETS's OWN
    # TCP/IP stack (_EtsTCPGetDeviceHandle("ether0") / _EtsTCPGetDeviceCfg)
    # which is statically linked into the image but is never brought up here,
    # because bringing it up means emulating the NE2000 the cabinet had. So the
    # link cannot initialise, _network_IsInitialized() stays false, and
    # _gamenet_ModuleInit returns 0, which aborts _GameRun's whole module list
    # and drops the machine into the operator menu. _network_Enable is the one
    # function that writes _Network_bEnabled, and 0 is the standalone state the
    # other ~50 readers already handle.
    ("network_enable",   ["_network_Enable"],   "the only writer of _Network_bEnabled"),
    ("network_enabled",  ["_Network_bEnabled"], "0 = standalone; read by ~50 sites"),
    # ether0 itself, and it is three functions rather than a chipset. Both
    # titles use exactly these: name to handle, read the 0x3a byte
    # configuration block, write it back with 192.168.200.<unit + 10> in it.
    # Nothing below Winsock is reached and no packet passes through any of
    # them, so a synthetic device is a shim over three calls rather than an
    # NE2000. The registration API underneath them (_EtsTCPRegisterDevice and
    # friends) is deliberately NOT anchored: registering a real device would
    # require _EtsTCPIPInit, which is an int 0xFE gate this host stubs.
    ("ets_tcp_get_device_handle", ["_EtsTCPGetDeviceHandle"],
     "name -> device handle; 0 is what makes both link stacks give up"),
    ("ets_tcp_get_device_cfg",    ["_EtsTCPGetDeviceCfg"],
     "handle -> 0x3a byte config block; +0x18 is the IPv4 address"),
    ("ets_tcp_configure_device",  ["_EtsTCPConfigureDevice"],
     "writes the block back; 0 = success, which is what Offroad tests"),
]

# Hand-supplied per-title extras: values that are neither a symbol nor derivable
# from a code pattern this script knows. Each one is a debt, so each says why.
EXTRAS = {
    "hydro": {
        # The attract-sequence stage table, read by profile_attract_poll(). Found
        # by hand while timing the attract demo race against 86Box and never
        # re-derived; it is diagnostic-only, so it stays an EXTRA rather than an
        # assertion that could fail a build.
        "attract_stage":  0x005BB8E8,
        "attract_table":  0x001F2A26,
        "attract_stride": 24,
        "attract_stages": 32,
    },
}


def find_int_gates(t):
    """ETS reaches its kernel through software interrupts, not just the __imp__
    table: `int 0xFE` is the kernel gate and `int 0xFF` the host-services gate.
    Under Windows these raise ACCESS_VIOLATION, so every function containing one
    must be replaced; a boundary the __imp__ analysis does not show.

    A raw `CD FE`/`CD FF` byte scan has false positives (those bytes occur inside
    data and mid-instruction), so the scan is confined to the ETS namespace,
    where those bytes are what they look like."""
    syms, data = t.symbols(), t.read()
    ents = sorted((v["va"], k) for k, v in syms.items()
                  if v["seg"] == 1 and v["va"]
                  and (k.startswith("_Ets") or k.startswith("__p_Ets")
                       or k.startswith("__dx_")))
    out = []
    for i, (va, name) in enumerate(ents):
        end = ents[i + 1][0] if i + 1 < len(ents) else va + 0x200
        end = min(end, va + 0x400)
        blob = data[va - t.image_base:end - t.image_base]
        for j in range(len(blob) - 1):
            if blob[j] == 0xCD and blob[j + 1] in (0xFE, 0xFF):
                out.append((name, va, blob[j + 1]))
                break
    return out


def observed_argbytes(t, names):
    """For each name, the set of `add esp,N` immediates seen after its call sites."""
    syms, data = t.symbols(), t.read()
    tgt = {syms[n]["va"]: n for n in names if n in syms}
    seen = {n: set() for n in names}
    i = t.code_start_off
    while i < t.real_code_end_off - 11:
        if data[i] == 0xE8:
            rel = struct.unpack_from("<i", data, i + 1)[0]
            n = tgt.get(i + 5 + rel + t.image_base)
            if n:
                nx = data[i + 5:i + 11]
                if nx[0] == 0x83 and nx[1] == 0xC4:
                    seen[n].add(nx[2])
                elif nx[0] == 0x81 and nx[1] == 0xC4:
                    seen[n].add(struct.unpack_from("<I", nx, 2)[0])
        i += 1
    return seen


# --- derived scans --------------------------------------------------------
#
# Each returns its value or None. None is emitted as 0 and reported; it means
# "this title does not have this pattern", which for a cross-title generator is
# an ordinary answer and not a failure.


# --- the draw phase as a script (drawinterp, Offroad) ----------------------
#
# Offroad has no draw function to call: `_gameloop_Start` inlines its draw phase
# as a straight run of calls from `call _shadow_PerFrameStart` to
# `call _grBufferSwap`. An in-between picture is that run again, so the run is
# read out of the binary as a script the host can replay:
#
#   DS_CALL  fn, arg-kind, arg      a call with at most one cdecl argument
#   DS_STOP  cell                   end the script if the dword at cell != 0
#   DS_SKIP  cell, to               jump to step `to` if the dword at cell == 0
#
# Arguments are an immediate, a register whose constant the function set before
# the run (`xor r,r` / `mov r,imm32`), or the previous call's return value.
# Stores into cells and stack fix-ups are dropped: the replay restores memory.
# ANY other instruction makes the derivation refuse, so a title shaped
# differently gets no script rather than a wrong one.
DS_CALL, DS_STOP, DS_SKIP = 1, 2, 3
ARG_NONE, ARG_IMM, ARG_RET = 0, 1, 2
DRAW_STEPS_MAX = 24


def d_draw_script(t, log):
    """(phase_site, swap_site, steps) or (0, 0, [])."""
    import x86len
    d = t.read()
    loop = t.va("_gameloop_Start")
    shadow = t.va("_shadow_PerFrameStart")
    swap = t.va("_grBufferSwap")
    if not (loop and shadow and swap):
        return 0, 0, []
    starts = sorted(v["va"] for v in t.symbols().values()
                    if v.get("va") and v["va"] > loop)
    end = starts[0] if starts else loop + 0x1000
    off = lambda va: va - t.image_base
    regs = {}
    va, begin = loop, 0
    while va < end:
        r = x86len.decode(d, off(va), t.image_base)
        if not r:
            return 0, 0, []
        size = r[0]
        b = d[off(va):off(va) + size]
        if b[0] == 0xE8 and va + 5 + int.from_bytes(b[1:5], "little", signed=True) == shadow:
            begin = va
            break
        if size == 2 and b[0] in (0x31, 0x33) and (b[1] >> 6) == 3 and ((b[1] >> 3) & 7) == (b[1] & 7):
            regs[b[1] & 7] = 0
        elif size == 5 and 0xB8 <= b[0] <= 0xBF:
            regs[b[0] - 0xB8] = int.from_bytes(b[1:5], "little")
        va += size
    if not begin:
        return 0, 0, []
    steps, pending, labels, fixups = [], None, {}, []
    va = begin
    last_cell = None
    while True:
        labels[va] = len(steps)
        b = d[off(va):off(va) + 8]
        if b[0] == 0xE8:
            tgt = va + 5 + int.from_bytes(b[1:5], "little", signed=True)
            if tgt == swap:
                swap_site = va
                break
            kind, arg = pending if pending else (ARG_NONE, 0)
            steps.append((DS_CALL, tgt, kind, arg))
            pending = None
            va += 5
        elif b[0] == 0x68:                                   # push imm32
            pending = (ARG_IMM, int.from_bytes(b[1:5], "little")); va += 5
        elif 0x50 <= b[0] <= 0x57:                           # push reg
            r = b[0] - 0x50
            if r == 0:
                pending = (ARG_RET, 0)
            elif r in regs:
                pending = (ARG_IMM, regs[r])
            else:
                log(f"draw script: push of an unknown register at 0x{va:08X}; refused")
                return 0, 0, []
            va += 1
        elif b[0] == 0x83 and b[1] == 0xC4:                  # add esp, imm8
            va += 3
        elif b[0] == 0xA1:                                   # mov eax, [cell]
            last_cell = int.from_bytes(b[1:5], "little"); va += 5
        elif b[0] == 0xA3 or b[0] == 0x48:                   # store / dec eax
            va += 5 if b[0] == 0xA3 else 1
        elif b[0] == 0x3B and b[1] == 0xC3 and regs.get(3) == 0:   # cmp eax, ebx(=0)
            va += 2
        elif b[0] == 0x0F and b[1] == 0x85 and last_cell:    # jne out: stop
            steps.append((DS_STOP, last_cell, 0, 0)); va += 6
        elif b[0] == 0x39 and b[1] == 0x1D and regs.get(3) == 0:   # cmp [cell], ebx(=0)
            last_cell = int.from_bytes(b[2:6], "little"); va += 6
        elif b[0] == 0x74 and last_cell:                     # je short: skip
            to = va + 2 + int.from_bytes(b[1:2], "little", signed=True)
            fixups.append(len(steps))
            steps.append((DS_SKIP, last_cell, to, 0)); va += 2
        else:
            log(f"draw script: unrecognised {b[:6].hex(' ')} at 0x{va:08X}; refused")
            return 0, 0, []
        if va - begin > 0x200 or len(steps) > DRAW_STEPS_MAX:
            log("draw script: no _grBufferSwap within reach; refused")
            return 0, 0, []
    for i in fixups:
        op, cell, to, _ = steps[i]
        if to not in labels:
            log(f"draw script: skip target 0x{to:08X} is not an instruction; refused")
            return 0, 0, []
        steps[i] = (op, cell, labels[to], 0)
    return begin, swap_site, steps


def d_timer_base(t):
    """The cells `_timer_ResetHiResTimer` stamps with `_cpuTimeStamp(hi, lo)`:
    the base the frame limiter measures its tick from. Both titles open it
    `push lo; push hi; call _cpuTimeStamp`. (hi, lo) or (0, 0)."""
    reset, stamp = t.va("_timer_ResetHiResTimer"), t.va("_cpuTimeStamp")
    if not reset or not stamp:
        return 0, 0
    d = t.read()
    o = reset - t.image_base
    b = d[o:o + 15]
    if b[0] != 0x68 or b[5] != 0x68 or b[10] != 0xE8:
        return 0, 0
    if reset + 15 + int.from_bytes(b[11:15], "little", signed=True) != stamp:
        return 0, 0
    lo = int.from_bytes(b[1:5], "little")
    hi = int.from_bytes(b[6:10], "little")
    return hi, lo


# Functions whose _linklist_InitPool pools hold world-space records that are
# recycled: a slot holds one record at tick N and another at N+1, and a blend
# between them draws something neither tick has (a skid quad stretched across
# the track, a splash droplet halfway between two). drawinterp never blends them.
# Offroad's spark pool is one, and also a pool whose launch writes only the low
# byte of a word (the type at +0x10): over the allocator's leftover bytes that
# word can read as a float on both ticks, and its blend hands _spark_DrawCluster
# a type past its five-entry curve table.
NOBLEND_POOL_OWNERS = ["_skid_Init", "_particle_ResetSystem", "_spark_Init"]
NOBLEND_POOLS_MAX = 4


def d_noblend_pools(t):
    """Every pool root a NOBLEND_POOL_OWNERS function hands _linklist_InitPool,
    each the last `push imm32` before its call. A list of at most
    NOBLEND_POOLS_MAX roots; an owner whose shape differs adds none."""
    import x86len
    pool = t.va("_linklist_InitPool")
    if not pool:
        return []
    d = t.read()
    fstarts = sorted(v["va"] for v in t.symbols().values() if v.get("va"))
    roots = []
    for owner in NOBLEND_POOL_OWNERS:
        init = t.va(owner)
        if not init:
            continue
        later = [a for a in fstarts if a > init]
        end = later[0] if later else init + 0x200
        found, last, va = [], None, init
        while va < end:
            r = x86len.decode(d, va - t.image_base, t.image_base)
            if not r:
                found = []
                break
            b = d[va - t.image_base:va - t.image_base + r[0]]
            if b[0] == 0x68 and r[0] == 5:
                last = int.from_bytes(b[1:5], "little")
            elif b[0] == 0xE8 and va + 5 + int.from_bytes(b[1:5], "little", signed=True) == pool:
                if not last:
                    found = []
                    break
                found.append(last)
            va += r[0]
        roots += found
    return roots[:NOBLEND_POOLS_MAX]


def _insns(t, name, limit=0x400):
    """(va, bytes) for each instruction of `name` from its entry, decoded
    linearly for at most `limit` bytes; [] if absent or undecodable."""
    import x86len
    va = t.va(name)
    if not va:
        return []
    d, out, end = t.read(), [], va + limit
    while va < end:
        r = x86len.decode(d, va - t.image_base, t.image_base)
        if not r:
            return []
        out.append((va, d[va - t.image_base:va - t.image_base + r[0]]))
        va += r[0]
    return out


def _s8(b):
    return b - 0x100 if b >= 0x80 else b


def d_record_tables(t):
    """Hydro's effect records whose slots change owner between ticks, so
    drawinterp must tell a record that is the same thing at tick N and N+1 from
    one that is not. All zero for a title without them.

    _waterspray_Work: the droplet buffer and its walk.
        8b 0d <abs32>   mov ecx,[count]
        a1 <abs32>      mov eax,[buffer pointer cell]
        8d 68 BB        lea ebp,[eax+BB]        walk pointer = record + BB
        d9 45 VV        fld [ebp+VV]            head velocity = BB + VV
        8d 4d PP        lea ecx,[ebp+PP]        head position = BB + PP
        83 c5 SS        add ebp,SS              record stride
    _fx_splash_InitInstance: the instance table, first free slot.
        b8 <abs32>      mov eax,base ; 83 38 00  cmp [eax],0 (active at +0)
        83 c0 SS        add eax,SS ; 3d <abs32> cmp eax,end
        c7 86 <abs32> 00000000   mov [esi+base+K],0   frame counter at +K
    _fx_particles_InitSystem: per-type blocks of records.
        bb <abs32>      mov ebx,first block ; be <abs32> mov esi,type table
        8d 43 LL        lea eax,[ebx+LL] ; 89 50 MM  mov [eax+MM],edx
                        first record at block + LL + MM, active bit 0 there
        b9 <imm32>      mov ecx,records per block ; 83 c0 RR  add eax,RR
        83 c6 TT        add esi,TT ; 81 c3 <imm32> add ebx,block stride
        81 fe <abs32>   cmp esi,table end: blocks = (end - table) / TT
    """
    r = dict.fromkeys([
        "spray_cell", "spray_count", "spray_stride", "spray_pos", "spray_vel",
        "splash_base", "splash_stride", "splash_n", "splash_counter",
        "fxpart_base", "fxpart_blocks", "fxpart_block_stride", "fxpart_recs",
        "fxpart_rec_stride", "fxpart_rec_off"], 0)
    u32 = lambda b, i: int.from_bytes(b[i:i + 4], "little")

    ins = [b for _, b in _insns(t, "_waterspray_Work", 0x200)]
    sp = {}
    for b in ins:
        if b[:2] == b"\x8b\x0d" and len(b) == 6 and "count" not in sp:
            sp["count"] = u32(b, 2)
        elif b[0] == 0xa1 and len(b) == 5 and "cell" not in sp:
            sp["cell"] = u32(b, 1)
        elif b[:2] == b"\x8d\x68" and len(b) == 3 and "bias" not in sp:
            sp["bias"] = b[2]
        elif b[:2] == b"\xd9\x45" and len(b) == 3 and "bias" in sp and "vel" not in sp:
            sp["vel"] = sp["bias"] + _s8(b[2])
        elif b[:2] == b"\x8d\x4d" and len(b) == 3 and "bias" in sp and "pos" not in sp:
            sp["pos"] = sp["bias"] + _s8(b[2])
    for _, b in _insns(t, "_waterspray_Work", 0x400):
        if b[:2] == b"\x83\xc5" and len(b) == 3:
            sp["stride"] = b[2]
            break
    if all(k in sp for k in ("count", "cell", "vel", "pos", "stride")):
        r.update(spray_cell=sp["cell"], spray_count=sp["count"],
                 spray_stride=sp["stride"], spray_pos=sp["pos"],
                 spray_vel=sp["vel"])

    ins = [b for _, b in _insns(t, "_fx_splash_InitInstance", 0x98)]
    fs = {}
    for i, b in enumerate(ins):
        if (b[0] == 0xb8 and len(b) == 5 and i + 1 < len(ins) and
                ins[i + 1] == b"\x83\x38\x00" and "base" not in fs):
            fs["base"] = u32(b, 1)
        elif b[:2] == b"\x83\xc0" and len(b) == 3 and "base" in fs and "stride" not in fs:
            fs["stride"] = b[2]
        elif b[0] == 0x3d and len(b) == 5 and "stride" in fs and "end" not in fs:
            fs["end"] = u32(b, 1)
        elif (b[:2] == b"\xc7\x86" and len(b) == 10 and u32(b, 6) == 0 and
              "end" in fs and "counter" not in fs and
              fs["base"] < u32(b, 2) < fs["base"] + fs["stride"]):
            fs["counter"] = u32(b, 2) - fs["base"]
    if (len(fs) == 4 and fs["stride"] and
            (fs["end"] - fs["base"]) % fs["stride"] == 0):
        r.update(splash_base=fs["base"], splash_stride=fs["stride"],
                 splash_n=(fs["end"] - fs["base"]) // fs["stride"],
                 splash_counter=fs["counter"])

    ins = [b for _, b in _insns(t, "_fx_particles_InitSystem", 0x90)]
    fp = {}
    for b in ins:
        if b[0] == 0xbb and len(b) == 5 and "base" not in fp:
            fp["base"] = u32(b, 1)
        elif b[0] == 0xbe and len(b) == 5 and "table" not in fp:
            fp["table"] = u32(b, 1)
        elif b[:2] == b"\x8d\x43" and len(b) == 3 and "lea" not in fp:
            fp["lea"] = _s8(b[2])
        elif b[:2] == b"\x89\x50" and len(b) == 3 and "lea" in fp and "off" not in fp:
            fp["off"] = fp["lea"] + _s8(b[2])
        elif b[0] == 0xb9 and len(b) == 5 and "recs" not in fp:
            fp["recs"] = u32(b, 1)
        elif b[:2] == b"\x83\xc0" and len(b) == 3 and "rstride" not in fp:
            fp["rstride"] = b[2]
        elif b[:2] == b"\x83\xc6" and len(b) == 3 and "tstep" not in fp:
            fp["tstep"] = b[2]
        elif b[:2] == b"\x81\xc3" and len(b) == 6 and "bstride" not in fp:
            fp["bstride"] = u32(b, 2)
        elif b[:2] == b"\x81\xfe" and len(b) == 6 and "tend" not in fp:
            fp["tend"] = u32(b, 2)
    if (all(k in fp for k in ("base", "table", "off", "recs", "rstride",
                              "tstep", "bstride", "tend")) and fp["tstep"] and
            (fp["tend"] - fp["table"]) % fp["tstep"] == 0 and
            fp["off"] + fp["recs"] * fp["rstride"] <= fp["bstride"]):
        r.update(fxpart_base=fp["base"],
                 fxpart_blocks=(fp["tend"] - fp["table"]) // fp["tstep"],
                 fxpart_block_stride=fp["bstride"], fxpart_recs=fp["recs"],
                 fxpart_rec_stride=fp["rstride"], fxpart_rec_off=fp["off"])
    return r


def d_draw_site(t):
    """Hydro: the loop's one `call dword ptr [_Gameloop_pDrawFunc]`. Hooking the
    SETTER caught only the race's draw function; attract runs the default one
    _gameloop_ModuleInit stores directly. (cell, site) or (0, 0)."""
    loop, cell = t.va("_gameloop_Start"), t.va("_Gameloop_pDrawFunc")
    if not loop or not cell:
        return 0, 0
    d = t.read()
    needle = b"\xff\x15" + cell.to_bytes(4, "little")
    a = loop - t.image_base
    hits = []
    pos = d.find(needle, a, a + 0x1000)
    while pos >= 0:
        hits.append(pos)
        pos = d.find(needle, pos + 1, a + 0x1000)
    if len(hits) != 1:
        return 0, 0
    return cell, t.image_base + hits[0]

def d_arrow_materials(t):
    """_anim_land_InitArrowSign caches one (mesh, material) pair per ramp material
    ID in an unnamed .bss cell:

        e8 <rel32>   call _mesh3d_FindFirstMaterialById
        83 c4 08     add  esp,8
        85 c0        test eax,eax
        a3 <abs32>   mov  ds:<material slot>,eax
    """
    find_va, init_va = t.va("_mesh3d_FindFirstMaterialById"), t.va("_anim_land_InitArrowSign")
    if not find_va or not init_va:
        return []
    d, off = t.read(), init_va - t.image_base
    slots = []
    for i in range(off, off + 0x140):
        if d[i] != 0xE8:
            continue
        if i + 5 + struct.unpack_from("<i", d, i + 1)[0] + t.image_base != find_va:
            continue
        j = i + 5
        if d[j:j + 5] != b"\x83\xc4\x08\x85\xc0" or d[j + 5] != 0xA3:
            continue
        slots.append(struct.unpack_from("<I", d, j + 6)[0])
    return slots


def d_material_texture(t):
    """The texture a material binds. _material_SetState loads it and skips
    _tmem_Select entirely when NULL: in which case the PREVIOUS texture stays
    bound, one way a surface renders with something that is not its own texture.

        8b 7e <off>      mov  edi,[esi+off]
        85 ff / 74 xx    test edi,edi ; je skip
        57 / e8 <rel32>  push edi ; call _tmem_Select
    """
    sel, setst = t.va("_tmem_Select"), t.va("_material_SetState")
    if not sel or not setst:
        return None
    d, off = t.read(), setst - t.image_base
    for i in range(off, off + 0x400):
        if d[i:i + 2] != b"\x8b\x7e" or d[i + 3:i + 5] != b"\x85\xff":
            continue
        k = i + 7
        if d[k] != 0x57 or d[k + 1] != 0xE8:
            continue
        if k + 6 + struct.unpack_from("<i", d, k + 2)[0] + t.image_base == sel:
            return d[i + 2]
    return None


def d_texobj(t):
    """The texture object's own fields, from the grTexSource call _tmem_Select makes."""
    sel, src = t.va("_tmem_Select"), t.va("_grTexSource")
    if not sel or not src:
        return None, None, None
    d, off = t.read(), sel - t.image_base
    for i in range(off, off + 0x400):
        if d[i:i + 5] != b"\x56\x55\x57\x6a\x00" or d[i + 5] != 0xE8:
            continue
        if i + 10 + struct.unpack_from("<i", d, i + 6)[0] + t.image_base != src:
            continue
        start = even = info = None
        for j in range(i - 0x40, i):
            if d[j:j + 2] == b"\x8b\x7b":
                start = d[j + 2]
            elif d[j:j + 2] == b"\x8b\x6b":
                even = d[j + 2]
            elif d[j:j + 2] == b"\x8d\x73":
                info = d[j + 2]
        return start, even, info
    return None, None, None


def d_audiolib_table(t, log):
    """Derive Offroad's portable 11-slot audio-driver boundary.

    _audiolib_ymf_ModuleOpen copies the immutable driver table into the live
    Audiolib table as `mov ecx,count; mov esi,source; mov edi,dest; rep movsd`.
    The host patches SOURCE before the title runs, then both native ModuleOpen
    callers publish it normally. Derive the count along with the address: a
    boundary's size is per-title data exactly as its address is. CodeView names
    cross-check both operands so a plausible byte sequence is not enough.
    """
    fn = t.va("_audiolib_ymf_ModuleOpen")
    source = t.va("_Audiolib_ymf_JumpTable")
    destination = t.va("_Audiolib_jump_table")
    if not fn or not source or not destination:
        return None, None

    d, base = t.read(), fn - t.image_base
    for i in range(base, base + 0x40):
        if (d[i] != 0xB9 or d[i + 5] != 0xBE or d[i + 10] != 0xBF or
                d[i + 15:i + 17] != b"\xF3\xA5"):
            continue
        count = struct.unpack_from("<I", d, i + 1)[0]
        got_source = struct.unpack_from("<I", d, i + 6)[0]
        got_destination = struct.unpack_from("<I", d, i + 11)[0]
        if got_source != source or got_destination != destination:
            log("audiolib: ModuleOpen copy operands disagree with CodeView "
                f"(source 0x{got_source:08X}/0x{source:08X}, destination "
                f"0x{got_destination:08X}/0x{destination:08X})")
            return None, None
        if count == 0 or count > 64:
            log(f"audiolib: implausible ModuleOpen jump count {count}")
            return None, None
        return source, count

    log("audiolib: _audiolib_ymf_ModuleOpen has no validated table-copy pattern")
    return None, None


def d_dcs_lock_depth(t):
    """DCS protects its state by masking the audio IRQ, with a recursion depth
    counter next to it. _dcs_StatusCallbackLock is the out-of-line copy of a
    pattern the compiler inlined into ~40 call sites, so the counter is the only
    handle on it: audio.c must neutralise it while its own thread holds the
    lock, or the game thread reads a nonzero depth and skips the mutex."""
    va = t.va("_dcs_StatusCallbackLock")
    if va is None:
        return None
    d, off = t.read(), va - t.image_base
    if d[off] != 0xA1:
        return None
    return struct.unpack_from("<I", d, off + 1)[0]


def d_speaker_mode(t):
    """0 = mono (pan forced to centre), 1 = stereo, 2 = reversed. Read by
    _audio_dspcmd_ChangePan, which the shim replaces, so the shim has to honour
    it or a mono cabinet setting would silently become stereo."""
    va = t.va("_audio_dspcmd_ChangePan")
    if va is None:
        return None
    d, off = t.read(), va - t.image_base
    if d[off + 0x25] != 0x8B or d[off + 0x26] != 0x0D:
        return None
    return struct.unpack_from("<I", d, off + 0x27)[0]


def d_ffb_cells(t):
    """The game's OWN force-feedback state, one cell at a time, so the host can
    read the events instead of the sum.

    THE DAC IS THE SUM AND THE SUM IS NOT ENOUGH. Both titles compose their
    steering force out of NAMED units and add them together before writing the
    one signed byte at io_dac_out. A wheel wants the sum; that is the whole
    point of it. A gamepad cannot use it: the units are not interchangeable and
    the sum has already thrown away which was which, so a pad driven from the
    byte reproduces a spring it cannot render and a dither it should not.
    Four rounds of trying to un-mix it said so.

    Hydro's tune table names eight units (FFB_UNIT_INTENSITY_MASTER, _MOTOR,
    _WATER, _STEER, _FWDCAV, _REVCAV, _BOOSTER, _COLLIDE), each a float slot the
    operator can scale; _phys_SimulateSingle evaluates all eight, sums them into
    one accumulator, multiplies by MASTER and clamps to +-1. THREE OF THEM ARE
    _xmath_RandomFloat; FWDCAV, REVCAV and BOOSTER are noise, sign-flipped on
    frame parity, and they dominate the byte's high-frequency content.

    Derived from _phys_AttachSteeringFeedback, which is the one function that
    touches every cell exactly once: it clears the lot with a run of
    `C7 05 <disp32> 00000000` (mov dword [abs], 0) in a fixed order. Seven of
    them, no more and no fewer, all in .bss, which is checked rather than
    assumed; a compiler that reordered the stores would change the meaning of
    every anchor here, so the count and the order ARE the derivation and a
    surprise must refuse rather than mislabel.

    Offroad does not share the shape and does not have to: it carries a whole
    named module, and two of its globals are the event stream on their own.
    _ffbnew_SetFFBForceType validates 0 <= type < 3 and stores the current force
    type; _ffbnew_CurHitVal is written by the three collision entry points
    (_ffbnew_HumanCollide, _ffbnew_HumanCollideWithWOB, _ffbnew_WallCollide) and
    decayed by _ffbnew_FFBFunc. Both are read, never written, by this host.
    """
    out = {k: None for k in ("ffb_accum", "ffb_collide", "ffb_impulse_a",
                             "ffb_impulse_b", "ffb_force_type", "ffb_hit_val",
                             "ffb_attached", "ffb_tire_jolts", "ffb_shocks_hist",
                             "ffb_shock_vals", "ffb_mode", "ffb_tire_jolts_n",
                             "ffb_shocks_hist_n", "ffb_shock_vals_n",
                             "ffb_hit_fn", "ffb_boost_field",
                             "boost_root", "boost_hop_n", "boost_hop1",
                             "boost_hop2", "boost_field", "boost_kind",
                             "boost_index", "boost_index_n")}
    d = t.read()

    va = t.va("_phys_AttachSteeringFeedback")
    if va is not None:
        base = va - t.image_base
        zeroed, i = [], base
        while i < base + 0x70:
            if (d[i] == 0xC7 and d[i + 1] == 0x05 and
                    struct.unpack_from("<I", d, i + 6)[0] == 0):
                zeroed.append(struct.unpack_from("<I", d, i + 2)[0])
                i += 10
                continue
            i += 1
        # accumulator, collide flag, A amplitude, A state, B amplitude,
        # B state, B rate, in the order the function clears them.
        if len(zeroed) == 7 and all(a > t.image_base for a in zeroed):
            out["ffb_accum"] = zeroed[0]
            out["ffb_collide"] = zeroed[1]
            out["ffb_impulse_a"] = zeroed[3]
            out["ffb_impulse_b"] = zeroed[5]
        # The object the feedback is attached to: the cell the same function
        # compares its argument against, `A1 disp32` at the very top.
        if d[base] == 0xA1:
            out["ffb_attached"] = struct.unpack_from("<I", d, base + 1)[0]

    va = t.va("_ffbnew_SetFFBForceType")
    if va is not None:
        base = va - t.image_base
        # the single `A3 imm32` (mov [abs], eax) inside the range check
        for i in range(base, base + 0x20):
            if d[i] == 0xA3:
                out["ffb_force_type"] = struct.unpack_from("<I", d, i + 1)[0]
                break
    # The rest of Offroad's module is named, and the names ARE the derivation:
    # a symbol table recovered from the binary is not a guess. Sizes come from
    # the next symbol along, which is why they are read here rather than
    # written down.
    #
    # THE COLLISION CELL IS NOT THE MODULE. _ffbnew_CurHitVal is one term of
    # several and it is the WORST of them to sample: _ffbnew_FFBFunc runs on the
    # 60 Hz thread and zeroes it every pass, so a frame-boundary poll at 30 Hz
    # essentially never sees one, measured, a whole Offroad race reported zero
    # hits while the DAC swung -128..127. The texture terms are the ones that
    # persist: per-tire jolts written by _KinWorkHack, a suspension history, and
    # the last shock value per corner. Those can be polled.
    out["ffb_hit_val"] = t.va("_ffbnew_CurHitVal")
    out["ffb_tire_jolts"] = t.va("_ffbnew_tireJolts")
    out["ffb_shocks_hist"] = t.va("_ffbnew_shocksHist")
    out["ffb_shock_vals"] = t.va("_lastShockVals")
    out["ffb_mode"] = t.va("_ffbnew_curMode")
    out["ffb_hit_fn"] = t.va("_ffbnew_FFBFunc")

    # Hydro's boost state. The BOOSTER unit is RandomFloat gated on it and is
    # computed inline, so it reaches no cell, but the STATE does, and it is a
    # field of the boat rather than a global: _phys_ChangeBoosterState(obj,
    # state) opens `mov [ecx + disp32], eax`. The object itself is ffb_attached,
    # which _phys_AttachSteeringFeedback already gives us. Derived from that one
    # store and checked for shape rather than assumed.
    va = t.va("_phys_ChangeBoosterState")
    if va is not None:
        base = va - t.image_base
        if d[base + 8] == 0x89 and d[base + 9] == 0x81:      # mov [ecx+d32],eax
            out["ffb_boost_field"] = struct.unpack_from("<I", d, base + 10)[0]
    # WHETHER BOOST IS FIRING, as the game itself knows it: a pointer chain
    # from a named global to one field, each hop taken from the game's own
    # loads. Pressing the button is not boosting: both titles need fuel the
    # player collects, and an empty tank presses to nothing.
    #
    #   Hydro    boost SECONDS, a float that falls while the booster fires.
    #            _hud_Work hands _powerup_CollectPowerup `[player + 0x48]`
    #            (`8B 49 d8; 51; 6A 00; E8 -> CollectPowerup`) for each entry of
    #            _Player_apData, and CollectPowerup opens `mov eax,[esp+8];
    #            mov edx,[eax+d8]` and adds a pickup's seconds with its first
    #            `fld [edx+d8]`. _Player_apData holds every boat, AI included,
    #            so the entry is indexed by the cell _race_Init loads (`A1 imm32`)
    #            to pick the boat it hands _phys_AttachSteeringFeedback:
    #            _Player_nHuman, the operator menu's UNIT ID, which is not 0 on
    #            a linked cabinet's Unit 2. No index, no chain: slot 0 would be
    #            some other boat.
    #   Offroad  the nitro FLAG, non-zero while it fires. _ffbnew_FFBFunc loads
    #            the vehicle as `mov esi,[_Racer_pPlayer]` ... `mov edi,[esi+d8]`
    #            and gates its nitro force on its first `mov eax,[edi+d32];
    #            add esp,0xC; cmp eax,ebp`.
    va_c = t.va("_powerup_CollectPowerup")
    va_h = t.va("_hud_Work")
    root = t.va("_Player_apData")
    if va_c is not None and va_h is not None and root is not None:
        bc, bh = va_c - t.image_base, va_h - t.image_base
        hop1 = hop2 = field = None
        for i in range(bh, bh + 0x800):
            if (d[i] == 0xE8 and
                    i + 5 + struct.unpack_from("<i", d, i + 1)[0] == bc and
                    d[i - 6] == 0x8B and d[i - 5] == 0x49 and
                    d[i - 3:i] == b"\x51\x6a\x00"):
                hop1 = d[i - 4]
                break
        if d[bc:bc + 4] == b"\x8b\x44\x24\x08" and d[bc + 4:bc + 6] == b"\x8b\x50":
            hop2 = d[bc + 6]
            for i in range(bc, bc + 0x80):
                if d[i] == 0xD9 and d[i + 1] == 0x42:
                    field = d[i + 2]
                    break
        index = None
        va_r = t.va("_race_Init")
        va_a = t.va("_phys_AttachSteeringFeedback")
        if va_r is not None and va_a is not None:
            br, ba = va_r - t.image_base, va_a - t.image_base
            for i in range(br, br + 0x1000):
                if (d[i] == 0xE8 and
                        i + 5 + struct.unpack_from("<i", d, i + 1)[0] == ba):
                    for j in range(i - 5, i - 0x30, -1):
                        if d[j] == 0xA1:
                            index = struct.unpack_from("<I", d, j + 1)[0]
                            break
                    break
        if index is not None and index != t.va("_Player_nHuman"):
            index = None
        index_n = t.sym_floats("_Player_apData")
        if None not in (hop1, hop2, field, index, index_n):
            out.update(boost_root=root, boost_hop_n=2, boost_hop1=hop1,
                       boost_hop2=hop2, boost_field=field, boost_kind=1,
                       boost_index=index, boost_index_n=index_n)
    va_f = t.va("_ffbnew_FFBFunc")
    root = t.va("_Racer_pPlayer")
    if va_f is not None and root is not None:
        bf = va_f - t.image_base
        hop1 = field = None
        for i in range(bf, bf + 0x100):
            if (d[i] == 0x8B and d[i + 1] == 0x35 and
                    struct.unpack_from("<I", d, i + 2)[0] == root):
                for j in range(i + 6, i + 0x20):
                    if d[j] == 0x8B and d[j + 1] == 0x7E:
                        hop1 = d[j + 2]
                        break
                break
        for i in range(bf, bf + 0x800):
            if (d[i] == 0x8B and d[i + 1] == 0x87 and
                    d[i + 6:i + 11] == b"\x83\xc4\x0c\x3b\xc5"):
                field = struct.unpack_from("<I", d, i + 2)[0]
                break
        if None not in (hop1, field):
            out.update(boost_root=root, boost_hop_n=1, boost_hop1=hop1,
                       boost_hop2=0, boost_field=field, boost_kind=2,
                       boost_index=0, boost_index_n=0)
    for key, sym in (("ffb_tire_jolts_n", "_ffbnew_tireJolts"),
                     ("ffb_shocks_hist_n", "_ffbnew_shocksHist"),
                     ("ffb_shock_vals_n", "_lastShockVals")):
        out[key] = t.sym_floats(sym) if hasattr(t, "sym_floats") else None
    return out


def d_dac_out(t):
    """The outbound board cell the game writes its steering-motor command into.

    BOTH cabinets drive force feedback through one signed byte on the I/O board's
    DAC channel 0, and both titles reach it through a leaf accessor that does
    nothing but store into the send buffer: there is no UART, no queue and no
    encoding between the game's force and this cell:

        hydro    _diego_io_write_dac(chan, v)   ->  mov [0x603948], al
        offroad  _mb_write_dac(chan, v)         ->  mov [ecx + 0x28b730], dl

    So the host does not have to hook, patch or intercept anything to read the
    commanded force: it reads this cell on the same poll thread that synthesises
    the inbound packet, which is exactly where the real board would have latched
    it. The accessor stays the game's own code and the game keeps its own gating
: Hydro's "USE FORCE FEEDBACK" / intensity pair and Offroad's _ffbnew_ mode
    machine both sit ABOVE this line and are therefore honoured for free.

    Offroad indexes the cell by channel (`[ecx + disp32]`, ecx = chan, and
    _mb_write_dac rejects chan >= 2); Hydro's has the channel argument and
    ignores it. Channel 0 is steering in both, so the base IS the anchor.

    Derived, not written down: the ONE byte store with a 32-bit absolute in the
    accessor's body; `A2 imm32` (mov moffs8, al) or `88 /r` with a disp32.
    Both functions contain exactly one, which is checked rather than assumed.

    The scale is the games' own and is identical in both: the caller computes
    `(int)(force * -128.0)` clamped to a signed byte, so the host recovers
    `force = -(int8)cell / 128` in -1..+1. Hydro's _controls_SetSteeringFeedback
    and Offroad's _controldriver_SetSteeringFeedback both multiply by the literal
    -128.0f; Offroad's _ffbnew_ path writes the same byte RAW, having done the
    clamp itself.
    """
    va = t.va("_diego_io_write_dac") or t.va("_mb_write_dac")
    if va is None:
        return None
    d, base = t.read(), va - t.image_base
    found = []
    i = base
    while i < base + 0x60:
        if d[i] == 0xA2:                                   # mov moffs8, al
            found.append(struct.unpack_from("<I", d, i + 1)[0])
            i += 5
            continue
        if d[i] == 0x88:                                   # mov r/m8, r8
            mod, rm = d[i + 1] >> 6, d[i + 1] & 7
            if mod == 0 and rm == 5:                       # disp32 only
                found.append(struct.unpack_from("<I", d, i + 2)[0])
                i += 6
                continue
            if mod == 2 and rm != 4:                        # base + disp32
                found.append(struct.unpack_from("<I", d, i + 2)[0])
                i += 6
                continue
        if d[i] == 0xC3:                                   # ret; end of body
            break
        i += 1
    if len(found) != 1:
        return None
    cell = found[0]
    # A send-buffer cell lives in DATA or .bss, never in CODE. A hit inside CODE
    # would mean the scan decoded an operand as an opcode.
    if not (t.data_va <= cell < t.bss_va + t.bss_size):
        return None
    return cell


def d_mb_handshake(t):
    """The three toggles _mb_receive compares the incoming flags byte against.

    Offroad's MagicBus protocol carries three request/acknowledge handshakes in
    p[0]: eeprom write (bit 4), security (bit 5) and eeprom read (bit 6). Each
    one is retired when the byte coming BACK matches the value the game last
    sent, and each of those last-sent values lives in an unnamed .bss cell.

    A synthesised packet therefore cannot invent them: send the wrong bit and an
    eeprom read stays pending forever, or (for bit 5) the mismatch branch
    arms the security exchange, which is best left unarmed, as on Hydro.
    Echoing the game's own cell back is correct by
    construction and needs no model of the board.

    Derived, not written down: scan _mb_receive for `and r32, imm8` with each
    mask, then take the LAST `mov r8, [abs32]` before the `cmp r32, r32` that
    follows; that load is the comparison operand.

    Last, not first, and the security toggle is why. Its sequence stores the
    masked bit to a cell, reads that cell straight back, and only then loads the
    value it compares against:

        and  eax, 0x20
        mov  ds:0x28b752, al      ; store what just arrived
        mov  cl,  ds:0x28b752     ; read it back   <- taking this one is circular
        mov  dl,  ds:0x28b759     ; the last-known state <- the real operand
        cmp  ecx, edx

    Taking the first load would make the packet echo its own bit back at itself,
    which compares equal to nothing and leaves the mismatch branch armed.
    """
    fn = t.va("_mb_receive")
    if fn is None:
        return {}
    d, base = t.read(), t.va("_mb_receive") - t.image_base
    out = {}
    for mask, field in ((0x10, "io_ack_write"), (0x20, "io_ack_security"),
                        (0x40, "io_ack_read")):
        for i in range(base, base + 0x400):
            # 83 /4 ib  ->  and r32, imm8   (modrm reg field == 4)
            if d[i] != 0x83 or ((d[i + 1] >> 3) & 7) != 4 or d[i + 2] != mask:
                continue
            last = None
            for j in range(i + 3, i + 0x28):
                # 8a 05|0d|15 <abs32>  ->  mov r8, [abs32]
                if d[j] == 0x8A and d[j + 1] in (0x05, 0x0D, 0x15):
                    last = struct.unpack_from("<I", d, j + 2)[0]
                # 3b /r or 39 /r with mod==3  ->  cmp r32, r32
                elif d[j] in (0x3B, 0x39) and (d[j + 1] >> 6) == 3:
                    break
            if last is not None:
                out[field] = last
                break
    return out


def d_vtxcache(t):
    """The vertex-cache decision inside _mesh3d_DrawGroup_NoFrustumTest: the one
    `cmp dword [_Mesh3d_nCacheKey], ecx` (3b 0d <abs32>) and the jump after it.
    Fall-through is the cache HIT, the branch target is the MISS that actually
    transforms a vertex; misses/frame IS vertices transformed per frame."""
    fn, key = t.va("_mesh3d_DrawGroup_NoFrustumTest"), t.va("_Mesh3d_nCacheKey")
    if not fn or not key:
        return None, None
    d = t.read()
    base = fn - t.image_base
    i = d.find(b"\x3b\x0d" + struct.pack("<I", key), base, base + 0x2000)
    if i < 0 or d[i + 6] != 0x75:
        return None, None
    hit = i + 8 + t.image_base
    return hit, hit + struct.unpack_from("<b", d, i + 7)[0]


def _cksum_loop(t, d, va, want_result=True):
    """One instance of the CODE-checksum loop, identified by its own shape.

    Both titles compile the same source. The function opens by loading the
    FIRST 'end' field of a (start,end) pair list and then re-loads the same
    address into esi as the walking cursor:

        a1 <T>            mov eax,[T]        ; T = &table[0].end
        57 33 ff 85 c0    push edi; xor edi,edi; test eax,eax
        ...
        be <T>            mov esi,T          ; the SAME T

    That repeated immediate is the signature: it is what separates this loop
    from every other function that happens to start with an absolute load
    (Offroad's _network_GetR2Checksum starts `a1 <abs32> 85 c0`, one byte
    apart). The accumulated sum is then stored to a DATA cell:

        89 3d <R>         mov [R],edi

    Returns (table_va, result_va); result_va is None where the caller stores
    the return value instead (Offroad's link copy ends `mov eax,edi; ret`)."""
    off = va - t.image_base
    if d[off] != 0xA1:
        return None, None
    table = struct.unpack_from("<I", d, off + 1)[0]
    if d.find(b"\xbe" + struct.pack("<I", table), off, off + 0x20) < 0:
        return None, None
    # The table is a DATA object: __pl_unpackrom materialises it, and a VA
    # below data_va means the signature matched something that is not it.
    if table < t.data_va:
        return None, None
    if not want_result:
        return table, None
    i = d.find(b"\x89\x3d", off, off + 0x60)
    if i < 0:
        return table, None
    result = struct.unpack_from("<I", d, i + 2)[0]
    return table, (result if result >= t.data_va else None)


def _accessor_cell(t, d, name):
    """A `mov eax,[abs32] / ret` accessor's cell, and the accessor's own VA.

    Both titles compile the two checksum accessors to exactly that and nothing
    else, so the whole body is the derivation and there is no window to scan.
    The VA is returned alongside because src/cksum.c's shim overwrites the
    accessor with `mov eax,imm32 / ret`, which is the same six bytes: a match
    here is therefore also the proof that the six bytes are the whole function.
    Returns (None, None) when the shape does not match."""
    va = t.va(name)
    if va is None:
        return None, None
    off = va - t.image_base
    if d[off] != 0xA1 or d[off + 5] != 0xC3:
        return None, None
    cell = struct.unpack_from("<I", d, off + 1)[0]
    return (va, cell) if cell >= t.data_va else (None, None)


def d_code_cksum(t):
    """The CODE checksum each title computes over ITSELF and exchanges as the
    link's compatibility gate, and the cells it leaves the answer in.

    Hydro runs the loop inline at the top of _main and stores the sum; a later
    _gameinit_ModuleInit argument copies it to the cell _gameinit_GetCodeChecksum
    returns, and _net_link_Init puts `sum ^ 0x10020` on the wire.

    Offroad has the loop as _exe_checksum_Generate, called first thing in
    _main2, plus a second byte-identical copy over its own copy of the same
    range table that _network_ModuleInit calls and whose return value it stores
    in __nMyExeChecksum. Nothing names that copy, so it is reached the way the
    game reaches it: the call in _network_ModuleInit whose target has the shape
    above and whose result is stored by the `a3 <abs32>` immediately after.

    Every field is per-title and absent in the other; 0 is the ordinary answer.
    """
    out = dict.fromkeys(("cksum_table", "cksum_result", "cksum_getter",
                         "cksum_getter_fn", "cksum_link_table",
                         "cksum_link_result", "cksum_link_fn",
                         "cksum_r2_result", "cksum_r2_link"))
    d = t.read()

    # --- the primary loop, wherever this title keeps it ---
    for name in ("_exe_checksum_Generate", "_main"):
        va = t.va(name)
        if va is None:
            continue
        table, result = _cksum_loop(t, d, va)
        if table:
            out["cksum_table"], out["cksum_result"] = table, result
            break

    # --- the accessors' cells, and the accessor the shim answers with ---
    out["cksum_getter_fn"], out["cksum_getter"] = \
        _accessor_cell(t, d, "_gameinit_GetCodeChecksum")

    # The OTHER half of the same gate. HereIAm carries two compatibility values
    # and this is the second: an XOR over the R2 archive's own directory,
    # accumulated as the archive is opened. Unlike the code checksum it is a
    # pure function of the asset file, which this host neither patches nor
    # hooks, so it is expected to survive unchanged. `__nMyR2Checksum` is the
    # memoised copy _network_ModuleInit puts on the wire; Hydro has no
    # equivalent, because _net_link_Init reads the accessor directly.
    _, out["cksum_r2_result"] = _accessor_cell(
        t, d, "_r2file_GetComprehensiveChecksum")
    out["cksum_r2_link"] = t.va("__nMyR2Checksum")

    # --- the link's own copy, via the call that produces __nMyExeChecksum ---
    va = t.va("_network_ModuleInit")
    if va is not None:
        base = va - t.image_base
        for i in range(base, base + 0x100):
            if d[i] != 0xE8 or d[i + 5] != 0xA3:
                continue
            tgt = i + 5 + struct.unpack_from("<i", d, i + 1)[0] + t.image_base
            table, _ = _cksum_loop(t, d, tgt, want_result=False)
            if not table:
                continue
            cell = struct.unpack_from("<I", d, i + 6)[0]
            out["cksum_link_table"] = table
            out["cksum_link_result"] = cell if cell >= t.data_va else None
            # The copy itself, which is what the shim overwrites: its caller
            # stores whatever it returns, so answering here reaches every
            # reader of the cell without touching any of them.
            out["cksum_link_fn"] = tgt
            break

    return out


def _prologue_len(data, off, need=5):
    """Bytes covered by WHOLE instructions from off, using a deliberately tiny
    whitelist. Returns 0 unless `need` bytes are reached with encodings that are
    provably position-independent: no relative operand can hide in them, which
    is what hook_call_through requires when it relocates a prologue."""
    n = 0
    while n < need + 8:
        op = data[off + n]
        if op in (0x8B, 0x89, 0x8D, 0x33, 0x31, 0x3B, 0x2B, 0x03, 0x85,
                  0x88, 0x8A, 0x01, 0x29, 0x39, 0x21, 0x09) or 0xD8 <= op <= 0xDF:
            modrm = data[off + n + 1]
            mod, rm = modrm >> 6, modrm & 7
            ln = 2 + (1 if (mod != 3 and rm == 4) else 0)
            if mod == 1:
                ln += 1
            elif mod == 2:
                ln += 4
            elif mod == 0 and rm == 5:
                ln += 4
            n += ln
        elif op == 0x83:
            modrm = data[off + n + 1]
            mod, rm = modrm >> 6, modrm & 7
            ln = 3 + (1 if (mod != 3 and rm == 4) else 0)
            if mod == 1:
                ln += 1
            elif mod == 2:
                ln += 4
            n += ln
        elif 0x50 <= op <= 0x5F:
            n += 1
        else:
            return 0
        if n >= need:
            return n
    return 0


def _hook_prologue(data, off, need=5):
    """The prologue metadata src/patch.c:hook_call_through() needs to relocate a
    function entry, as `len | (reloc_off << 8)`; 0 means "not hookable, refuse".

    This exists because a prologue LENGTH IS PER-TITLE DATA in exactly the way an
    address is, and it was the one thing the port still hardcoded. Every C hook
    site carried a constant read off Hydro, and Offroad's _init3dfx_Init opens
    `81 ec 94 00 00 00` (6 bytes) where Hydro's opens `a1 <abs32>` (5). Copying
    five bytes of a six-byte instruction into the trampoline built
    `sub esp,0xE9000094` (the jmp's own opcode swallowed as the immediate),
    which moved esp 385 MB and killed the process so hard that no exception could
    be delivered on the way out. That was Offroad's entire "dies in video init".

    Two differences from _prologue_len(), which stays as it is for the loop-head
    census: a wider whitelist (a real function entry is not the tiny subset a
    loop head is), and ONE relative-operand instruction may be relocated, since
    Offroad's _hardware_caps_driver_QueryHardwareCaps reaches five bytes only by
    including a `call rel32`. `reloc_off` is where its displacement sits inside
    the copied bytes; patch.c rewrites it for the trampoline's address. Offset 0
    can never hold a displacement, so 0 unambiguously means "none"."""
    n, reloc = 0, 0

    def modrm_len(base, at):
        modrm = data[at + 1]
        mod, rm = modrm >> 6, modrm & 7
        ln = base + (1 if (mod != 3 and rm == 4) else 0)
        if mod == 1:
            ln += 1
        elif mod == 2:
            ln += 4
        elif mod == 0 and rm == 5:
            ln += 4
        return ln

    while n < need + 8:
        at = off + n
        op = data[at]
        if op in (0x8B, 0x89, 0x8D, 0x33, 0x31, 0x3B, 0x2B, 0x03, 0x85,
                  0x88, 0x8A, 0x01, 0x29, 0x39, 0x21, 0x09, 0x0B, 0x23,
                  0x84, 0x87, 0x63) or 0xD8 <= op <= 0xDF:
            n += modrm_len(2, at)                      # r/m, r
        elif op == 0x83:
            n += modrm_len(3, at)                      # group1 r/m32, imm8
        elif op in (0x81, 0xC7):
            n += modrm_len(6, at)                      # group1/mov r/m32, imm32
        elif op in (0xC6, 0x80):
            n += modrm_len(3, at)                      # byte forms, imm8
        elif op in (0xC0, 0xC1):
            n += modrm_len(3, at)                      # shift r/m32, imm8
        elif op in (0xD1, 0xD3):
            n += modrm_len(2, at)
        elif 0x50 <= op <= 0x5F or op in (0x90, 0x99, 0x98, 0xFC, 0xFD):
            n += 1
        elif 0xB8 <= op <= 0xBF:
            n += 5                                     # mov r32, imm32
        elif 0xB0 <= op <= 0xB7:
            n += 2
        elif 0xA0 <= op <= 0xA3:
            n += 5                                     # mov eax, moffs32
        elif op == 0x68:
            n += 5                                     # push imm32
        elif op == 0x6A:
            n += 2                                     # push imm8
        elif op in (0xE8, 0xE9):                       # call/jmp rel32
            if reloc:
                return 0                               # only one is supported
            reloc = n + 1
            n += 5
        else:
            return 0
        if n >= need:
            if n > 0xFF or reloc > 0xFF:
                return 0
            return n | (reloc << 8)
    return 0


def d_hooks(t, log):
    """Per-title prologue metadata for every entry patch.c relocates rather than
    simply overwrites. An absent function emits 0, and so does a present one
    whose first five bytes cannot be relocated: the installer must then refuse,
    exactly as it does for an absent address."""
    d = t.read()
    sites = [
        ("hook_init3dfx",        "_init3dfx_Init"),
        ("hook_queryhwcaps",     "_hardware_caps_driver_QueryHardwareCaps"),
        ("hook_viewport_init",   "_viewport_InitToFullScreen"),
        ("hook_screen_fade_hydro",   "_gutil_DrawBlackOrWhiteScreenFade"),
        ("hook_screen_fade_offroad", "_camera_DoFade"),
        ("hook_post_fade",       "_camera_CallPostFadeCallbacks"),
        ("hook_drawortho",       "_mesh3d_DrawOrtho"),
        ("hook_sky_viewport",    "_sky_ChangeViewport"),
        ("hook_sky_init",        "_sky_Init"),
        ("hook_ws_lensflare",    "_anim_air_DoLensFlare"),
        ("hook_ws_drawqueue",    "_drawqueue_Draw"),
        ("hook_ws_spark_cluster", "_spark_DrawCluster"),
        ("hook_ws_racer_tag",    "_racer_ShowRacerTag"),
        ("hook_ws_race_update",  "_race_UpdateDynamics"),
        ("hook_ws_flag_icon",    "__hud_DrawFlagIcon"),
        ("hook_ws_select_draw",  "_wpr_select_Draw"),
        ("hook_hud_text_draw",   "_hud_text_Draw"),
        ("hook_text_printstr",   "_text_PrintStr"),
        ("hook_gametext_print",  "__GameTextPrint"),
        ("hook_text_flush",      "_text_Flush"),
        ("hook_setdrawfunc",     "_gameloop_SetDrawFunc"),
        ("hook_xfm_push",        "_xfm_Push"),
        ("hook_xfm_initcamerarot", "_xfm_InitCameraRot"),
        ("hook_sky_work",        "_sky_Work"),
        ("hook_hud_draw",        "_hud_Draw"),
        ("hook_mesh3d_slidest",  "_mesh3d_SlideST"),
        ("hook_mvplayer_openmovie", "_mvplayer_OpenMovie"),
        ("hook_linklist_initpool", "_linklist_InitPool"),
        ("hook_ymf_alloc",       "_ymf_AllocateSoundBuffer"),
        ("hook_ymf_free",        "_ymf_DeallocateSoundBuffer"),
        ("hook_r2_find_name",    "_r2file_FindObjectNameInDir"),
        ("hook_r2_name_to_doid", "_r2file_NameToDoid"),
        # Offroad's force-feedback tick. It is hooked to READ, not to change:
        # it consumes _ffbnew_CurHitVal and then zeroes it, on the game's own
        # 60 Hz thread, so every collision is gone before a 30 Hz poll from the
        # frame boundary can see it, measured, two full races reported zero
        # hits while the DAC swung -128..127.
        ("hook_ffb_hit",         "_ffbnew_FFBFunc"),
        # The five call-timer targets. Their prologue lengths are derived per
        # title and never written down: Offroad's _mesh3d_DetermineLod opens
        # `mov eax,[esp+4]` (4) then `mov ecx,[eax+0xd8]` (6), so Hydro's 5 lands
        # one byte inside the second instruction. A boundary's SIZE is per-title
        # data exactly as its ADDRESS is.
        ("hook_time_sqrt",       "_xmath_sqrt"),
        ("hook_time_calcunit",   "_vec3_CalcUnit"),
        ("hook_time_testbound",  "_mesh3d_TestBoundWithFrustum"),
        ("hook_time_lod",        "_mesh3d_DetermineLod"),
        ("hook_time_drawgroup",  "_mesh3d_DrawGroup_NoFrustumTest"),
    ]
    out = {}
    for field, name in sites:
        va = t.va(name)
        if not va:
            out[field] = None
            continue
        meta = _hook_prologue(d, va - t.image_base)
        out[field] = meta or None
        if not meta:
            log(f"hook: {name} @ 0x{va:08X} has no relocatable prologue "
                f"({d[va - t.image_base:va - t.image_base + 8].hex(' ')}) "
                f"-- the shim must refuse this hook")
    return out



# --- widescreen HUD at the edges ------------------------------------------
#
# A race HUD is authored for 512 columns: dials hug the 4:3 edges. Moving one
# to the wide edge means giving each ELEMENT one horizontal offset, decided by
# where it sits, so the host needs the element boundaries (the functions that
# draw one element each) and, for text, the place the string's position lives
# before it reaches the seam, because both titles buffer HUD text and draw it
# later in one flush where strings can no longer be told apart.

def _calls_in(t, d, va, starts, known):
    """Direct call targets inside one function, in order. `starts` is the sorted
    function-start list that bounds it; a target must be in `known`, which
    rejects E8 bytes inside immediates. `known=None` accepts any target in
    CODE, for a callee with one call site (func_table needs two); the caller
    must then filter by what the target does."""
    i = bisect.bisect_right(starts, va)
    end = starts[i] if i < len(starts) else va + 0x1000
    out, off = [], va - t.image_base
    for at in range(off, end - t.image_base - 4):
        if d[at] == 0xE8:
            tgt = at + 5 + struct.unpack_from("<i", d, at + 1)[0] + t.image_base
            if (tgt in known) if known is not None else \
                    (t.code_va <= tgt < t.real_code_end_va):
                out.append(tgt)
    return out


def d_text_glyphs(t, log):
    """Hydro: where `_text_PrintStr` appends its glyph records. With
    `_Text_nAttr & 0x100` (every HUD string: `_text_SetFont` sets 0x104) a
    string is not drawn but appended to a buffer `_text_Flush` draws later:

        mov eax, [count]  ; and eax, 0xffff         the record count (u16)
        lea eax, [eax+eax*4] ; lea eax, [eax*8+buf]  40-byte records
        mov edx, [_Text_fX] ... mov [eax+X], edx     the pen x of each glyph

    Offroad's records are 64 bytes behind a different load and do not match;
    its HUD text is moved by object instead (d_gametext)."""
    keys = ("text_glyph_count", "text_glyph_buf", "text_glyph_stride", "text_glyph_x")
    out = dict.fromkeys(keys)
    va, fx = t.va("_text_PrintStr"), t.va("_Text_fX")
    if not va or not fx:
        return out
    d = t.read()
    body = d[va - t.image_base:va - t.image_base + 0x400]
    m = re.search(rb"\xa1(....)\x25\xff\xff\x00\x00", body, re.S)
    n = re.search(rb"\x8d\x04\x80\x8d\x04\xc5(....)", body, re.S)
    x = re.search(rb"\x8b\x15" + re.escape(struct.pack("<I", fx)) +
                  rb".{0,12}?\x89\x50(.)", body, re.S)
    if not (m and n and x and m.start() < n.start() < x.start()):
        return out
    out["text_glyph_count"] = struct.unpack("<I", m.group(1))[0]
    out["text_glyph_buf"] = struct.unpack("<I", n.group(1))[0]
    out["text_glyph_stride"] = 5 * 8
    out["text_glyph_x"] = x.group(1)[0]
    log(f"HUD text glyphs           : count @0x{out['text_glyph_count']:08X} "
        f"buf @0x{out['text_glyph_buf']:08X}, {out['text_glyph_stride']}-byte "
        f"records, pen x at +{out['text_glyph_x']}")
    return out


def d_finish_stats(t, log):
    """Hydro: `_hud_text_SetupFinishStats` turns the result screen on by
    setting one bit in `_hud_text_Draw`'s flags word:

        mov eax, [flags] ... or eax, BIT ... mov [flags], eax

    While it is set the text pass draws the result composition (your time,
    best time, place, new high score), which is centred and must not split."""
    va = t.va("_hud_text_SetupFinishStats")
    if not va:
        return None, None
    d = t.read()
    body = d[va - t.image_base:va - t.image_base + 0x60]
    m = re.search(rb"\xa1(....).{0,24}?\x0d(....).{0,24}?\xa3\1", body, re.S)
    if not m:
        raise SystemExit(f"gen-gamedefs: {t.id} _hud_text_SetupFinishStats: "
                         f"no flags read-or-write found")
    flags = struct.unpack("<I", m.group(1))[0]
    bit = struct.unpack("<I", m.group(2))[0]
    log(f"HUD finish stats          : flags @0x{flags:08X} bit 0x{bit:X}")
    return flags, bit


def d_tracking_cards(t, log):
    """Hydro: `_boats_DrawAllTrackingNumbers` puts a card on the screen's edge
    for each boat beside or behind the camera, one `_mesh3d_DrawOrtho(mesh, x,
    y, ...)` each, x about the field's centre. A side card's x is a literal
    pushed just before the mesh; a bottom card's is the rival's sideways
    direction times a constant, the last `fmul m32` before the call:

        push imm32 ; push r32 ; call _mesh3d_DrawOrtho          side, x = +-k'
        fmul [k] .. fstp [esp] ; push r32 ; call ...            bottom, |x| <= k

    Returns (ra_lo, ra_hi, &k): the span of the calls' return addresses and
    where k lives. Offroad places its racer tags by projecting against the
    viewport, which the ws_racer_tag repair already widens."""
    va, ortho = t.va("_boats_DrawAllTrackingNumbers"), t.va("_mesh3d_DrawOrtho")
    if not va or not ortho:
        return None, None, None
    d = t.read()
    starts = func_table(t)
    end = starts[bisect.bisect_right(starts, va)] - t.image_base
    lo = va - t.image_base
    ras, sides, ks = [], set(), set()
    for at in range(lo, end - 4):
        if d[at] != 0xE8 or (at + 5 + struct.unpack_from("<i", d, at + 1)[0]
                             + t.image_base) != ortho:
            continue
        if not 0x50 <= d[at - 1] <= 0x57:
            raise SystemExit(f"gen-gamedefs: {t.id} tracking card call at "
                             f"0x{at + t.image_base:08X}: no mesh push before it")
        ras.append(at + 5 + t.image_base)
        if d[at - 6] == 0x68:
            sides.add(abs(struct.unpack_from("<f", d, at - 5)[0]))
            continue
        m = None
        for m in re.finditer(rb"\xd8\x0d(....)", d[at - 0x30:at], re.S):
            pass
        if not m:
            raise SystemExit(f"gen-gamedefs: {t.id} tracking card call at "
                             f"0x{at + t.image_base:08X}: no x scale before it")
        ks.add(struct.unpack("<I", m.group(1))[0])
    if len(sides) != 1 or len(ks) != 1 or len(ras) < 2:
        raise SystemExit(f"gen-gamedefs: {t.id} _boats_DrawAllTrackingNumbers: "
                         f"expected one side x and one bottom scale, found "
                         f"{sorted(sides)} and {[hex(k) for k in ks]}")
    k = ks.pop()
    kf = struct.unpack_from("<f", d, k - t.image_base)[0]
    side = sides.pop()
    if not 0.0 < kf <= side < 512.0:
        raise SystemExit(f"gen-gamedefs: {t.id} tracking cards: bottom scale "
                         f"{kf} and side x {side} do not nest")
    log(f"tracking cards            : {len(ras)} ortho calls, sides at x "
        f"+-{side:g}, bottom x scale @0x{k:08X} = {kf:g}")
    return min(ras), max(ras), k


def d_gametext(t, log):
    """Offroad: `__GameTextPrint(obj)` sets the font, scale and attributes from
    a gametext object and calls `_text_PrintF(obj->x, obj->y, obj->str)`; the
    object's style word goes to `_Text_nStyle`, whose bit 4 centres the string
    on x (`x -= width * 0.5`, both titles). Both offsets are read from the
    function rather than assumed."""
    out = {"gametext_x": None, "gametext_style": None}
    va, st, pf = t.va("__GameTextPrint"), t.va("_Text_nStyle"), t.va("_text_PrintF")
    if not va or not st or not pf:
        return out
    d = t.read()
    off = va - t.image_base
    body = d[off:off + 0xB0]
    s = re.search(rb"\x8b\x46(.).{0,4}?\xa3" + re.escape(struct.pack("<I", st)),
                  body, re.S)
    for m in re.finditer(rb"\x8b\x4e(.).{0,6}?\x51\xe8(....)", body, re.S):
        at = off + m.end() - 5
        if at + 5 + struct.unpack("<i", m.group(2))[0] + t.image_base == pf:
            out["gametext_x"] = m.group(1)[0]
    if s:
        out["gametext_style"] = s.group(1)[0]
    if out["gametext_x"] is not None and out["gametext_style"] is not None:
        log(f"HUD gametext object       : x at +0x{out['gametext_x']:X}, "
            f"style at +0x{out['gametext_style']:X}")
    return out


def d_hud_text_vars(t, log):
    """Offroad: the pointer variables holding the gametext objects that sit at
    a screen edge: the race HUD's `__hud_p*Text`. Only these objects move;
    every other string keeps the cabinet's place: high-score tables, legal
    text, racer tags, and attract's yellow credit line (`__attract_p*Gametext`,
    which the owner wants left where the cabinet had it)."""
    syms = t.symbols()
    out = sorted((v["va"], k) for k, v in syms.items() if v["va"] and
                 re.match(r"^__hud_p\w+Text$", k))
    if out:
        log(f"HUD text objects          : {len(out)} "
            f"({', '.join(k for _, k in out)})")
    return out


def d_hud_elements(t, log):
    """The functions that draw one HUD element each. Every primitive one of
    them submits gets the same offset, decided by where the first lands.

    Hydro: `_hud_Draw` calls `_hud_radar_Draw` (the right dial) and then two
    unnamed functions (the speed and boost dials, left) before the lens flare,
    once in each of two switch arms; and `_hud_text_Draw` calls one unnamed
    helper that draws a time as ortho digits (it converts with
    `_xmath_ConvertAFloatSecs2NumOfMinSecCSec`). Attract's overlay is the one
    unnamed callee of `_wpr_attract_Draw` that draws ortho meshes. With no
    high-score table up it draws the logo and the web address, one
    `_mesh3d_DrawOrtho` each, and returns; past that first `ret` it draws the
    table one mesh per character, which must stay put. So there an element is
    each `_mesh3d_DrawOrtho` whose return address is before that `ret`. Offroad: every `__hud_Draw*` that
    `_hud_Draw` calls, except `__hud_DrawFlagIcon`, whose widescreen hook
    already exists and opens the element itself; and
    `__attract_Draw2DAssetEZ`, whose one caller draws the demo's logo."""
    d = t.read()
    starts = func_table(t)
    known = set(starts)
    syms = t.symbols()
    named = {v["va"]: k for k, v in syms.items() if v["va"]}
    hud = t.va("_hud_Draw")
    if not hud:
        return []
    els = []
    radar, flare = t.va("_hud_radar_Draw"), t.va("_anim_air_DoLensFlare")
    if radar and flare:
        calls = _calls_in(t, d, hud, starts, known)
        arms = {tuple(calls[i + 1:i + 3]) for i, c in enumerate(calls)
                if c == radar and i + 3 < len(calls) and calls[i + 3] == flare}
        if len(arms) != 1:
            raise SystemExit(f"gen-gamedefs: {t.id} _hud_Draw: expected one "
                             f"radar/dial/dial/flare order, found {sorted(arms)}")
        els.append((radar, "_hud_radar_Draw", (0, 0, 0)))
        for va in next(iter(arms)):
            if va in named:
                raise SystemExit(f"gen-gamedefs: {t.id} _hud_Draw dial "
                                 f"0x{va:08X} is named {named[va]}")
            els.append((va, f"_hud_Draw dial 0x{va:06X}", (0, 0, 0)))
        txt, conv = t.va("_hud_text_Draw"), t.va("_xmath_ConvertAFloatSecs2NumOfMinSecCSec")
        ortho = t.va("_mesh3d_DrawOrtho")
        digits = sorted({c for c in _calls_in(t, d, txt, starts, known)
                         if c not in named and conv in _calls_in(t, d, c, starts, known)
                         and ortho in _calls_in(t, d, c, starts, known)})
        if len(digits) != 1:
            raise SystemExit(f"gen-gamedefs: {t.id} _hud_text_Draw: expected one "
                             f"time-digit helper, found {[hex(v) for v in digits]}")
        els.append((digits[0], f"_hud_text_Draw digits 0x{digits[0]:06X}", (0, 0, 0)))
        att = t.va("_wpr_attract_Draw")
        over = sorted({c for c in _calls_in(t, d, att, starts, None)
                       if c not in named and ortho in _calls_in(t, d, c, starts, known)})
        if len(over) != 1:
            raise SystemExit(f"gen-gamedefs: {t.id} _wpr_attract_Draw: expected "
                             f"one ortho overlay, found {[hex(v) for v in over]}")
        o = over[0] - t.image_base
        ret = d.index(b"\xc3", o)              # the logo path's own return
        ras = [at + 5 + t.image_base for at in range(o, ret - 4)
               if d[at] == 0xE8 and at + 5 + struct.unpack_from("<i", d, at + 1)[0]
               + t.image_base == ortho]
        if len(ras) != 2:
            raise SystemExit(f"gen-gamedefs: {t.id} attract overlay: expected "
                             f"two ortho draws before its first ret, found "
                             f"{[hex(r) for r in ras]}")
        els.append((over[0], f"_wpr_attract_Draw overlay 0x{over[0]:06X}",
                    (1, ras[0], ras[-1])))
    else:
        seen = []
        for c in _calls_in(t, d, hud, starts, known):
            n = named.get(c, "")
            if n.startswith("__hud_Draw") and n != "__hud_DrawFlagIcon" and c not in seen:
                seen.append(c)
        els = [(va, named[va], (0, 0, 0)) for va in seen]
        logo = t.va("__attract_Draw2DAssetEZ")
        if logo:
            els.append((logo, "__attract_Draw2DAssetEZ", (0, 0, 0)))
    out = []
    for va, name, kind in els:
        meta = _hook_prologue(d, va - t.image_base)
        if not meta:
            log(f"hook: HUD element {name} @ 0x{va:08X} has no relocatable "
                f"prologue -- it stays where the cabinet put it")
            continue
        out.append((va, meta, kind, name))
    log(f"HUD elements              : {len(out)} ({', '.join(n for _, _, _, n in out)})")
    return out

MENU_CELLS = ("menu_steer", "menu_sel_active", "menu_sel_sub",
              "menu_sel_cur_a", "menu_sel_cur_b", "menu_sel_states_a",
              "menu_sel_states_b", "menu_sel_field", "menu_item_index",
              "menu_band_a_lo", "menu_band_a_hi", "menu_band_b_lo",
              "menu_band_b_hi", "menu_sel_index", "menu_sel_target",
              "menu_sel_count", "menu_sel_filtered", "menu_sel_enabled", "menu_notch",
              "menu_ini_state", "menu_ini_state_want", "menu_ini_state_kind",
              "menu_ini_gate", "menu_ini_gate_want", "menu_ini_slot",
              "menu_ini_wheel", "menu_ini_field",
              "menu_sel_enable", "menu_list_a", "menu_stride_a", "menu_count_a",
              "menu_list_b", "menu_stride_b", "menu_count_b", "menu_avail_a",
              "menu_avail_lists", "menu_ini_count", "menu_ini_letters",
              "menu_ini_k_off", "menu_ini_k_scale", "menu_ini_k_inv",
              "menu_ini_k_bins", "menu_ini_k_hyst", "menu_ini_k_split",
              "menu_ini_slot_max", "menu_ini_end_lo", "menu_ini_end_hi",
              "menu_sel_k_off", "menu_sel_k_scale", "menu_remap_a",
              "menu_remap_a_states", "menu_remap_b", "menu_remap_b_states",
              "menu_en_count_off", "menu_en_flag_off", "menu_en_flag_mask",
              "menu_en_lock_off", "menu_en_stride", "menu_ini_k_n",
              "menu_ini_top", "menu_ini_rate_sign", "menu_ini_k_snap",
              "menu_ini_k_slow")


def d_menu_cells(t, log):
    """The select screens' own state: which screen is up, which item it has,
    and where the item sits on the wheel, for src/menu_wheel.c. Read, never
    written.

    Every select screen reads the steering as a POSITION through one field of
    _Controls_InputState. Each cell comes from the shape of the one function that
    owns it, matched instruction by instruction; a function that exists with a
    different shape refuses rather than labelling the wrong cell.

    Hydro: _wpr_select_GetCurrentSelection is the selection model whole,
        mov eax,[active] ; test ; je ; mov eax,[sub] ; cmp eax,n ; ja ;
        jmp [eax*4+table] ; mov eax,[item_a] ... ret ; mov eax,[item_b] ...
    and the sub-screens whose case loads item_a (tracks) or item_b (boats)
    are the two state masks. _wpr_select_Work then gives each list's band,
    the two `fcomp [r+d8]` after its load of the item into r, and the item's
    index word, the second `mov r16,[r+d8]`. Initials: _wpr_hiscore_Work's
    gate and state; the entering state is the case that reads the wheel, and
    the slot is the first `mov [abs],eax` after that read.

    Offroad: _wrapper_Draw dispatches the select screens on _Wrapper_nState
    and the states whose case calls __UpdateSelectionControls are the mask;
    that function holds the target position (its first `fstp [abs]`), the
    choice count and filtered position (`fld [count] ; fmul [pos] ; call`)
    and the index (`mov [abs],edi`). Initials: _initials_Work's two gates and
    __initials_WorkSelectionWheel's letter wheel and letter index.

    A steering FIELD is an offset into _Controls_InputState, from the reading
    function's first reference into it: Hydro's select reads +8 and its
    initials +0, so it is per screen, not per title."""
    import x86len
    out = {k: None for k in MENU_CELLS}
    d, base = t.read(), t.image_base
    starts = func_table(t)
    steer = t.va("_Controls_InputState")
    if not steer:
        return out
    lo_data, hi_data = t.data_va, t.bss_va + t.bss_size
    u32 = lambda va: struct.unpack_from("<I", d, va - base)[0]
    abs32 = lambda b, at: struct.unpack_from("<I", b, at)[0]

    def refuse(name, why):
        raise SystemExit(f"gen-gamedefs: {t.id} {name}: {why}")

    def insns(name):
        """[(va, bytes)] over the function's extent; stops at what does not
        decode, which is where a trailing jump table begins."""
        return insns_at(t.va(name))

    def insns_at(va):
        if not va:
            return []
        end, out_ = starts[bisect.bisect_right(starts, va)], []
        while va < end:
            r = x86len.decode(d, va - base, base)
            if not r:
                break
            out_.append((va, d[va - base:va - base + r[0]]))
            va += r[0]
        return out_

    def cell(v, what):
        if not lo_data <= v < hi_data:
            raise SystemExit(f"gen-gamedefs: {t.id} {what} 0x{v:08X} is not "
                             f"in DATA or .bss")
        return v

    def steer_ref(name, ins):
        """(index, field) of the first instruction naming _Controls_InputState."""
        for k, (va, b) in enumerate(ins):
            for i in range(1, len(b) - 3):
                v = abs32(b, i)
                if steer <= v < steer + 0x20:
                    return k, v - steer
        refuse(name, "reads no _Controls_InputState field")

    def op_abs(b, prefix):
        """The abs32 after `prefix` when the instruction is exactly that."""
        if b.startswith(prefix) and len(b) == len(prefix) + 4:
            return abs32(b, len(prefix))
        return None

    def item_address(ins, k):
        """(base, stride) of the item list the store at ins[k] addresses.
        Walks back to the `and r,0xffff` that zero-extends the index, then
        evaluates the chain forward with every register a multiple of the
        index: mov, shl, sub, lea [b+i*s], lea [i*s+disp]."""
        def zext(b):            # and r,0xffff; eax has its own short form
            if b[:1] == b"\x25" and len(b) == 5 and abs32(b, 1) == 0xFFFF:
                return 0
            if b[:1] == b"\x81" and len(b) == 6 and (b[1] & 0xF8) == 0xE0 \
                    and abs32(b, 2) == 0xFFFF:
                return b[1] & 7
            return None
        st = next((j for j in range(k - 1, max(k - 14, -1), -1)
                   if zext(ins[j][1]) is not None), None)
        if st is None:
            return None
        coef, disp = {zext(ins[st][1]): 1}, None
        for _, b in ins[st + 1:k]:
            m = b[1] if len(b) > 1 else 0
            if b[:1] == b"\x8b" and len(b) == 2 and m >= 0xC0:
                coef[(m >> 3) & 7] = coef.get(m & 7, 0)
            elif b[:1] == b"\xc1" and len(b) == 3 and (m & 0xF8) == 0xE0:
                coef[m & 7] = coef.get(m & 7, 0) << b[2]
            elif b[:1] == b"\x2b" and len(b) == 2 and m >= 0xC0:
                r = (m >> 3) & 7
                coef[r] = coef.get(r, 0) - coef.get(m & 7, 0)
            elif b[:1] == b"\x8d" and (m & 7) == 4 and len(b) >= 3:
                sib, mod = b[2], m >> 6
                sc, ix, bs = 1 << (sib >> 6), (sib >> 3) & 7, sib & 7
                c = coef.get(ix, 0) * sc
                if mod == 0 and bs == 5 and len(b) == 7:
                    disp = abs32(b, 3)
                elif mod == 0 and len(b) == 3:
                    c += coef.get(bs, 0)
                else:
                    return None
                coef[(m >> 3) & 7] = c
            else:
                return None
        last = ins[k - 1][1]
        if disp is None or last[:1] != b"\x8d":
            return None
        return disp, coef[(last[1] >> 3) & 7]

    def jump_table(ins, k):
        """(n, table, k') from `cmp eax,imm8 ; ja ; jmp [eax*4+table]` at k."""
        if k + 2 >= len(ins):
            return None
        cmpb, ja, jmp = ins[k][1], ins[k + 1][1], ins[k + 2][1]
        if not (cmpb[:2] == b"\x83\xf8" and len(cmpb) == 3 and
                ja[:1] in (b"\x77", b"\x0f") and jmp[:3] == b"\xff\x24\x85"):
            return None
        return cmpb[2], abs32(jmp, 3), k + 3

    def states(tab, n, targets):
        m = 0
        for k in range(n + 1):
            if u32(tab + 4 * k) in targets:
                m |= 1 << k
        return m

    out["menu_steer"] = steer

    # ---- Hydro: track and boat select -------------------------------------
    ins = insns("_wpr_select_GetCurrentSelection")
    if ins:
        active = op_abs(ins[0][1], b"\xa1")
        sub = op_abs(ins[3][1], b"\xa1") if len(ins) > 3 else None
        jt = jump_table(ins, 4)
        if active is None or sub is None or not jt or \
                ins[1][1] != b"\x85\xc0" or ins[2][1][:1] != b"\x74":
            refuse("_wpr_select_GetCurrentSelection", "not the shape the "
                   "derivation reads")
        n, tab, k = jt
        loads = [(va, op_abs(b, b"\xa1")) for va, b in ins[k:]
                 if op_abs(b, b"\xa1") is not None]
        if len(loads) < 2:
            refuse("_wpr_select_GetCurrentSelection", "one list, not two")
        (case_a, item_a), (case_b, item_b) = loads[0], loads[1]
        out["menu_sel_active"] = cell(active, "select active")
        out["menu_sel_sub"] = cell(sub, "select sub-screen")
        out["menu_sel_cur_a"] = cell(item_a, "track item")
        out["menu_sel_cur_b"] = cell(item_b, "boat item")
        out["menu_sel_states_a"] = states(tab, n, {case_a})
        out["menu_sel_states_b"] = states(tab, n, {case_b})
        if not out["menu_sel_states_a"] or not out["menu_sel_states_b"]:
            refuse("_wpr_select_GetCurrentSelection", "the jump table does "
                   "not split into the two lists")

        w = insns("_wpr_select_Work")
        if not w:
            refuse("_wpr_select_Work", "absent beside its accessor")
        out["menu_sel_field"] = steer_ref("_wpr_select_Work", w)[1]
        idx, kload = set(), {}
        for key, item in (("a", item_a), ("b", item_b)):
            found = None
            for k, (va, b) in enumerate(w):
                if op_abs(b, b"\xa1") == item:
                    reg = 0
                elif b[:1] == b"\x8b" and len(b) == 6 and (b[1] & 0xC7) == 0x05 \
                        and abs32(b, 2) == item:
                    reg = (b[1] >> 3) & 7
                else:
                    continue
                words, fc = [], []
                for _, c in w[k + 1:k + 40]:
                    if c[:2] == b"\x66\x8b" and len(c) == 4 and \
                            (c[2] & 0xC0) == 0x40 and (c[2] & 7) == reg:
                        words.append(c[3])
                    if c[:1] == b"\xd8" and len(c) == 3 and \
                            (c[1] & 0xF8) == 0x58 and (c[1] & 7) == reg:
                        fc.append(c[2])
                if len(words) >= 2 and len(fc) >= 2:
                    found = (words[1], fc[0], fc[1])
                    kload[key] = k
                    break
            if not found or found[2] != found[1] + 4:
                refuse("_wpr_select_Work", f"list {key}'s block is not the "
                       f"shape the derivation reads ({found})")
            idx.add(found[0])
            out[f"menu_band_{key}_lo"], out[f"menu_band_{key}_hi"] = found[1:]
        if len(idx) != 1:
            refuse("_wpr_select_Work", f"the two lists keep their index at "
                   f"different offsets {sorted(idx)}")
        out["menu_item_index"] = idx.pop()

        # Each list: the store that makes item i current gives its base and
        # stride; the `cmp r16,imm8 ; jae` before it, the last index. Only a
        # list whose new item is tested with `cmp byte [r+d],r8` before it is
        # taken has an availability byte (tracks; boats lock on a flag).
        lists, odd = {}, t.va("_Gameloop_bOddFrame")
        for key, item in (("a", item_a), ("b", item_b)):
            stores = [k for k, (_, b) in enumerate(w) if op_abs(b, b"\xa3") == item]
            if not stores:
                refuse("_wpr_select_Work", f"list {key} is never made current")
            stores = [k for k in stores if k > kload[key]]
            if not stores:
                refuse("_wpr_select_Work", f"list {key} is never made current")
            addr = item_address(w, stores[0])
            bounds = [w[j][1][3] for j in range(kload[key], stores[-1])
                      if w[j][1][:2] == b"\x66\x83" and len(w[j][1]) == 4 and
                      (w[j][1][2] & 0xF8) == 0xF8 and
                      w[j + 1][1][:1] in (b"\x73", b"\x0f")]
            bound = bounds[0] if len(set(bounds)) == 1 else None
            if not addr or bound is None:
                refuse("_wpr_select_Work", f"list {key}'s items are not "
                       f"addressed the way the derivation reads ({addr}, {bound})")
            avail = None
            nxt = stores[1] if len(stores) > 1 else min(len(w), stores[0] + 30)
            for _, b in w[stores[0] + 1:nxt]:
                if b[:1] == b"\x38" and len(b) in (2, 3) and \
                        (b[1] >> 6) in (0, 1) and (b[1] & 7) not in (4, 5):
                    avail = b[2] if len(b) == 3 else 0
                    break
            lists[key] = (addr[0], addr[1], bound + 1, avail)
        out["menu_list_a"], out["menu_stride_a"], out["menu_count_a"], av_a = lists["a"]
        out["menu_list_b"], out["menu_stride_b"], out["menu_count_b"], av_b = lists["b"]
        if av_b is not None:
            refuse("_wpr_select_Work", "boats grew an availability byte")
        out["menu_avail_a"] = av_a if av_a is not None else 0
        out["menu_avail_lists"] = 1 if av_a is not None else 0
        for key in "ab":
            if not out[f"menu_band_{key}_hi"] < out[f"menu_stride_{key}"]:
                refuse("_wpr_select_Work", f"list {key}'s band is outside its item")

        # Selection changes are enabled: the cell each case tests just before
        # it tests the odd frame.
        en = set()
        for k, (_, b) in enumerate(w):
            if op_abs(b, b"\x39\x1d") == odd:
                prev = [op_abs(c, b"\x39\x1d") for _, c in w[max(0, k - 6):k]]
                prev = [v for v in prev if v is not None]
                if prev:
                    en.add(prev[-1])
        if len(en) != 1:
            refuse("_wpr_select_Work", f"no single enable gate ({sorted(en)})")
        out["menu_sel_enable"] = cell(en.pop(), "selection changes enabled")

    # ---- Hydro: initials ---------------------------------------------------
    ins = insns("_wpr_hiscore_Work")
    if ins:
        gate = op_abs(ins[1][1], b"\xa1") if len(ins) > 1 else None
        st = next((k for k in range(2, min(len(ins), 12))
                   if op_abs(ins[k][1], b"\xa1") is not None), None)
        # mov eax,[state] ; mov ecx,n ; cmp eax,ecx ; ja ; jmp [eax*4+table]
        ok = st is not None and st + 4 < len(ins) and \
            ins[st + 1][1][:1] == b"\xb9" and ins[st + 2][1] == b"\x3b\xc1" and \
            ins[st + 4][1][:3] == b"\xff\x24\x85"
        if gate is None or not ok:
            refuse("_wpr_hiscore_Work", "not the shape the derivation reads")
        n, tab = abs32(ins[st + 1][1], 1), abs32(ins[st + 4][1], 3)
        k, fld = steer_ref("_wpr_hiscore_Work", ins)
        rd_at = ins[k][0]
        cases = sorted((u32(tab + 4 * s), s) for s in range(n + 1))
        want = [s for c, s in cases if c <= rd_at]
        slot = next((op_abs(b, b"\xa3") for _, b in ins[k:]
                     if op_abs(b, b"\xa3") is not None), None)
        if not want or slot is None:
            refuse("_wpr_hiscore_Work", "no case reads the wheel into a slot")
        out["menu_ini_gate"] = cell(gate, "initials gate")
        out["menu_ini_gate_want"] = 0
        out["menu_ini_state"] = cell(op_abs(ins[st][1], b"\xa1"), "initials state")
        out["menu_ini_state_want"] = want[-1]
        out["menu_ini_state_kind"] = 1                  # entering: state == want
        out["menu_ini_slot"] = cell(slot, "initials slot")
        out["menu_ini_field"] = fld

        # The slot formula, in its own order: the letter count read before
        # the wheel, `cmp eax,letters`, then fsub off, fmul scale, fmul inv,
        # fmul bins, fsub hyst, `cmp eax,max`, fcomp split, and the two slots
        # the last case picks between. menu_wheel.c computes each slot's band
        # from these, so a formula of another shape must not reach it.
        cnt = next((op_abs(b, b"\xa1") for _, b in ins[max(0, k - 3):k]
                    if op_abs(b, b"\xa1") is not None), None)
        cmps = [b[2] for _, b in ins[k:k + 60]
                if b[:2] == b"\x83\xf8" and len(b) == 3]
        fl = []
        for _, b in ins[k:k + 60]:
            if b[:1] == b"\xd8" and len(b) == 6 and b[1] in (0x25, 0x0D, 0x1D):
                fl.append((b[1], abs32(b, 2)))
                if b[1] == 0x1D:
                    break
        # After the split's fcomp: test ah,0x41 ; mov eax,left ; jne ;
        # mov eax,right. The jne keeps `left` for a field at or below it.
        fc = next((j for j in range(k, min(len(ins), k + 80))
                   if ins[j][1][:2] == b"\xd8\x1d"), None)
        tail = []
        if fc is not None:
            seq = [b for _, b in ins[fc + 1:fc + 8]]
            movs = [j for j, b in enumerate(seq) if b[:1] == b"\xb8" and len(b) == 5]
            if b"\xf6\xc4\x41" in seq and len(movs) >= 2 and \
                    seq[movs[0] + 1][:1] == b"\x75":
                tail = [abs32(seq[movs[0]], 1), abs32(seq[movs[1]], 1)]
        if cnt is None or len(cmps) < 2 or len(tail) < 2 or \
                [o for o, _ in fl] != [0x25, 0x0D, 0x0D, 0x0D, 0x25, 0x1D]:
            refuse("_wpr_hiscore_Work", f"the slot formula is not the shape "
                   f"menu_wheel.c computes ({cnt}, {cmps}, {fl}, {tail})")
        out["menu_ini_count"] = cell(cnt, "initials letters entered")
        out["menu_ini_letters"], out["menu_ini_slot_max"] = cmps[0], cmps[1]
        (out["menu_ini_k_off"], out["menu_ini_k_scale"], out["menu_ini_k_inv"],
         out["menu_ini_k_bins"], out["menu_ini_k_hyst"],
         out["menu_ini_k_split"]) = [v for _, v in fl]
        out["menu_ini_end_lo"], out["menu_ini_end_hi"] = tail[0], tail[1]

    # ---- Offroad: the select screens ---------------------------------------
    ins = insns("_wrapper_Draw")
    upd, sub = t.va("__UpdateSelectionControls"), t.va("_Wrapper_nState")
    if ins and upd and sub:
        active = op_abs(ins[0][1], b"\xa1")
        k = next((k for k, (_, b) in enumerate(ins) if op_abs(b, b"\xa1") == sub), None)
        jt = jump_table(ins, k + 1) if k is not None else None
        if active is None or ins[1][1] != b"\x85\xc0" or not jt:
            refuse("_wrapper_Draw", "not the shape the derivation reads")
        calls = {va for va, b in ins if b[:1] == b"\xe8" and len(b) == 5 and
                 va + 5 + struct.unpack_from("<i", b, 1)[0] == upd}
        out["menu_sel_active"] = cell(active, "wrapper active")
        out["menu_sel_sub"] = cell(sub, "_Wrapper_nState")
        out["menu_sel_states_a"] = states(jt[1], jt[0], calls)
        if not out["menu_sel_states_a"]:
            refuse("_wrapper_Draw", "no state runs __UpdateSelectionControls")
        u = insns("__UpdateSelectionControls")
        out["menu_sel_field"] = steer_ref("__UpdateSelectionControls", u)[1]
        tgt = next((op_abs(b, b"\xd9\x1d") for _, b in u
                    if op_abs(b, b"\xd9\x1d") is not None), None)
        cnt = pos = None
        for j in range(len(u) - 2):
            c0, c1 = op_abs(u[j][1], b"\xd9\x05"), op_abs(u[j + 1][1], b"\xd8\x0d")
            if c0 is not None and c1 is not None and u[j + 2][1][:1] == b"\xe8":
                cnt, pos = c0, c1
                break
        ix = next((op_abs(b, b"\x89\x3d") for _, b in u
                   if op_abs(b, b"\x89\x3d") is not None), None)
        if tgt is None or cnt is None or ix is None:
            refuse("__UpdateSelectionControls", "not the shape the derivation reads")
        out["menu_sel_target"] = cell(tgt, "select target")
        out["menu_sel_count"] = cell(cnt, "select count")
        out["menu_sel_filtered"] = cell(pos, "select filtered position")
        out["menu_sel_index"] = cell(ix, "select index")
        out["menu_sel_enabled"] = t.va("__nNumEnabledChoices")
        out["menu_notch"] = t.va("_ffbnew_VIFYPos")

        # The target the filter settles on: fld [steer] ; fsub [off] ;
        # ... ; fmul [scale], before its first store.
        k0 = steer_ref("__UpdateSelectionControls", u)[0]
        ops = [(c[1], abs32(c, 2)) for _, c in u[k0 + 1:k0 + 8]
               if c[:1] == b"\xd8" and len(c) == 6 and c[1] in (0x25, 0x0D)]
        if [o for o, _ in ops[:2]] != [0x25, 0x0D]:
            refuse("__UpdateSelectionControls", f"the target is not "
                   f"(steer - off) * scale ({ops})")
        out["menu_sel_k_off"], out["menu_sel_k_scale"] = ops[0][1], ops[1][1]

        # A screen that moves the index to the nearest enabled item: the
        # call after __UpdateSelectionControls in its case is the screen,
        # and the screen pushes its list for __NearestOneThatIsEnabled.
        near = t.va("__NearestOneThatIsEnabled")
        call_to = lambda va, c: va + 5 + struct.unpack_from("<i", c, 1)[0] \
            if c[:1] == b"\xe8" and len(c) == 5 else None
        lists = {}
        for st_ in range(jt[0] + 1):
            if not out["menu_sel_states_a"] >> st_ & 1:
                continue
            case = u32(jt[1] + 4 * st_)
            j = next((j for j, (va, _) in enumerate(ins) if va == case), None)
            fn = call_to(*ins[j + 1]) if j is not None and j + 1 < len(ins) \
                else None
            sc = insns_at(fn) if fn else []
            for i in range(min(len(sc), 24) - 1):
                if sc[i][1][:1] == b"\x68" and len(sc[i][1]) == 5 and \
                        call_to(*sc[i + 1]) == near and near:
                    lst = cell(abs32(sc[i][1], 1), "selection list")
                    lists[lst] = lists.get(lst, 0) | 1 << st_
                    break
        if len(lists) > 2:
            refuse("_wrapper_Draw", f"more than two lists remap ({lists})")
        for key, (lst, mask) in zip("ab", sorted(lists.items())):
            out[f"menu_remap_{key}"], out[f"menu_remap_{key}_states"] = lst, mask
        if lists:
            # __NearestOneThatIsEnabled's item test, as the code makes it:
            # mov r16,[list+count] ... lea esi,[list+lock] ;
            # test byte [esi-d],mask ; je ; cmp dword [esi],0 ... add esi,stride
            n_ = insns("__NearestOneThatIsEnabled")
            cnt_ = lea_ = flag_ = stride_ = None
            for _, c in n_:
                if c[:2] == b"\x66\x8b" and len(c) == 4 and (c[2] & 0xC0) == 0x40:
                    cnt_ = cnt_ if cnt_ is not None else c[3]
                elif c[:1] == b"\x8d" and len(c) == 3 and (c[1] & 0xC0) == 0x40:
                    lea_ = c[2]
                elif c[:2] == b"\xf6\x46" and len(c) == 4:
                    flag_ = (struct.unpack_from("<b", c, 2)[0], c[3])
                elif c[:2] == b"\x83\xc6" and len(c) == 3:
                    stride_ = c[2]
            if None in (cnt_, lea_, flag_, stride_) or \
                    b"\x83\x3e\x00" not in [c for _, c in n_]:
                refuse("__NearestOneThatIsEnabled", f"not the item test the "
                       f"derivation reads ({cnt_}, {lea_}, {flag_}, {stride_})")
            out["menu_en_count_off"] = cnt_
            out["menu_en_lock_off"] = lea_
            out["menu_en_flag_off"] = lea_ + flag_[0]
            out["menu_en_flag_mask"] = flag_[1]
            out["menu_en_stride"] = stride_

    # ---- Offroad: initials -------------------------------------------------
    ins, w = insns("_initials_Work"), insns("__initials_WorkSelectionWheel")
    if ins and w:
        act = op_abs(ins[0][1], b"\xa1")
        gate = next((op_abs(b, b"\x8b\x0d") for _, b in ins[:8]
                     if op_abs(b, b"\x8b\x0d") is not None), None)
        gk = next((k for k, (_, b) in enumerate(ins[:8]) if op_abs(b, b"\x8b\x0d") is not None), None)
        wheel = op_abs(w[1][1], b"\xd9\x05") if len(w) > 1 else None
        slot = next((op_abs(b, b"\xa3") for _, b in w
                     if op_abs(b, b"\xa3") is not None), None)
        if act is None or gate is None or ins[gk + 1][1] != b"\x83\xf9\xff" or \
                w[0][1] != b"\x83\xec\x08" or wheel is None or slot is None:
            refuse("_initials_Work", "not the shape the derivation reads")
        out["menu_ini_state"] = cell(act, "initials active")
        out["menu_ini_state_want"] = 0
        out["menu_ini_state_kind"] = 2                  # entering: state != want
        out["menu_ini_gate"] = cell(gate, "initials gate")
        out["menu_ini_gate_want"] = 0xFFFFFFFF
        out["menu_ini_wheel"] = cell(wheel, "letter wheel")
        out["menu_ini_slot"] = cell(slot, "letter index")
        out["menu_ini_field"] = steer_ref("__initials_WorkSelectionWheel", w)[1]

        # The letter: fld [wheel] ; fmul [N] ... mov eax,top ; sub eax,esi,
        # so the letter falls as the wheel rises. The steering: fld [steer] ;
        # fchs? ; fcom [0] ; ... fcom [snap] ; ... fcom [slow], and the wheel
        # moves by its speed times the first fmul after them.
        k_n = next((op_abs(c, b"\xd8\x0d") for _, c in w[2:5]
                    if op_abs(c, b"\xd8\x0d") is not None), None)
        top = next((abs32(c, 1) for j, (_, c) in enumerate(w[:-1])
                    if c[:1] == b"\xb8" and len(c) == 5 and
                    w[j + 1][1] == b"\x2b\xc6"), None)
        ks = steer_ref("__initials_WorkSelectionWheel", w)[0]
        flip = w[ks + 1][1] == b"\xd9\xe0"
        coms = [(j, abs32(c, 2)) for j, (_, c) in enumerate(w[ks:ks + 30], ks)
                if c[:2] == b"\xd8\x15" and len(c) == 6]
        step = next((abs32(c, 2) for _, c in w[coms[2][0]:coms[2][0] + 30]
                     if c[:2] == b"\xd8\x0d" and len(c) == 6), None) \
            if len(coms) >= 3 else None
        if k_n is None or top is None or step is None:
            refuse("__initials_WorkSelectionWheel", f"not the letter wheel "
                   f"the derivation reads ({k_n}, {top}, {coms}, {step})")
        f32 = lambda va: struct.unpack_from("<f", d, va - base)[0]
        # Steering right: the wheel moves by -speed * step * (flip ? -1 : 1)
        # and the letter by minus the wheel.
        sign = (1 if f32(step) < 0 else -1) * (-1 if flip else 1) * -1
        out["menu_ini_k_n"] = k_n
        out["menu_ini_top"] = top
        out["menu_ini_rate_sign"] = sign & 0xFFFFFFFF
        out["menu_ini_k_snap"], out["menu_ini_k_slow"] = coms[1][1], coms[2][1]

    have = sum(1 for v in out.values() if v is not None)
    log(f"select screens            : {have}/{len(out)} cells"
        + ("" if out["menu_sel_field"] is None else
           f", select reads steer +{out['menu_sel_field']}")
        + ("" if out["menu_ini_field"] is None else
           f", initials +{out['menu_ini_field']}"))
    return out


def _branch_targets(data, lo, hi, image_base):
    tg, i = set(), lo
    while i < hi - 6:
        op = data[i]
        if op in (0xE8, 0xE9):
            rel, ln = struct.unpack_from("<i", data, i + 1)[0], 5
        elif op == 0xEB or 0x70 <= op <= 0x7F:
            rel, ln = struct.unpack_from("<b", data, i + 1)[0], 2
        elif op == 0x0F and 0x80 <= data[i + 1] <= 0x8F:
            rel, ln = struct.unpack_from("<i", data, i + 2)[0], 6
        else:
            i += 1
            continue
        tg.add(i + ln + rel + image_base)
        i += 1
    return tg


def d_loop_heads(t, log):
    """Diagnostic lower-bound census of loops in the hot mesh function.

    Candidates are targets of byte-scanned backward branches, then filtered for a
    relocatable prologue and for branches into the overwritten patch span. These
    are LOWER BOUNDS: a linear byte scan can find false branches in immediates
    and cannot prove a target is an instruction boundary. Do not turn the counts
    into cycles/iteration until discovery is replaced with control-flow
    disassembly."""
    fn = t.va("_mesh3d_DrawGroup_NoFrustumTest")
    if not fn:
        return []
    d = t.read()
    base, end = fn - t.image_base, fn - t.image_base + 0x1230
    heads = {}
    i = base
    while i < end - 6:
        op = d[i]
        if op in (0xE9, 0xEB) or (0x70 <= op <= 0x7F) or \
           (op == 0x0F and 0x80 <= d[i + 1] <= 0x8F):
            if op == 0xEB or 0x70 <= op <= 0x7F:
                rel, ln = struct.unpack_from("<b", d, i + 1)[0], 2
            elif op == 0xE9:
                rel, ln = struct.unpack_from("<i", d, i + 1)[0], 5
            else:
                rel, ln = struct.unpack_from("<i", d, i + 2)[0], 6
            tgt = i + ln + rel
            if base <= tgt < i:
                heads[tgt] = max(heads.get(tgt, 0), i - tgt)
        i += 1
    # Ordered by ADDRESS, not body size: ranking by size picks the outermost
    # loops, which iterate least. Patching a head overwrites five bytes, so
    # anything branching INTO those bytes lands mid-instruction: not
    # theoretical, it made the game drop triangles four times.
    targets = _branch_targets(d, base, base + 0x1230, t.image_base)
    out, skipped, unsafe = [], 0, 0
    for tgt, body in sorted(heads.items()):
        pl = _prologue_len(d, tgt)
        if not pl:
            skipped += 1
            continue
        va = tgt + t.image_base
        if any(va + k in targets for k in range(1, max(pl, 5))):
            unsafe += 1
            continue
        out.append((va, pl, body))
    log(f"loop heads: {len(out)} instrumented, {skipped} unrelocatable, "
        f"{unsafe} branched-into")
    return out


def func_table(t):
    """Function starts for the sampling profiler, so it can aggregate BY FUNCTION:
    bucketing at 64 bytes spreads one hot function across a dozen buckets, none of
    which survives a top-N list.

    Symbol starts alone leave gaps of several hundred bytes (MSVC's static
    functions are not in the CodeView table), and "nearest preceding symbol" then
    charges a whole unnamed neighbourhood to the last named function. That is not
    hypothetical: _vec3_CalcUnit is 0x80 bytes and was credited with 12% of a
    frame it could not possibly have used.

    Direct `call rel32` targets fill the gaps. A linear byte scan also finds E8s
    inside data, so require TWO independent call sites before accepting a target;
    false positives from decoded data essentially never agree. The threshold is
    validated, not guessed: at >=2 the derived extent of
    _mesh3d_DrawGroup_NoFrustumTest is 0x1230, exactly what objdump gives; >=1
    splits it on a false start and >=3 swallows the next function."""
    d = t.read()
    lo, hi = t.code_va, t.real_code_end_va
    syms = {v["va"] for v in t.symbols().values() if v["va"] and lo <= v["va"] < hi}
    hits = {}
    i = t.code_start_off
    while i < t.real_code_end_off - 5:
        if d[i] == 0xE8:
            tgt = i + 5 + struct.unpack_from("<i", d, i + 1)[0] + t.image_base
            if lo <= tgt < hi:
                hits[tgt] = hits.get(tgt, 0) + 1
        i += 1
    return sorted(syms | {a for a, n in hits.items() if n >= 2})


# --- emission -------------------------------------------------------------

def build(t, log):
    syms = t.symbols()

    glide = sorted(n for n, v in syms.items()
                   if re.match(r"^_g[ru][A-Z]", n) and v["seg"] == 1 and v["va"])
    obs = observed_argbytes(t, glide)
    # Where the cleanup immediate is not observable, the size comes from the
    # game's own push sequences: NOT from a wrapper's exports. The old table
    # is asserted against the result and is never a source; see
    # scripts/glide2_pushes.py.
    # Scoped to the entries the provider actually implements. Push counting can
    # size a call site the old wrapper-checked table simply had no row for, and
    # that is a change of SCOPE rather than of provenance: it would have added
    # grSstWinOpenPartial and grStuff to Offroad's table (62 -> 64), which the
    # provider does not export and the host has never bound. entries.def is the
    # ONE list of what it does.
    implemented = provider_entries()
    pushed, push_hist = push_argbytes(
        t, {syms[n]["va"]: n for n in glide if n[1:] in implemented})
    rows, conflicts, unsized = [], [], []
    for n in glide:
        fallback = FALLBACK_ARG_BYTES.get(n[1:])
        o = sorted(obs[n])
        if len(o) == 1:
            nb, source = o[0], "observed"
        elif len(o) == 0 and n in pushed:
            nb, source = pushed[n], "pushes"
        else:
            nb, source = None, None
        if nb is not None and fallback is not None and nb != fallback:
            conflicts.append((n, nb, fallback))
        if nb is None:
            unsized.append(n)
            continue
        rows.append((n, syms[n]["va"], nb, source))

    imp = sorted((v["va"], k.replace("__imp_", "", 1))
                 for k, v in syms.items() if k.startswith("__imp__"))
    stdcall = re.compile(r"^_[A-Za-z]\w*@\d+$")
    impset = {n for _, n in imp}
    direct = sorted(k for k, v in syms.items()
                    if stdcall.match(k) and v["seg"] == 1 and k not in impset)

    gates = find_int_gates(t)
    hw = [(n, va, "ETSGATE", f"int 0x{vec:02X} kernel gate") for n, va, vec in gates]
    gate_names = {g[0] for g in gates}
    for pre, grp, note in HW:
        for n in sorted(k for k, v in syms.items()
                        if k.startswith(pre) and v["seg"] == 1 and v["va"]):
            if n not in gate_names:
                hw.append((n, syms[n]["va"], grp, note))

    log(f"Glide entry points        : {len(glide)}")
    log(f"  sized from cleanup      : {sum(1 for r in rows if r[3] == 'observed')}")
    log(f"  sized from own pushes   : {sum(1 for r in rows if r[3] == 'pushes')}")
    log(f"  ABI conflicts           : {len(conflicts)}"
        f"  {'<-- INVESTIGATE' if conflicts else '(none)'}")
    for n, o, d in conflicts:
        log(f"     {n}: derived {o} from the game, tripwire table says {d}")
    log(f"  unsized (shim must stub): {len(unsized)}  {', '.join(unsized) or '-'}")
    log(f"__imp__ slots             : {len(imp)}")
    log(f"direct Win32 entries      : {len(direct)}")
    log(f"ETS int-gate functions    : {len(gates)}  "
        f"(int 0xFE: {sum(1 for g in gates if g[2] == 0xFE)}, "
        f"int 0xFF: {sum(1 for g in gates if g[2] == 0xFF)})")
    log(f"hardware entry points     : {len(hw)}")

    over = [(k, n, MAX[k]) for k, n in
            (("glide", len(rows)), ("imp", len(imp)), ("w32", len(direct)))
            if n > MAX[k]]
    for k, n, cap in over:
        log(f"ERROR: {k} table has {n} entries, ceiling VCT_MAX_{k.upper()} is {cap}")
    return {"glide": rows, "imp": imp, "w32": direct, "hw": hw,
            "unsized": unsized, "over": over}


def emit(t, tables, log):
    syms = t.symbols()
    extras = EXTRAS.get(t.id, {})
    out = []
    w = out.append
    tid = t.id

    w(f"/* GENERATED by scripts/gen-gamedefs.py: do not edit.\n"
      f" * Title   : {t.id} ({t.name})\n"
      f" * Image   : {t.meta['exe']}\n"
      f" * sha256  : {t.sha256()}\n"
      f" * Symbols : the image's own CodeView table (scripts/p1-symbols.py)\n"
      f" * Regenerate after any analysis change:\n"
      f" *     python3 scripts/gen-gamedefs.py --title {t.id}\n"
      f" */\n"
      f'#include "../src/game.h"\n\n')

    # --- anchors ---
    fields, missing = [], []
    for field, cands, note in ANCHORS:
        va, used = 0, None
        for c in cands:
            v = syms.get(c, {}).get("va")
            if v:
                va, used = v, c
                break
        if va:
            fields.append((field, va, f"{used}: {note}"))
        else:
            missing.append(field)
            fields.append((field, 0, f"ABSENT in {tid}: {'/'.join(cands)}"))

    # --- derived ---
    mat_tex = d_material_texture(t)
    tex_start, tex_even, tex_info = d_texobj(t)
    dcs_depth = d_dcs_lock_depth(t)
    audiolib_table, audiolib_count = d_audiolib_table(t, log)
    spk = d_speaker_mode(t)
    vhit, vmiss = d_vtxcache(t)
    arrows = d_arrow_materials(t)
    ds_begin, ds_swap, ds_steps = d_draw_script(t, log)
    pdraw_cell, pdraw_site = d_draw_site(t)
    tbase_hi, tbase_lo = d_timer_base(t)
    noblend = d_noblend_pools(t) + [0] * NOBLEND_POOLS_MAX
    rect = d_record_tables(t)
    loops = d_loop_heads(t, log)
    funcs = func_table(t)

    if len(loops) > MAX["loop"]:
        log(f"ERROR: loop table has {len(loops)} entries, ceiling is {MAX['loop']}")
        tables["over"].append(("loop", len(loops), MAX["loop"]))

    mbh = d_mb_handshake(t)
    dac_out = d_dac_out(t)
    ffb = d_ffb_cells(t)
    cks = d_code_cksum(t)
    hooks = d_hooks(t, log)
    glyph = d_text_glyphs(t, log)
    fin_flags, fin_bit = d_finish_stats(t, log)
    track_lo, track_hi, track_k = d_tracking_cards(t, log)
    gtext = d_gametext(t, log)
    hud_txt = d_hud_text_vars(t, log)
    hud_els = d_hud_elements(t, log)
    menu = d_menu_cells(t, log)
    if len(hud_els) > MAX["hud"]:
        log(f"ERROR: HUD element table has {len(hud_els)} entries, ceiling is {MAX['hud']}")
        tables["over"].append(("hud", len(hud_els), MAX["hud"]))
    if len(hud_txt) > MAX["hudtext"]:
        log(f"ERROR: HUD text table has {len(hud_txt)} entries, ceiling is {MAX['hudtext']}")
        tables["over"].append(("hudtext", len(hud_txt), MAX["hudtext"]))
    derived = [
        ("hook_init3dfx", hooks["hook_init3dfx"],
         "_init3dfx_Init prologue: len | reloc<<8"),
        ("hook_queryhwcaps", hooks["hook_queryhwcaps"],
         "_hardware_caps_driver_QueryHardwareCaps prologue: len | reloc<<8"),
        ("hook_viewport_init", hooks["hook_viewport_init"],
         "_viewport_InitToFullScreen prologue: len | reloc<<8"),
        ("hook_screen_fade",
         hooks["hook_screen_fade_hydro"] or hooks["hook_screen_fade_offroad"],
         "full-screen fade prologue: len | reloc<<8"),
        ("hook_drawortho", hooks["hook_drawortho"],
         "_mesh3d_DrawOrtho prologue: len | reloc<<8"),
        ("hook_post_fade", hooks["hook_post_fade"],
         "_camera_CallPostFadeCallbacks prologue: len | reloc<<8"),
        ("hook_sky_viewport", hooks["hook_sky_viewport"],
         "_sky_ChangeViewport prologue: len | reloc<<8"),
        ("hook_sky_init", hooks["hook_sky_init"],
         "_sky_Init prologue: len | reloc<<8"),
        ("hook_ws_lensflare", hooks["hook_ws_lensflare"],
         "_anim_air_DoLensFlare prologue: len | reloc<<8"),
        ("hook_ws_drawqueue", hooks["hook_ws_drawqueue"],
         "_drawqueue_Draw prologue: len | reloc<<8"),
        ("hook_ws_spark_cluster", hooks["hook_ws_spark_cluster"],
         "_spark_DrawCluster prologue: len | reloc<<8"),
        ("hook_ws_racer_tag", hooks["hook_ws_racer_tag"],
         "_racer_ShowRacerTag prologue: len | reloc<<8"),
        ("hook_ws_race_update", hooks["hook_ws_race_update"],
         "_race_UpdateDynamics prologue: len | reloc<<8"),
        ("hook_ws_flag_icon", hooks["hook_ws_flag_icon"],
         "__hud_DrawFlagIcon prologue: len | reloc<<8"),
        ("hook_ws_select_draw", hooks["hook_ws_select_draw"],
         "_wpr_select_Draw prologue: len | reloc<<8"),
        ("hook_hud_text_draw", hooks["hook_hud_text_draw"],
         "_hud_text_Draw prologue: len | reloc<<8"),
        ("hook_text_printstr", hooks["hook_text_printstr"],
         "_text_PrintStr prologue: len | reloc<<8"),
        ("hook_gametext_print", hooks["hook_gametext_print"],
         "__GameTextPrint prologue: len | reloc<<8"),
        ("hook_text_flush", hooks["hook_text_flush"],
         "_text_Flush prologue: len | reloc<<8"),
        ("hook_setdrawfunc", hooks["hook_setdrawfunc"],
         "_gameloop_SetDrawFunc prologue: len | reloc<<8"),
        ("hook_xfm_push", hooks["hook_xfm_push"],
         "_xfm_Push prologue: len | reloc<<8"),
        ("hook_xfm_initcamerarot", hooks["hook_xfm_initcamerarot"],
         "_xfm_InitCameraRot prologue: len | reloc<<8"),
        ("hook_sky_work", hooks["hook_sky_work"],
         "_sky_Work prologue: len | reloc<<8"),
        ("hook_hud_draw", hooks["hook_hud_draw"],
         "_hud_Draw prologue: len | reloc<<8"),
        ("hook_mesh3d_slidest", hooks["hook_mesh3d_slidest"],
         "_mesh3d_SlideST prologue: len | reloc<<8"),
        ("hook_mvplayer_openmovie", hooks["hook_mvplayer_openmovie"],
         "_mvplayer_OpenMovie prologue: len | reloc<<8"),
        ("hook_linklist_initpool", hooks["hook_linklist_initpool"],
         "_linklist_InitPool prologue: len | reloc<<8"),
        ("noblend_pool_a", noblend[0],
         "a recycled pool drawinterp never blends (NOBLEND_POOL_OWNERS)"),
        ("noblend_pool_b", noblend[1], "likewise"),
        ("noblend_pool_c", noblend[2], "likewise"),
        ("noblend_pool_d", noblend[3], "likewise"),
        ("spray_cell", rect["spray_cell"],
         "Hydro: _waterspray's droplet buffer pointer cell (d_record_tables)"),
        ("spray_count", rect["spray_count"], "Hydro: its droplet count"),
        ("spray_stride", rect["spray_stride"], "Hydro: bytes per droplet record"),
        ("spray_pos", rect["spray_pos"], "Hydro: offset of a droplet's head position"),
        ("spray_vel", rect["spray_vel"], "Hydro: offset of a droplet's head velocity"),
        ("splash_base", rect["splash_base"], "Hydro: _fx_splash's instance table"),
        ("splash_stride", rect["splash_stride"], "Hydro: bytes per splash instance"),
        ("splash_n", rect["splash_n"], "Hydro: splash instances"),
        ("splash_counter", rect["splash_counter"],
         "Hydro: offset of a splash instance's frame counter"),
        ("fxpart_base", rect["fxpart_base"], "Hydro: _fx_particles' first record block"),
        ("fxpart_blocks", rect["fxpart_blocks"], "Hydro: record blocks, one per effect type"),
        ("fxpart_block_stride", rect["fxpart_block_stride"], "Hydro: bytes per block"),
        ("fxpart_recs", rect["fxpart_recs"], "Hydro: records per block"),
        ("fxpart_rec_stride", rect["fxpart_rec_stride"], "Hydro: bytes per record"),
        ("fxpart_rec_off", rect["fxpart_rec_off"],
         "Hydro: offset of a block's first record (active bit 0 of its first dword)"),
        ("gameloop_pdrawfunc", pdraw_cell,
         "Hydro: _Gameloop_pDrawFunc, the loop's draw function cell"),
        ("gameloop_draw_site", pdraw_site,
         "Hydro: the loop's one call [_Gameloop_pDrawFunc]"),
        ("draw_phase_site", ds_begin,
         "Offroad: the loop's call _shadow_PerFrameStart, where its draw phase begins"),
        ("draw_swap_site", ds_swap,
         "Offroad: the loop's call _grBufferSwap, where its draw phase ends"),
        ("timer_base_hi", tbase_hi,
         "_timer_ResetHiResTimer's base stamp, high dword: the limiter's tick start"),
        ("timer_base_lo", tbase_lo,
         "_timer_ResetHiResTimer's base stamp, low dword"),
        ("text_glyph_count", glyph["text_glyph_count"],
         "Hydro: u16 count of buffered glyph records, from _text_PrintStr"),
        ("text_glyph_buf", glyph["text_glyph_buf"],
         "Hydro: the glyph record buffer, from _text_PrintStr"),
        ("text_glyph_stride", glyph["text_glyph_stride"],
         "Hydro: bytes per glyph record, from _text_PrintStr's lea pair"),
        ("text_glyph_x", glyph["text_glyph_x"],
         "Hydro: offset of a record's pen x, stored from _Text_fX"),
        ("hud_finish_flags", fin_flags,
         "Hydro: _hud_text_Draw's flags word, from _hud_text_SetupFinishStats"),
        ("hud_finish_bit", fin_bit,
         "Hydro: the bit in it that puts the result screen up"),
        ("track_ra_lo", track_lo,
         "Hydro: first return address of _boats_DrawAllTrackingNumbers' card draws"),
        ("track_ra_hi", track_hi, "Hydro: and the last"),
        ("track_spread", track_k,
         "Hydro: the float a bottom card's sideways direction is scaled by"),
        ("gametext_x", gtext["gametext_x"],
         "Offroad: offset of a gametext object's x, from __GameTextPrint"),
        ("gametext_style", gtext["gametext_style"],
         "Offroad: offset of its style word, copied to _Text_nStyle"),
        ("hook_ymf_alloc", hooks["hook_ymf_alloc"],
         "_ymf_AllocateSoundBuffer prologue: len | reloc<<8"),
        ("hook_ymf_free", hooks["hook_ymf_free"],
         "_ymf_DeallocateSoundBuffer prologue: len | reloc<<8"),
        ("hook_r2_find_name", hooks["hook_r2_find_name"],
         "_r2file_FindObjectNameInDir prologue: len | reloc<<8"),
        ("hook_r2_name_to_doid", hooks["hook_r2_name_to_doid"],
         "_r2file_NameToDoid prologue: len | reloc<<8"),
        ("hook_time_sqrt", hooks["hook_time_sqrt"],
         "_xmath_sqrt prologue: len | reloc<<8"),
        ("hook_time_calcunit", hooks["hook_time_calcunit"],
         "_vec3_CalcUnit prologue: len | reloc<<8"),
        ("hook_time_testbound", hooks["hook_time_testbound"],
         "_mesh3d_TestBoundWithFrustum prologue: len | reloc<<8"),
        ("hook_time_lod", hooks["hook_time_lod"],
         "_mesh3d_DetermineLod prologue: len | reloc<<8"),
        ("hook_time_drawgroup", hooks["hook_time_drawgroup"],
         "_mesh3d_DrawGroup_NoFrustumTest prologue: len | reloc<<8"),
        ("io_ack_write", mbh.get("io_ack_write"),
         "board eeprom-write ack toggle, from _mb_receive"),
        ("io_ack_security", mbh.get("io_ack_security"),
         "board security ack toggle, from _mb_receive"),
        ("io_ack_read", mbh.get("io_ack_read"),
         "board eeprom-read ack toggle, from _mb_receive"),
        ("io_dac_out", dac_out,
         "steering-motor DAC channel 0, from the title's own write_dac"),
        ("ffb_accum", ffb["ffb_accum"],
         "Hydro: the summed force before MASTER and the clamp"),
        ("ffb_collide", ffb["ffb_collide"],
         "Hydro: COLLIDE's +-1 flag, set by _phys_Collided, spent each frame"),
        ("ffb_impulse_a", ffb["ffb_impulse_a"],
         "Hydro: decaying impulse A state"),
        ("ffb_impulse_b", ffb["ffb_impulse_b"],
         "Hydro: decaying impulse B state, fired by _phys_Collided"),
        ("ffb_attached", ffb["ffb_attached"],
         "Hydro: the object the steering feedback is attached to"),
        ("ffb_force_type", ffb["ffb_force_type"],
         "Offroad: _ffbnew_curForceType, 0..2, from _ffbnew_SetFFBForceType"),
        ("ffb_hit_val", ffb["ffb_hit_val"],
         "Offroad: _ffbnew_CurHitVal, set by the three collision entry points"),
        ("ffb_tire_jolts", ffb["ffb_tire_jolts"],
         "Offroad: _ffbnew_tireJolts, written by _KinWorkHack"),
        ("ffb_tire_jolts_n", ffb["ffb_tire_jolts_n"],
         "floats in _ffbnew_tireJolts, from the next symbol along"),
        ("ffb_shocks_hist", ffb["ffb_shocks_hist"],
         "Offroad: _ffbnew_shocksHist, the suspension history"),
        ("ffb_shocks_hist_n", ffb["ffb_shocks_hist_n"],
         "floats in _ffbnew_shocksHist"),
        ("ffb_shock_vals", ffb["ffb_shock_vals"],
         "Offroad: _lastShockVals, the last shock value per corner"),
        ("ffb_shock_vals_n", ffb["ffb_shock_vals_n"],
         "floats in _lastShockVals"),
        ("ffb_mode", ffb["ffb_mode"],
         "Offroad: _ffbnew_curMode, which _ffbnew_DoFFB gates the whole module on"),
        ("ffb_boost_field", ffb["ffb_boost_field"],
         "Hydro: boost state, offset into ffb_attached, from _phys_ChangeBoosterState"),
        ("boost_root", ffb["boost_root"],
         "the global the boost chain starts from: Hydro _Player_apData, Offroad _Racer_pPlayer"),
        ("boost_index", ffb["boost_index"],
         "Hydro: _Player_nHuman, the local boat's entry in boost_root; 0: deref boost_root itself"),
        ("boost_index_n", ffb["boost_index_n"], "entries in boost_root when indexed"),
        ("boost_hop_n", ffb["boost_hop_n"], "pointer hops after dereferencing boost_root"),
        ("boost_hop1", ffb["boost_hop1"], "first hop's offset, from the game's own load"),
        ("boost_hop2", ffb["boost_hop2"], "second hop's offset, from the game's own load"),
        ("boost_field", ffb["boost_field"],
         "Hydro: boost seconds (float); Offroad: the nitro flag (int)"),
        ("boost_kind", ffb["boost_kind"],
         "1: fuel, firing while it falls; 2: flag, firing while non-zero"),
        ("ffb_hit_fn", ffb["ffb_hit_fn"],
         "Offroad: _ffbnew_FFBFunc, hooked to read CurHitVal before it zeroes it"),
        ("hook_ffb_hit", hooks["hook_ffb_hit"],
         "_ffbnew_FFBFunc prologue: len | reloc<<8"),
        ("material_texture", mat_tex, "material->texture, from _material_SetState"),
        ("texobj_start", tex_start, "startAddress, from _tmem_Select"),
        ("texobj_evenodd", tex_even, "evenOdd, from _tmem_Select"),
        ("texobj_texinfo", tex_info, "GrTexInfo, from _tmem_Select"),
        ("dcs_lock_depth", dcs_depth, "from _dcs_StatusCallbackLock"),
        ("speaker_mode", spk, "from _audio_dspcmd_ChangePan"),
        ("audiolib_ymf_jump_table", audiolib_table,
         "source table validated against _audiolib_ymf_ModuleOpen"),
        ("audiolib_jump_count", audiolib_count,
         "rep-movsd count from _audiolib_ymf_ModuleOpen"),
        ("mesh3d_vtx_hit", vhit, "cached vertex reused"),
        ("mesh3d_vtx_miss", vmiss, "vertex transformed"),
        ("cksum_table", cks["cksum_table"],
         "CODE checksum: &table[0].end, the (start,end) pair list the loop walks"),
        ("cksum_result", cks["cksum_result"],
         "CODE checksum: cell the primary loop stores its sum in"),
        ("cksum_getter", cks["cksum_getter"],
         "CODE checksum: cell _gameinit_GetCodeChecksum returns (Hydro)"),
        ("cksum_getter_fn", cks["cksum_getter_fn"],
         "CODE checksum: _gameinit_GetCodeChecksum itself, the shim site (Hydro)"),
        ("cksum_link_table", cks["cksum_link_table"],
         "CODE checksum: the link copy's own range table (Offroad)"),
        ("cksum_link_result", cks["cksum_link_result"],
         "CODE checksum: __nMyExeChecksum, the value on the wire (Offroad)"),
        ("cksum_link_fn", cks["cksum_link_fn"],
         "CODE checksum: the unnamed link copy, the shim site (Offroad)"),
        ("cksum_r2_result", cks["cksum_r2_result"],
         "R2 checksum: cell _r2file_GetComprehensiveChecksum returns"),
        ("cksum_r2_link", cks["cksum_r2_link"],
         "R2 checksum: __nMyR2Checksum, the value on the wire (Offroad)"),
        ("menu_steer", menu["menu_steer"],
         "_Controls_InputState, the select screens' steering source"),
        ("menu_sel_active", menu["menu_sel_active"],
         "select screens up: Hydro _wpr_select's flag, Offroad _wrapper_Draw's"),
        ("menu_sel_sub", menu["menu_sel_sub"],
         "the select sub-screen: Hydro's selection state, Offroad _Wrapper_nState"),
        ("menu_sel_cur_a", menu["menu_sel_cur_a"],
         "Hydro: the current track item, from _wpr_select_GetCurrentSelection"),
        ("menu_sel_cur_b", menu["menu_sel_cur_b"],
         "Hydro: the current boat item, from _wpr_select_GetCurrentSelection"),
        ("menu_sel_states_a", menu["menu_sel_states_a"],
         "mask of sub-screens reading list a (Hydro tracks; Offroad every select)"),
        ("menu_sel_states_b", menu["menu_sel_states_b"],
         "mask of sub-screens reading list b (Hydro boats)"),
        ("menu_sel_field", menu["menu_sel_field"],
         "offset into menu_steer the select screens read"),
        ("menu_item_index", menu["menu_item_index"],
         "Hydro: the item's index word, from _wpr_select_Work"),
        ("menu_band_a_lo", menu["menu_band_a_lo"],
         "Hydro: a track item's band, low edge offset"),
        ("menu_band_a_hi", menu["menu_band_a_hi"],
         "Hydro: a track item's band, high edge offset"),
        ("menu_band_b_lo", menu["menu_band_b_lo"],
         "Hydro: a boat item's band, low edge offset"),
        ("menu_band_b_hi", menu["menu_band_b_hi"],
         "Hydro: a boat item's band, high edge offset"),
        ("menu_sel_index", menu["menu_sel_index"],
         "Offroad: the selected index, from __UpdateSelectionControls"),
        ("menu_sel_target", menu["menu_sel_target"],
         "Offroad: the wheel's position as 0..1, before the filter"),
        ("menu_sel_count", menu["menu_sel_count"],
         "Offroad: the choice count (float) the index is scaled by"),
        ("menu_sel_filtered", menu["menu_sel_filtered"],
         "Offroad: the filtered position the index is taken from"),
        ("menu_sel_enabled", menu["menu_sel_enabled"],
         "Offroad: __nNumEnabledChoices"),
        ("menu_notch", menu["menu_notch"],
         "Offroad: _ffbnew_VIFYPos, the notch the motor is told to hold"),
        ("menu_ini_state", menu["menu_ini_state"],
         "initials: the state cell (Hydro) or active flag (Offroad)"),
        ("menu_ini_state_want", menu["menu_ini_state_want"],
         "initials: the value menu_ini_state_kind compares against"),
        ("menu_ini_state_kind", menu["menu_ini_state_kind"],
         "initials: 1 entering when state == want, 2 when state != want"),
        ("menu_ini_gate", menu["menu_ini_gate"],
         "initials: the gate the wheel case also needs"),
        ("menu_ini_gate_want", menu["menu_ini_gate_want"],
         "initials: the gate's value while entering"),
        ("menu_ini_slot", menu["menu_ini_slot"],
         "initials: the slot or letter the wheel selects"),
        ("menu_ini_wheel", menu["menu_ini_wheel"],
         "Offroad: the letter wheel's angle, 0..1"),
        ("menu_ini_field", menu["menu_ini_field"],
         "offset into menu_steer the initials read"),
        ("menu_sel_enable", menu["menu_sel_enable"],
         "Hydro: _wpr_select_EnableSelectionChanges, the wheel steps the list"),
        ("menu_list_a", menu["menu_list_a"], "Hydro: the track items"),
        ("menu_stride_a", menu["menu_stride_a"], "Hydro: a track item's size"),
        ("menu_count_a", menu["menu_count_a"], "Hydro: track items"),
        ("menu_list_b", menu["menu_list_b"], "Hydro: the boat items"),
        ("menu_stride_b", menu["menu_stride_b"], "Hydro: a boat item's size"),
        ("menu_count_b", menu["menu_count_b"], "Hydro: boat items"),
        ("menu_avail_a", menu["menu_avail_a"],
         "Hydro: a track item's available byte, beside menu_avail_lists bit 0"),
        ("menu_avail_lists", menu["menu_avail_lists"],
         "Hydro: lists with an available byte, bit 0 tracks, bit 1 boats"),
        ("menu_ini_count", menu["menu_ini_count"],
         "Hydro initials: letters entered so far"),
        ("menu_ini_letters", menu["menu_ini_letters"],
         "Hydro initials: letters, after which only two slots are offered"),
        ("menu_ini_k_off", menu["menu_ini_k_off"],
         "Hydro initials: the slot formula's offset (a float in the image)"),
        ("menu_ini_k_scale", menu["menu_ini_k_scale"],
         "Hydro initials: the slot formula's scale (a float in the image)"),
        ("menu_ini_k_inv", menu["menu_ini_k_inv"],
         "Hydro initials: one slot as a fraction (a float in the image)"),
        ("menu_ini_k_bins", menu["menu_ini_k_bins"],
         "Hydro initials: slots over the range (a float in the image)"),
        ("menu_ini_k_hyst", menu["menu_ini_k_hyst"],
         "Hydro initials: the hysteresis in slots (a float in the image)"),
        ("menu_ini_k_split", menu["menu_ini_k_split"],
         "Hydro initials: the two end slots' split (a float in the image)"),
        ("menu_ini_slot_max", menu["menu_ini_slot_max"],
         "Hydro initials: the last slot"),
        ("menu_ini_end_lo", menu["menu_ini_end_lo"],
         "Hydro initials: the end slot left of the split"),
        ("menu_ini_end_hi", menu["menu_ini_end_hi"],
         "Hydro initials: the end slot right of the split"),
        ("menu_sel_k_off", menu["menu_sel_k_off"],
         "Offroad: the select target is (steer - off) * scale; off (a float)"),
        ("menu_sel_k_scale", menu["menu_sel_k_scale"],
         "Offroad: the select target's scale (a float in the image)"),
        ("menu_remap_a", menu["menu_remap_a"],
         "Offroad: a list whose screens move to the nearest enabled item"),
        ("menu_remap_a_states", menu["menu_remap_a_states"],
         "Offroad: the states menu_remap_a's screen is up in"),
        ("menu_remap_b", menu["menu_remap_b"],
         "Offroad: a second such list"),
        ("menu_remap_b_states", menu["menu_remap_b_states"],
         "Offroad: the states menu_remap_b's screen is up in"),
        ("menu_en_count_off", menu["menu_en_count_off"],
         "Offroad: a list's item count (a word) at this offset"),
        ("menu_en_flag_off", menu["menu_en_flag_off"],
         "Offroad: item 0's enabled flag byte, from the list"),
        ("menu_en_flag_mask", menu["menu_en_flag_mask"],
         "Offroad: the enabled flag's bits"),
        ("menu_en_lock_off", menu["menu_en_lock_off"],
         "Offroad: item 0's lock word, from the list; enabled when 0"),
        ("menu_en_stride", menu["menu_en_stride"],
         "Offroad: a list item's size"),
        ("menu_ini_k_n", menu["menu_ini_k_n"],
         "Offroad initials: letters over the wheel (a float in the image)"),
        ("menu_ini_top", menu["menu_ini_top"],
         "Offroad initials: the letter is top - round(wheel * n)"),
        ("menu_ini_rate_sign", menu["menu_ini_rate_sign"],
         "Offroad initials: 1 when steering right raises the letter, -1 not"),
        ("menu_ini_k_snap", menu["menu_ini_k_snap"],
         "Offroad initials: inside this deflection the wheel snaps (float)"),
        ("menu_ini_k_slow", menu["menu_ini_k_slow"],
         "Offroad initials: below this it turns at its slowest (float)"),
    ]
    for f, v, note in derived:
        if v is None:
            missing.append(f)
        fields.append((f, v or 0, note if v is not None else f"NOT FOUND in {tid}: {note}"))
    for f, v in extras.items():
        fields.append((f, v, "hand-supplied; see EXTRAS in gen-gamedefs.py"))

    # --- tables ---
    w(f"static const game_fn_t GLIDE_{tid}[] = {{\n")
    for n, va, nb, src in tables["glide"]:
        w(f'    {{ 0x{va:08X}, "{n[1:]}", {nb:>3} }},  /* {src} */\n')
    w("};\n")
    if tables["unsized"]:
        w("/* not implemented by the provider: the shim supplies these itself:\n")
        for n in tables["unsized"]:
            w(f" *   {n}  @ 0x{syms[n]['va']:08X}\n")
        w(" */\n")

    w(f"\nstatic const game_imp_t IMP_{tid}[] = {{\n")
    for va, name in tables["imp"]:
        w(f'    {{ 0x{va:08X}, "{name[1:]}" }},\n')
    w("};\n")

    w(f"\nstatic const game_imp_t W32_{tid}[] = {{\n")
    for n in tables["w32"]:
        w(f'    {{ 0x{syms[n]["va"]:08X}, "{n[1:]}" }},\n')
    w("};\n")

    w(f"\nstatic const game_hw_t HW_{tid}[] = {{\n")
    for n, va, grp, _ in tables["hw"]:
        w(f'    {{ 0x{va:08X}, "{n}", "{grp}" }},\n')
    w("};\n")

    w(f"\nstatic const game_loop_t LOOP_{tid}[] = {{\n")
    for va, pl, body in loops:
        w(f"    {{ 0x{va:08X}, {pl}, {body} }},\n")
    if not loops:
        w("    { 0, 0, 0 },   /* none found; loop_count is 0 */\n")
    w("};\n")

    w(f"\nstatic const uint32_t ARROW_{tid}[] = {{\n")
    for v in arrows:
        w(f"    0x{v:08X},\n")
    if not arrows:
        w("    0,   /* none found; arrow_material_count is 0 */\n")
    w("};\n")

    w(f"\nstatic const uint32_t DRAWSCRIPT_{tid}[] = {{\n")
    for op, a, b2, c in ds_steps:
        w(f"    {op}, 0x{a:08X}, {b2}, 0x{c:08X},\n")
    if not ds_steps:
        w("    0,   /* none derived; draw_step_count is 0 */\n")
    w("};\n")

    w(f"\nstatic const game_hud_t HUD_{tid}[] = {{\n")
    for va, meta, (kind, lo, hi), name in hud_els:
        w(f'    {{ 0x{va:08X}, 0x{meta:08X}, {kind}, 0x{lo:08X}, 0x{hi:08X}, '
          f'"{name}" }},\n')
    if not hud_els:
        w("    { 0, 0, 0, 0, 0, 0 },   /* none found; hud_elem_count is 0 */\n")
    w("};\n")

    w(f"\nstatic const uint32_t HUDTXT_{tid}[] = {{\n")
    for va, name in hud_txt:
        w(f"    0x{va:08X},   /* {name} */\n")
    if not hud_txt:
        w("    0,   /* none found; hud_text_var_count is 0 */\n")
    w("};\n")

    w(f"\nstatic const uint32_t FUNCS_{tid}[] = {{\n")
    for i in range(0, len(funcs), 8):
        w("    " + " ".join(f"0x{v:08X}," for v in funcs[i:i + 8]) + "\n")
    w("};\n")

    # --- the profile ---
    w(f"\nconst game_profile_t g_profile_{tid} = {{\n")
    w(f'    .id = "{t.id}", .name = "{t.name}",\n')
    w(f'    .exe = "{os.path.basename(t.meta["exe"])}",\n')
    w(f'    .sha256 = "{t.sha256()}",\n')
    w(f"    .file_size = {len(t.read())},\n")
    w(f'    .io_board = "{t.meta["io"]}",\n')
    w(f'    .build = "{t.meta["build"]}",\n\n')
    for f, v in [("image_base", t.image_base), ("code_file_off", t.code_file_off),
                 ("code_size", t.code_size), ("code_va", t.code_va),
                 ("real_code_end_va", t.real_code_end_va), ("data_va", t.data_va),
                 ("bss_va", t.bss_va), ("stack_va", t.stack_va),
                 ("stack_size", t.stack_size), ("span_end", t.span_end)]:
        w(f"    .{f} = 0x{v:08X},\n")
    w("\n")
    for f, v, note in fields:
        w(f"    .{f} = 0x{v:08X},".ljust(46) + f"/* {note} */\n")
    w("\n")
    for name, arr, cnt in [("glide", f"GLIDE_{tid}", len(tables["glide"])),
                           ("imp", f"IMP_{tid}", len(tables["imp"])),
                           ("w32", f"W32_{tid}", len(tables["w32"])),
                           ("hw", f"HW_{tid}", len(tables["hw"])),
                           ("loop", f"LOOP_{tid}", len(loops)),
                           ("arrow_material", f"ARROW_{tid}", len(arrows)),
                           ("func_va", f"FUNCS_{tid}", len(funcs)),
                           ("hud_elem", f"HUD_{tid}", len(hud_els)),
                           ("hud_text_var", f"HUDTXT_{tid}", len(hud_txt)),
                           ("draw_step", f"DRAWSCRIPT_{tid}", len(ds_steps))]:
        cf = "func_count" if name == "func_va" else f"{name}_count"
        w(f"    .{name} = {arr}, .{cf} = {cnt},\n")
    w("};\n")

    log(f"anchors resolved          : {len(fields) - len(missing)}/{len(fields)}")
    if missing:
        log(f"  absent in {tid} (emitted 0, installers must refuse):")
        for m in missing:
            log(f"    {m}")
    log(f"function starts           : {len(funcs)}")
    return "".join(out)


def emit_span(tids):
    """The one thing that CANNOT be a runtime profile field.

    VCThunder.exe reserves the game's address space by being based at 0x100000
    with a .bss large enough that its SizeOfImage covers the whole span: the
    Windows loader reserves SizeOfImage at ImageBase before the kernel places
    thread stacks or NLS mappings, and that is the only way to own this range
    (VirtualAlloc there fails with ERROR_INVALID_ADDRESS however early you ask).
    A .bss size is fixed at link time, so the stub has to be built for the
    LARGEST span of any title it might host, and check at runtime that the title
    it actually got fits inside what it reserved.

    SizeOfHeaders must stay below the SMALLEST code_file_off, because CODE lands
    on top of this image's own PE headers and anything the loader still needs
    from them has to fit underneath.
    """
    ts = [get(t) for t in tids]
    span = max(t.span_end for t in ts)
    who = [t.id for t in ts if t.span_end == span]
    base = {t.image_base for t in ts}
    if len(base) != 1:
        raise SystemExit(f"titles disagree on image base: {base}")
    return (
        "/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n"
        "#ifndef VCT_SPAN_H\n#define VCT_SPAN_H\n\n"
        f"#define VCT_IMAGE_BASE     0x{base.pop():08X}\n"
        f"#define VCT_SPAN_END       0x{span:08X}"
        f"  /* largest span: {', '.join(who)} */\n"
        f"#define VCT_CODE_FILE_OFF  0x{min(t.code_file_off for t in ts):08X}"
        "  /* smallest across titles */\n\n"
        + "".join(f"/*   {t.id:8s} span {t.image_base:#08x}..{t.span_end:#08x}"
                  f"  ({(t.span_end - t.image_base) // 1024} KB) */\n" for t in ts)
        + "\n#endif\n")


def emit_registry(tids):
    """The list every build links. Kept generated so adding a title is one
    command, and so a profile that failed to generate cannot be silently
    referenced by a stale hand-written list."""
    out = ["/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n",
           '#include "../src/game.h"\n\n']
    for t in tids:
        out.append(f"extern const game_profile_t g_profile_{t};\n")
    out.append("\nconst game_profile_t *const g_profiles[] = {\n")
    for t in tids:
        out.append(f"    &g_profile_{t},\n")
    out.append("    0\n};\n")
    out.append("\nconst game_mame_disk_t g_mame_disks[] = {\n")
    for d in MAME_DISKS:
        if d["title"] not in tids + win32_titles():
            continue
        out.append(f'    {{ "{d["set"]}", "{d["chd"]}", "{d["name"]}", '
                   f'"{d["title"]}",\n'
                   f'      "{d["sha1"]}", "{d["rawsha1"]}",\n'
                   f'      "{d["exe_sha256"] or ""}" }},\n')
    out.append("    { 0 }\n};\n")
    return "".join(out)


# A PATTERN THAT DOES NOT MATCH EMITS 0 AND SAYS SO. That is the whole point:
# an anchor absent from a title is a fact about the title, and the shim's
# installers check for it. Guessing a plausible address instead would produce a
# shim that patches the wrong function and reports success.
def same_as_disk(path, src):
    """True when the committed file is exactly what the dump produces."""
    rel = os.path.relpath(path, REPO)
    try:
        have = open(path).read()
    except FileNotFoundError:
        print(f"  {rel}: missing; 'make defs' writes it")
        return False
    if have != src:
        print(f"  {rel}: DIFFERS from what the dump produces; 'make defs'"
              f" rewrites it, and the diff is reviewed like code")
        return False
    return True


def emit_win32_ids(t):
    """A win32 title has no anchors: the host maps it as an ordinary image and
    binds its imports. What it needs from the dump is IDENTITY, the executable
    and each of the game's own DLLs pinned by SHA-256 and size, so the host
    refuses a different build exactly as an ETS title refuses one."""
    # Executable first, then the modules in REGISTRATION order: src/arctic.h
    # names the slots by position (exe, goose.dll, gobj.dll).
    files = [(t.exe, t.meta["sha256"])] + list(t.meta["modules"].items())
    d = os.path.dirname(t.exe)
    out = ["/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n",
           '#include "../src/arctic.h"\n\n',
           f'/* {t.name}, {t.meta["build"]}: the images the host maps. */\n',
           f"const arctic_ids_t g_{t.id}_ids = {{\n",
           f'    "{t.id}", "{t.name}", "{t.meta["build"]}",'
           f' "{t.meta.get("settings", t.id)}",\n',
           "    {\n"]
    for name, sha in files:
        path = os.path.join(d, os.path.basename(name))
        size = os.path.getsize(path)
        out.append(f'        {{ "{os.path.basename(name)}", "{sha}", {size}u }},\n')
    out.append("    },\n")
    out.append("    {\n")
    for path, sha in win32_diag_files(t):
        out.append(f'        {{ "{os.path.basename(path)}", "{sha}",'
                   f' {os.path.getsize(path)}u }},\n')
    disk = t.meta["disk_dir"]
    diag = posixpath.normpath(posixpath.join(disk, t.meta["diag"]["dir"]))
    out.append("    },\n")
    out.append(f'    "{disk}",\n    "{diag}",\n')
    upd = t.meta.get("update")
    if upd:
        out.append(f'    "{upd["base"]}", "{upd["file"][0]}", "{upd["file"][1]}"\n}};\n')
    else:
        out.append("    0, 0, 0\n};\n")
    return "".join(out)


def win32_diag_files(t):
    """The operator system's two files, (path, sha256): diag.exe from the
    package's diagnostics folder, then the game's diagdll.dll beside snow.exe.
    The dump keeps diagnostics/ beside the game folder; a pack built from a
    clone keeps it as data/<id>-diagnostics."""
    g = t.meta["diag"]
    d = os.path.dirname(t.exe)
    ddir = os.path.normpath(os.path.join(d, g["dir"]))
    if not os.path.isdir(ddir):
        ddir = os.path.join(REPO, "data", f"{t.id}-diagnostics")
    return [(os.path.join(ddir, g["exe"][0]), g["exe"][1]),
            (os.path.join(d, g["dll"][0]), g["dll"][1])]


def pe_layout(path):
    """ImageBase, SizeOfImage and whether relocations are stripped, read from
    the PE32 header by offset (no package needed)."""
    b = open(path, "rb").read(4096)
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    if b[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE image")
    chars = struct.unpack_from("<H", b, pe + 4 + 18)[0]
    opt = pe + 24
    if struct.unpack_from("<H", b, opt)[0] != 0x10B:
        raise ValueError(f"{path}: not PE32")
    base = struct.unpack_from("<I", b, opt + 28)[0]
    size = struct.unpack_from("<I", b, opt + 56)[0]
    return base, size, bool(chars & 0x0001)


def pe_sections(b):
    """(name, rva, vsize, raw_off, raw_size) of a PE32 image's sections."""
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    optsz = struct.unpack_from("<H", b, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        name = b[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize, rva, rsize, roff = struct.unpack_from("<IIII", b, o + 8)
        out.append((name, rva, vsize, roff, rsize))
    return out


def pe_rva_to_off(b, rva):
    for _, va, vs, ro, rs in pe_sections(b):
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    raise ValueError(f"RVA {rva:#x} is in no section")


# The read-only constant pool the widescreen roles reach into: where the Intel
# Reference Compiler 4.5 put every float literal. It is that COMPILER'S section
# name, not the platform's, and every image this generator reads is one of its
# builds -- measured: snow.exe, goose.dll, gobj.dll, diag.exe and diagdll.dll
# all carry .text/.rdata/.data/.data1. An MSVC build of the same game has no
# .data1 at all and keeps its literals in .rdata, so another compiler's win32
# title needs its own pool name here, derived with its own signatures, or no
# pool at all: Ultimate's MSVC build shares its literals, so its sites
# redirect operands ("o") and never resolve this. Refuse by name rather than
# falling back to .rdata: these images have both, and a
# pool resolved to the wrong section would still pass the one-reader test,
# on an unrelated constant, and generate a plausible table of wrong addresses.
CONST_POOL = ".data1"


def pe_range(ranges, name, what):
    """ranges[name], or a refusal that says which image and which sections it
    does have. An absent section is a real answer about the build, so it
    stops the generator instead of raising KeyError out of a PE parser -- `what` is the image, for the message."""
    if name not in ranges:
        raise ValueError(f"{what}: no {name} section; it has "
                         f"{', '.join(ranges)}. A build by another compiler "
                         f"needs its own section names and its own signatures")
    return ranges[name]


def pe_exports(b):
    """{name: rva} from a PE32 image's export table."""
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    rva = struct.unpack_from("<I", b, pe + 24 + 96)[0]
    if not rva:
        return {}
    d = pe_rva_to_off(b, rva)
    n_names, funcs, names, ords = struct.unpack_from("<IIII", b, d + 24)
    out = {}
    for i in range(n_names):
        nrva = struct.unpack_from("<I", b, pe_rva_to_off(b, names) + 4 * i)[0]
        no = pe_rva_to_off(b, nrva)
        name = b[no:b.index(b"\0", no)].decode("latin-1")
        o = struct.unpack_from("<H", b, pe_rva_to_off(b, ords) + 2 * i)[0]
        out[name] = struct.unpack_from("<I", b, pe_rva_to_off(b, funcs) + 4 * o)[0]
    return out


def pe_import_slot(b, name):
    """The IAT RVA a PE32 image imports `name` through (by name), or None."""
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    rva = struct.unpack_from("<I", b, pe + 24 + 96 + 8)[0]
    d = pe_rva_to_off(b, rva)
    while True:
        ilt, _, _, _, iat = struct.unpack_from("<IIIII", b, d)
        if not iat:
            return None
        o, k = pe_rva_to_off(b, ilt or iat), 0
        while True:
            e = struct.unpack_from("<I", b, o + 4 * k)[0]
            if not e:
                break
            if not e & 0x80000000:
                no = pe_rva_to_off(b, e) + 2
                if b[no:b.index(b"\0", no)].decode("latin-1") == name:
                    return iat + 4 * k
            k += 1
        d += 20


def diag_exit_rva(path):
    """diag.exe's exit routine: the one function that scans the I/O, reads the
    DIP switches and tests switch 1 before it writes every changed game's CMOS
    and calls ExitProcess(0) (diagmain.c). Found
    as `call diag_io_scan; call read_dip_direct; test al,1` -- both exported
    names -- and walked back to its `push ebp; mov ebp,esp` (55 8B EC). Exactly one
    match, or nothing is generated."""
    b = open(path, "rb").read()
    ex = pe_exports(b)
    text = [s for s in pe_sections(b) if s[0] == ".text"]
    if len(text) != 1:
        raise ValueError(f"{path}: {len(text)} .text sections, not 1")
    _, va, vs, ro, rs = text[0]
    code = b[ro:ro + rs]
    hits = []
    for i in range(len(code) - 12):
        if code[i] != 0xE8 or code[i + 5] != 0xE8 or code[i + 10:i + 12] != b"\xA8\x01":
            continue
        here = va + i
        t1 = (here + 5 + struct.unpack_from("<i", code, i + 1)[0]) & 0xFFFFFFFF
        t2 = (here + 10 + struct.unpack_from("<i", code, i + 6)[0]) & 0xFFFFFFFF
        if t1 == ex.get("diag_io_scan") and t2 == ex.get("read_dip_direct"):
            back = code.rfind(b"\x55\x8B\xEC", max(0, i - 32), i)
            if back >= 0:
                hits.append(va + back)
    if len(hits) != 1:
        raise ValueError(f"{path}: the exit routine matched {len(hits)} times")
    return hits[0]


def emit_win32_diag_h(t):
    """diag.exe's range: it has no relocations, so it loads at its ImageBase
    or not at all, and the host places it there, inside VCThunder.exe's span
    (src/arctic_diag.c, which checks the range against generated/span.h)."""
    path = win32_diag_files(t)[0][0]
    base, size, stripped = pe_layout(path)
    if not stripped:
        raise ValueError(f"{path}: carries relocations; the fixed-base host"
                         f" design (src/arctic_diag.c) assumes none")
    rva = diag_exit_rva(path)
    return ("/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n"
            f"/* {t.name}'s diag.exe: ImageBase and SizeOfImage from its PE\n"
            " * header. No relocations, so this is the only place it can load. */\n"
            f"#ifndef VCT_{t.id.upper()}_DIAG_H\n#define VCT_{t.id.upper()}_DIAG_H\n"
            f"#define {t.id.upper()}_DIAG_BASE 0x{base:08X}u\n"
            f"#define {t.id.upper()}_DIAG_SIZE 0x{size:08X}u\n"
            "/* Its exit routine: writes each changed game's CMOS, then exits. */\n"
            f"#define {t.id.upper()}_DIAG_EXIT_RVA 0x{rva:08X}u\n"
            "#endif\n")


# ---- Arctic Thunder's widescreen sites (src/arctic_widescreen.c) ----------
#
# snow.exe has no symbols. Each function below is found by the bytes at its
# entry, relocated dwords masked (".."), and must match exactly once in .text;
# each site is an offset from that entry and is checked for what its role
# starts from. Kinds:
#   c   an abs32 operand naming a CONST_POOL float the role rewrites; nothing
#       else in .text may read it
#   d   the same for a double
#   i   the instruction's imm32, a float     n   the same, an integer
#   o   an abs32 float operand pointed at a host float instead: the operand
#       names the GR_VIEWPORT width (HI), or a read-only constant holding the
#       value the role starts from
#   q   the same for a double operand (ORIGIN, NORIGIN)
#   x   `fldl d(%ebp|%esp); fchs`: t = -x0/(x1-x0) at a left plane of 0
#   g   `flds d(%esp); fchs` in code shared by two planes: the entry adds
#       (gdisp, gval, tail), the frame dword naming the plane and the left
#       plane's value, checked against the switch that dispatches on it (the
#       other left plane's case jumps to the site, or to an `fstp %st(0)`
#       just before it); what follows the site must be `tail`, which writes
#       the flags before it reads them
#   v   `fdivrs (r0); movl $0,(r1)`, then `fchs` (after at most an `inc
#       %ebp`): t = -x0/(x1-x0) and the new vertex's x, at a left plane of 0
#   t   a jump-table entry whose case is `mov %ebp,(r); jmp`, the clamped x
#       stored from the function's zero register; the function must zero ebp
#   a   `fadds abs32` (the vertical bound) between `fld st0; fchs; fsts;
#       fld st1` and two `fstps`: the left bound -m is st(1) there
#   r   `test r,r; jl out; cmp r,w; jg out` on an integer x, rewritten as one
#       unsigned range test that uses ecx: what follows, up to both returns,
#       must be the title's WS_XRANGE_TAIL, which does not read ecx
# Roles (src/arctic.h): LO/HI/NHI an x bound of 0/w/-w, FRUSTUM a corner's
# +-1, RASTER/ORIGIN/NORIGIN moved into raster coordinates. XC and YC are read
# only: the viewport's centre, which is where the screen size comes from.
# Every "wholly inside, skip the clipper" test is left native: failing it only
# sends a triangle to the widened planes.
#
# Ultimate Arctic Thunder (uarctic) is the same game rebuilt by MSVC, and the
# compiler changed the shapes, not the roles: its literals are shared .rdata
# constants (640.0 has 34 readers, 0.0 has 884), so a bound is an operand
# redirected ("o"), never a constant rewritten; a corner's +-1 and the
# viewport's centre are pushed immediates; the left planes divide by memory
# ("v"); one clipper switches on its plane and shares the y and x left
# planes' code ("g", "t"); hobj computes its margin bounds from one value
# ("a"). Every uarctic function and site below was paired with 1.002's.
WS_SITES = {"arctic": [
    # camera_compute_camera_matrix: the viewport's x and y centre
    ("81 ec c4 02 00 00 d9 05", [(0x1643, "c", "XC"), (0x16c2, "c", "YC")]),
    # the culling frustum's corners, x of each (-1, -1, 1, 1)
    ("83 ec 44 d9 05 .. .. ..", [(0x005, "c", "FRUSTUM"), (0x01d, "c", "FRUSTUM"),
                                 (0x035, "c", "FRUSTUM"), (0x04d, "c", "FRUSTUM")]),
    # clipper 1: the x<=w plane, the x>=0 plane, the clamped vertex's x
    ("55 8b ec 81 ec d0 02 00", [(0x527, "c", "HI"), (0x549, "c", "HI"),
                                 (0x57a, "c", "HI"), (0x588, "c", "NHI"),
                                 (0x7c7, "c", "LO"), (0x7e9, "c", "LO"),
                                 (0x81a, "c", "LO"), (0x826, "x", "LO"),
                                 (0x5a8, "i", "HI"), (0x844, "i", "LO")]),
    # clipper 2, the same planes
    ("55 8b ec 81 ec 08 04 00", [(0x39b, "c", "HI"), (0x3b0, "c", "HI"),
                                 (0x3d1, "c", "NHI"), (0x344, "c", "LO"),
                                 (0x359, "c", "LO"), (0x378, "x", "LO"),
                                 (0x857, "i", "HI"), (0x83c, "i", "LO")]),
    # clipper 3, the same planes
    ("55 8b ec 81 ec 20 03 00", [(0x577, "c", "HI"), (0x599, "c", "HI"),
                                 (0x5ca, "c", "HI"), (0x5d8, "c", "NHI"),
                                 (0x849, "c", "LO"), (0x86b, "c", "LO"),
                                 (0x89a, "c", "LO"), (0x8a6, "x", "LO"),
                                 (0x5f8, "i", "HI"), (0x8c4, "i", "LO")]),
    # the line clipper (both ends per plane)
    ("81 ec 84 00 00 00 d9 40", [(0x1a4, "c", "HI"), (0x1b4, "c", "HI"),
                                 (0x1c6, "c", "HI"), (0x20a, "c", "HI"),
                                 (0x218, "c", "HI"), (0x305, "c", "LO"),
                                 (0x315, "c", "LO"), (0x367, "c", "LO"),
                                 (0x325, "x", "LO"), (0x373, "x", "LO"),
                                 (0x1da, "i", "HI"), (0x22c, "i", "HI"),
                                 (0x337, "i", "LO"), (0x385, "i", "LO")]),
    # hobj_give_vert_vis_class: the cull test of both branches (its
    # "wholly inside" test stays native: failing it only means clipping)
    ("57 56 55 53 81 ec 0c 01 00 00 8b", [(0x174, "c", "LO"), (0x188, "c", "HI"),
                                          (0x3a0, "c", "LO"), (0x3b8, "c", "HI")]),
    # the lens flare: the sun on screen; its occlusion read in raster x
    ("83 ec 14 33 c0 d9 05 ..", [(0x4d8, "c", "LO"), (0x4e2, "c", "HI"),
                                 (0x110, "d", "ORIGIN"), (0x219, "c", "RASTER"),
                                 (0x213, "n", "RASTER")]),
    # a ring of projected points, each drawn only on screen
    ("53 8b dc 83 ec 08 83 e4 f0 83 c4 08 eb 03 53 8b dc 55 55 8b 6b 04 "
     "89 6c 24 04 8b ec 81 ec 20", [(0x353, "c", "LO"), (0x379, "c", "HI")]),
    # two projected markers, hidden off [0, screen width]
    ("57 56 55 53 83 ec 44 dd", [(0xdb1, "c", "LO"), (0xdd5, "o", "HI")]),
    ("53 8b 5c 24 08 83 ec 28 85", [(0x174, "c", "LO"), (0x198, "o", "HI")]),
    # the midpoint of two projected track points: what is anchored there
    # fades out while it is off [0, w] x [0, h]
    ("d9 44 24 04 56 55 53 83 ec 40 d9 44 24 54", [(0xa9, "r", "HI")]),
]}

WS_SITES["uarctic"] = [
    # the camera matrix: the viewport's x and y centre, pushed to the
    # translate it ends with (320, 240, 0)
    ("d9 05 .. .. .. .. d8 15 .. .. .. .. 81", [(0xe34, "i", "XC"), (0xe28, "i", "YC")]),
    # the culling frustum's corners, x of each (-1, -1, 1, 1): pushed
    ("68 .. .. .. .. 68 00 00 80 bf 68 00 00 80 bf",
     [(0x00b, "i", "FRUSTUM"), (0x01f, "i", "FRUSTUM"),
      (0x033, "i", "FRUSTUM"), (0x047, "i", "FRUSTUM")]),
    # clipper 1: the x<=w plane, the clamped vertex's x, the x>=0 plane
    ("8b 4c 24 04 8b 54 24 0c 81 ec 38",
     [(0x41d, "o", "HI"), (0x435, "o", "HI"), (0x449, "o", "HI"),
      (0x458, "o", "HI"), (0x46a, "i", "HI"),
      (0x5df, "o", "LO"), (0x5f7, "o", "LO"), (0x60b, "o", "LO"),
      (0x624, "v", "LO")]),
    # clipper 2, the same planes, one case each of a switch on the plane
    ("8b 4c 24 04 8b 54 24 0c 81 ec fc",
     [(0x37d, "o", "HI"), (0x39b, "o", "HI"), (0x3c9, "o", "HI"),
      (0x66c, "i", "HI"), (0x432, "o", "LO"), (0x450, "o", "LO"),
      (0x474, "g", "LO", 0x28, 4, "d9 44 24 14 d8 64 24 10 de f9 3b"),
      (0x7d8, "t", "LO")]),
    # clipper 3, as clipper 1
    ("8b 4c 24 04 8b 54 24 0c 81 ec 88",
     [(0x438, "o", "HI"), (0x450, "o", "HI"), (0x464, "o", "HI"),
      (0x473, "o", "HI"), (0x485, "i", "HI"),
      (0x615, "o", "LO"), (0x62d, "o", "LO"), (0x641, "o", "LO"),
      (0x65b, "v", "LO")]),
    # the line clipper, which clips in place
    ("8b 54 24 04 d9 42 08 d8",
     [(0x16f, "o", "HI"), (0x179, "o", "HI"), (0x18f, "o", "HI"),
      (0x1c6, "o", "HI"), (0x19b, "i", "HI"), (0x1d2, "i", "HI"),
      (0x27b, "o", "LO"), (0x285, "o", "LO"),
      (0x29b, "v", "LO"), (0x2d5, "v", "LO")]),
    # hobj_give_vert_vis_class: the cull set's x bounds, 0 and w, or -m and
    # w+m with a margin m (the "wholly inside" set stays native)
    ("83 ec 40 53 55 56 57 e8 24",
     [(0x0ba, "i", "LO"), (0x066, "o", "HI"), (0x30d, "a", "LO"),
      (0x31d, "o", "HI")]),
    # the lens flare's occlusion read, moved into raster x, and the sun on
    # screen; neither function has a static caller in either build
    ("a1 .. .. .. .. 81 ec 88",
     [(0x038, "q", "NORIGIN"), (0x08d, "o", "RASTER"), (0x099, "n", "RASTER")]),
    ("83 ec 20 53 55 57 e8 a5", [(0x05e, "o", "LO"), (0x073, "o", "HI")]),
    # a ring of projected points, each drawn only on screen
    ("8b 0d .. .. .. .. 53 56 85", [(0x2d3, "o", "LO"), (0x2f0, "o", "HI")]),
    # two projected markers, hidden off [0, screen width]
    ("8b 0d .. .. .. .. 56 8b 74 24 08 8b 46", [(0x0eb, "o", "LO"), (0x0fc, "o", "HI")]),
    ("83 ec 1c 53 55 56 57 e8", [(0x254, "o", "LO"), (0x265, "o", "HI")]),
    # the midpoint of two projected track points: what is anchored there
    # fades out while it is off [0, w] x [0, h]
    ("d9 44 24 04 56 e8 b6 10", [(0x015, "r", "HI")]),
]

# What follows an "r" site, up to both returns; it must not read ecx.
WS_XRANGE_TAIL = {
    "arctic": "85 d2 7c .. 81 fa .. .. .. .. 7e .. 83 c4 .. b8 01 00 00 00 5b 5d "
              "5e c3 83 c4 .. 33 c0 5b 5d 5e c3",
    "uarctic": "85 c0 7c .. 3d .. .. .. .. 7f .. 33 c0 5e c3 b8 01 00 00 00 5e c3",
}

# The width the game reads GR_VIEWPORT into: grGet(0x26, 16, buf), the size
# checked, then the width converted and stored (`fildl; ...; fstps W`). The
# markers' HI "o" sites name it.
WS_VIEWPORT = {
    "arctic": "c7 04 24 26 00 00 00 e8 .. .. .. .. 83 f8 10 0f 85 .. .. .. .. "
              "db 45 .. db 45 .. d9 c9 d9 1d",
    "uarctic": "6a 10 6a 26 e8 .. .. .. .. 83 f8 10 74 .. 6a .. 68 .. .. .. .. "
               "68 .. .. .. .. 68 .. .. .. .. e8 .. .. .. .. 83 c4 10 "
               "db 44 24 0c 5e d9 1d",
}

# An "a" site's x87 context: before it `fld st0; fchs; fsts d(%esp); fld
# st1` (st0 m, st1 -m), after it `fstps; fstps`, the second storing st(1).
WS_ST1_BEFORE = "d9 c0 d9 e0 d9 54 24 .. d9 c1"
WS_ST1_AFTER = "d9 5c 24 .. d9 5c 24 .."


# ---- Arctic Thunder's HUD at the screen's edges (src/arctic_widescreen.c) --
#
# [graphics] widescreen_hud = edges. The race HUD is gobj.dll sprites and
# strings (a string draws as one sprite per glyph, each owning the string at
# +0x08). A HUD element is one object CREATED by a call below; the host hooks
# gobj's three create entries and remembers which objects these calls made.
# Each function is found as WS_SITES finds one; each site is an offset to its
# `call *slot(%reg)`, checked for the slot. Slots: 0x8c a sprite, 0x154 a
# string, 0x168 a printf string. The line was drawn by a census of a race,
# every draw keyed by the call that created its object: what is listed is
# each race-HUD piece left or right of the centre; the timer, the messages,
# the banners and the rival markers are not HUD and stay where they are.
HUD_SITES = {"arctic": [
    # the head-to-head icon (555, 310)
    ("83 ec 1c 33 c0 ba 00 00", [(0x080, 0x8c)]),
    # the special attack icon (68, 404)
    ("83 ec 1c b8 00 00 80 42", [(0x07b, 0x8c)]),
    # the HUD: speedometer, needle, turbo, glass, health and its pipe and
    # glasses, missile, punch (all right); radar, place frame and glass (left)
    ("57 56 55 53 83 ec 2c 33 c0", [(0x09b, 0x8c), (0x152, 0x8c), (0x1e2, 0x8c),
                                    (0x27c, 0x8c), (0x307, 0x8c), (0x392, 0x8c),
                                    (0x41d, 0x8c), (0x4c7, 0x8c), (0x58b, 0x8c),
                                    (0x656, 0x8c), (0x750, 0x8c), (0x807, 0x8c),
                                    (0x8be, 0x8c)]),
    # the track map's blips, made at load, and the map and its blips at the
    # race's start (left)
    ("53 83 ec 28 8b 1d .. ..", [(0x284, 0x8c), (0x342, 0x8c), (0x3c9, 0x8c),
                                 (0x42f, 0x8c), (0x478, 0x8c)]),
    ("57 56 55 53 81 ec 8c 00 00 00 8b 2d", [(0x0bd, 0x8c), (0x56e, 0x8c),
                                             (0x62f, 0x8c), (0x6ce, 0x8c),
                                             (0x737, 0x8c), (0x780, 0x8c)]),
    # the place: its digit (177, 427) and the field's size (235, 438) in the
    # place frame, and "%d OF %d" (552, 281)
    ("55 53 83 ec 3c 8b d8 8b", [(0x396, 0x154), (0x410, 0x168), (0x4a0, 0x168)]),
    # the atomic snowball count beside the missile icon, a string at
    # (475, 436) made again whenever the count changes (right); the census
    # race took none, so it was never drawn there
    ("53 8b 5c 24 08 83 ec 08 85 db 7c", [(0x1ae, 0x154)]),
]}

# Ultimate's calls. MSVC mostly loads `lea slot(%t),%reg` and calls
# `*(%reg)` (or `*0x0(%ebp)`), so the call does not name its slot: the
# generator finds the lea before it, and at run time a wrong claim can only
# fail to match, since the thunk for a slot sees the call's return address.
# The track map's three blip calls of 1.002 are one shared call here.
HUD_SITES["uarctic"] = [
    # the head-to-head icon (555, 310)
    ("a1 .. .. .. .. 56 c7 05 .. .. .. .. .. .. .. .. c7 05 .. .. .. .. 00 00 80 43",
     [(0x080, 0x8c)]),
    # the special attack icon (68, 404)
    ("a1 .. .. .. .. 56 c7 05 .. .. .. .. .. .. .. .. c7 05 .. .. .. .. 00 00 80 42",
     [(0x080, 0x8c)]),
    # the HUD, the same thirteen pieces as 1.002's
    ("a1 .. .. .. .. 83 ec 08 c7", [(0x09e, 0x8c), (0x162, 0x8c), (0x205, 0x8c),
                                    (0x2b0, 0x8c), (0x353, 0x8c), (0x3f5, 0x8c),
                                    (0x498, 0x8c), (0x527, 0x8c), (0x608, 0x8c),
                                    (0x70a, 0x8c), (0x800, 0x8c), (0x8c3, 0x8c),
                                    (0x98a, 0x8c)]),
    # the track map at the race's start, then its blips
    ("53 55 56 57 8b 3d .. .. .. .. 33 db", [(0x0c2, 0x8c)]),
    ("83 ec 08 56 57 8b 7c 24", [(0x0a9, 0x8c), (0x0eb, 0x8c), (0x1bc, 0x8c),
                                 (0x253, 0x8c)]),
    # the place: its digit, the field's size, and "%d OF %d"
    ("83 ec 18 56 8b 74 24 20 57 8b 7c", [(0x2eb, 0x154), (0x34e, 0x168),
                                          (0x3c1, 0x168)]),
    # the atomic snowball count, as 1.002's
    ("6a ff 68 e4 07 00 00 ff", [(0x0f0, 0x154)]),
]

# What gobj.dll is, checked by bytes: the function table its export
# get_function_table_ptr returns, and in it each create entry (HUD_SLOTS:
# the slot, its signature, the argument dwords it reads). Then the facts the
# host reads an object by: FACTS, each matching once in .text, with the
# offset a fact names at (byte, its offset within the match).
HUD_SLOTS = [
    # a sprite: (desc, x, y, z, ?) with no owner (0 at the internal create's
    # first argument)
    (0x8c, "83 ec 2c a1 .. .. .. .. c7 44 24 18 01 00 00 00 c7 04 24 00 00 00 00 "
           "8b 54 24 30 8b 4c 24 34 89 54 24 04 8b 54 24 38 89 4c 24 08 "
           "8b 4c 24 3c 89 54 24 0c 8b 54 24 40", 5),
    # a string: (text); its glyphs are made, then the text state copied in
    (0x154, "57 56 55 53 8b 6c 24 14 83 ec 0c 89 2c 24 e8 .. .. .. .. 8b d8 85 db "
            "74 .. 89 2c 24 89 5c 24 04 e8 .. .. .. .. 8d bb 98 00 00 00 be .. .. .. .. "
            "b9 10 00 00 00 f3 a5", 1),
    # a printf string: (format, ...) formatted into a buffer, then the string
    # entry's own function; room for eight dwords, where the HUD passes three
    (0x168, "83 ec 14 c7 04 24 .. .. .. .. 8b 44 24 18 8d 54 24 1c 89 44 24 04 "
            "89 54 24 08 ff 15 .. .. .. .. c7 04 24 .. .. .. .. e8", 8),
]
HUD_FACTS = {
    # the internal create: x to +0x78, the owner (its first argument) to +0x08
    "sprite_x": ("8b 55 10 89 b3 e4 00 00 00 8b 4d 14 89 53 78", 14),
    "owner": ("8b 4d 08 66 89 53 12 88 93 f9 00 00 00 89 93 f0 00 00 00 89 4b 08", 21),
    # a glyph is made with its string as the owner: (string, desc, x, 0, y, 0)
    "glyph": ("8b 44 24 64 89 6c 24 04 d9 5c 24 10 d9 5c 24 08 c7 44 24 0c 00 00 00 00 "
              "89 04 24 c7 44 24 14 00 00 00 00 e8", None),
    # the text position (x, y, z): x is the first dword of the text state,
    # which the string entry copies to +0x98
    "text_pos": ("8b 44 24 0c 8b 54 24 08 83 ec 1c 89 54 24 08 a3 .. .. .. .. "
                 "db 44 24 08 db 44 24 20 d9 1d", None),
    # every vertex layout gobj sets puts x first: grVertexLayout(XY, 0, on)
    "xy": ("b8 01 00 00 00 89 44 24 08 c7 44 24 04 00 00 00 00 89 04 24 e8", None),
    # the draw loop: each sprite's four vertices at +0x20, 20 bytes apart, as
    # one fan through grDrawVertexArrayContiguous
    "draw": ("8d 43 20 c7 44 24 0c 14 00 00 00 89 44 24 08 c7 44 24 04 04 00 00 00 "
             "c7 04 24 05 00 00 00 c7 05 .. .. .. .. 00 00 00 00 e8", 2),
}


def emit_win32_hud(t, img, base, text, rel):
    """The HUD half of generated/<id>_widescreen.c (HUD_SITES, HUD_SLOTS,
    HUD_FACTS), every value found and checked, or a ValueError."""
    gpath = os.path.join(os.path.dirname(t.exe), "gobj.dll")
    gb = open(gpath, "rb").read()
    gimg, gbase, granges, _ = pe_image(gb)
    gtext = pe_range(granges, ".text", gpath)
    u32 = lambda m, rva: struct.unpack_from("<I", m, rva)[0]
    once = lambda m, rng, sig, what: (lambda h: h[0] if len(h) == 1 else
                                      (_ for _ in ()).throw(ValueError(
                                          f"{what}: '{sig[:40]}...' matched {len(h)} times")))(
        ws_find(m, rng, sig))

    exp = pe_exports(gb)["get_function_table_ptr"]
    if gimg[exp] != 0xB8 or gimg[exp + 5] != 0xC3:
        raise ValueError(f"{gpath}: get_function_table_ptr is not `mov $t,%eax; ret`")
    table = u32(gimg, exp + 1) - gbase
    slots = []
    for slot, sig, words in HUD_SLOTS:
        target = u32(gimg, table + slot) - gbase
        if once(gimg, gtext, sig, f"{gpath} slot {slot:#x}") != target:
            raise ValueError(f"{gpath}: table slot {slot:#x} is not the function"
                             f" its signature finds")
        slots.append((slot, target, words))
    # the printf string's call is the string entry's own function
    pf = slots[2][1] + len(HUD_SLOTS[2][1].split())
    if pf + 4 + struct.unpack_from("<i", gimg, pf)[0] != slots[1][1]:
        raise ValueError(f"{gpath}: the printf string does not call the string entry")
    # the string entry copies the text state to +0x98 from the address the
    # position entry stores x at
    st = slots[1][1] + 43
    if u32(gimg, st - 4) != 0x98 or gimg[st] != 0xBE:
        raise ValueError(f"{gpath}: the string entry's state copy moved")
    state = u32(gimg, st + 1)
    tp = once(gimg, gtext, HUD_FACTS["text_pos"][0], gpath)
    if u32(gimg, tp + len(HUD_FACTS["text_pos"][0].split())) != state:
        raise ValueError(f"{gpath}: the text position's x is not the state's first dword")
    once(gimg, gtext, HUD_FACTS["glyph"][0], gpath)
    sx = once(gimg, gtext, HUD_FACTS["sprite_x"][0], gpath)
    ow = once(gimg, gtext, HUD_FACTS["owner"][0], gpath)
    sprite_x, owner = gimg[sx + HUD_FACTS["sprite_x"][1]], gimg[ow + HUD_FACTS["owner"][1]]
    for at in ws_find(gimg, gtext, HUD_FACTS["xy"][0]) or [None]:
        call = None if at is None else at + len(HUD_FACTS["xy"][0].split()) - 1
        if call is None or pe_import_slot(gb, "_grVertexLayout@12") != \
                u32(gimg, call + 5 + struct.unpack_from("<i", gimg, call + 1)[0] + 2) - gbase:
            raise ValueError(f"{gpath}: no grVertexLayout(XY, 0) where it was")
    dr = once(gimg, gtext, HUD_FACTS["draw"][0], gpath)
    vert_off = gimg[dr + HUD_FACTS["draw"][1]]
    call = dr + len(HUD_FACTS["draw"][0].split()) - 1
    thunk = call + 5 + struct.unpack_from("<i", gimg, call + 1)[0]
    if gimg[thunk:thunk + 2] != b"\xff\x25":
        raise ValueError(f"{gpath}: the draw loop's call is not an import thunk")
    if pe_import_slot(gb, "_grDrawVertexArrayContiguous@16") != u32(gimg, thunk + 2) - gbase:
        raise ValueError(f"{gpath}: the draw loop does not call grDrawVertexArrayContiguous")
    draw_ret = call + 5

    sites = []
    for sig, entries in HUD_SITES[t.id]:
        at = once(img, text, sig, t.exe)
        for off, slot in entries:
            rva = at + off
            m = img[rva:rva + 6]
            if m[0] == 0xFF and 0x90 <= m[1] <= 0x97 and m[1] != 0x94 and \
                    struct.unpack_from("<I", m, 2)[0] == slot:
                n = 6
            elif m[0] == 0xFF and (0x10 <= m[1] <= 0x17 and m[1] not in (0x14, 0x15)
                                   or m[1:3] == b"\x55\x00"):
                # `call *(%reg)`: a `lea slot(%t),%reg` comes first
                n, reg = (2, m[1] & 7) if m[1] != 0x55 else (3, 5)
                if not any(img[k] == 0x8D and img[k + 1] & 0xC0 == 0x80 and
                           (img[k + 1] >> 3) & 7 == reg and img[k + 1] & 7 != 4 and
                           struct.unpack_from("<I", img, k + 2)[0] == slot
                           for k in range(rva - 0x200, rva)):
                    raise ValueError(f"{t.exe}: +{rva:#x} calls through a register"
                                     f" no `lea {slot:#x}(..)` loaded")
            else:
                raise ValueError(f"{t.exe}: +{rva:#x} is not a call of slot {slot:#x}")
            sites.append((rva, [s for s, _, _ in slots].index(slot), n))

    out = ["\n/* The race HUD (widescreen_hud = edges): gobj.dll's create entries and"
           " the\n * snow.exe calls whose objects are the HUD. RVAs; each is checked"
           " again at\n * run time. */\n",
           "static const arctic_hud_site_t k_hud_site[] = {\n"]
    for rva, k, n in sorted(sites):
        out.append(f"    {{ 0x{rva:06X}u, {k}, {n} }},\n")
    out.append("};\n\n")
    out.append(f"const arctic_hud_table_t g_{t.id}_hud = {{\n"
               f"    0x{table:06X}u,\n"
               + "    { " + ", ".join(f"0x{s:03X}u" for s, _, _ in slots) + " },\n"
               + "    { " + ", ".join(f"0x{g:06X}u" for _, g, _ in slots) + " },\n"
               + "    { " + ", ".join(f"{w}" for _, _, w in slots) + " },\n"
               f"    0x{draw_ret:06X}u, 0x{vert_off:02X}, 0x{owner:02X},"
               f" 0x{sprite_x:02X}, 0x98,\n"
               f"    sizeof k_hud_site / sizeof k_hud_site[0], k_hud_site\n}};\n")
    return "".join(out)


# ---- Arctic Thunder's force-feedback cells (src/arctic_ffb.c) --------------
#
# What the cabinet's feel is made of, read by the host and never written. Each
# function is found as WS_SITES finds one; each cell is the abs32 operand of
# the instruction at an offset, checked for its opcode. Cells:
#   JOLT       the handlebar jolt, 0..1: the game's impact sites set it, the
#              effects callback decays it (read there, `flds JOLT`)
#   SHAKE      the handlebar shake multiplier, -1..1 (`fmuls SHAKE`)
#   SHAKER_ON  the seat shaker thread's run flag (`mov SHAKER_ON,%eax`)
#   PATTERN    the pattern it plays (`mov PATTERN,%eax`)
#   BOOST      the PATTERN value the shaker's own test mode labels "BOOST":
#              `movl $id,PATTERN` then the label handed to its printer,
#              `movl $"BOOST",(%esp)` (Intel) or `push $"BOOST"` (MSVC)
FFB_CELLS = {"arctic": [
    # the handlebar effects callback, run by the feedback thread
    ("83 ec 34 a1 .. .. .. .. ba ff ff ff ff 03 d0 85 c0 89 15",
     [(0x0e0, "d9 05", "JOLT"), (0x1b5, "d8 0d", "SHAKE")]),
    # the seat shaker thread: one pattern byte every 32 ms
    ("55 8b ec 83 ec 0c ff 15 .. .. .. .. 83 c4 f8 c7 44 24 04 0f 00 00 00",
     [(0x030, "a1", "SHAKER_ON"), (0x04a, "a1", "PATTERN")]),
    # the shaker's per-frame process, which picks the pattern
    ("56 55 53 83 ec 08 a1 .. .. .. .. 85 c0 75 .. 8b 15",
     [(0x2f6, "c7 05", "BOOST")]),
], "uarctic": [
    # the same three functions, rebuilt by MSVC: the cells' roles, clamps and
    # decay steps (jolt 0.04, shake timer 0.13 per 32 ms) are 1.002's
    ("a1 .. .. .. .. 8b c8 48 85 c9 a3 .. .. .. .. 0f 8f",
     [(0x0f5, "d9 05", "JOLT"), (0x1d1, "d9 05", "SHAKE")]),
    ("56 57 6a 0f ff 15 .. .. .. .. 50 ff 15 .. .. .. .. 8b 35 .. .. .. .. 8b 3d"
     " .. .. .. .. a1 .. .. .. .. 85 c0 74",
     [(0x01d, "a1", "SHAKER_ON"), (0x045, "a1", "PATTERN")]),
    ("a1 .. .. .. .. 53 55 56 33 f6 57 3b c6 74",
     [(0x125, "c7 05", "BOOST")]),
]}


def emit_win32_ffb(t):
    """generated/<id>_ffb.c: the cells src/arctic_ffb.c reads, each found and
    checked here (FFB_CELLS), or nothing is generated."""
    b = open(t.exe, "rb").read()
    img, base, ranges, rel = pe_image(b)
    text = pe_range(ranges, ".text", t.exe)
    data = pe_range(ranges, ".data", t.exe)
    u32 = lambda rva: struct.unpack_from("<I", img, rva)[0]
    cell, site = {}, {}
    for sig, entries in FFB_CELLS[t.id]:
        hits = ws_find(img, text, sig)
        if len(hits) != 1:
            raise ValueError(f"{t.exe}: '{sig}' matched {len(hits)} times")
        for off, op, name in entries:
            at = hits[0] + off
            opb = bytes(int(x, 16) for x in op.split())
            if img[at:at + len(opb)] != opb or at + len(opb) not in rel:
                raise ValueError(f"{t.exe}: +{at:#x} is not '{op}' on an address")
            rva = u32(at + len(opb)) - base
            if not data[0] <= rva < data[1]:
                raise ValueError(f"{t.exe}: {name} {rva + base:#x} is not in .data")
            cell[name], site[name] = rva, at
    # BOOST names PATTERN, stores an id, and the next instruction hands the
    # shaker test's printer the string "BOOST".
    at = site["BOOST"]
    if cell["BOOST"] != cell["PATTERN"]:
        raise ValueError(f"{t.exe}: +{at:#x} stores somewhere other than PATTERN")
    boost = u32(at + 6)
    label = lambda o: o in rel and bytes(img[u32(o) - base:][:7]) == b"BOOST\n\0"
    if not any(img[o:o + len(op)] == op and label(o + len(op))
               for o in range(at + 10, at + 24)
               for op in (b"\xc7\x04\x24", b"\x68")):
        raise ValueError(f"{t.exe}: no \"BOOST\" label follows the store at"
                         f" +{at:#x}")
    return ("/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n"
            '#include "../src/arctic.h"\n\n'
            f"/* {t.name}, {t.meta['build']}: the cells the cabinet's feel is"
            f" made of\n * (src/arctic_ffb.c). RVAs in snow.exe; each site is"
            f" the instruction that\n * names its cell, checked again at run"
            f" time. */\n"
            f"const arctic_ffb_cells_t g_{t.id}_ffb = {{\n"
            f"    {{ 0x{cell['JOLT']:06X}u, 0x{cell['SHAKE']:06X}u,"
            f" 0x{cell['SHAKER_ON']:06X}u, 0x{cell['PATTERN']:06X}u }},\n"
            f"    {{ 0x{site['JOLT'] + 2:06X}u, 0x{site['SHAKE'] + 2:06X}u,"
            f" 0x{site['SHAKER_ON'] + 1:06X}u, 0x{site['PATTERN'] + 1:06X}u }},\n"
            f"    {boost}\n}};\n")


# The select screens' notches (src/menu_wheel.c). feedback_set_notching(n)
# stores the count (0 in a race, 2..32 on a select screen) and resets the
# current notch to -1; the feedback thread maps the bars' position into one of
# the n equal slots, with hysteresis, and the screens read that slot back.
# POS is the position it maps, -1..1.
MENU_SIGS = {"arctic": [
    # feedback_set_notching: 0 <= n <= 32, n != 1
    ("83 ec 14 8b 44 24 18 85 c0 7c .. 8b 44 24 18 83 f8 01 74 .. 8b 44 24 18"
     " 83 f8 20 7f .. 8b 44 24 18 3b 05",
     [(0x2d, "c7 05", "INDEX"), (0x37, "a3", "COUNT")]),
    # the feedback thread's notch branch: count > 1, then (pos + 1) ...
    ("8b 15 .. .. .. .. 83 fa 01 0f 8e .. .. .. .. a1 .. .. .. .. d9 05 .. .. .."
     " .. d8 05",
     [(0x00, "8b 15", "COUNT"), (0x1a, "d8 05", "POS")]),
    # its hysteresis: the current notch is kept while |pos - its centre| is
    # under HYST notch widths (fld [0] ; fld [HYST] ; fmul st(3) ; ...)
    ("d9 05 .. .. .. .. d9 05 .. .. .. .. d8 cb d9 c2 d9 e0",
     [(0x06, "d9 05", "HYST")]),
], "uarctic": [
    # the same two, rebuilt by MSVC
    ("56 8b 74 24 08 85 f6 7c .. 83 fe 01 74 .. 83 fe 20 7e .. 68 7d 03 00 00",
     [(0x38, "89 35", "COUNT"), (0x3e, "c7 05", "INDEX")]),
    ("8b 0d .. .. .. .. 83 f9 01 0f 8e .. .. .. .. a1 .. .. .. .. 89 44 24 14"
     " db 44 24 14 d8 3d .. .. .. .. d9 05",
     [(0x00, "8b 0d", "COUNT"), (0x22, "d9 05", "POS")]),
    # (fld st(1) ; fmul [HYST] ; fxch ; fcompp ...)
    ("d9 c1 d8 0d .. .. .. .. d9 c9 de d9 df e0 f6 c4 01",
     [(0x02, "d8 0d", "HYST")]),
]}


def emit_win32_menu(t):
    """generated/<id>_menu.c: the notch cells src/menu_wheel.c reads, each
    found and checked here (MENU_SIGS), or nothing is generated. COUNT is
    named by both functions and must be one cell."""
    b = open(t.exe, "rb").read()
    img, base, ranges, rel = pe_image(b)
    text = pe_range(ranges, ".text", t.exe)
    data = pe_range(ranges, ".data", t.exe)
    # A constant is a literal: .rdata, or .data1 where that compiler put its
    # literals.
    consts = [ranges[k] for k in (".rdata", ".data1") if k in ranges]
    u32 = lambda rva: struct.unpack_from("<I", img, rva)[0]
    cell, site = {}, {}
    for sig, entries in MENU_SIGS[t.id]:
        hits = ws_find(img, text, sig)
        if len(hits) != 1:
            raise ValueError(f"{t.exe}: '{sig}' matched {len(hits)} times")
        for off, op, name in entries:
            at = hits[0] + off
            opb = bytes(int(x, 16) for x in op.split())
            if img[at:at + len(opb)] != opb or at + len(opb) not in rel:
                raise ValueError(f"{t.exe}: +{at:#x} is not '{op}' on an address")
            rva = u32(at + len(opb)) - base
            where = consts if name == "HYST" else [data]
            if not any(lo <= rva < hi for lo, hi in where):
                raise ValueError(f"{t.exe}: {name} {rva + base:#x} is not in"
                                 f" {'a literal section' if name == 'HYST' else '.data'}")
            if name in cell and cell[name] != rva:
                raise ValueError(f"{t.exe}: two {name} cells, {cell[name] + base:#x}"
                                 f" and {rva + base:#x}")
            if name not in cell:
                cell[name], site[name] = rva, at + len(opb)
    return ("/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n"
            '#include "../src/arctic.h"\n\n'
            f"/* {t.name}, {t.meta['build']}: the select screens' notches"
            f" (src/menu_wheel.c).\n * RVAs in snow.exe; each site is the"
            f" operand that names its cell, checked\n * again at run time. */\n"
            f"const arctic_menu_cells_t g_{t.id}_menu = {{\n"
            f"    {{ 0x{cell['COUNT']:06X}u, 0x{cell['INDEX']:06X}u,"
            f" 0x{cell['POS']:06X}u, 0x{cell['HYST']:06X}u }},\n"
            f"    {{ 0x{site['COUNT']:06X}u, 0x{site['INDEX']:06X}u,"
            f" 0x{site['POS']:06X}u, 0x{site['HYST']:06X}u }}\n}};\n")


def pe_image(b):
    """(image, base, {section: (rva, end)}, relocated-dword rvas): the file
    laid out at its RVAs, unrelocated. The caller names the sections it needs
    through pe_range(), so an image built by another compiler is refused by
    name instead of raising out of here."""
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", b, opt + 28)[0]
    size = struct.unpack_from("<I", b, opt + 56)[0]
    img = bytearray(size)
    ranges = {}
    for name, va, vs, ro, rs in pe_sections(b):
        img[va:va + min(vs, rs)] = b[ro:ro + min(vs, rs)]
        ranges[name] = (va, va + vs)
    rel = set()
    rrva, rsize = struct.unpack_from("<II", b, opt + 96 + 5 * 8)
    o = pe_rva_to_off(b, rrva) if rrva else 0
    end = o + rsize
    while rrva and o < end:
        page, n = struct.unpack_from("<II", b, o)
        if n < 8:
            break
        for k in range((n - 8) // 2):
            e = struct.unpack_from("<H", b, o + 8 + 2 * k)[0]
            if e >> 12 == 3:
                rel.add(page + (e & 0xFFF))
        o += n
    return img, base, ranges, rel


def ws_find(img, text, sig):
    """Every RVA in .text where the signature matches, ".." matching any
    byte."""
    pat = [None if t == ".." else int(t, 16) for t in sig.split()]
    head = bytes(pat[:next(i for i, v in enumerate(pat) if v is None)]
                 if None in pat else pat)
    hits, i = [], img.find(head, text[0])
    while 0 <= i < text[1]:
        if all(v is None or img[i + k] == v for k, v in enumerate(pat)):
            hits.append(i)
        i = img.find(head, i + 1)
    return hits


def pe_writable(b, rva):
    """Whether the section holding `rva` is writable (IMAGE_SCN_MEM_WRITE)."""
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    optsz = struct.unpack_from("<H", b, pe + 20)[0]
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        vsize, va = struct.unpack_from("<II", b, o + 8)
        if va <= rva < va + vsize:
            return bool(struct.unpack_from("<I", b, o + 36)[0] & 0x80000000)
    raise ValueError(f"RVA {rva:#x} is in no section")


def ws_dispatch(img, base, fn, end, table):
    """The register a `jmp *table(,%r,4)` in [fn, end) dispatches on, or
    None: FF 24 85+8r <table>."""
    for at in range(fn, end - 7):
        if img[at] == 0xFF and img[at + 1] == 0x24 and \
                img[at + 2] & 0xC7 == 0x85 and \
                struct.unpack_from("<I", img, at + 3)[0] == table + base:
            return (img[at + 2] >> 3) & 7, at
    return None, None


def emit_win32_widescreen(t):
    """generated/<id>_widescreen.c: every snow.exe site the host widens, each
    found and checked here (WS_SITES), or nothing is generated."""
    b = open(t.exe, "rb").read()
    img, base, ranges, rel = pe_image(b)
    text = pe_range(ranges, ".text", t.exe)
    u32 = lambda rva: struct.unpack_from("<I", img, rva)[0]
    f32 = lambda rva: struct.unpack_from("<f", img, rva)[0]
    f64 = lambda rva: struct.unpack_from("<d", img, rva)[0]
    once = lambda rng, sig, what: (lambda h: h[0] if len(h) == 1 else
                                   (_ for _ in ()).throw(ValueError(
                                       f"{t.exe}: {what} matched {len(h)} times")))(
        ws_find(img, rng, sig))
    readers = {}
    for r in rel:
        if text[0] <= r < text[1]:
            readers.setdefault(u32(r), []).append(r)

    vsig = WS_VIEWPORT[t.id]
    width_va = u32(once(text, vsig, "the GR_VIEWPORT store") + len(vsig.split()))

    sites, centre = [], {}
    for sig, entries in WS_SITES[t.id]:
        fn = once(text, sig, f"'{sig}'")
        for off, kind, role, *extra in entries:
            at = fn + off
            if kind in "cdoqt" and at not in rel:
                raise ValueError(f"{t.exe}: +{at:#x} is not an address")
            if kind in "cd":
                # the pool is resolved only for a title that rewrites its
                # constants in place (CONST_POOL)
                pool = pe_range(ranges, CONST_POOL, t.exe)
                rva = u32(at) - base
                if not pool[0] <= rva < pool[1] or len(readers[u32(at)]) != 1:
                    raise ValueError(f"{t.exe}: {u32(at):#x} is not a"
                                     f" {CONST_POOL} constant with one reader")
            else:
                rva = at
            if role in ("XC", "YC"):
                centre[role] = f32(rva)
                continue
            if kind == "x":
                m = img[rva:rva + 6]
                if m[0:2] == b"\xdd\x45" and m[3:5] == b"\xd9\xe0":
                    reg, disp = 5, m[2]
                elif m[0:3] == b"\xdd\x44\x24" and m[4:6] == b"\xd9\xe0":
                    reg, disp = 4, m[3]
                else:
                    raise ValueError(f"{t.exe}: +{rva:#x} is not fld; fchs")
                sites.append((rva, "AWS_XSUB", role, reg, disp, u32(rva), 0, 0, 0))
                continue
            if kind == "g":
                gdisp, gval, tail = extra
                m = img[rva:rva + 6]
                if m[0:3] != b"\xd9\x44\x24" or m[4:6] != b"\xd9\xe0" or \
                        not ws_find(img, (rva + 6, rva + 7), tail):
                    raise ValueError(f"{t.exe}: +{rva:#x} is not flds; fchs"
                                     f" before '{tail}'")
                # the plane is switched on from gdisp(%esp): the case for
                # gval is the last before the site, and the case before it
                # (the other left plane) jumps to the site
                disp_at = None
                for k in range(fn, rva):
                    if img[k] == 0x8B and img[k + 1] & 0xC7 == 0x44 and \
                            img[k + 2] == 0x24 and img[k + 3] == gdisp:
                        r = (img[k + 1] >> 3) & 7
                        for j in range(k + 4, k + 0x30):
                            if img[j] == 0xFF and img[j + 1] == 0x24 and \
                                    img[j + 2] == (0x85 | r << 3):
                                disp_at = j
                if disp_at is None:
                    raise ValueError(f"{t.exe}: no switch on {gdisp:#x}(%esp)"
                                     f" before +{rva:#x}")
                tab = u32(disp_at + 3) - base
                cases = [u32(tab + 4 * k) - base for k in range(gval + 1)]
                mine, other = cases[gval], cases[gval - 1]
                if not mine < rva or any(mine < c <= rva for c in cases) or \
                        not any(img[k] == 0xEB and
                                bytes(img[k + 2 + _s8(img[k + 1]):rva]) in
                                (b"", b"\xdd\xd8")      # fstp %st(0) first
                                for k in range(other, mine)):
                    raise ValueError(f"{t.exe}: +{rva:#x} is not plane {gval}'s"
                                     f" tail of the switch at +{disp_at:#x}")
                sites.append((rva, "AWS_XSUBG", role, 4, m[3], u32(rva), 0,
                              gdisp, gval))
                continue
            if kind == "v":
                m = img[rva:rva + 11]
                r0, r1 = m[1] & 7, m[3]
                if (m[0] != 0xD8 or m[1] & 0xF8 != 0x38 or m[2] != 0xC7 or
                        r1 > 7 or r0 in (4, 5) or r1 in (4, 5) or
                        u32(rva + 4) != 0 or
                        m[8:10] != b"\xd9\xe0" and m[8:11] != b"\x45\xd9\xe0"):
                    raise ValueError(f"{t.exe}: +{rva:#x} is not fdivrs;"
                                     f" movl $0; fchs")
                sites.append((rva, "AWS_XDIV", role, r0, r1, u32(rva), 0, 0, 0))
                continue
            if kind == "t":
                case = u32(at) - base
                m = img[case:case + 4]
                if not text[0] <= case < text[1] or m[0] != 0x89 or \
                        m[1] & 0xF8 != 0x28 or m[1] & 7 in (4, 5) or m[2] != 0xEB:
                    raise ValueError(f"{t.exe}: +{at:#x} does not name a"
                                     f" `mov %ebp,(r); jmp` case")
                # a table some switch of this function dispatches on holds
                # it, and the function zeroes ebp before that switch
                tab = max((u32(k + 3) - base for k in range(fn, at - 7)
                           if img[k] == 0xFF and img[k + 1] == 0x24 and
                           img[k + 2] & 0xC7 == 0x85 and
                           u32(k + 3) - base <= at), default=None)
                reg, sw = ws_dispatch(img, base, fn, at, tab) if tab else (None, None)
                if sw is None or (at - tab) % 4 or \
                        not any(img[k:k + 2] == b"\x33\xed" for k in range(fn, sw)):
                    raise ValueError(f"{t.exe}: +{at:#x} is not an entry of a"
                                     f" table switched on after ebp is zeroed")
                sites.append((rva, "AWS_JTAB", role, m[1] & 7, 0, u32(at), 0, 0, 0))
                continue
            if kind == "a":
                if (img[rva:rva + 2] != b"\xd8\x05" or rva + 2 not in rel or
                        not ws_find(img, (rva - 10, rva - 9), WS_ST1_BEFORE) or
                        not ws_find(img, (rva + 6, rva + 7), WS_ST1_AFTER)):
                    raise ValueError(f"{t.exe}: +{rva:#x} is not the margin's"
                                     f" fadds between its bounds")
                sites.append((rva, "AWS_ST1LO", role, 0, 0, u32(rva + 2), 0, 0, 0))
                continue
            if kind == "r":
                m = img[rva:rva + 12]
                r = m[1] & 7
                tail = ws_find(img, (rva + 12, rva + 13), WS_XRANGE_TAIL[t.id])
                if (m[0] != 0x85 or m[1] != (0xC0 | r << 3 | r) or r in (1, 4) or
                        m[2] != 0x7c or m[4] != 0x81 or m[5] != (0xF8 | r) or
                        m[10] != 0x7f or rva + 4 + m[3] != rva + 12 + m[11] or
                        not tail):
                    raise ValueError(f"{t.exe}: +{rva:#x} is not the x range test")
                sites.append((rva, "AWS_XRANGE", role, r, 0, u32(rva + 6), 0, 0, 0))
                continue
            if kind in "oq":
                # a float (D8/D9) or double (DC/DD) memory operand, mod 00 rm 101
                op = img[at - 2]
                if img[at - 1] & 0xC7 != 0x05 or \
                        op not in ((0xD8, 0xD9) if kind == "o" else (0xDC, 0xDD)):
                    raise ValueError(f"{t.exe}: +{at:#x} is not a"
                                     f" {'float' if kind == 'o' else 'double'}"
                                     f" operand")
            kinds = {"c": "AWS_F32", "i": "AWS_F32", "d": "AWS_F64",
                     "n": "AWS_I32", "o": "AWS_OPND", "q": "AWS_OPND64"}
            sites.append((rva, kinds[kind], role, 0, 0, u32(rva),
                          u32(rva + 4) if kind == "d" else 0, 0, 0))

    w, h = 2 * centre["XC"], 2 * centre["YC"]
    want = {"LO": 0.0, "HI": w, "NHI": -w, "RASTER": w}
    def operand_ok(va, role, size):
        """An "o"/"q" operand: the GR_VIEWPORT width (HI), or a read-only
        constant holding what the role starts from."""
        if va == width_va:
            return role == "HI" and size == 4
        if pe_writable(b, va - base):
            return False
        v = f32(va - base) if size == 4 else f64(va - base)
        if role == "FRUSTUM":
            return abs(v) == 1.0
        if role in ("ORIGIN", "NORIGIN"):
            return size == 8 and 0 < abs(v) < w
        return size == 4 and v == want.get(role)
    for rva, kind, role, _, _, raw, _, _, _ in sites:
        v = struct.unpack("<f", struct.pack("<I", raw))[0]
        ok = {"AWS_F32": lambda: (abs(v) == 1.0) if role == "FRUSTUM" else v == want.get(role),
              "AWS_F64": lambda: role == "ORIGIN",
              "AWS_I32": lambda: role == "RASTER" and 0 < raw < w,
              "AWS_OPND": lambda: operand_ok(raw, role, 4),
              "AWS_OPND64": lambda: operand_ok(raw, role, 8),
              "AWS_XSUB": lambda: role == "LO",
              "AWS_XSUBG": lambda: role == "LO",
              "AWS_XDIV": lambda: role == "LO",
              "AWS_JTAB": lambda: role == "LO",
              "AWS_ST1LO": lambda: role == "LO" and not pe_writable(b, raw - base) and
                           f32(raw - base) == h,
              "AWS_XRANGE": lambda: role == "HI" and raw == w}[kind]()
        if not ok:
            raise ValueError(f"{t.exe}: +{rva:#x} ({kind} {role}) holds {raw:#x}")

    out = ["/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n",
           '#include "../src/arctic.h"\n\n',
           f"/* {t.name}, {t.meta['build']}: what decides visibility in the"
           f" game's own\n * {int(w)}x{int(h)} coordinates"
           f" (src/arctic_widescreen.c). */\n",
           "static const arctic_ws_site_t k_site[] = {\n"]
    for rva, kind, role, reg, disp, raw, hi, gdisp, gval in sorted(sites):
        out.append(f"    {{ 0x{rva:06X}u, {kind}, AWS_{role}, {reg}, {disp:#04x},"
                   f" 0x{raw:08X}u, 0x{hi:08X}u, {gdisp:#04x}, {gval} }},\n")
    out.append("};\n\n")
    out.append(f"const arctic_ws_table_t g_{t.id}_ws = {{\n"
               f"    {int(w)}, {int(h)}, sizeof k_site / sizeof k_site[0], k_site\n}};\n")
    NATIVE[t.id] = (int(w), int(h))
    if t.id in HUD_SITES:
        out.append(emit_win32_hud(t, img, base, text, rel))
    return "".join(out)


# What each title's widescreen pass derived the composed picture to be, filled
# by emit_win32_widescreen as it runs. A title with no signatures of its own
# can only inherit from a title already in here, which is why win32_main does
# the titles that have them first.
NATIVE = {}
# The titles this run actually wrote a ws table for, which is what the
# registry lists.
MADE_WS = set()


def emit_win32_native(t, peer):
    """generated/<id>_widescreen.c for a title with no widescreen signatures.

    The host opens its window before the game runs, so it needs the size of
    the picture the game composes even when it will never widen it -- and
    refusing the title outright over a feature it is not using would be the
    wrong trade (src/arctic.c, THE WIN32 game_require()).

    The size is not assumed here, it is inherited, and only from a title whose
    gobj.dll holds THE SAME CODE: gobj.dll is what creates the Glide window
    and opens it at the game's resolution (src/arctic.c b_CreateWindowExA), so
    two titles whose gobj.dll .text is byte-identical compose the same
    picture. The table has
    no sites, which is what makes widescreen refuse for this title rather than
    claim to work (src/arctic_widescreen.c, arctic_ws_arm)."""
    w, h = NATIVE[peer]
    return ("/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n"
            '#include "../src/arctic.h"\n\n'
            f"/* {t.name}, {t.meta['build']}: no widescreen signatures match this\n"
            f" * build, so there are no sites and widescreen refuses. The"
            f" {w}x{h}\n * picture is {peer}'s: the two titles' gobj.dll hold"
            f" the same .text, and that\n * is the module that opens the Glide"
            f" window (src/arctic_widescreen.c). */\n"
            "static const arctic_ws_site_t k_site[1];\n\n"
            f"const arctic_ws_table_t g_{t.id}_ws = {{\n"
            f"    {w}, {h}, 0, k_site\n}};\n")


def win32_gobj_text(t):
    """gobj.dll's .text, as bytes: the code that creates the Glide window and
    opens it at the game's resolution. Whole-file hashes are no use for this
    -- two builds of the same code differ in the PE header's timestamp and
    checksum and in the debug directory's timestamp, and in nothing else."""
    path = os.path.join(os.path.dirname(t.exe), "gobj.dll")
    img, _, ranges, _ = pe_image(open(path, "rb").read())
    lo, hi = pe_range(ranges, ".text", path)
    return img[lo:hi]


def win32_native_peer(t):
    """The derived title this one may take its picture size from: one with
    widescreen signatures whose gobj.dll runs the same code as this title's."""
    mine = win32_gobj_text(t)
    for peer in NATIVE:
        if win32_gobj_text(get(peer)) == mine:
            return peer
    return None


def emit_win32_registry(tids):
    """generated/win32_titles.c: the win32 titles the build links, each with
    the tables derived for it. The same reason profiles.c is generated -- a
    hand-written list can reference a table that failed to generate -- and one
    more: A TABLE A TITLE HAS NO SIGNATURES FOR IS 0 HERE, which is what makes
    the feature refuse instead of reading another title's RVAs.
    `tids` is what this run generated, so a title that was refused is absent
    rather than listed with stale tables. A ws table is listed when one was
    WRITTEN (MADE_WS), not when the title has signatures: a title with none
    still gets a sites-free table carrying the picture size."""
    out = ["/* GENERATED by scripts/gen-gamedefs.py: do not edit. */\n",
           '#include "../src/arctic.h"\n']
    tables = (("ws", MADE_WS), ("hud", HUD_SITES), ("ffb", FFB_CELLS),
              ("menu", MENU_SIGS))
    for t in tids:
        out.append(f"\nextern const arctic_ids_t g_{t}_ids;\n")
        for field, have in tables:
            if t in have:
                ty = {"ws": "arctic_ws_table_t", "hud": "arctic_hud_table_t",
                      "ffb": "arctic_ffb_cells_t",
                      "menu": "arctic_menu_cells_t"}[field]
                out.append(f"extern const {ty} g_{t}_{field};\n")
        out.append(f"static const arctic_title_t k_{t} = {{\n"
                   f"    &g_{t}_ids,\n")
        for field, have in tables:
            out.append(f"    {f'&g_{t}_{field}' if t in have else '0'},"
                       f"{'' if t in have else '  /* no signatures */'}\n")
        out.append("};\n")
    out.append("\nconst arctic_title_t *const g_arctic_titles[] = {\n")
    for t in tids:
        out.append(f"    &k_{t},\n")
    out.append("    0\n};\n")
    return "".join(out)


def win32_main(check):
    """Verify every pinned file of every win32 title, then write (or --check)
    generated/<title>_ids.c and the registry. Returns the names that failed."""
    failed, ok = [], []
    MADE_WS.clear()
    NATIVE.clear()
    # Signature-bearing titles first: a title with none inherits the size of
    # the picture it composes from one of them (emit_win32_native).
    order = sorted(win32_titles(), key=lambda tid: tid not in WS_SITES)
    for tid in order:
        t = get(tid)
        print(f"\n=== {tid} ({t.name}, win32) ===")
        d = os.path.dirname(t.exe)
        bad = []
        for name, sha in [(os.path.basename(t.exe), t.meta["sha256"])] + \
                list(t.meta["modules"].items()):
            got = hashlib.sha256(open(os.path.join(d, name), "rb").read()).hexdigest()
            if got != sha:
                bad.append(name)
                print(f"  {name}: sha256 {got} is not the registered {sha}")
        for path, sha in win32_diag_files(t):
            got = hashlib.sha256(open(path, "rb").read()).hexdigest()
            if got != sha:
                bad.append(os.path.basename(path))
                print(f"  {path}: sha256 {got} is not the registered {sha}")
        if bad:
            failed.append(tid)
            print(f"  REFUSING to generate {tid}: not the registered build")
            continue
        outs = [(f"{tid}_ids.c", emit_win32_ids(t)),
                (f"{tid}_diag.h", emit_win32_diag_h(t))]
        if tid in WS_SITES:
            outs.append((f"{tid}_widescreen.c", emit_win32_widescreen(t)))
            MADE_WS.add(tid)
        else:
            peer = win32_native_peer(t)
            if peer:
                outs.append((f"{tid}_widescreen.c", emit_win32_native(t, peer)))
                MADE_WS.add(tid)
                print(f"  no widescreen signatures match; the {NATIVE[peer][0]}x"
                      f"{NATIVE[peer][1]} picture is {peer}'s, whose gobj.dll"
                      f" holds the same .text")
            else:
                print(f"  no widescreen signatures and no title shares its"
                      f" gobj.dll: {tid} has no picture size and the host will"
                      f" refuse it")
        if tid in FFB_CELLS:
            outs.append((f"{tid}_ffb.c", emit_win32_ffb(t)))
        if tid in MENU_SIGS:
            outs.append((f"{tid}_menu.c", emit_win32_menu(t)))
        ok.append(tid)
        for name, src in outs:
            path = os.path.join(OUTDIR, name)
            if check:
                if not same_as_disk(path, src):
                    failed.append(name)
                continue
            open(path, "w").write(src)
            print(f"wrote generated/{name}")

    # The registry last, and only when every win32 title generated: written
    # from a short list it would drop a title the build still has tables for.
    src = emit_win32_registry(ok)
    path = os.path.join(OUTDIR, "win32_titles.c")
    if check:
        if not same_as_disk(path, src):
            failed.append("win32_titles.c")
    elif not failed:
        open(path, "w").write(src)
        print(f"wrote generated/win32_titles.c "
              f"({len(ok)} title{'' if len(ok) == 1 else 's'})")
    return failed


def main():
    argv = sys.argv[1:]
    check = "--check" in argv
    tids = [argv[argv.index("--title") + 1]] if "--title" in argv else ets_titles()

    failed = []
    for tid in tids:
        t = get(tid)
        print(f"\n=== {tid} ({t.name}) ===")
        # REFUSE, do not warn. Every address below is an offset into THIS file;
        # generated against a different build they are a table of plausible
        # numbers pointing at the wrong instructions, and the port would report
        # success while patching whatever happens to live there. A loud warning
        # scrolls past: this is the one check that must stop the build.
        if not t.verify():
            failed.append(tid)
            print(f"  REFUSING to generate {tid}: the dump is not the registered"
                  f" build. Fix the file, or register the new hash in"
                  f" scripts/titles.py if it is genuinely a different version.")
            continue
        log = print
        tables = build(t, log)
        src = emit(t, tables, log)
        if tables["over"]:
            failed.append(tid)
            print(f"  REFUSING to write {tid}: table over ceiling")
            continue
        path = os.path.join(OUTDIR, f"{tid}_profile.c")
        if check:
            if not same_as_disk(path, src):
                failed.append(tid)
            continue
        os.makedirs(OUTDIR, exist_ok=True)
        open(path, "w").write(src)
        print(f"wrote generated/{tid}_profile.c")

    # generated/ is committed, so --check is the drift guard: what the dumps
    # produce must be byte-identical to what the build compiles.
    if check and not failed and len(tids) == len(ets_titles()):
        for name, src in (("profiles.c", emit_registry(tids)),
                          ("span.h", emit_span(tids))):
            if not same_as_disk(os.path.join(OUTDIR, name), src):
                failed.append(name)
    if not check and not failed:
        ok = [t for t in tids if t not in failed]
        open(os.path.join(OUTDIR, "profiles.c"), "w").write(emit_registry(ok))
        print(f"\nwrote generated/profiles.c ({len(ok)} titles)")
        # Only rewrite span.h when generating EVERY title, or a single-title run
        # would shrink the reservation and the next Hydro run would fail its own
        # span check with a message about a file it never touched.
        if len(ok) == len(ets_titles()):
            open(os.path.join(OUTDIR, "span.h"), "w").write(emit_span(ok))
            print("wrote generated/span.h")
        else:
            print("skipped span.h (regenerate with no --title to update it)")
    if "--title" not in argv:
        failed += win32_main(check)
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
