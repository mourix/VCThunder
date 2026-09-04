# tools

## Ingest

- **`ingest-chd.py`**: the front door. Turns a MAME CHD (or a raw image, or an
  already-extracted drive tree) into the flat `data/<title>/` directory this host
  reads, reading both the container and the FAT partitions itself, and verifies the
  result against the registered build. `docs/building.md`.
- **`chd.py`**: the container half of that, and a tool in its own right. Reads a
  MAME CHD v5 -- header, compressed map, and the `lzma`, `zlib`, `huff` and `flac`
  codecs -- with nothing but the Python standard library, and presents it as a file
  object, so an ingest decodes only the hunks its files occupy and writes no image
  at all. `chd.py <file>` prints what `chdman info -v` prints; `--verify` decodes
  every hunk against its own CRC-16 and the image against the SHA-1 in the header;
  `--selftest` checks the plumbing with no CHD at all; `--jobs` sets the decode
  width, which is spread across every core by default. chdman remains the fallback
  for a pre-v5 CHD or one with a parent -- `--chdman <path>` forces it.

## The conformance harness

Built by `make tools`. `make conformance` is the standing check that uses it.

| Tool | What it does |
|---|---|
| `glide-replay.c` | Replays a `.vglc` Glide call stream into **any** Glide 2.x provider. Its output modes are separate questions and are not mixed: timed by default, `--hash` for one hash per frame, `--out` for a PNG per frame, `--present` to watch it, `--list` to summarise without rendering. Links against neither the host nor the renderer, on purpose: what it proves about a capture must not depend on either. |
| `frame-diff.py` | Scores two directories of frames per pixel: identical, max channel delta, mean. No imaging library; it reads the PNGs the harness writes. |
| `vglc-census.py` | Reports the **distinct argument tuples** each entry point was called with, not just how often: Hydro calls `grAlphaCombine` 177,725 times with eleven distinct tuples. That is what scopes the renderer -- implement exactly that set -- and `--tex` and `--vertex` do the same for `GrTexInfo` fields and `GrVertex` component ranges. |
| `vglc-scan.py` | Reads a `.vglc` capture without running anything, on any platform: per-entry counts, calls with their arguments by frame, payload blob sizes, LFB lock/unlock/region traffic, and `--blobless`, the calls whose payload rule produced nothing. Deliberately a **second, independent** implementation of the format `glide-replay.c` reads; two readers disagreeing is a finding. |

## Everything else

`make-icon.py` and `scan-contraband.py` are build-time helpers. `fetch-sdl3.py` installs the
pinned, hash-verified x86 SDK outside the repository; `embed-text.py` turns the
project-authored HLSL into a generated C array. `pack-datadir.py` points a freshly staged
pack at data you have already ingested, instead of copying gigabytes of it twice.

Four are run by `make check`: `check-launcher-settings.py` holds `src/settings.def` and the
shipped ini to each other, `check-links.py` holds every relative markdown link to a file that
is really there, `make-conformance-capture.py` authors a synthetic `.vglc` scene so the replay
check runs on a clone with no game data, and `stamp.py` decides whether the two checks that
re-read gigabytes have anything new to read.

`regress.py` is `make regress`, and is not part of `make check`: it builds a control renderer
from a git revision, replays the captures named in `tests/regress-captures.def` through both
backends, and compares per-frame hashes. The captures it needs are game material that cannot
live here -- on a clone it reports that it did not run.

## Address-space probes

Small, disposable programs that answer one question each. They are why the address span is
owned rather than requested (`docs/architecture.md` §1).

Build and run each from a normal Windows location (`C:\Users\Public\...`), not from the WSL
mount: `/mnt/c` interop adds its own mappings and Defender treats binaries there differently.

```sh
i686-w64-mingw32-gcc -O1 -Wl,--image-base,0x10000000 -Wl,--disable-dynamicbase \
    -o build/probe-reserve.exe tools/probe-reserve.c
```

| Probe | Question | Answer, measured |
|---|---|---|
| `probe-reserve.c` | Can a minimal process `VirtualAlloc` 0x100000..0x71d830 as its first action? | **No.** `ERROR_INVALID_ADDRESS` (487), even for a program whose entire body is that one call. |
| `probe-imagebase.c` | Can the shim make the span part of its own PE image (base 0x100000, `--section-start` pushing `.text` above 0x71e000)? | **No.** Windows refuses to load the image: "Access is denied". |
| `probe-parent.c` + `probe-child.c` | Is the span free in a `CREATE_SUSPENDED` child, before its loader runs? | **No.** 2,056 KB of the 6,262 KB span is already committed: 8 KB at exactly `0x00100000` (PEB) and 2 MB at `0x00200000` (initial thread stack), both inside the game's CODE range. |

A single self-spawning probe would be simpler and is deliberately **not** here: that is the
textbook process-hollowing signature, and Defender locks the binary on sight.
