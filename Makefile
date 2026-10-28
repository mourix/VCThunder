# Makefile -- build the host, the renderer, the tools, the tests and the pack.
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
#
# VCThunder: a native-Windows host for Midway's Phar Lap ETS arcade titles.
# 32-bit. Builds from WSL (cross) or from an MSYS2 MINGW32 shell (native).
#
# Titles: Hydro Thunder (1999) and Offroad Thunder (2000). Both are hosted by
# ONE binary; the profile is chosen at startup from generated/<title>_profile.c.
# Arctic Thunder is NOT one of these: it is an ordinary Win32/Glide3 program
# and needs a wrapper, not this loader.
#
# The target MUST be 32-bit x86: the host executes the game's own 1999 i386 code
# in-process at its link-time base of 0x100000. It runs on Windows 10/11 x64
# under WOW64, and on ARM64 Windows under x86 emulation.
#
# make            build build/VCThunder.exe + build/VCThunder.dll
# make defs       regenerate generated/ from the game binaries (maintainer only:
#                 generated/ is committed, and building needs no game data)
# make deps       fetch the pinned SDL3 SDK and the CHD importer's decoders
# make pack       deliver the binaries into the run directory
# (run/vcthunder/, inside this tree, always)
# make clean

# TOOLCHAIN. The same i686 mingw-w64 compiler is reached by two different names
# depending on where you build:
#
#   WSL / Linux      i686-w64-mingw32-gcc     (a cross-compiler)
#   MSYS2 MINGW32    i686-w64-mingw32-gcc, and windres as plain `windres`
#
# MSYS2 ships the cross-prefixed gcc but NOT a cross-prefixed windres, so the
# two are detected separately. Both are overridable: `make CC=... WINDRES=...`.
CC      := $(if $(shell command -v i686-w64-mingw32-gcc 2>/dev/null),i686-w64-mingw32-gcc,gcc)

# The analysis tooling is host Python and has nothing to do with the 32-bit
# target, so it does not have to come from the same environment as the compiler.
# ANY python3 works: MSYS2 has no capstone for any 32-bit environment, so
# scripts/x86len.py decodes without it. capstone is still used when the
# interpreter has it, it is the oracle x86len is validated against, but
# nothing requires it.
PYTHON  ?= python3
# -Werror: the tree has been warning-clean at these flags throughout, so this
# costs nothing today and is the only way it stays true. Nothing here is worth
# shipping past a warning: this code patches a live game image by address.
# -MMD -MP: every object writes a .d beside itself naming every header it
# actually included, and -include pulls those back in below. Without it the only
# header dependencies are the ones spelled out by hand in the pattern rules, so
# editing a struct in a header relinks stale objects and the build reports
# success. The explicit prerequisites on the rules stay: they are what the FIRST
# build has before any .d file exists.
# -msse2 -mfpmath=sse: THE HOST RUNS ON THE GAME'S OWN THREAD. A float
# expression compiled for the default i386 ABI uses the same eight x87
# registers the game is holding values in, and widescreen.c's ws_shift() runs
# per vertex inside grDrawTriangle. SSE2 is on every CPU that can run Windows
# 10. The two files here that deliberately MEASURE x87 (profile.c's time_fmul
# and x87_probe) use explicit inline asm and are unaffected.
CFLAGS  := -std=gnu11 -Wall -Wextra -Wshadow -Werror -O1 -g -I. -MMD -MP \
           -msse2 -mfpmath=sse

# -Wl,--no-insert-timestamp: WITHOUT IT, TWO BUILDS OF IDENTICAL SOURCE PRODUCE
# DIFFERENT BINARIES. mingw's ld stamps the PE header and the export directory
# with the wall clock, so relinking an unchanged DLL moves three bytes and its
# SHA-256. A change is signed off by comparing hashes against a control build,
# and with a wall-clock stamp a hash can never say that a binary is unchanged.
# Every link therefore gets this flag, and `make` twice yields the same bytes.
LDFLAGS_REPRO := -Wl,--no-insert-timestamp

# Two modules, and the split is architectural; see src/stub.c.
#
# VCThunder.exe is based at 0x100000 and carries a .bss large enough that its
# SizeOfImage covers the largest title's whole span. The Windows loader reserves
# SizeOfImage at ImageBase BEFORE the kernel places thread stacks or NLS
# mappings, which is the only way to own 0x100000..span_end: VirtualAlloc there
# fails with ERROR_INVALID_ADDRESS however early you ask. ASLR must be off so the
# loader cannot move the image off that base.
#
# That puts the stub's own code inside the game's CODE range, so it gets
# overwritten. Everything real therefore lives in VCThunder.dll, which the
# loader places high (~0x6c000000), outside the span.
#
# The stub is FREESTANDING on purpose. Linking the normal CRT gives the image a
# TLS directory whose callbacks live in .text (which the game overwrites), so
# every thread the Glide backend creates would dispatch into game code. -nostdlib
# removes the CRT, the TLS directory and the atexit machinery, leaving nothing
# that can hold a pointer into this module after the game is mapped.
#
# It is also the one module that cannot use the runtime game profile: a .bss size
# is fixed at link time, so it builds against generated/span.h, which carries the
# largest span across every title in the build.
# --disable-reloc-section is load bearing and is not the same statement as
# --disable-dynamicbase. The latter only clears a hint; mandatory ASLR ignores
# hints and relocates any image that carries relocations, which this one did.
# Measured with the mitigation forced on one child process: WITH a .reloc
# section the image was moved and the stub then
# faulted at 0xC0000005 reading 0x100000; with the section stripped the image
# loads at 0x100000 and the host runs normally under the same policy. An image
# with no relocations cannot be moved, so the guarantee stops depending on a
# system-wide setting this project does not own.
EXE_LDFLAGS := $(LDFLAGS_REPRO) -nostdlib -nostartfiles -nodefaultlibs \
               -Wl,-e,_stub_entry \
               -Wl,--image-base,0x100000 \
               -Wl,--disable-dynamicbase \
               -Wl,--disable-reloc-section
EXE_LIBS    := -lkernel32
DLL_LDFLAGS := $(LDFLAGS_REPRO) -static-libgcc -shared
# -ldinput8 is a DATA dependency only. src/dinput.c resolves DirectInput8Create
# through GetProcAddress, exactly as it does XInput, so nothing here adds an
# import: the library is linked for `c_dfDIJoystick2`, the DIJOYSTATE2 data
# format, which is a static table in the import library rather than an export.
# Verify with `objdump -p build/VCThunder.dll | grep 'DLL Name'`; dinput8.dll
# must not appear, or a machine without it stops loading the host at all.
LIBS        := -lkernel32 -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 \
               -lole32 -lavrt -lbcrypt -ldinput8 -lversion

# The per-title profiles are generated C, compiled in alongside the sources.
GEN_SRC := $(wildcard generated/*.c)
DLL_SRC := $(filter-out src/stub.c,$(wildcard src/*.c)) $(GEN_SRC)
DLL_OBJ := $(patsubst %.c,build/obj/%.o,$(DLL_SRC))
# The source list as a file, so that DELETING one relinks. Named here rather
# than beside its rule because both links list it as a prerequisite, and a
# prerequisite list is expanded where it is written.
SRC_LIST := build/obj/.sources
EXE     := build/VCThunder.exe
DLL     := build/VCThunder.dll
INI_DOC_TEST := build/tests/ini-doc-test.exe
WS_SHIFT_TEST     := build/tests/ws-shift-test.exe
NET_LINK_TEST     := build/tests/net-link-test.exe
RUMBLE_TEST       := build/tests/rumble-model-test.exe
BUTTON_TEST       := build/tests/button-mask-test.exe
LAUNCHER_PAD_TEST := build/tests/launcher-pad-test.exe
KEYLOK_TEST       := build/tests/keylok-test.exe
MENU_MODEL_TEST   := build/tests/menu-model-test.exe
IMPORT_TEST       := build/tests/import-test.exe
BEZEL_TEST        := build/tests/bezel-test.exe
CPU_TEST          := build/tests/cpu-test.exe
DI_ROLE_TEST      := build/tests/di-role-test.exe
LAUNCHER_MODEL_TEST := build/tests/launcher-model-test.exe
CONFIG_TEST       := build/tests/config-test.exe
ROUND_TRIP_TEST   := build/tests/settings-round-trip-test.exe
# NOT patch-emit-test.exe. WINDOWS REFUSES TO LAUNCH AN UNELEVATED EXE WHOSE
# FILENAME CONTAINS `patch`, `setup` or `install`: UAC's installer-detection
# heuristic decides a legacy installer needs elevation, and an unelevated
# non-interactive launch fails before main() with "Invalid argument" and no
# other word. The SOURCE keeps its name; only what lands on disk matters.
EMITTER_TEST      := build/tests/emitter-test.exe

# The window icon rides in the DLL, because the exe's PE headers are overwritten
# by the game long before there is a window to put an icon on. The .ico is
# checked in, so a build needs no Pillow.
WINDRES := $(if $(shell command -v i686-w64-mingw32-windres 2>/dev/null),i686-w64-mingw32-windres,windres)
OBJDUMP := $(if $(shell command -v i686-w64-mingw32-objdump 2>/dev/null),i686-w64-mingw32-objdump,objdump)
DLLTOOL := $(if $(shell command -v i686-w64-mingw32-dlltool 2>/dev/null),i686-w64-mingw32-dlltool,dlltool)
RES     := build/obj/res/vcthunder.res.o

SPAN    := generated/span.h

SDL3_VERSION := 3.4.10

# SDL3 LIVES UNDER build/, ALWAYS. `make deps` fetches the official i686 MinGW
# SDK and checks its SHA-256 before extracting, into build/deps/, which
# .gitignore covers, and which `make clean` deliberately does NOT remove, since
# re-downloading a pinned dependency on every clean serves nobody.
#
# WHAT THE BUILD USES MUST NOT DEPEND ON WHAT HAPPENS TO SIT NEXT TO THE TREE:
# the SDK comes from build/deps and nowhere else. Override SDL3_ROOT if you keep
# it somewhere else.
SDL3_DIR     := SDL3/SDL3-$(SDL3_VERSION)/i686-w64-mingw32
SDL3_HOME    := build/deps
SDL3_ROOT    ?= $(SDL3_HOME)/$(SDL3_DIR)
SDL3_IMPORT  := $(SDL3_ROOT)/lib/libSDL3.dll.a
SDL3_RUNTIME := $(SDL3_ROOT)/bin/SDL3.dll
# The one file every renderer object needs, and the reason it is a variable: it
# is a prerequisite of the object rule, so a fresh clone that runs a bare `make`
# sees the "run 'make deps'" refusal below instead of the first compile dying on
# `SDL3/SDL.h: No such file or directory`.
SDL3_HEADER  := $(SDL3_ROOT)/include/SDL3/SDL.h

# The run directory is the USER'S: one pack, both titles, holding the tuned ini,
# both game dumps and the NVRAM. It is ALWAYS run/ INSIDE THIS TREE. There is no
# workspace probe and no branch here on purpose: a build packs into the
# repository it was built in, and nowhere else. A conditional nobody remembers
# is worth less than a path everybody can see.
#
# WHAT THIS COSTS, AND THE ONE COMMAND THAT MUST NOT BE RUN CASUALLY: run/ holds
# the tuned ini, both game dumps and the NVRAM, and none of it is reproducible
# from this repository. `.gitignore` covers /run/ and `make clean` does not touch
# it, but `git clean -xdf` deletes ignored files BY DEFINITION and would take all
# of it. Use `git clean -xdf -e /run/`. `rm -rf build` is safe: run/ is not
# under build/.
RUN     ?= run
TITLES  := hydro offroad

# ---- the renderer -------------------------------------------------------
#
# vcglide.dll is a Glide 2.x PROVIDER: it exports the same entry points the
# games call, is loaded statically by the host, and rasterises every one of
# them itself.
#
# It shares NO source with the host. That is the point: the seam is a DLL
# boundary, which is also where the licence boundary sits.
# core/ is portable C; win32/ is the one platform file (see present.c). A
# port replaces the second directory and nothing in the first.
# Compile-and-link-in-one-step rules (the stub, the replay harness, the
# ini-doc test) produce no object for a .d to sit beside, so they drop the
# dependency flags rather than scattering strays through build/.
LINK_CFLAGS := $(filter-out -MMD -MP,$(CFLAGS))

VGL_SRC := $(wildcard renderer/core/*.c) $(wildcard renderer/win32/*.c)
#
# The renderer gets its own flags, for one reason each.
#
#   -O2          it is the only compute code in this repository. The host is
#                glue and does not care; a span loop does.
#   -msse2       -mfpmath=sse
#                THE RASTERISER MUST NOT TOUCH THE x87 STACK. The seam's
#                stubs avoid it deliberately because the GAME's code is using
#                x87 across these calls, and a rasteriser is all floating
#                point. With the default i386 ABI
#                every float expression here would push and pop the same eight
#                registers the game is holding values in. SSE2 has been present
#                on every CPU that can run Windows 10.
#                It also makes our own output REPRODUCIBLE: x87's 80-bit
#                intermediates make a result depend on register allocation, so
#                the same source at two optimisation levels can disagree in the
#                last bit, which, for a provider whose whole test is a
#                per-pixel diff, would be indistinguishable from a bug.
# -msse2 -mfpmath=sse is in CFLAGS now (see the note there: src/ needs it for
# the same reason renderer/ does), so all this rule still changes is -O1 -> -O2.
VGL_CFLAGS := $(filter-out -O1,$(CFLAGS)) -O2 \
              -I$(SDL3_ROOT)/include -Ibuild/obj/renderer
VGL_OBJ := $(patsubst %.c,build/obj/%.o,$(VGL_SRC))
VGL_DLL := build/vcglide.dll
VGL_DEF := build/obj/renderer/vcglide.def
VGL_SHADER_INC := build/obj/renderer/gpu_shader.inc
SDL3_RUNTIME_OUT := build/SDL3.dll

# The exported names carry the leading underscore (_grDrawTriangle@12), and
# a mingw .def is the only way to say that. It is GENERATED from entries.def
# so the export list and the stubs cannot drift: one list, two consumers.
#
# The `EXPORTED = internal` form is load-bearing. Given a bare name, ld adds
# the C leading underscore ITSELF while looking the symbol up, so a line
# reading `_grDrawTriangle@12` sends it hunting for __grDrawTriangle@12 and
# the link fails with "symbol not defined" on all 69 at once. Naming both
# sides says exactly what is meant: export _grDrawTriangle@12, and it is the
# C symbol grDrawTriangle@12 (which mingw itself spells _grDrawTriangle@12).
# Depends on this Makefile too: the export list is written HERE, and a def that
# only watched entries.def kept a stale export list through a build that added
# one.
$(VGL_DEF): renderer/api/entries.def Makefile
	@mkdir -p $(dir $@)
	@{ echo "LIBRARY vcglide.dll"; echo "EXPORTS"; \
	   sed -n 's/^VGL_ENTRY(\([A-Za-z0-9_]*\),  *\([0-9]*\).*/    _\1@\2 = \1@\2/p' \
	     $<; \
	   echo "    vglInit@0 = vglInit@0"; \
	   echo "    vglHealth@0 = vglHealth@0"; \
	   echo "    vglBlackMargins@0 = vglBlackMargins@0"; \
	   echo "    vglSpecBegin@0 = vglSpecBegin@0"; \
	   echo "    vglSpecEnd@0 = vglSpecEnd@0"; \
	   echo "    vglSetBezel@28 = vglSetBezel@28"; \
	   echo "    _grLfbReadRegion@28 = grLfbReadRegion@28"; } > $@
	@echo "generated $@ ($$(grep -c '^VGL_ENTRY' $<) game entry points + 7)"

# THE CHD IMPORTER'S TWO THIRD-PARTY DECODERS, fetched by `make deps` like SDL3
# and never committed: zlib's inflate (zlib licence) for a CHD's `zlib` hunks,
# and 7-Zip's public-domain LzmaDec.c for its `lzma` hunks. Compiled as they
# come, so their warnings are not ours to fix (-w), and at -O2: they are compute
# code with no patched entry point. src/chd.c is the only file that includes
# them.
ZLIB_DIR := build/deps/zlib/zlib-1.3.2
LZMA_DIR := build/deps/lzma/C
DEC_SRC  := $(addprefix $(ZLIB_DIR)/,inflate.c inftrees.c inffast.c zutil.c \
                                     adler32.c crc32.c) \
            $(LZMA_DIR)/LzmaDec.c
DEC_OBJ  := $(patsubst build/deps/%.c,build/obj/deps/%.o,$(DEC_SRC))
DEC_HDR  := $(ZLIB_DIR)/zlib.h $(LZMA_DIR)/LzmaDec.h

build/obj/deps/%.o: build/deps/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -g -w -msse2 -mfpmath=sse -c -o $@ $<

build/obj/src/chd.o: CFLAGS += -O2 -fno-strict-aliasing -I$(ZLIB_DIR) -I$(LZMA_DIR)
build/obj/src/chd.o: $(DEC_HDR)
build/obj/src/bezel.o: CFLAGS += -O2 -I$(ZLIB_DIR)
build/obj/src/bezel.o: $(DEC_HDR)

$(DEC_HDR) $(DEC_SRC):
	@echo "The CHD importer's decoders are missing: $@"
	@echo
	@echo "    make deps"
	@echo
	@echo "downloads the pinned zlib and LZMA sources into build/deps and checks"
	@echo "their SHA-256."
	@exit 1

# Missing SDK: refuse by name, the way generated/ does, rather than leaving the
# compiler to report a missing include. Nothing regenerates this file, so once
# `make deps` has put it there make never runs this recipe again.
$(SDL3_HEADER):
	@echo "SDL3 $(SDL3_VERSION) is missing: the renderer needs its headers."
	@echo
	@echo "    make deps"
	@echo
	@echo "downloads the pinned SDK into build/deps and checks its SHA-256,"
	@echo "or 'make setup' to do that, build, and stage a run directory."
	@exit 1

# Its own rule, ahead of the host's: renderer/ must not acquire a dependency
# on src/vcthunder.h or generated/span.h, and a stray -I. include of either
# would be a design regression rather than a build error.
build/obj/renderer/%.o: renderer/%.c renderer/core/vcglide.h \
                        renderer/core/vgl.h renderer/core/capture.h \
                        renderer/api/glide2.h renderer/api/entries.def \
                        $(SDL3_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(VGL_CFLAGS) -c -o $@ $<

$(VGL_SHADER_INC): renderer/win32/gpu_shader.hlsl tools/embed-text.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/embed-text.py $< $@ vgl_gpu_shader_source

build/obj/renderer/win32/gpu.o: $(VGL_SHADER_INC)

$(VGL_DLL): $(VGL_OBJ) $(VGL_DEF) $(SRC_LIST)
	@mkdir -p $(dir $@)
	@test -f $(SDL3_IMPORT) || { echo "SDL3 $(SDL3_VERSION) is missing; run 'make deps'"; exit 1; }
	$(CC) $(LDFLAGS_REPRO) -shared -static-libgcc -o $@ $(VGL_OBJ) $(VGL_DEF) \
	  -L$(SDL3_ROOT)/lib -lSDL3 -lkernel32 -luser32 -lgdi32 -ld3d9 \
	  -ld3dcompiler_47 -lm
	@echo "built $@ (Glide 2.x provider and rasteriser)"
	@sha256sum $@

# The same guard as $(VGL_DLL)'s, and it has to be on BOTH: under -j this rule
# is reached first, and without it a build before `make deps` fails with make's
# own "No rule to make target .../SDL3.dll" instead of the sentence that says
# what to run. An order-only prerequisite would not do it either; the file is
# genuinely absent, so the message has to come from a recipe that can run.
$(SDL3_RUNTIME_OUT):
	@mkdir -p $(dir $@)
	@test -f $(SDL3_RUNTIME) || { echo "SDL3 $(SDL3_VERSION) is missing; run 'make deps'"; exit 1; }
	cp $(SDL3_RUNTIME) $@

# The conformance harness: replay a captured call stream into any provider and
# write out what it drew. 32-bit, because a Glide2 provider is. It links
# against nothing (not the host, not the renderer), so that what it proves
# about a capture does not depend on either.
REPLAY := build/glide-replay.exe

$(REPLAY): tools/glide-replay.c renderer/core/png.c renderer/api/glide2.h \
           renderer/core/capture.h renderer/core/png.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -o $@ $< renderer/core/png.c -lkernel32 -luser32
	@echo "built $@ (capture replay / golden-frame harness)"

# ---- the synthetic conformance capture ----------------------------------
#
# The renderer's own regression test, and the one check that needs no game
# data. Every capture of a real title is game material, and scan-contraband.py
# correctly refuses to let any into this repository, so "replay a capture and
# prove the change moved no pixel" cannot be a check that lives here.
#
# tools/make-conformance-capture.py has no game in it: the scene is authored
# from the published Glide semantics and expanded into a .vglc at build time,
# so the whole loop runs on a clone with no dumps. It covers gouraud and four
# blend modes, both texture filters, wrap and clamp, a P_8 palette and an
# RGB_565 chain, mip selection across the whole chain, the SPLIT EVEN/ODD chain
# cross-faded on LOD_FRACTION across two TMUs, chroma key, alpha test, fog, the
# W buffer, a polygon vertex list and an LFB page.
#
# It is NOT an oracle. It says the renderer draws what it drew before; only
# 86Box can say it draws what a Voodoo2 drew. And it does not replace replaying
# real game captures, which exercise state no authored scene thinks of.
#
# CPU backend on purpose: it is the reference path, and the GPU backend is not
# expected to be bit-identical to it. The hashes are this toolchain's output.
CONF_CAPTURE := build/conformance.vglc
CONF_GOLDEN  := tests/conformance-frames.hash

$(CONF_CAPTURE): tools/make-conformance-capture.py renderer/api/entries.def
	@$(PYTHON) $< $@

# The harness is a Windows binary. Under WSL its arguments and environment have
# to reach it as Windows sees them: relative paths from a directory Windows can
# open, and WSLENV or the variables are simply absent. Under
# MSYS2 the harness is native and WSLENV means nothing, so it is set only where
# it does something.
WSLENV_PASS := $(if $(wildcard /proc/sys/fs/binfmt_misc/WSLInterop),WSLENV=VCGLIDE_RENDERER:VCGLIDE_PRESENT:VCGLIDE_LOG/p,)
# A binfmt entry matching every MZ binary (mono's `cli`) claims a .exe before
# WSL interop does, and then nothing runs. Where one is enabled, go through
# cmd.exe; anywhere else, by path, which also works from a Linux-side checkout.
WIN_EXE := $(if $(filter enabled,$(shell head -n1 /proc/sys/fs/binfmt_misc/cli 2>/dev/null)),/init /mnt/c/WINDOWS/System32/cmd.exe /c ,./)
conformance: $(CONF_CAPTURE) $(VGL_DLL) $(REPLAY)
	@cd build && $(WSLENV_PASS) \
	   VCGLIDE_RENDERER=cpu VCGLIDE_PRESENT=0 VCGLIDE_LOG=conformance.log \
	   $(WIN_EXE)glide-replay.exe conformance.vglc --provider vcglide.dll \
	   --hash conformance.hash > conformance.out 2>&1 \
	  || { echo "conformance: the replay itself failed:"; cat conformance.out; exit 1; }
	@diff -u $(CONF_GOLDEN) build/conformance.hash > /dev/null 2>&1 \
	  && echo "conformance: $$(wc -l < build/conformance.hash) frames match the golden hashes" \
	  || { echo "conformance: THE RENDERER'S OUTPUT CHANGED"; \
	       diff -u $(CONF_GOLDEN) build/conformance.hash; \
	       echo "If that was intended, look at the frames first --"; \
	       echo "  make conformance-frames   writes build/conf-frames/*.png"; \
	       echo "then 'make conformance-accept' to adopt the new hashes."; exit 1; }

conformance-frames: $(CONF_CAPTURE) $(VGL_DLL) $(REPLAY)
	@mkdir -p build/conf-frames
	@cd build && $(WSLENV_PASS) \
	   VCGLIDE_RENDERER=cpu VCGLIDE_PRESENT=0 VCGLIDE_LOG=conformance.log \
	   $(WIN_EXE)glide-replay.exe conformance.vglc --provider vcglide.dll \
	   --hash conformance.hash --out conf-frames > conformance.out 2>&1
	@echo "wrote build/conf-frames/*.png: LOOK AT THEM before accepting anything"

# Deliberate, and never automatic: adopting a hash is saying the new picture is
# the right one, which is a judgement about pixels and not about a build.
conformance-accept: conformance-frames
	@cp build/conformance.hash $(CONF_GOLDEN)
	@echo "adopted build/conformance.hash as $(CONF_GOLDEN)"
	@cat $(CONF_GOLDEN)


.PHONY: all check check-full defs deps pack pack-cfg pack-ini clean renderer tools ini-doc-test \
        launcher-model-test launcher-pad-test \
        config-test round-trip-test emitter-test button-test di-role-test import-test bezel-test \
        keylok-test menu-model-test cpu-test \
        FORCE \
        conformance conformance-frames conformance-accept widescreen-test net-link-test \
        probe-dll

# WITHOUT THIS, A BARE `make` BUILDS THE GENERATED .def AND NOTHING ELSE.
# GNU make's default goal is the first target in the file, and $(VGL_DEF)'s
# rule has to appear early because the renderer's own rules precede the host's.
# The symptom is silent and expensive: `make` prints
# "'build/obj/renderer/vcglide.def' is up to date", exits 0, and leaves
# VCThunder.dll as it was several source edits ago, which is exactly how a
# live run comes up in the wrong rendering mode with an ini key the binary has
# never heard of.
.DEFAULT_GOAL := all

# The generated header dependencies. A missing .d is not an error: that is
# the first build, which the pattern rules' explicit prerequisites cover.
DEPS := $(DLL_OBJ:.o=.d) $(VGL_OBJ:.o=.d)
-include $(DEPS)


all: $(EXE) $(DLL) $(VGL_DLL) $(SDL3_RUNTIME_OUT)

renderer: $(VGL_DLL) $(SDL3_RUNTIME_OUT)
tools: $(REPLAY)

deps:
	$(PYTHON) tools/fetch-deps.py --into build/deps

$(EXE): src/stub.c $(SPAN)
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(EXE_LDFLAGS) -o $@ src/stub.c $(EXE_LIBS)
	@# A linker that ignored --disable-reloc-section leaves an image mandatory
	@# ASLR is allowed to move, and nothing at the preferred base would ever
	@# show it. src/stub.c refuses such an image at runtime; fail here instead,
	@# because this is a property of the link and not of the machine it runs on.
	@if $(OBJDUMP) -p $@ | grep -q 'relocations stripped'; then \
	  echo "$@: relocations stripped, so mandatory ASLR cannot move it"; \
	else \
	  echo "$@: REFUSED: the linker kept a .reloc section despite"; \
	  echo "    -Wl,--disable-reloc-section, so mandatory ASLR may relocate"; \
	  echo "    this image and the game's span would not be reserved."; \
	  exit 1; \
	fi
	@echo "built $@ (span-reserving stub, base 0x100000)"
	@sha256sum $@

$(DLL): $(DLL_OBJ) $(DEC_OBJ) $(RES) $(SRC_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(DLL_LDFLAGS) -o $@ $(DLL_OBJ) $(DEC_OBJ) $(RES) $(LIBS)
	@echo "built $@ (all logic, loaded outside the span)"
	@sha256sum $@

# vcthunder.ini IS a prerequisite: it is embedded as RCDATA and written out by
# config_seed_ini() when a pack has no settings file. Editing the shipped ini
# and not rebuilding would leave the binary handing out the previous one.
$(RES): res/vcthunder.rc res/vcthunder.ico src/resource.h vcthunder.ini
	@mkdir -p $(dir $@)
	$(WINDRES) -I . -i $< -o $@

# THE HOST STAYS AT -O1, AND -O2 IS NOT A FREE CHOICE HERE.
#
# widescreen.c compiled at -O2 faults on the THIRD FRAME (0xC0000005 executing
# a non-executable page at a fresh heap address); at -O1 it runs indefinitely.
# The cause is its twelve `__cdecl` entry points the game jumps into through a
# relocated prologue: pin just those to -O1 and the rest of the file is fine at
# -O2. So the hazard is not the file, it is the CLASS: our functions that the
# game's own code enters through a patched prologue, on the game's stack, under
# a contract the compiler cannot see.
#
# Twelve files install such code (`patch_jmp`, `patch_call`, `hook_call_through`)
# and every one of them stays at -O1. The exception below is the only host file
# with a real compute loop and NO patched entry point:
#
#   audio_offroad.c   the 32-voice mixer, per sample, on the WASAPI thread. The
#                     game reaches this file only through the Audiolib jump
#                     table: an ordinary indirect call with a declared
#                     signature, which is a different thing entirely.
#
# glide_bind.c and audio.c are not in this list: both carry patched entry points
# (gl_init3dfx, av_alloc_buffer, av_free_buffer).
#
# -fno-strict-aliasing rides with the -O2: this host reads and writes the game's
# memory through casts of arbitrary addresses to arbitrary pointer types, and
# -O2 turns on the aliasing assumptions that make that undefined.
HOT := build/obj/src/audio_offroad.o
$(HOT): CFLAGS += -O2 -fno-strict-aliasing

build/obj/%.o: %.c src/vcthunder.h src/game.h $(SPAN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# THE SOURCE LIST, AS A FILE THE LINKS DEPEND ON.
#
# DLL_OBJ and VGL_OBJ come from $(wildcard), so DELETING a .c is invisible to
# make: the object stops being listed, the binary is still newer than every
# object that remains, and nothing relinks. The DLL goes on carrying the
# translation unit whose source you just deleted, and there is no longer an
# object on disk to suggest it. That is the silently-stale binary this rule
# exists to prevent; sweeping the orphan objects is the cosmetic half.
#
# The stamp is only REWRITTEN when the list actually changes, so it does not
# relink on every build; `cmp` is what makes that true, not the recipe running.
# When it does change, the orphans go with it, because that is the one moment
# their absence is known. The keep-list IS the link list, so this can never
# delete an object that is about to be needed.
#
# $(RES) is named explicitly: it is built from a .rc and would otherwise look
# sourceless. A new kind of object added without being added here would be
# pruned on every build, which is noisy rather than silent, and that is the
# intended failure.
KEEP_OBJ := $(DLL_OBJ) $(VGL_OBJ) $(DEC_OBJ) $(RES)

FORCE:

$(SRC_LIST): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' $(DLL_SRC) $(VGL_SRC) | cmp -s - $@ && exit 0; \
	existed=$$(test -f $@ && echo yes); \
	printf '%s\n' $(KEEP_OBJ) $(KEEP_OBJ:.o=.d) | LC_ALL=C sort -u > $@.keep; \
	find build/obj \( -name '*.o' -o -name '*.d' \) | LC_ALL=C sort > $@.have; \
	comm -13 $@.keep $@.have | while IFS= read -r f; do \
	  echo "pruned $$f: its source is gone"; rm -f "$$f"; \
	done; \
	rm -f $@.keep $@.have; \
	printf '%s\n' $(DLL_SRC) $(VGL_SRC) > $@; \
	test -z "$$existed" || echo "the source list changed; relinking"

# Everything a CI job would run, run locally. A CI workflow, if one is added,
# should call exactly this target so the two cannot drift.
#
# Deliberately ordered cheapest-first: the scan takes milliseconds and is the one
# whose failure is unrecoverable once published.
#
# A CHECK THAT RE-READS GIGABYTES WHICH CANNOT CHANGE BETWEEN RUNS records a key
# when it passes (tools/stamp.py) and re-reads only when something it depends
# on has moved. The key is content for the checker and its outputs, size+mtime
# for the gigabytes, which is a cache key and not evidence, so the skip prints
# as a skip and `make check-full` ignores every stamp. What each key covers is
# spelled out rather than derived: forgetting an input here buys a stale pass.
STAMPS := build/.stamps

# generated/ is checked against the dumps by the generator, so the key is: the
# generator and everything it imports (scripts/*.py), the Glide entry table it
# reads, the files it would emit, and per title the dump plus the symbol cache
# derived from it.
GAMEDEFS_STAMP_IN = --hash $(wildcard scripts/*.py) renderer/api/entries.def \
                           $(wildcard generated/*) \
                    --stat $$($(PYTHON) scripts/titles.py --stamp-inputs)

# CHECK_FORCE=1 ignores every stamp. `make check-full` is the spelling.
check-full:
	@$(MAKE) --no-print-directory check CHECK_FORCE=1

check:
	@echo "== contraband scan =="
	@$(PYTHON) tools/scan-contraband.py
	@# EVERY PYTHON TOOL PARSES. The C is checked by being compiled; a Python
	@# tool whose own check SKIPS without game data would otherwise break
	@# unseen. Parsing costs milliseconds.
	@for f in tools/*.py scripts/*.py; do \
	   $(PYTHON) -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" $$f \
	     || { echo "$$f does not parse"; exit 1; }; \
	 done
	@echo "python: $$(ls tools/*.py scripts/*.py | wc -l) tool(s) parse"
	@$(PYTHON) tools/check-launcher-settings.py
	@# Every relative markdown link resolves. It runs in milliseconds, so a
	@# broken link is caught by the commit that broke it.
	@$(PYTHON) tools/check-links.py
	@echo
	@echo "== build =="
	@$(MAKE) --no-print-directory all
	@echo
	@echo "== INI document round trip =="
	@$(MAKE) --no-print-directory ini-doc-test
	@echo
	@echo "== the launcher's model, with no launcher =="
	@$(MAKE) --no-print-directory launcher-model-test
	@echo
	@echo "== the loader's own parser, over an ini a person wrote =="
	@$(MAKE) --no-print-directory config-test
	@echo
	@echo "== every setting, launcher -> file -> host =="
	@$(MAKE) --no-print-directory round-trip-test
	@echo
	@echo "== the runtime code emitter, and the code it emits =="
	@$(MAKE) --no-print-directory emitter-test
	@echo
	@echo "== widescreen primitive shift =="
	@$(MAKE) --no-print-directory widescreen-test
	@echo
	@echo "== the link's single machine case, over real sockets =="
	@$(MAKE) --no-print-directory net-link-test
	@echo
	@echo "== the derived pad rumble, on a pinned timeline =="
	@$(MAKE) --no-print-directory rumble-test
	@echo
	@echo "== the 128-button mask, against the 32-bit code it replaced =="
	@$(MAKE) --no-print-directory button-test
	@echo
	@echo "== the launcher driven from a controller =="
	@$(MAKE) --no-print-directory launcher-pad-test
	@echo
	@echo "== the cabinet key, against snow.exe's own checks =="
	@$(MAKE) --no-print-directory keylok-test
	@echo
	@echo "== the select screens' virtual wheel, on modelled screens =="
	@$(MAKE) --no-print-directory menu-model-test
	@echo
	@echo "== steering, gas and brake from different controllers =="
	@$(MAKE) --no-print-directory di-role-test
	@echo
	@echo "== the CHD importer, on a disk built in memory =="
	@$(MAKE) --no-print-directory import-test
	@echo
	@echo "== bezel art: PNG decoding, the screen hole, the layout =="
	@$(MAKE) --no-print-directory bezel-test
	@echo
	@echo "== the cores a hybrid processor gives the game =="
	@$(MAKE) --no-print-directory cpu-test
	@echo
	@echo "== renderer conformance (synthetic capture, CPU reference) =="
	@$(MAKE) --no-print-directory conformance
	@echo
	@echo "== generated tables match the registered dumps =="
	@# Two outcomes, not three. Missing capstone does not skip this check;
	@# it would skip it on exactly the host that cannot install capstone,
	@# MSYS2 MINGW32, which has no capstone package in any 32-bit repository.
	@# The generator decodes with capstone when it is there and with x86len
	@# when it is not, and both emit the same bytes. No dumps is a real skip,
	@# because a check that did not run must never read as one that passed.
	@if ! $(PYTHON) scripts/titles.py --have-dumps > /dev/null 2>&1; then \
	  echo "SKIPPED: no game dumps resolvable (see docs/building.md)"; \
	elif [ -z "$(CHECK_FORCE)" ] && \
	     when=$$($(PYTHON) tools/stamp.py check $(STAMPS)/gamedefs $(GAMEDEFS_STAMP_IN)); then \
	  echo "NOT RE-READ: nothing it reads has moved since it passed at $$when."; \
	  echo "             This is a cache, not a result: 'make check-full' reads them."; \
	else \
	  $(PYTHON) scripts/gen-gamedefs.py --check > /dev/null \
	    && { echo "generated/: consistent with the registered dumps"; \
	         $(PYTHON) tools/stamp.py write $(STAMPS)/gamedefs $(GAMEDEFS_STAMP_IN); } \
	    || { echo "generated/: REFUSED; see the output of 'make defs'"; exit 1; }; \
	fi
	@echo
	@echo "== the fallback decoder still agrees with capstone =="
	@# x86len.py is validated AGAINST capstone, so this can only run where
	@# capstone is installed. Where it is not, the check is honestly absent
	@# rather than quietly passing.
	@if ! $(PYTHON) -c "import capstone" > /dev/null 2>&1; then \
	  echo "SKIPPED: $(PYTHON) has no capstone, so there is nothing to compare"; \
	  echo "         x86len.py against. This check did NOT run."; \
	elif ! $(PYTHON) scripts/titles.py --have-dumps > /dev/null 2>&1; then \
	  echo "SKIPPED: no game dumps resolvable (see docs/building.md)"; \
	else \
	  $(PYTHON) scripts/x86len.py --verify > /dev/null \
	    && echo "x86len.py: agrees with capstone on every shared offset" \
	    || { echo "x86len.py: DISAGREES with capstone; run it directly"; exit 1; }; \
	fi
	@echo
	@echo "all checks passed"

# The document model and the value grammar, and nothing else: the test file
# #includes both .c files directly.
$(INI_DOC_TEST): tests/ini_doc_test.c src/ini_doc.c src/ini_doc.h \
                 src/ini.c src/ini.h src/vcthunder.h src/game.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -ffunction-sections -fdata-sections \
	  -Wl,--gc-sections -o $@ $< -lkernel32

ini-doc-test: $(INI_DOC_TEST)
	@./$(INI_DOC_TEST)

# THE LAUNCHER'S MODEL, WITHOUT THE LAUNCHER. src/launcher_model.c holds the
# binding validators, the section-precedence rule, the write loop and the ini
# problem scan, so they can be tested without linking a window procedure. Same
# shape as ini-doc-test above, and the same rule: if this ever needs a stub for
# something that draws, the model has grown something it should not have.
$(LAUNCHER_MODEL_TEST): tests/launcher_model_test.c src/launcher_model.c \
                src/launcher_model.h src/settings.c src/ini.c src/ini_doc.c \
                src/bindings.def src/settings.def src/settings.h src/ini.h \
                src/ini_doc.h src/io.h src/vcthunder.h src/game.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -o $@ $< -lkernel32

launcher-model-test: $(LAUNCHER_MODEL_TEST)
	@./$(LAUNCHER_MODEL_TEST)

# config.c against a vcthunder.ini a person wrote, and settings.def against the
# round trip through the launcher's document model. They are separate binaries
# because they ask opposite questions; what happens to a LEGAL value, and what
# happens to one that is not, and a single "config tests passed" would say
# neither. Both #include the .c files directly, as ini-doc-test does: there is
# no library here to link against, and the parser under test is mostly static
# functions.
$(CONFIG_TEST): tests/config_test.c src/config.c src/settings.c src/ini.c \
                src/settings.def src/settings.h src/ini.h src/vcthunder.h src/game.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -o $@ $< -lkernel32

config-test: $(CONFIG_TEST)
	@./$(CONFIG_TEST)

$(ROUND_TRIP_TEST): tests/settings_round_trip_test.c src/config.c src/settings.c \
                    src/ini.c src/ini_doc.c src/settings.def src/settings.h \
                    src/ini.h src/ini_doc.h src/vcthunder.h src/game.h \
                    src/di_role.c src/di_role.h src/io.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -o $@ $< -lkernel32

round-trip-test: $(ROUND_TRIP_TEST)
	@./$(ROUND_TRIP_TEST)

# The code emitter, over bytes and then over a CPU: the last two cases put the
# emitted sequences in the executable pool and CALL them, which is the only way
# to catch a sequence that is wrong in the same way twice. See EMITTER_TEST
# above for why the binary is not called patch-anything.
$(EMITTER_TEST): tests/patch_emit_test.c src/patch.c src/vcthunder.h src/game.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -o $@ $< -lkernel32

emitter-test: $(EMITTER_TEST)
	@./$(EMITTER_TEST)

# build/vcprobe.dll: the module --probe-image needs and nothing ships.
#
# It exists because the host cannot ask these questions of itself. VCThunder.exe
# must have NO TLS directory (src/stub.c refuses to run with one) and
# VCThunder.dll has no delay-loaded import to fire, so a module with both is
# built here, loaded before the game is mapped and exercised after it. `make
# pack` does not deliver it and the contraband scan sees only project-authored
# text; without it the probe reports those cases NOT RUN, which is not a pass.
#
# dlltool -k builds the delay-import library: the def names the decorated
# stdcall symbol this object references and -k maps it to the undecorated name
# version.dll actually exports.
PROBE_DLL   := build/vcprobe.dll
PROBE_DELAY := build/obj/libversion_delay.a

$(PROBE_DELAY): tests/version_delay.def
	@mkdir -p $(dir $@)
	$(DLLTOOL) -k -d $< -y $@

$(PROBE_DLL): tests/image_probe_dll.c $(PROBE_DELAY)
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -static-libgcc -shared -o $@ $< $(PROBE_DELAY) \
	  -lkernel32
	@sha256sum $@

probe-dll: $(PROBE_DLL)

# The vertex path is not reachable from --dry-run: it only runs once frames are
# being produced. So the one function in this host that changes a coordinate the
# game computed is tested against its own previous implementation, checked in
# verbatim as tests/ws_shift_before.inc.
$(WS_SHIFT_TEST): tests/widescreen_shift_test.c tests/ws_shift_before.inc \
                  src/widescreen.c src/glide_int.h src/game.h src/vcthunder.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -Isrc -Itests -o $@ $< -lkernel32

widescreen-test: $(WS_SHIFT_TEST)
	@./$(WS_SHIFT_TEST)

# The link's socket path is not reachable from --dry-run either: the game only
# creates a socket once its OWN operator settings enable the link. So the two
# things worth checking rather than assuming, that two units can hold one port
# on this machine and that a broadcast reaches the other one,
# are checked here over real sockets, with no game data involved.
$(NET_LINK_TEST): tests/net_link_loopback_test.c src/net_link.c src/vcthunder.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -Isrc -Itests -o $@ $< \
	  -lkernel32 -lws2_32

net-link-test: $(NET_LINK_TEST)
	@./$(NET_LINK_TEST)

# The derived pad rumble is the one INVENTED effect in this project, and what
# it claims is a discrimination, a held force to the heavy motor, a shaking
# one to the light motor; that no screenshot shows and no hand can separate
# from "the motors are on". So the model is driven here on a pinned timeline,
# with no game data and no device. It also pins ini_read_scoped()'s
# absolute-path rule for dinput.c.
$(RUMBLE_TEST): tests/rumble_model_test.c src/rumble.c src/ini.c src/io.h \
                src/vcthunder.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -Isrc -Itests -o $@ $< src/ini.c \
	  -lkernel32

rumble-test: $(RUMBLE_TEST)
	@./$(RUMBLE_TEST)

# The 128-button mask against the 32-bit expression it replaced. pad_down() is
# on every user's play path, and buttons past 32 need a device that has them.
# So the control is the 32-bit expression, evaluated side by side over ordinals
# 1..32, which is the whole range both are meant to agree on.
$(BUTTON_TEST): tests/button_mask_test.c src/io.h src/vcthunder.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -Isrc -Itests -o $@ $< -lkernel32

button-test: $(BUTTON_TEST)
	@./$(BUTTON_TEST)

# The launcher driven from a controller: what a press means and where a move
# lands, on a pinned clock, with no window and no device.
$(LAUNCHER_PAD_TEST): tests/launcher_pad_test.c src/launcher_pad.c \
                src/launcher_pad.h src/io.h src/vcthunder.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -Isrc -Itests -o $@ $< -lkernel32

launcher-pad-test: $(LAUNCHER_PAD_TEST)
	@./$(LAUNCHER_PAD_TEST)

# The key Ultimate Arctic Thunder asks for, against every check the game
# performs on one. No key, no device and no game: src/keylok.c is pure.
$(KEYLOK_TEST): tests/keylok_test.c src/keylok.c src/keylok.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -Isrc -Itests -o $@ \
	  tests/keylok_test.c src/keylok.c -lkernel32

keylok-test: $(KEYLOK_TEST)
	@./$(KEYLOK_TEST)

# A key, a hat or a stick on a select screen: one item a press, held where it
# was left, and handed back to a wheel that is turned. src/menu_model.c is
# pure, and the screens it is driven against are modelled in the test.
$(MENU_MODEL_TEST): tests/menu_model_test.c src/menu_model.c src/menu_model.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -Isrc -Itests -o $@ \
	  tests/menu_model_test.c src/menu_model.c -lkernel32 -lm

menu-model-test: $(MENU_MODEL_TEST)
	@./$(MENU_MODEL_TEST)

# The CHD importer's pieces with no game: a cabinet-shaped disk built in
# memory through src/fat.c, an NTFS volume through src/ntfs.c, a CD image
# through src/iso.c, hand-assembled FLAC frames and CD hunks, the CD parity
# checked by its own syndromes, CRC-16's check value.
$(IMPORT_TEST): tests/import_test.c src/chd.c src/chd.h src/fat.c src/fat.h \
                src/iso.c src/iso.h \
                src/ntfs.c src/ntfs.h $(DEC_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) -I$(ZLIB_DIR) -I$(LZMA_DIR) $(LDFLAGS_REPRO) -o $@ \
	  tests/import_test.c src/chd.c src/fat.c src/iso.c src/ntfs.c $(DEC_OBJ) -lkernel32

import-test: $(IMPORT_TEST)
	@./$(IMPORT_TEST)

# Bezel art with no game and no window: PNGs assembled in memory in every
# colour type, depth and filter, the screen-hole finder, and the layout both
# presenters share (renderer/core/layout.c, which depends on nothing).
$(BEZEL_TEST): tests/bezel_test.c src/bezel.c src/bezel.h \
               renderer/core/layout.c renderer/core/layout.h $(DEC_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) -I$(ZLIB_DIR) $(LDFLAGS_REPRO) -o $@ \
	  tests/bezel_test.c src/bezel.c renderer/core/layout.c $(DEC_OBJ) \
	  -lkernel32 -lm

bezel-test: $(BEZEL_TEST)
	@./$(BEZEL_TEST)

# The choice src/cpu.c makes on a hybrid processor, over CPU set lists built
# in the test: the machine running it is only ever one kind of processor.
$(CPU_TEST): tests/cpu_test.c src/cpu.c src/vcthunder.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -o $@ $< -lkernel32

cpu-test: $(CPU_TEST)
	@./$(CPU_TEST)

# Which controller each axis role is read from, and what it reads when that
# controller does not answer. src/di_role.c
# holds no DirectInput on purpose, so this drives it with a fake slot table
# and a fake reading, and needs no device.
$(DI_ROLE_TEST): tests/di_role_test.c src/di_role.c src/di_role.h src/ini.c \
                 src/ini.h src/io.h src/vcthunder.h src/game.h \
                 src/xinput_hid.c src/xinput_hid.h
	@mkdir -p $(dir $@)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS_REPRO) -o $@ $< -lkernel32 -luser32

di-role-test: $(DI_ROLE_TEST)
	@./$(DI_ROLE_TEST)

# The address tables are derived, never hand-written. Regenerate after any
# change to the analysis; never edit anything under generated/.
#
# It REFUSES a dump whose SHA-256 is not the registered one. Every address it
# emits is an offset into that exact file; against another build they are
# plausible numbers pointing at the wrong instructions.
# The symbol cache comes first, and is rebuilt every time rather than reused.
# gen-gamedefs.py reads it and only p1-symbols.py writes it, so a clone needs no
# second command, and the cache can never be stale against the dump beside it.
# THE ONE COMMAND. A user who has just ingested their CHDs should not have to
# learn the difference between defs, all and pack: this is the whole journey.
.PHONY: setup
setup:
	@$(MAKE) --no-print-directory deps
	@echo
	@$(MAKE) --no-print-directory all
	@echo
	@$(MAKE) --no-print-directory pack

defs:
	@# tr -d '\r': MSYS2's python is a native Windows build, so its stdout is
	@# CRLF-translated and the shell's word split leaves "hydro\r", which
	@# titles.py then rejects as an unknown title. Harmless under WSL.
	@for t in $$($(PYTHON) scripts/titles.py --ets-ids | tr -d '\r'); do \
	  $(PYTHON) scripts/p1-symbols.py --title $$t > /dev/null || exit 1; \
	done
	$(PYTHON) scripts/gen-gamedefs.py

# generated/ is committed: the per-title address tables, derived from the
# registered dumps by `make defs` and checked against them by `make check`
# wherever a dump is present. Building needs no game data. Missing files mean
# an incomplete checkout, not a missing step, and make's own "No rule to make
# target" would not say so.
$(SPAN) generated/profiles.c:
	@echo "generated/ is incomplete: it is part of the repository, so this"
	@echo "checkout is damaged. 'git checkout -- generated' restores it."
	@false

# Deliver the binaries into the user's run directory: ONE pack holding both
# titles, at run/vcthunder/ inside this tree. A cabinet runs one game; a desktop
# does not, and two packs meant two copies of every binary and two inis that had
# to agree.
#
# THE GAME DATA IS COPIED IN, so the pack stands alone: moving or deleting the
# clone does not break it. Only a title the pack lacks, and only while the ini
# reads the pack's own data/; the data never leaves your machine. LINK_DATA=1
# on the first pack points data_dir at the ingested data/ instead of copying it.
#
# Everything here is written with the assumption that the destination is the
# USER'S and this build is the guest. Binaries are overwritten; nothing else is.
PACKDIR := $(RUN)/vcthunder

pack: $(EXE) $(DLL) $(VGL_DLL) $(SDL3_RUNTIME_OUT)
	@mkdir -p $(PACKDIR)/data $(PACKDIR)/save
	@cp $(EXE) $(DLL) $(VGL_DLL) $(SDL3_RUNTIME_OUT) $(PACKDIR)/
	@echo "staged $(PACKDIR)"
	@$(MAKE) --no-print-directory pack-cfg RUNDIR=$(PACKDIR)
	@if grep -q "^data_dir = data$$" $(PACKDIR)/vcthunder.ini 2>/dev/null; then \
	  for d in data/*/; do \
	    [ -d "$$d" ] || continue; t=$$(basename "$$d"); \
	    [ -d $(PACKDIR)/data/$$t ] && continue; \
	    echo "  copying data/$$t into the pack ($$(du -sh "$$d" | cut -f1))"; \
	    rm -rf $(PACKDIR)/data/$$t.part; \
	    cp -r "$$d" $(PACKDIR)/data/$$t.part && \
	      mv $(PACKDIR)/data/$$t.part $(PACKDIR)/data/$$t || exit 1; \
	  done; fi
	@echo
	@echo "the run directory is $(RUN)/, in the tree that built it."
	@if grep -q "^data_dir = ." $(PACKDIR)/vcthunder.ini 2>/dev/null && \
	    ! grep -q "^data_dir = data$$" $(PACKDIR)/vcthunder.ini 2>/dev/null; then \
	  printf 'its game data is%s\n' "$$(grep '^data_dir = ' $(PACKDIR)/vcthunder.ini | cut -d= -f2-)"; \
	elif [ ! -d $(PACKDIR)/data/hydro ] || [ ! -d $(PACKDIR)/data/offroad ]; then \
	  echo "import your games into it: put your .chd files in $(PACKDIR)/ and run VCThunder.exe"; \
	fi
	@echo "run it as:  VCThunder.exe (launcher) | VCThunder.exe hydro | VCThunder.exe offroad | VCThunder.exe arctic"

# The INI is the RUN directory's configuration, not a build output. Copying it
# unconditionally would silently discard whatever was being tested there, so
# seed it when absent and never overwrite; `make pack-ini` forces a refresh.
# But then a changed SHIPPED DEFAULT, or a new key, never reaches a run
# directory that already has an ini. So keep the file, but say what it is
# missing.
pack-cfg:
	@if [ -f $(RUNDIR)/vcthunder.ini ]; then \
	  echo "  kept existing vcthunder.ini (make pack-ini to overwrite)"; \
	  new=$$(awk -F= '/^[ \t]*[A-Za-z_][A-Za-z_0-9]*[ \t]*=/ { \
	      gsub(/[ \t]/, "", $$1); \
	      if (NR == FNR) have[$$1] = 1; else if (!($$1 in have)) print $$1 }' \
	      $(RUNDIR)/vcthunder.ini vcthunder.ini | sort -u | tr '\n' ' '); \
	  if [ -n "$$new" ]; then \
	    echo "  WARNING: it has no $$new-- new shipped keys, defaults apply"; fi; \
	else cp vcthunder.ini $(RUNDIR)/vcthunder.ini; \
	  echo "  seeded vcthunder.ini"; \
	  if [ -n "$(LINK_DATA)" ] && [ ! -d $(RUNDIR)/data/hydro ] \
	     && [ ! -d $(RUNDIR)/data/offroad ] && [ -d data ]; then \
	    abs=$$(cd data && pwd); \
	    if command -v wslpath > /dev/null 2>&1; then abs=$$(wslpath -w "$$abs"); \
	    elif command -v cygpath > /dev/null 2>&1; then abs=$$(cygpath -w "$$abs"); fi; \
	    $(PYTHON) tools/pack-datadir.py $(RUNDIR)/vcthunder.ini "$$abs"; \
	    echo "     (your game data stays where it was ingested; the pack points"; \
	    echo "      at it rather than copying 1.4 GB into it)"; \
	  fi; fi
# No .bat launchers are staged: a bare VCThunder.exe opens the settings
# launcher, whose Launch page has a Play button per title.
# NOTHING THIRD-PARTY IS STAGED except SDL3.dll.

pack-ini:
	@cp vcthunder.ini $(PACKDIR)/vcthunder.ini
	@echo "overwrote $(PACKDIR)/vcthunder.ini with the shipped defaults"

clean:
	rm -rf build/obj build/tests $(EXE) $(DLL) $(VGL_DLL) $(SDL3_RUNTIME_OUT) \
	       $(STAMPS) build/regress
	@echo "cleaned build outputs. $(RUN)/ is untouched: it holds the tuned ini,"
	@echo "both dumps and the NVRAM. Only 'git clean -xdf' can reach those."
