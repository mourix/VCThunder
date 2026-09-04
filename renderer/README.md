# renderer: vcglide, this project's Glide 2.x provider

`vcglide.dll` exports the same Glide entry points the two games call, and **the host always
loads the copy sitting beside `VCThunder.exe`**. There is no key that selects a provider.

**It rasterises every call, and loads nothing.** Two keys choose and scale the backend behind
the same decoded Glide state.

```ini
[graphics]
vcglide_renderer = auto       ; auto | gpu | cpu
render_scale     = 2          ; SDL GPU internal target, 1..8
```

The standing regression check is a `glide-replay --hash` replay against the previously packed
`vcglide.dll`: two hash files that compare equal are a whole capture of byte-identical frames.

## How this code is sourced

Everything here is derived from the published Glide documentation, from our own measurements
of the two games, and from first-party experimentation, and from nothing else. **Other Glide
implementations may be run and observed; they may not be read.** That rule is stricter than
the rest of the repository's and binds from the first commit here, because it cannot be
retrofitted.

Every non-obvious constant names its origin at the point of use: `GR_VERTEX_BYTES = 60` is
derived from Hydro's own statically linked Glide advancing a vertex array by `count * 3 * 5 * 4`.
Structure layouts are written as offsets poked by byte arithmetic, never as transcribed struct
declarations, and `api/glide2.h` is authored here rather than being an SDK header.

## Layout

```
api/entries.def   the 69 entry points, their measured argument sizes, and what each one
                  references through a pointer. ONE list: it produces the index enum, the
                  descriptor array, the exported stubs and the linker .def.
api/glide2.h      the interface facts needed to read those references, as offsets and sizes
                  with their derivation recorded. Not an SDK header and must not become one.
core/vcglide.c    the seam: configuration, the exported stubs, the frame dump.
core/capture.c    the .vglc writer.
core/capture.h    the capture format.
core/png.c        a dependency-free PNG writer, shared with the replay harness so that
                  "565 to RGB" cannot round two different ways on the two sides of a diff.
core/vgl.h        the rasteriser's device and texture-unit state, and its enumerants.
core/native.c     one case per entry point: the ONLY place that knows what an argument
                  word means. Everything below it sees decoded parameters.
core/device.c     VGL-1: surface, buffers, clear, swap, clip, the LFB, save/restore state.
core/raster.c     VGL-2: triangle setup, the span loop, the combine units, the pipeline.
core/tex.c        VGL-3: texture memory, mip chains, texel formats, filtering, LOD.
win32/present.c   XRGB8888 D3D9/GDI presentation. Four functions; a port replaces it.
win32/thread.c    the CPU rasteriser's span-worker pool. Three functions.
win32/gpu.c       SDL3 GPU raster, render targets, readback and presentation (D3D12 today).
win32/gpu_shader.hlsl
                  Project-authored fixed-function emulation, embedded at build time.
```

`../tools/embed-text.py` generates the embedded shader byte array, so no shader compiler is a
build dependency.

`core/` is C99 plus `<math.h>` and contains no Windows API by rule. `win32/` is where a window
is allowed to exist.

`renderer/` shares no source and no header with `src/`. Its Makefile rule deliberately does
not depend on `src/vcthunder.h` or `generated/span.h`.
