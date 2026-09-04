#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""gen-gamedefs.py -- emit one game_profile_t per title, from the binary.

READ-ONLY with respect to the game; writes generated/<title>_profile.c. The
output is DATA, so VCThunder can carry several titles in one image and choose
at startup. Per title it emits:

  * image layout (CODE extent, DATA/.bss/.stack, the record-stream boundary)
  * named anchors, resolved through a CANDIDATE LIST so one table serves titles
    whose subsystems have different names (_diegoio_comm_* vs _mb_io_comm_*)
  * glide[]  Glide entry points with VA and argument BYTES for cdecl->stdcall thunks
  * imp[]    __imp__ slots: slot VA + the kernel32/ws2_32 name to store there
  * w32[]    Win32 entries with no __imp__ slot: entry VA + name, patched with a jmp
  * hw[]     hardware boundaries the shim implements itself, incl. ETS int gates
  * derived offsets and call sites found by scanning code patterns

A pattern that does not match emits 0 and says so. That is the whole point: an
anchor absent from a title is a fact about the title, and the shim's installers
check for it. Guessing a plausible address instead would produce a shim that
patches the wrong function and reports success.

Usage:
    python3 scripts/gen-gamedefs.py                  # every ETS title
    python3 scripts/gen-gamedefs.py --title offroad
    python3 scripts/gen-gamedefs.py --check          # verify only, write nothing
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from titles import get, ets_titles, REPO                   # noqa: E402
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
MAX = {"glide": 96, "imp": 160, "w32": 96, "loop": 32}

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
    # Consumers found by the complete 2026-08-16 `_Viewport_*` audit. They
    # make a clip/read/state decision before the primitive-origin seam, so the
    # host gives each one a scoped coordinate or native-FOV view. The absent
    # title emits 0 for every member it does not own.
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

def d_r2_dir(t):
    """R2 directory pointer and count: unnamed file-static DATA, taken from
    _r2file_NameToDoid's first two absolute loads rather than baked in."""
    va = t.va("_r2file_NameToDoid")
    if va is None:
        return None, None
    d, off = t.read(), va - t.image_base
    if d[off] != 0xA1 or d[off + 0x16] != 0xA1:
        return None, None
    count = struct.unpack_from("<I", d, off + 1)[0]
    dirp = struct.unpack_from("<I", d, off + 0x17)[0]
    return dirp, count


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
    arms the security exchange, which is precisely what the Hydro work found it
    is best to leave unarmed. Echoing the game's own cell back is correct by
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
        ("hook_ymf_alloc",       "_ymf_AllocateSoundBuffer"),
        ("hook_ymf_free",        "_ymf_DeallocateSoundBuffer"),
        ("hook_r2_find_name",    "_r2file_FindObjectNameInDir"),
        ("hook_r2_name_to_doid", "_r2file_NameToDoid"),
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
      f" * Symbols : {os.path.relpath(t.symbols_path, REPO)}\n"
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
    r2_dir, r2_cnt = d_r2_dir(t)
    mat_tex = d_material_texture(t)
    tex_start, tex_even, tex_info = d_texobj(t)
    dcs_depth = d_dcs_lock_depth(t)
    audiolib_table, audiolib_count = d_audiolib_table(t, log)
    spk = d_speaker_mode(t)
    vhit, vmiss = d_vtxcache(t)
    arrows = d_arrow_materials(t)
    loops = d_loop_heads(t, log)
    funcs = func_table(t)

    if len(loops) > MAX["loop"]:
        log(f"ERROR: loop table has {len(loops)} entries, ceiling is {MAX['loop']}")
        tables["over"].append(("loop", len(loops), MAX["loop"]))

    mbh = d_mb_handshake(t)
    dac_out = d_dac_out(t)
    cks = d_code_cksum(t)
    hooks = d_hooks(t, log)
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
        ("r2_directory", r2_dir, "derived from _r2file_NameToDoid"),
        ("r2_directory_count", r2_cnt, "derived from _r2file_NameToDoid"),
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
    w(f'    .io_board = "{t.meta["io"]}",\n\n')
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
                           ("func_va", f"FUNCS_{tid}", len(funcs))]:
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
    return "".join(out)


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
        if check:
            continue
        os.makedirs(OUTDIR, exist_ok=True)
        path = os.path.join(OUTDIR, f"{tid}_profile.c")
        open(path, "w").write(src)
        print(f"wrote generated/{tid}_profile.c")

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
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
