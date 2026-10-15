# VCThunder

**VCThunder** is a Midway Quicksilver II Windows host for **Hydro Thunder** (1999) and **Offroad Thunder** (2000).

VCThunder is **not an emulator.** The original 32-bit x86 game code runs natively on your CPU, in-process, unmodified. What VCThunder replaces is everything *around* that code:
the Phar Lap ETS kernel becomes real Win32, the Glide 2.53 boundary becomes our own rasteriser, and the cabinet's I/O boards, audio hardware, timebase and NVRAM become host implementations.

This project was a personal challenge to see how far Claude Opus 5 could be leveraged to vibe code an application like this, and succeeded far beyond expectations. 
At this point there is a few hundred man hours into this project alongside a handful of testers. The result is these games being playable on a level never seen before.

## Getting started

**Download the latest release** from <https://github.com/mourix/VCThunder/releases>, unzip it
somewhere you can write to (your Documents or Desktop, not Program Files), and run
`VCThunder.exe`. Nothing has to be installed and nothing has to be built.

**You supply the games**, as your own MAME CHDs: put the `.chd` files next to `VCThunder.exe`
and run it, and it imports each game that is not there yet. A `.chd` kept elsewhere: press
**Import...** on a game in the launcher and choose it, or drop it onto `VCThunder.exe`. VCThunder reads
the game straight out of the CHD, checks it is a build it supports (below) before writing
anything, and puts it in the folder beside it. A few seconds later **Play** lights up.

`README.txt` in the download says the same, plus how to update and where the log is.

## Building from source

Nothing from the games is needed to build: the address tables are committed under `generated/`.
Build from **WSL** or from an **MSYS2 MINGW32** shell, **not from PowerShell or `cmd`**.

```sh
sudo apt install gcc-mingw-w64-i686 make python3   # WSL / Debian / Ubuntu
pacman -S mingw-w64-i686-toolchain make python     # MSYS2, from the MINGW32 shell
make setup                                         # fetch, build, stage run/vcthunder/
run/vcthunder/VCThunder.exe                        # then import your CHDs there
```

`make setup` fetches SDL3 and the two decompressors the CHD importer uses, each checked against
its pinned SHA-256, builds the host, and writes a pack into `run/vcthunder/`.

[`docs/building.md`](docs/building.md) is the long build doc, including the smoke test.  
[`docs/configuration.md`](docs/configuration.md) covers every ini key and the controls the pack ships with.  
[`docs/architecture.md`](docs/architecture.md) explains how the host is put together.

## Supported builds

Every address VCThunder patches is an offset into one exact executable. This host supports exactly two files:

| Title | Build | SHA-256 of the executable |
|---|---|---|
| Hydro Thunder | v01.01b, 1999-02-27 | `a421f5fd4ee0d90f57815975d4a47bb784f2b46d48085e42b422605a029f8e3f` |
| Offroad Thunder | 2000-02-25 | `fa739d0f1a80c6e429ca73fdee04c3d6d6325bb95848cb701114f74bf2a5e9bf` |

The import checks the executable inside a CHD against these before it writes anything, and says which build you have.

## State

**V0.3**, the third public beta release: the first with a download that needs no building.

**Both titles playable**: gameplay, controls, audio, NVRAM, windowing, force feedback, and I/O, at a locked 30 fps.

| Title | Released | I/O board | State |
|---|---|---|---|
| **Hydro Thunder** | 1999 | Diego | Playable |
| **Offroad Thunder** | 2000 | MagicBus | Playable |

Link play is an early beta: **only two instances on one machine have ever been linked.** That's it.

### Roadmap

The goal is a **replacement system for the original Quicksilver II cabinets**.

Todo:
- **Link play between two machines**: both simulated and real cabinet.
- **Real cabinet IO**: the Diego and MagicBus I/O boards over RS232, and the security relay a real board originates.
- **Real cabinet video**: native 25 kHz to the cabinet's own monitor, at the game's own 512x384 coin-op timing.
- **Linux support and Vulkan renderer**.
- **Bugfixes and comparison to real hardware where possible**.

## Licensing

Original VCThunder source and documentation are MIT-licensed. See [`LICENSE`](LICENSE).

Three pieces of other people's work are built in, none of them committed here: `make deps` downloads each pinned release and verifies its SHA-256.

- The renderer's GPU backend uses **SDL3**, under the [zlib license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt) and © the SDL contributors; `SDL3.dll` ships beside the host binaries.
- The CHD importer uses **zlib**'s inflate, under the [zlib license](https://zlib.net/zlib_license.html), and **7-Zip**'s `LzmaDec.c`, which its author placed in the public domain.

A release's `THIRD-PARTY.txt` carries each notice.

Hydro Thunder, Offroad Thunder, Midway and 3dfx marks belong to their respective owners. This project is unaffiliated with them and exists for preservation and for interoperability with hardware its users own.
