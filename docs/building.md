# Building and running

## Layout

**A clone of this repository needs nothing beside it, and building needs no game data.**
Everything the build writes goes inside the clone, the run directory included, and every input
is either here already or fetched by `make deps`. Your own game CHDs are needed to *play*:

```
VCThunder/                     ← the clone; this is the whole of it
├─ chd/                        optional: somewhere to keep your CHDs
├─ generated/                  committed: the address tables, derived by `make defs`
├─ build/                      objects, the three binaries, and build/deps/ (SDL3, zlib, LZMA)
└─ run/vcthunder/              written by `make pack`: the playable pack, games imported into it
```

`build/` and `run/` are written by tools and git-ignored, so from a fresh clone:

```sh
make setup                              # deps, all, pack
run/vcthunder/VCThunder.exe             # with your CHDs beside it: it imports them
```

`make clean` removes objects and binaries only; it does not touch `run/`, because that holds
the games you imported, your settings and your saves.

## Where to build: WSL or MSYS2

The target is always a 32-bit Windows binary. Two host environments produce it, and both are
supported and tested:

| | Toolchain | Notes |
|---|---|---|
| **WSL / Linux** | `i686-w64-mingw32-gcc` (cross) | What this project is developed on |
| **MSYS2 MINGW32** | `i686-w64-mingw32-gcc` (native) | No WSL needed |

The Makefile detects which it is on; `CC`, `WINDRES`, `OBJDUMP` and `PYTHON` are all overridable
if the detection is wrong for you.

`objdump` comes from the same binutils as the linker and is not an extra dependency, but the exe
link depends on it: it verifies that the linker honoured `-Wl,--disable-reloc-section` and
**fails the build** if a `.reloc` section survived. An image that carries relocations is one
mandatory ASLR is entitled to move.

**The target must be 32-bit x86.** The host executes the game's own 1999 i386 code in-process at
its link-time base. It runs on Windows 10/11 x64 under WOW64, and on ARM64 Windows under x86
emulation.

### Disassembly, and why capstone is not required

`make defs` derives the Glide ABI by disassembling the games' own call sites rather than by
trusting a table, so it needs a decoder. **It does not need `capstone`**: `scripts/x86len.py`
is a length-only x86-32 decoder covering exactly what `scripts/glide2_pushes.py` reads, and the
generator uses it whenever capstone is not importable. Plain `python3` with no packages is
enough to build from a fresh clone, which matters because **MSYS2 has no capstone for any
32-bit environment**, so in a MINGW32 shell `pacman` cannot help.

capstone is still the oracle where it exists:

| | Decoder used | `make check` |
|---|---|---|
| capstone importable | capstone | also cross-checks x86len.py against it |
| capstone absent | `scripts/x86len.py` | that cross-check is reported SKIPPED |

`python3 scripts/x86len.py --verify` runs the cross-check directly: it disassembles both dumps'
whole CODE segment with each decoder and reports every disagreement. It reports zero on both
supported builds, and the generator emits byte-identical profiles either way.

To use capstone under MSYS2, install it into a 64-bit environment; the tooling Python is
unrelated to the 32-bit target and does **not** have to match the compiler:

```sh
pacman -S mingw-w64-ucrt-x86_64-python-capstone
make defs PYTHON=/ucrt64/bin/python3
make defs PYTHON=/c/Python313/python.exe      # or a native Windows Python: pip install capstone
```

### WSL

Install the cross toolchain; nothing else is needed and no Python packages are:

```sh
sudo apt install gcc-mingw-w64-i686 make python3
```

`gcc-mingw-w64-i686` pulls in `binutils-mingw-w64-i686`, which is where the `objdump` the exe link
checks with comes from.

### MSYS2

Install the 32-bit toolchain and build from the **MINGW32** shell:

```sh
pacman -S mingw-w64-i686-toolchain make python
```

`windres` is not cross-prefixed under MSYS2 and the Makefile handles that.

**MINGW32 is on a clock.** MSYS2 began phasing out its 32-bit environments on 2023-12-13 and
removed CLANG32 on 2024-12-18; `mingw-w64-i686-*` packages are dropped as they go. The toolchain
itself is current. `mingw-w64-i686-gcc` 16.2.0-3, built 2026-08-09, so this route works today
and WSL is the one that is not on anyone else's schedule.

## Targets

```sh
make            # -> host/provider binaries + staged build/SDL3.dll
make setup      # deps, all, pack: everything, from a fresh clone
make deps       # fetch and SHA-256-verify SDL3's x86 SDK and the CHD decoders
make renderer   # just build/vcglide.dll, the Glide 2.x provider
make tools      # build/glide-replay.exe, the capture replay / golden-frame harness
make check      # the local CI: scans, link check, build, tests, generated/ vs the dumps
make check-full # the same, ignoring the stamps on the two checks that cache
make defs       # maintainer: regenerate generated/ from ALL titles' dumps
make pack       # deliver the binaries into run/vcthunder/
make pack-ini   # overwrite the run directory's ini with the shipped defaults
make pack-cfg   # the pack's ini and launchers only: seeds them, never overwrites
make clean      # objects, the three binaries, and staged SDL3.dll only
```

`make check` is the one to reach for. Underneath, each check is also a target of its own, in the
order `check` runs them: `ini-doc-test`, `launcher-model-test`, `config-test`,
`round-trip-test`, `emitter-test`, `widescreen-test`, `net-link-test`, `rumble-test`,
`button-test`, `di-role-test`, `import-test` and `conformance`. None of them needs game data;
`conformance` replays a synthetic scene through the renderer. A failing check prints what to do
about it: accepting a legitimately changed picture is `make conformance-frames`, looking at the
PNGs it writes, then `make conformance-accept`.

After them, `make check` compares `generated/` with the two dumps, and reports SKIPPED where
there are none. Those inputs cannot change between runs, so the check records a key when it
passes and re-reads only when something it depends on has moved, reporting `NOT RE-READ` rather
than a pass when it skips.
**`make check-full` ignores every such key**, and `make clean` removes them.

`make deps` (`tools/fetch-deps.py`) downloads three pinned archives and refuses any whose
SHA-256 differs: `SDL3-devel-3.4.10-mingw.tar.gz` from SDL's GitHub release, `zlib-1.3.2.tar.gz`
from zlib's, and `7z2603-src.tar.xz` from 7-Zip's, of which only the six public-domain files
around `LzmaDec.c` are extracted. No third-party source or binary is vendored into this
repository.

**`make defs` must be run without `--title`**, or `generated/span.h` is not updated: that file
carries the largest span across every title and is what the stub reserves at link time.

**A dump is required to play, not to build.** `generated/` is committed: it is derived from the
registered dumps by `make defs`, which refuses a dump whose SHA-256 is not the registered one,
and **never edited by hand**. Where a dump is present, `make check` regenerates it in memory and
compares it **byte for byte** with the committed files; with no dump that check reports SKIPPED.
A changed table therefore only ever arrives as a `make defs` diff, reviewed like code.

**The release version is `src/resource.h`**, three macros and no other definition. `windres`
and the compiler both read that file, so the DLL's Windows file properties, the log's first
line (`VCThunder 0.3: ...`) and the settings launcher's title bar cannot disagree. It is a
version, not a build stamp: it moves when a release does and never because a build ran.

`make` twice on unchanged source produces byte-identical binaries, and so does a rebuild from
`make clean`: every link carries `-Wl,--no-insert-timestamp` and nothing compiles in
`__DATE__`/`__TIME__`. `make` prints each binary's SHA-256 as it links it. Note that `make` can
hit clock skew under WSL and silently not rebuild; if a hash looks wrong, force it with
`touch src/*.c renderer/*/*.c && make`. **Not `make -B`**, it forces `generated/`'s rule, whose
recipe is a refusal, and the build stops there.

Deleting a source is the case make cannot see for itself, since the object lists are
`$(wildcard)`s. `build/obj/.sources` is the source list as a file, both DLLs depend on it, and it
is rewritten only when the list changes, so a deletion relinks and the orphaned objects are
swept in the same step.

## From a MAME CHD to a running game

**You supply the game. This repository contains no game material and never will.** VCThunder
imports it from your own MAME CHD itself, four ways:

- CHDs next to `VCThunder.exe`: the launcher imports them as it opens, one per game not
  imported yet, the supported build first; a game already there is never touched;
- the launcher: **Import...** on a game's card (which looks next to the exe first), or
  **Import from CHD...** on *Files & Logging* (listed once *Show advanced settings* is on);
- dropping one or more `.chd` files onto `VCThunder.exe`, which opens the launcher and imports;
- `VCThunder.exe import [--replace] [--data DIR] [FILE.chd...]` from a command line, which prints
  what it does and exits non-zero on any refusal; with no file, the CHDs next to the exe.

The importer (`src/chd.c`, `src/fat.c`, `src/ingest.c`) reads a CHD version 5 directly and
decodes only the hunks the game's files occupy: no chdman, no MAME, no unpacked disk image.
In order:

1. **Which game, and which build, is decided by what is on the disk**, never by the file name:
   the executable is found and hashed where it lies, in about a second. **A CHD of a build this
   host has no tables for is refused before anything is written**, saying which build it holds.
2. **Every FAT partition is read and merged flat** into `data/<title>/`, the cabinets' logical
   drives included. `fs_map_path()` strips any drive specifier, so `C:\HYDRO.EXE` and
   `D:\SAMAZAA0.CSH` belong in one directory. **A partition it cannot read fails the import**,
   because the executable's hash says nothing about the rest of the disk.
3. Every hunk is checked against the CRC-16 the CHD carries for it; a damaged CHD fails by hunk
   number.
4. The files go to `data/<title>.part/`, and **only once the executable written there verifies
   is it renamed into place**. A failure, a cancel or a full disk leaves nothing behind.

A title folder that already holds the supported build is left alone; one that holds something
else is replaced only with `--replace`, or when the launcher's prompt is answered yes. A folder
that cannot be written, such as one under Program Files, is refused with a sentence saying so.
A CHD older than version 5, or one with a parent, is declined by name: MAME's `chdman copy`
makes a whole version 5 CHD of either.

Both games together are about 1.4 GB written, in a few seconds on a recent machine.

## The run directory

`make pack` delivers into `run/vcthunder/` inside the clone, which `.gitignore` covers and
`make clean` does not touch. **One pack holds both titles**, chosen by a command-line word:

```
run/vcthunder/  VCThunder.exe  VCThunder.dll  vcglide.dll  SDL3.dll
                vcthunder.ini
                data/hydro/     HYDRO.EXE  HT.R2  *.CSH
                data/offroad/   OFFROAD.EXE  GD3.R2  *.omf
                save/           htlowres.bin  *.NVR
```

**The run directory is yours, not build output.** `make pack` overwrites the binaries and
**nothing else**: an existing `vcthunder.ini` is kept, and it is checked for keys this build
ships that it does not have. Your tuning, your dumps and your NVRAM live there, so do not
delete it.

**A RELEASE ZIP CARRIES NO `vcthunder.ini`.** The shipped file is a resource inside
`VCThunder.dll` (`res/vcthunder.rc`, `IDR_DEFAULT_INI`) and the host writes it out when the pack
has none, so an update extracted over somebody's folder cannot replace the one file that holds
their `[devices]` table, their axis section and every binding. `$(RES)` depends on
`vcthunder.ini`: editing the shipped file rebuilds the resource, and a build that skipped it
would hand out the previous one. Deleting the file and starting VCThunder again is the whole of
"give me the shipped defaults back", for a user and for a test pack alike.

**Import your games into the pack itself** (above); that is where its `data/` is read from.
A clone that has a `data/` of its own, from an older build, still works:
**`make pack` copies that game data into the pack**: each title in the clone's `data/` that
`run/vcthunder/data/` does not have yet, while the ini's `data_dir` is the default `data`. The
pack then stands alone, and moving or deleting the clone does not break it. That costs the
data's size a second time (1.4 GB for both titles); `make pack LINK_DATA=1` on the first pack
points `data_dir` at the clone's `data/` instead. The data is your own lawful copy and never
leaves your machine.

## Smoke test

`--dry-run` maps, patches and unpacks without starting the game. Under WSL, interop runs the exe
directly, so both titles can be checked without leaving the shell:

```sh
cd run/vcthunder
./VCThunder.exe hydro   --dry-run
./VCThunder.exe offroad --dry-run
```

This is the fastest regression test there is: it exercises the loader, the Win32 binding, the
stubs, the Glide binding into the provider, the profile and `__pl_unpackrom` end to end. Anything
that changes the reported counts is a regression. The line that means the run reached the end is
`--dry-run: image mapped, unpacked and patched`; the profile's `glide N __imp__ N` line prints
before anything is mapped, and prints just as happily when startup then fails.

The title is the command-line word and there is no other source for it: `VCThunder.exe` with no
title opens the launcher, and `VCThunder.exe --trace` refuses and names the two that work.

## Running for real

```sh
cmd.exe /c "taskkill /F /IM VCThunder.exe"          # it holds its own files
make pack
cmd.exe /c "cd /d C:\path\to\run\vcthunder && VCThunder.exe hydro --trace"
```

The log is the deliverable. `--trace` logs every stub hit, every binding and every Glide call by
name. A fault prints the faulting EIP, the module, and the exact command that names the function:

```sh
python3 scripts/p1-symbols.py --lookup 0x1981AB
```

On a successful run the log contains both `game control callback registered` and `game control
callback active; decoded inputs now reach gameplay`.
