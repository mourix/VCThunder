# chd: the files you supply

Drop your game CHDs here. **Nothing in this directory is part of VCThunder**, nothing
in it is committed, and the contraband scanner skips it.

## What to put here

    chd/hydrthnd.chd    your own Hydro Thunder CHD
    chd/offrthnd.chd    your own Offroad Thunder CHD

Then, from the repository root:

    python3 tools/ingest-chd.py --auto

**The CHDs are yours to supply.** This repository contains no game material. The filenames
above are only the usual ones: `--auto` works out which game each CHD is by looking inside
it, so any name is fine, and several CHDs of one game are fine too -- it uses the build
this host has address tables for and reports the ones it passes over.

A CHD of another revision is refused in about a second and nothing is written: this host's
addresses are offsets into one exact executable, so another build needs its own entry in
`scripts/titles.py`. `--any-build` unpacks it anyway, which is where registering one starts.

## You do not need chdman, or MAME, or any free space

`tools/chd.py` opens a CHD v5 itself, using nothing but the Python standard library, and
decodes only the parts of the disk your files occupy. Nothing is unpacked to disk.

You can look inside a CHD with it directly, and check it against itself:

    python3 tools/chd.py chd/hydrthnd.chd            # what `chdman info -v` prints
    python3 tools/chd.py --check chd/hydrthnd.chd    # the map, and a spread of hunks; seconds
    python3 tools/chd.py --verify chd/hydrthnd.chd   # every hunk's CRC-16, then the image SHA-1

`--verify` decodes the whole disk rather than just the files it holds, so it is slower than
an ingest: 30 seconds for Hydro's 3.2 GB and 1m42s for Offroad's 6.4 GB, on sixteen cores.

## When chdman is still needed

A CHD older than version 5, or one that has a parent CHD, is declined by name and the ingest
falls back to MAME's own extractor. Only then does it want room: chdman writes the full
uncompressed disk beside the CHD before anything is read out of it -- Hydro's 166 MB extracts
to 3.2 GB and Offroad's 1.3 GB to 6.4 GB.

`chdman` ships inside the MAME binary distribution: <https://www.mamedev.org/release.html>,
or the release archives at <https://github.com/mamedev/mame/releases>. Copy just that one
file here; nothing else from MAME is needed.

    chd/chdman.exe      (Windows)
    chd/chdman          (Linux)

It is looked for here first, then on `PATH`. `--chdman <path>` names it directly, and also
*forces* that route if you would rather extract with MAME's own code.
