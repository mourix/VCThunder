# Architecture

How VCThunder is put together, and why.

There is no CPU emulation here. The game's x86 code executes natively. Everything below is about the
*boundaries* that code reaches through. The OS, graphics library, hardware and how each is replaced.

---

## 1. The four facts everything follows from

1. **The ETS kernel boundary *is* Win32.** Phar Lap ETS was sold as a Win32 subset and statically
   links one, with real decorated names (`_CreateThread@24`, `_VirtualAlloc@16`), most of it
   reached through an `__imp__` pointer table in a few contiguous runs. Rewriting that table
   swaps scheduler, heap, virtual memory, filesystem, TCP/IP and console for real
   `kernel32`/`ws2_32` in a single pass.
2. **The address span is *owned*, not requested.** The game is a LinkLoc flat image with no
   `.reloc`: it must live at its link-time base, from `0x100000`. That range cannot be
   `VirtualAlloc`ed -- five methods were tried and all return `ERROR_INVALID_ADDRESS`, because
   the PEB and the initial thread stack are already inside it. So `VCThunder.exe` is *based* at
   `0x100000` with a `.bss` large enough that its `SizeOfImage` covers the whole span, and the
   Windows loader reserves it before the kernel places anything.
3. **Addresses and sizes are per-title data.** Nothing is hardcoded. Every address comes from a
   runtime profile generated from the title's own executable; an anchor a title does not have is
   `0`, and the host refuses rather than patching it.
4. **ETS reaches its kernel through `int 0xFE`/`int 0xFF` software gates**, which fault under
   Windows. They are found by scanning the image, not by name.

---

## 2. Two modules, and the split is structural

```
VCThunder.exe   ImageBase 0x100000, ASLR off, freestanding (-nostdlib).
                Its .bss is sized so SizeOfImage covers the largest title's
                whole span. Contains no logic: its own .text sits inside the
                game's CODE range and is overwritten.

VCThunder.dll   Everything real. The loader places it high (~0x6c000000),
                outside the span. The stub loads it, calls shim_run(), and
                never gets control back.
```

The link flags are load-bearing rather than tuning: `--image-base 0x100000` with
`--disable-dynamicbase`, or the executable does not own the span. `src/stub.c` verifies its own
base at runtime and refuses to continue rather than corrupting something later.

`generated/span.h` is the one value that cannot be decided at runtime (a `.bss` size is fixed at
link time), so it carries the largest span across every title in the build. That is why
`make defs` is run without a `--title`.

---

## 3. Startup order, which is not negotiable

```
0. preload    LoadLibrary every DLL, CoInitializeEx, open WASAPI
1. map        VirtualProtect the span RWX, copy CODE from the file
2. patch      __imp__ slots, jmp'd Win32 entries, ETS gates, hardware stubs,
              Glide binding, timebase, NVRAM, I/O board
3. unpack     call __pl_unpackrom -> expands DATA + .bss
4. run        _mainCRTStartup -> __cinit -> _main
```

Entry is at `_mainCRTStartup`, **not** the PE entry `__p_start`, which writes `fs:[4]`/`fs:[8]`;
those are the TEB's StackBase/StackLimit on Win32, and letting it run corrupts the thread.

---

## 4. The boundaries

1. **Win32/Winsock**: the ETS kernel boundary, name for name. `src/win32_bind.c`.
2. **Glide**: every `_gr*`/`_gu*` entry point, bound onto the provider through cdecl→stdcall
   thunks, because the game's Glide is cdecl. `src/glide_bind.c`.
3. **ETS `int 0xFE`/`0xFF` gates**: stubbed, and necessarily *before* `__pl_unpackrom` runs.
4. **ETS hardware drivers**: 8259 PIC, ISR priority, PC/AT timer, keyboard, IDT, PCI config.
   Curated by name, because opcode censuses do not work here.
5. **Game hardware**: PC speaker, IDE, watchdog, UART, YMF, the Diego/MagicBus I/O boards.
6. **Inline `cli`/`sti` in game code**: not a function boundary at all. Handled by taking the
   fault and emulating the instruction.
7. **Audio**: Hydro is cut below its DSP command layer; Offroad's equivalent really is DMA/MMIO,
   so its cut is the jump table *above* it. Both keep their own bookkeeping.
8. **The title's own switches**, where a subsystem this host does not implement would be fatal.
   These are states the binary already supports, not patched-out checks.
9. **The title's own diagnostic channel**, where the build compiled one out.

---

## 5. Quitting

A cabinet is switched off at the mains, so `_main` is an endless loop with no exit path and
nothing to hook. Escape and the window's close button are therefore handled entirely on this
side, in the buffer-swap message pump, and cannot unwind: the request arrives on the game thread
inside the game's own stack, so the host flushes NVRAM, stops the mixer and I/O threads, and
calls `ExitProcess`.

---

## 6. The source map

| Path | Role |
|---|---|
| `src/game.h`, `src/game.c` | The multi-title contract: the profile type, the active profile `G`, title selection. Read the header comment before touching any installer. |
| `generated/` | **Generated from your own dump; never edited.** Image layout, named anchors, the Glide/`__imp__`/Win32/hardware tables, and the span. Produced by `scripts/gen-gamedefs.py`. |
| `src/stub.c` | `VCThunder.exe`. Reserves the span via its own `SizeOfImage`, verifies the loader honoured it, hands off to the DLL. |
| `src/launcher.c` | The native no-argument launcher: settings, per-title bindings, live device testing. |
| `src/loader.c` | Maps the span, copies CODE, calls `__pl_unpackrom`, enters the game. |
| `src/patch.c` | The patch primitives: slot, jmp, single call site, cdecl→stdcall thunk, prologue-relocating hook. |
| `src/win32_bind.c` | Replaces the ETS kernel. Unresolved entries fall back to ETS's own code, logged. |
| `src/stubs.c` | Stubs for every boundary that would execute a privileged instruction. Not optional: the first four things `_main` touches all reach hardware. |
| `src/glide_bind.c` | Binds the Glide entry points onto the provider; owns the mode/surface decision and the frame-pacing reports. |
| `src/widescreen.c` | The Hor+ engine: seam origin, widened frustum, and the title-scoped repair hooks. |
| `src/io_*.c`, `src/diego.c` | The cabinet I/O boundary, split along a seam a real board or a traffic replayer can drop into. |
| `src/audio.c`, `src/audio_offroad.c` | The shared WASAPI transport and each title's own audio backend. |
| `src/timebase.c` | Makes a modern CPU look like a 333 MHz Celeron. Without it the game hangs forever in its own sleep. |
| `src/settings.def` | **The one settings table**: every key the host reads, expanded by both the config parser and the launcher. |
| `renderer/` | **A separate module and a separate DLL**, sharing no source or header with `src/`. `core/` is portable C99; `win32/` holds the CPU presenter/worker pool and the SDL3 GPU backend. |
| `tools/` | The CHD ingest front door, the conformance harness (`glide-replay.c`, `frame-diff.py`, the capture readers), the audits `make check` runs, and the address-space probes. |
| `tests/` | The checks that are programs rather than scripts, each `#include`ing the `.c` files it covers: the INI document model, the configuration parser, every setting round-tripped launcher -> file -> host, the runtime code emitter, the widescreen primitive shift, and the link over real sockets. `make check` runs all six. |

---

## 7. The renderer

`vcglide.dll` exports the Glide 2.x entry points the games call and rasterises all of them. It is
the only provider, and the host always loads the copy beside it.

It has two backends behind one interface. The **CPU** backend is a parallel software rasteriser
and the reference. The **GPU** backend runs on SDL3's GPU API with selectable 1×–8× internal
render targets. They are different code paths, and a result from one says nothing about the
other.
