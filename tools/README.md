# tools

## Ingest

- **`ingest-chd.py`**: the front door. Turns a MAME CHD (or a raw image, or an
  already-extracted drive tree) into the flat `data/<title>/` directory this host
  reads, reading both the container and the FAT partitions itself, and verifies the
  result against the registered build. `docs/building.md`.
- **`chd.py`**: the container half of that, and a tool in its own right. Reads a
  MAME CHD v5, header, compressed map, and the `lzma`, `zlib`, `huff` and `flac`
  codecs, with nothing but the Python standard library, and presents it as a file
  object, so an ingest decodes only the hunks its files occupy and writes no image
  at all. `chd.py <file>` prints what `chdman info -v` prints; `--verify` decodes
  every hunk against its own CRC-16 and the image against the SHA-1 in the header;
  `--selftest` checks the plumbing with no CHD at all; `--jobs` sets the decode
  width, which is spread across every core by default. chdman remains the fallback
  for a pre-v5 CHD or one with a parent, `--chdman <path>` forces it.

## The conformance harness

Built by `make tools`. `make conformance` is the standing check that uses it.

| Tool | What it does |
|---|---|
| `glide-replay.c` | Replays a `.vglc` Glide call stream into **any** Glide 2.x provider. Its output modes are separate questions and are not mixed: timed by default, `--hash` for one hash per frame, `--out` for a PNG per frame, `--present` to watch it, `--list` to summarise without rendering. Links against neither the host nor the renderer, on purpose: what it proves about a capture must not depend on either. |


## Everything else

`scan-contraband.py` is a build-time helper. `fetch-sdl3.py` installs the
pinned, hash-verified x86 SDK outside the repository; `embed-text.py` turns the
project-authored HLSL into a generated C array. `pack-datadir.py` points a freshly staged
pack at data you have already ingested, instead of copying gigabytes of it twice.

Four are run by `make check`: `check-launcher-settings.py` holds `src/settings.def` and the
shipped ini to each other, `check-links.py` holds every relative markdown link to a file that
is really there, `make-conformance-capture.py` authors a synthetic `.vglc` scene so the replay
check runs on a clone with no game data, and `stamp.py` decides whether the two checks that
re-read gigabytes have anything new to read.


