# Configuration

**§8 is the key reference**; everything before it is what a key list cannot say.

> Inline `;` and `#` comments are accepted when whitespace separates them from the value. An
> invalid boolean or number is logged and keeps the value from the layer before.

---

## 1. The file model

**One pack, one file, both cabinets.**

```
[section]            applies to both titles
[<title>.section]    applies to that title only, and wins key by key

built-in defaults  ->  [section]  ->  [<title>.section]  ->  command line
```

Last wins, key by key: `[offroad.audio] output = front` leaves Hydro alone, and anything a title
does not name falls through to the plain section.

**The section names inside a scope are the same ones**, so `[offroad.digital]` *is* Offroad's
keyboard map. They cannot collapse into one flat `[offroad]`, because `[digital]` and `[gamepad]`
use the same key names for different things: `coin1` is a keyboard key in one and a pad button
ordinal in the other.

**Which section a value came from is logged**, and so is any key the host does not recognise,
which is named once and ignored. A value outside a key's range or choice set is refused with a
reason, and the previous value stands.

`log_file` is read only from the plain `[diagnostics]`: it names a file already open by the time
a title is known, so `[hydro.diagnostics] log_file` would read as applied and would not be.

### 1.1 The launcher

`VCThunder.exe` with no arguments opens the settings launcher, which exposes every shipped key
and writes it back: shared values stay in their plain sections, and control differences go only
to the four `[<title>.digital]` and `[<title>.gamepad]` sections. Any other per-title section is
preserved untouched, with a warning. A save writes `vcthunder.ini.bak` first, then atomically
replaces the file, keeping ordering, comments, unknown keys and line endings.

A checkbox cannot show "neither", so a boolean the file spells wrongly is shown -- and saved --
as the key's shipped default rather than as `false`.

Every key -- its caption, type, default, range and legal values -- comes from
[`src/settings.def`](../src/settings.def), which the loader and the launcher both expand, so
they cannot disagree. A key that carries a warning leads with `Caution:` or `Warning:` **in the
text** rather than in a colour.

### 1.2 The command line

**The title is a command-line word and there is no `title` key**: one pack carries both dumps, so
a key that chose the game would make every other key conditional on it.

```sh
VCThunder.exe                VCThunder.exe hydro          VCThunder.exe offroad
```

These words override the file for one run:

| Word | Overrides | For |
|---|---|---|
| `--link off\|lan\|local` | `[link] link` | the link transport (§7) |
| `--save DIR` | `[paths] save_dir` | a second instance: one save directory is one cabinet's settings |
| `--log FILE` | `[diagnostics] log_file` | a second instance: both logs are opened `"w"` |
| `--glide-log FILE` | `[graphics] vcglide_log` | a second instance, for the same reason |
| `--data DIR` / `--exe PATH` | `[paths] data_dir` / `exe` | a dump outside the pack |

Two cabinets on one machine therefore differ by four words:

```sh
VCThunder.exe hydro --link local --save save2 --log link2.log --glide-log vcglide2.log
```

`--trace` raises the log level for one run and `--dry-run` stops after mapping and patching,
without starting the game; neither is a setting and neither is written to the file.
[`docs/building.md`](building.md) has both.

## 2. Paths

Everything the run directory owns is named relative to `VCThunder.exe`, which sets its own
working directory to make that true.

**The dump is never written to, so it can be read-only**, and that holds by construction: a file
is routed by the ACCESS MODE the game opens it with, so anything opened for writing lands in
`save_dir`, and anything opened for reading comes from `save_dir` first and `data_dir`
otherwise.

## 3. The picture

`[graphics]` is the whole of it. The first two constraints below come from the cabinet's
hardware rather than from taste.

### 3.1 The raster

**The raster is 400 lines, and 512x400 is not a legal Glide mode.** `raster_height = 400` takes
a 640x400 surface -- the shortest legal mode containing the raster -- and lets the game clip
itself to the left 512 columns; `vcglide` crops the other 128 away. `raster_height = auto`
instead normalises the game to 512x384 and **cuts the top off every full-screen bitmap**.

`video_mode = auto` leaves the game asking for its own mode 1 -- 512x384 plus the cabinet's
512x400 CRT timing -- which is the only correct setting for a cabinet, and `raster_height` is
ignored while any other mode is set. **Mode `2` is 640x480 and may damage an original arcade
monitor.**

### 3.2 Window, fullscreen and aspect

**Fullscreen never changes the display mode**: it is a borderless window covering the monitor
the game is on, toggled with Alt+Enter. Exclusive fullscreen is not an option, because the arcade
binary has no message loop, so its mode-change handshake never completes and `grSstWinOpen`
hangs.

`aspect` means the same thing windowed and fullscreen: `raster` preserves the active game grid,
`4:3` and `16:9` fit that ratio, `stretch` fills the monitor, and every preserving mode is
centred with black bars. `window_scale` fixes the window at a multiple of the raster; there is
no maximise button and no resize frame.

`render_scale` re-renders the 3D scene at a multiple of the cabinet's resolution on the GPU
backend, and does cost GPU time; HUD and menu pixels stay native and are point-upscaled over it.
The CPU backend ignores it.

### 3.3 Widescreen

`widescreen` **ships off**, and the launcher calls it *Widescreen hack*, which is what it is:
Hor+ widening, not anything the cabinet did.

With it on, the game keeps its **entire native coordinate system** and the host carries the
difference as an origin at the Glide seam: the culling frustum widens while every viewport member
the 2D layer reads is put back to the cabinet's value, so no HUD, text, dial or menu coordinate
is rewritten anywhere. With `video_mode = auto` and `raster_height = 400` that is a **711x400
game raster inside an 856x480 backing surface**, field at x=99..610. Six consumers need a
per-title repair to follow it, so **widescreen refuses to arm if any address is absent**.

`aspect = 4:3` contradicts it and wins, turning `widescreen` off with a warning: presenting a
16:9 raster at 4:3 would squash the picture to 75% horizontally.

### 3.4 Frame rate: 30, locked, and there is no key

There is no `frame_rate` key, shipped or developer. Both titles hold a locked 30 fps in play, up
to `render_scale = 5`.

**In both titles one rendered frame is exactly one simulation tick**, so rendering more often
means ticking more often, and the game runs at double speed. Both ways out have been measured and
neither ships. `render_scale` is unaffected by any of this.

### 3.5 The provider, and GPU or CPU

`vcglide.dll` is the fixed provider in every pack and rasterises every call itself: the host
loads the copy beside `VCThunder.exe`, and there is no provider choice. `SDL3.dll` is the GPU
runtime staged beside it, and is the pack's only third-party file.

| `vcglide_renderer` | Result |
|---|---|
| `auto` | **Default.** Try SDL3 GPU; fall back to CPU if shader, device or window setup fails. |
| `gpu` | Request SDL3 GPU, and log an explicit warning before the same safe CPU fallback. |
| `cpu` | Never initialise SDL or a GPU driver; use the CPU reference rasteriser. |

The GPU path is SDL 3.4.10 on Direct3D 12, and a render-target allocation failure steps
`render_scale` down one at a time rather than failing the game open; the CPU path presents
through Direct3D 9/GDI.

**The rasteriser is not conformant**: it reproduces both games' 2D and LFB output exactly and
their 3D output to a threshold. The CPU path is the control; GPU output is replay-tested but not
byte-identical to it, by design.

`vcglide_threads` is how many workers the CPU rasteriser splits a triangle across, and **0, the
default, is one per PHYSICAL core**: hyperthread siblings contend for the execution units a span
loop wants. **It cannot change the picture** -- the split is by scanline row, so no two workers
share a pixel.

### 3.6 Captures and frame dumps

**`glide_capture` and `glide_frames` are diagnostics, not modes.** A capture writes ~280 KB of
vertices per frame, and a frame dump reads back the whole framebuffer and writes a PNG; both run
on the render thread, so nothing timed during such a run means anything. `glide_capture_frames`
bounds both, so a live run's frames and a replay's line up.

## 4. Controls

The host synthesises the cabinet's I/O board packets and runs the game's own decoder, so
everything downstream -- the operator menus' switch and analogue tests included -- behaves as it
did on the cabinet. `input_background = true` samples keys process-wide, so another window
holding focus cannot suppress cabinet controls.

### 4.1 Choosing a device

`[analog] provider = auto` tries **DirectInput, then XInput, then WinMM**, which costs pad
users nothing: unnamed, DirectInput claims only a controller that publishes force-feedback
effects -- a wheel does, an XInput pad does not. `dinput_name` or `dinput_device` claims a named
device whatever it is.

`pov_digital` defaults to `true`, which is a pad's behaviour: on a pad the D-pad *is* the
steering. **Set it `false` on a wheel**, where the hat is under a thumb that is also steering and
a full-lock override is a crash. `[diagnostics] input_trace` names every axis and button the
device reports, which is how it gets mapped.

### 4.2 The two control panels

The cabinets do not share one, which is why the per-title layer exists. Hydro has a throttle
lever, three views, and Boost as start and confirm; Offroad has a pedal box, a separate START and
NITRO, three cameras, and a three-speed shifter, where **releasing all three shift buttons is
neutral**. That shifter is a *maintained* gate, so a paddle cannot be wired to one position:
`shift_up` / `shift_down` step a latch on each rising edge and hold the selected line, leaving
the direct `shift1..3` bindings untouched.

### 4.3 Force feedback

**During gameplay the game computes the force and the host only carries it**, from the I/O
board's DAC byte to a DirectInput constant-force effect. No effect is invented -- no canned
rumble, no road texture -- so what the wheel does is the arcade physics. The launcher's Test
button is separate: one bounded 850 ms pulse.

**Three switches gate it, in three different places:** `[ffb] enabled`; `[analog] provider`,
which has to admit a wheel at all; and **the title's own F2 operator menu**, which the host
deliberately does not override. `[diagnostics] ffb_trace` censuses what the game commanded, so
"the wheel is not answering" and "the game is not asking" stay distinguishable.

`autocenter` must stay off while a constant force plays: the driver's spring is a second force
on the same axis and the wheel feels the sum. That leaves the wheel limp wherever the game
commands nothing, and **nothing here fills that gap on purpose** -- a centring spring would be an
invented force fighting the arcade's own.

## 5. Audio

`[audio] output` is not a preference. Both cabinets wired the left output to the front speakers
bridged to mono, and the right to the woofer in the seat -- two transducers, not a stereo image
-- so faithful playback sounds broken on PC speakers.

| `output` | What it does |
|---|---|
| `mix` | **Default.** Both cabinet buses to both speakers. |
| `cabinet` | The faithful wiring: front bus left, seat woofer right. |
| `front` | The front bus to both speakers; the seat-woofer bus is dropped. |

`balance` attenuates only, in both directions: 50 is unity on both buses, below it the seat
woofer comes down and above it the front speakers do. **The shipped default is 15**, which suits
desk speakers; a cabinet wants 50.

## 6. Diagnostics

`[diagnostics]` is off by default, and several of its switches ruin the picture on purpose. Two
rules apply to all of them:

- **Be sceptical of a number any of them produces.** Measure against a control at the same
  cadence, scale and depth.
- **Restore every switch after a run.** An experiment left enabled in a run directory is a
  hidden variable in everything measured afterwards.

**Four keys ship in the ini**: `log_file`, `log_level`, and the read-only `r2_cache` and
`stall_ms`, which are safe to leave on.

**The rest are developer switches and ship in no file.** Each is off by default and still works
if the line is added: the parser takes any key it knows, wherever it finds it, and **says so in
the log** (`[audio] render_scale belongs in [graphics]. It was applied anyway, but move it`). A
key of `[analog]`, `[ffb]` or a binding section gets a stronger line: those are read only within
their own section, so a misplaced one does nothing at all.

## 7. The arcade link

`[link]` selects **the transport this host puts under the games' own link protocol**, and is the
whole of what the ini decides about link play. Both keys are on the launcher's **Link Play** page:

| Key | Default | What it does |
|---|---|---|
| `link` | `off` | `off` \| `lan` \| `local`. `off` is standalone: nothing looks for a peer. `local` is two instances of one pack on this machine, and is the case that has been **measured**. `lan` is a second machine on the network: it runs, but has never had one to answer it |
| `link_rx_budget` | `64` | datagrams a single frame may take before `recvfrom` answers `WSAEWOULDBLOCK`, 1..4096 |

**`link` is not the link's on/off switch, and it is not a cabinet number.** Whether a cabinet
links at all, and which unit it is, are the GAME's own operator settings in its NVRAM, reached
through the service menu, `F2`. Turning `link` on does not enable the link; it makes that
setting able to work. A second instance needs three files of its own (§1.2).

## 8. Key reference

Defaults are the shipped ini's.

### `[paths]`

| Key | Default | Meaning |
|---|---|---|
| `data_dir` | `data` | the title's assets: `data/<title>/`, or a title's files directly |
| `save_dir` | `save` | everything the game writes: operator settings, audits, high scores, race statistics |
| `exe` | empty | developer key: an executable outside the pack |

Both are relative to `VCThunder.exe`, and the dump is never written to (§2). **A second instance
needs its own `--save`**: one save directory is one cabinet's settings.

### `[graphics]`

| Key | Default | Values |
|---|---|---|
| `vcglide_renderer` | `auto` | `auto` \| `gpu` \| `cpu` (§3.5) |
| `vcglide_threads` | `0` | CPU-backend workers; 0 = one per physical core, 1 = off |
| `vcglide_log` / `vcglide_verbose` | `vcglide.log` / `false` | the provider's log; verbose adds a line per entry point |
| `fullscreen` | `false` | borderless; Alt+Enter toggles, the display mode never changes |
| `window_scale` | `1` | 1..8 or `auto`; the window cannot be resized |
| `render_scale` | `1` | 1..8 internal 3D resolution, GPU backend only |
| `aspect` | `raster` | `raster` \| `4:3` \| `16:9` \| `stretch`; **`4:3` turns `widescreen` off** |
| `widescreen` | `false` | 711x400 Hor+ raster when on (§3.3) |
| `raster_height` | `400` | the cabinet's raster; `auto` gives 512x384 and **cuts the top off every 400-row bitmap** |
| `video_mode` | `auto` | the game's own mode table. **`2` is 640x480 and may damage an original monitor** |
| `glide_capture` / `glide_frames` | empty | write a `.vglc` call stream / a PNG per frame (§3.6) |
| `glide_capture_frames` | `60` | how many frames both of those cover; 0 = until exit |

### `[timing]`

| Key | Default | Values |
|---|---|---|
| `cpu_hz` | `333000000` | 1 MHz..4000 MHz: the cabinet's Celeron identity, which its `rdtsc` pacing was tuned to. It does *not* control game speed. The launcher shows MHz |
| `speed_percent` | `100` | the virtual clock's ratio, 25..400 |

There is no `frame_rate` key; §3.4 says why.

### `[audio]`

| Key | Default | Values |
|---|---|---|
| `audio` | `true` | sound on |
| `volume` | `100` | host-side gain, percent |
| `output` | `mix` | `mix` \| `cabinet` \| `front` (§5) |
| `balance` | `15` | 0..49; attenuates only, 50 is unity on both buses |
| `dcs_ms` | `5` | 1..100: the thread standing in for the ~5.4 ms audio interrupt, a scheduling granularity rather than a latency |

### `[controls]`

| Key | Default | Values |
|---|---|---|
| `dip_switches` | `0xFF` | the cabinet's DIP block |
| `io_ms` | `16` | board poll period; the cabinet ran ~60 Hz |
| `input_background` | `true` | sample keys process-wide (§4) |

`backend` and `port` also parse here, for a board transport other than the synthesised one. Both
are developer keys and ship in no file.

### `[digital]` — the keyboard

**This is what the pack ships bound.** Offroad overrides six of the shared rows:

| Action | Hydro | Offroad | | Action | Both |
|---|---|---|---|---|---|
| `steer_left` / `steer_right` | Left / Right | Left / Right | | `coin1` / `coin2` | 5 / 6 |
| `throttle` | Up | **LCtrl** | | `start1` | 1 |
| `brake` | Down | **LAlt** | | `service1` | 9 |
| `boost` | LCtrl | **Space** | | `service_mode` (F2 menu) | F2 |
| `view_high` / `view_low` / `view_pilot` | LAlt / Space / LShift | **V / B / N** | | `volume_down` / `volume_up` | - / = |
| `shift1` / `shift2` / `shift3` | — | **Z / X / C** | | | |

Steering and throttle are analogue cabinet fields, so a digital binding moves them to full
lock, and Offroad will not leave the grid on the throttle alone: its gate wants a gear. Escape
quits and Alt+Enter toggles fullscreen; neither is bindable. Key names are MAME's (`A`-`Z`,
`F1`-`F24`, `LEFT`, `LCTRL`, `SPACE`, `MINUS`, `NONE`, …), with an optional `KEYCODE_` prefix.

### `[analog]` — wheels and pads

| Key | Default | Values |
|---|---|---|
| `provider` | `auto` | `auto` (DirectInput, XInput, WinMM in that order) \| `dinput` \| `xinput` \| `winmm` \| `none` |
| `deadzone_percent` | `12` | 0..49 |
| `pov_digital` | `true` | let a hat or D-pad drive the axes at full lock. **`false` on a wheel** (§4.1) |
| `xinput_device` / `winmm_device` | `0` | zero-based device index |
| `dinput_device` | `auto` | `auto` is the attached controller that has a motor |
| `dinput_name` | empty | a substring of the product name; overrides `dinput_device` |
| `dinput_deadzone_percent` | `0` | DirectInput's own, separate from `deadzone_percent` |
| `dinput_steering_range_percent` | `100` | how much travel reaches full lock: 100 suits the cabinet's ~270°, 30 a wheel left at 900°. **Check the wheel first** |

Then one axis group per backend: `<backend>_steering_axis`, `_throttle_axis`, `_brake_axis`,
each with a matching `_invert`.

| Backend | Axis names |
|---|---|
| XInput | `lx ly rx ry lt rt none` |
| WinMM | `x y z r u v none` |
| DirectInput | `x y z rx ry rz slider0 slider1 none`, as its own control panel names them |

`winmm_pedal_mode` and `dinput_pedal_mode` are `combined` (one centred axis) or `separate` (brake
subtracted from throttle). **A wheel whose driver merges its pedals has only one axis, and
pointing `separate` at a second one is a permanent half-brake.**

### `[ffb]`

| Key | Default | Values |
|---|---|---|
| `enabled` | `true` | the host's own switch; two more gate it (§4.3) |
| `invert` | `true` | which way the cabinet's motor was wired |
| `strength_percent` | `100` | 0..200, this host's scale on the game's force |
| `device_gain_percent` | `100` | the device's overall gain |
| `autocenter` | `false` | **and it must stay off** (§4.3) |

### `[gamepad]` — button ordinals

Numbered as the device's own control panel numbers them; `0` unbinds, 32 is the ceiling. XInput
is A=1 B=2 X=3 Y=4 LB=5 RB=6 Back=7 Start=8 LStick=9 RStick=10.

| Action | Default | | Action | Default |
|---|---|---|---|---|
| `boost` | `1` | | `start1` | `8` |
| `view_high` | `2` | | `coin1` | `7` |
| `view_low` | `3` | | `coin2` | `0` |
| `view_pilot` | `4` | | `service1` / `service_mode` | `0` |
| `shift1` / `shift2` / `shift3` | `0` | | `volume_down` / `volume_up` | `0` |
| `shift_up` / `shift_down` | `0` | | | |

`shift1..3` are Offroad's gate directly; `shift_up` / `shift_down` are the same gate on two
paddles (§4.2).

### `[link]`

`link` (`off`) and `link_rx_budget` (`64`): §7.

### `[diagnostics]`

| Key | Default | Values |
|---|---|---|
| `log_file` | `vcthunder.log` | read only from the plain section (§1) |
| `log_level` | `info` | `error` \| `warn` \| `info` \| `trace`; `trace` logs every stub hit and binding |
| `r2_cache` | `true` | read-only accelerator, safe to leave on |
| `stall_ms` | `60` | frame-stall threshold in ms; a read-only classification |

Everything else in this section is a developer switch that ships in no file: §6.
