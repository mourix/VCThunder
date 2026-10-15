# tools

## Ingest

**Importing a game is the program's own job**: `VCThunder.exe import FILE.chd`, or the
launcher's *Import...*, reads the CHD in `src/chd.c` and `src/fat.c` and needs no Python.

## The conformance harness

Built by `make tools`. `make conformance` is the standing check that uses it.

| Tool | What it does |
|---|---|
| `glide-replay.c` | Replays a `.vglc` Glide call stream into **any** Glide 2.x provider. Its output modes are separate questions and are not mixed: timed by default, `--hash` for one hash per frame, `--out` for a PNG per frame, `--present` to watch it, `--list` to summarise without rendering. Links against neither the host nor the renderer, on purpose: what it proves about a capture must not depend on either. |

## Everything else

`scan-contraband.py` is a build-time helper. `fetch-deps.py` installs the pinned,
hash-verified third-party packages under `build/deps/`: the x86 SDL3 SDK, and the CHD importer's
two decoders, zlib's inflate and 7-Zip's public-domain `LzmaDec.c`; `embed-text.py` turns the
project-authored HLSL into a generated C array. `pack-datadir.py` points a freshly staged
pack at data you have already ingested, instead of copying gigabytes of it twice.

Four are run by `make check`: `check-launcher-settings.py` holds `src/settings.def` and the
shipped ini to each other, `check-links.py` holds every relative markdown link to a file that
is really there, `make-conformance-capture.py` authors a synthetic `.vglc` scene so the replay
check runs on a clone with no game data, and `stamp.py` decides whether a check that re-reads
gigabytes has anything new to read.

