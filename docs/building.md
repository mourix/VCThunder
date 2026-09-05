# Building and running

## Layout

**A clone of this repository needs nothing beside it.** Everything the build writes goes
inside the clone -- the run directory included -- and every input except your own game CHDs
is either here already or fetched by `make deps`:

```
VCThunder/                     ← the clone; this is the whole of it
├─ chd/                        ← YOU put your two CHDs here
├─ data/                       written by tools/ingest-chd.py: the unpacked games
├─ generated/                  written by `make defs`: the address tables, from YOUR dump
├─ build/                      objects, the three binaries, and build/deps/SDL3
└─ run/vcthunder/              written by `make pack`: the playable pack
```

Four of those five are generated, and every one of them is git-ignored, so `make setup` from
a fresh clone is the whole story:

```sh
python3 tools/ingest-chd.py --auto      # chd/ -> data/
make setup                              # deps, defs, all, pack
```

`make clean` removes objects and binaries only; it does not touch `data/`, `generated/` or
`run/`, because re-deriving those means having the CHDs to hand again.

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
enough to build from a fresh clone -- which matters because **MSYS2 has no capstone for any
32-bit environment**, so in a MINGW32 shell `pacman` cannot help.

capstone is still the oracle where it exists:

| | Decoder used | `make check` |
|---|---|---|
| capstone importable | capstone | also cross-checks x86len.py against it |
| capstone absent | `scripts/x86len.py` | that cross-check is reported SKIPPED |

`python3 scripts/x86len.py --verify` runs the cross-check directly: it disassembles both dumps'
whole CODE segment with each decoder and reports every disagreement. It reports zero on both
supported builds, and the generator emits byte-identical profiles either way.

To use capstone under MSYS2, install it into a 64-bit environment -- the tooling Python is
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
make setup      # deps, defs, all, pack: everything, from a fresh clone
make deps       # fetch and SHA-256-verify SDL 3.4.10's official x86 MinGW SDK
make renderer   # just build/vcglide.dll, the Glide 2.x provider
make tools      # build/glide-replay.exe, the capture replay / golden-frame harness
make check      # the local CI: scans, link check, build, tests, generated/ vs the dumps
make check-full # the same, ignoring the stamps on the two checks that cache
make regress    # replay real captures against a control renderer; needs captures
make defs       # regenerate generated/ from ALL titles
make pack       # deliver the binaries into run/vcthunder/
make pack-ini   # overwrite the run directory's ini with the shipped defaults
make pack-cfg   # the pack's ini and launchers only: seeds them, never overwrites
make clean      # objects, the three binaries, and staged SDL3.dll only
```

`make check` is the one to reach for. Underneath, each check is also a target of its own, in the
order `check` runs them: `ini-doc-test`, `config-test`, `round-trip-test`, `emitter-test`,
`widescreen-test`, `net-link-test` and `conformance`. `make conformance` on its own builds only
the renderer and the replay harness, so it is the one check that runs on a clone with no dumps
at all. A failing check prints what to do about it: accepting a legitimately changed picture is
`make conformance-frames`, looking at the PNGs it writes, then `make conformance-accept`.

Two of the checks read inputs that cannot change between runs -- four CHDs against their own
checksums, and `generated/` against the two dumps. Each records a key when it passes and
re-reads only when something it depends on has moved, reporting `NOT RE-READ` rather than a pass
when it skips. **`make check-full` ignores both**, and `make clean` removes the stamps.

**`make regress` is the renderer's real-capture A/B** and is not part of `make check`: it builds
a control renderer from a git revision, replays whole captures of both titles through both
backends and compares per-frame hashes. The captures it needs are recordings of the games
themselves, so they cannot live in this repository -- `tests/regress-captures.def` names them and
**on a clone this reports that it did not run**.

`make deps` downloads `SDL3-devel-3.4.10-mingw.tar.gz` from SDL's official GitHub release and
requires SHA-256 `39dd2ac3…22639a8`. No SDL source or binary is vendored into this repository.

**`make defs` must be run without `--title`**, or `generated/span.h` is not updated: that file
carries the largest span across every title and is what the stub reserves at link time.

**A dump is required to build.** There is no dumpless fallback: `generated/span.h` carries the
address span the stub reserves at link time, and without a dump that number cannot be derived.
`make check` runs its contraband scan and settings audit without a dump and stops at the build.
`generated/` is **not committed**, and `make defs` refuses a dump whose SHA-256 is not the
registered one.

**The release version is `src/resource.h`**, three macros and no other definition. `windres`
and the compiler both read that file, so the DLL's Windows file properties, the log's first
line (`VCThunder 0.1: ...`) and the settings launcher's title bar cannot disagree. It is a
version, not a build stamp: it moves when a release does and never because a build ran.

`make` twice on unchanged source produces byte-identical binaries, and so does a rebuild from
`make clean`: every link carries `-Wl,--no-insert-timestamp` and nothing compiles in
`__DATE__`/`__TIME__`. `make` prints each binary's SHA-256 as it links it. Note that `make` can
hit clock skew under WSL and silently not rebuild; if a hash looks wrong, force it with
`touch src/*.c renderer/*/*.c && make`. **Not `make -B`** -- it forces `generated/`'s rule, whose
recipe is a refusal, and the build stops there.

Deleting a source is the case make cannot see for itself, since the object lists are
`$(wildcard)`s. `build/obj/.sources` is the source list as a file, both DLLs depend on it, and it
is rewritten only when the list changes -- so a deletion relinks and the orphaned objects are
swept in the same step.

## From a MAME CHD to a running game

**You supply the game. This repository contains no game material and never will.** Your two
CHDs go in `chd/`; [`../chd/README.md`](../chd/README.md) is the whole of what you need to put
there, including why **`chdman` is not required**.

```sh
python3 tools/ingest-chd.py --auto
```

`--auto` works out which game each CHD is from what is inside it rather than from its name,
and when a directory holds more than one CHD of a title it hashes the executable in each and
uses the build this host has tables for, reporting the ones it passes over and why. **A CHD
of a build this host has no tables for is refused before anything is written**, in about a
second, and so is a `data/<title>/` that is already there and holds one. `--any-build` unpacks
it anyway, which is the first step of registering a new build and not a way to play it. Naming
them yourself does the same thing:

```sh
python3 tools/ingest-chd.py --title hydro   --chd chd/hydrthnd.chd --out data
python3 tools/ingest-chd.py --title offroad --chd chd/offrthnd.chd --out data
```

It reads the CHD itself, reads every FAT partition inside it, and merges them into
`data/<title>/`. The merge is not a simplification: `fs_map_path()` strips any drive specifier,
so `C:\HYDRO.EXE` and `D:\SAMAZAA0.CSH` both land in the same directory, which is why one flat
directory per title is the correct shape. It then hashes what it extracted against the
registered build, and **a mismatch is reported as an unsupported build, not as a corrupt
download** -- the README's *Supported builds* says what to do with one.

`chd/` is gitignored and the contraband scanner skips it. `make check` runs `chd.py --check` on
every CHD the ingest can find, and reports SKIPPED when there is none.

**Building from WSL with a Windows `chdman` works** and is handled: a Windows binary cannot open
a `/mnt/c/...` path (it fails as though the file did not exist), so the script translates paths
through `wslpath`. It is also why the image is never staged in `/tmp`, which a Windows binary
cannot see at all.

If you already have the image or an extracted drive tree, start further along:

| You have | Use |
|---|---|
| a CHD | `--chd game.chd` |
| a raw disk image | `--img game.img` |
| an extracted tree (`<dir>/C`, `<dir>/D`) | `--tree <dir>` |

If your dump lives somewhere else entirely, point the build at it:

```sh
VCT_DUMPS=$PWD/data make defs
```

`titles.py` also honours `VCT_DUMP_HYDRO` / `VCT_DUMP_OFFROAD` if you need to name a single
executable directly.

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

The game dumps are not staged by `make pack`. They are hundreds of megabytes and they are
Midway's; supply your own lawful copy.

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
`--dry-run: image mapped, unpacked and patched` -- the profile's `glide N __imp__ N` line prints
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
