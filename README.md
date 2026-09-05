# VCThunder

**Vibe Code Thunder** is a Midway Quicksilver II Windows host for **Hydro Thunder** (1999) and **Offroad Thunder** (2000).

VCThunder is **not an emulator.** The original 32-bit x86 game code runs natively on your CPU, in-process, unmodified. What VCThunder replaces is everything *around* that code:
the Phar Lap ETS kernel becomes real Win32, the Glide 2.53 boundary becomes our own rasteriser, and the cabinet's I/O boards, audio hardware, timebase and NVRAM become host implementations.

This project was a personal challenge to see how far Claude Opus 5 could be leveraged to fully vibe code an application like this, and succeeded far beyond expectations.

## Building

Build from **WSL** or from an **MSYS2 MINGW32** shell, **not from PowerShell or `cmd`**.

**0. Get the toolchain.** `i686-w64-mingw32-gcc`, `make` and `python3`.

```sh
sudo apt install gcc-mingw-w64-i686 make python3   # WSL / Debian / Ubuntu
pacman -S mingw-w64-i686-toolchain make python     # MSYS2, from the MINGW32 shell
```

**1. Put your two game CHDs in `chd/`.** You supply the legal dumps.

**2. Unpack the games:**

```sh
python3 tools/ingest-chd.py --auto
```

It finds the CHDs, works out which game each one is by looking inside it, unpacks both into `data/`, and checks each for correctness.

**3. Build and stage it:**

```sh
make setup
```

It fetches SDL3, builds the address tables, builds the host, and writes a playable pack into `run/vcthunder/`.

**4. Launch it:**

```sh
run/vcthunder/VCThunder.exe
```

[`docs/building.md`](docs/building.md) is the long build doc, including the layout the build expects and the smoke test.  
[`docs/configuration.md`](docs/configuration.md) covers every ini key and the controls the pack ships with.  
[`docs/architecture.md`](docs/architecture.md) explains how the host is put together.

## Supported builds

Every address VCThunder patches is an offset into one exact executable. This host supports exactly two files:

| Title | Build | SHA-256 of the executable |
|---|---|---|
| Hydro Thunder | v01.01b, 1999-02-27 | `a421f5fd4ee0d90f57815975d4a47bb784f2b46d48085e42b422605a029f8e3f` |
| Offroad Thunder | 2000-02-25 | `fa739d0f1a80c6e429ca73fdee04c3d6d6325bb95848cb701114f74bf2a5e9bf` |

`tools/ingest-chd.py` hashes what it extracts and says which you have.  

## State

**V0.1**, the first public beta release.

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

The renderer's GPU backend uses **SDL3**, under the [zlib license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt) and © the SDL contributors. `make deps` downloads the official pinned release and verifies its SHA-256, and `make pack` copies `SDL3.dll` beside the host binaries.

Hydro Thunder, Offroad Thunder, Midway and 3dfx marks belong to their respective owners. This project is unaffiliated with them and exists for preservation and for interoperability with hardware its users own.
