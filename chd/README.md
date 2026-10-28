# chd: a place for your CHDs

**You do not need this folder to play.** VCThunder imports a game from your own MAME CHD
itself: put the `.chd` next to `VCThunder.exe` (in a clone, `run/vcthunder/`) and run it,
press **Import...** in its launcher, drop the `.chd` onto `VCThunder.exe`, or run

    VCThunder.exe import path\to\your.chd

from the pack's folder. It reads the CHD directly (no chdman, no MAME, no unpacked disk
image), checks the game's executable against the build VCThunder supports **before writing
anything**, and writes the game into the pack's `data/` folder.

Ultimate Arctic Thunder is the exception: it comes from its update CD (MAME's
`040503_1309.chd`, or the disc's `.cue` and `.bin`), laid over Arctic Thunder, so import
Arctic Thunder's CHD too.

This directory exists so a clone has one obvious place to keep CHDs. **Nothing in it is part
of VCThunder**, nothing in it is committed, and the contraband scanner skips it.

A CHD older than version 5, or one with a parent CHD, is declined by name: MAME's
`chdman copy` turns either into a whole version 5 CHD.
