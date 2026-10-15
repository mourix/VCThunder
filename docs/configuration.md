# Configuration

**Most people need none of this page.** Run `VCThunder.exe` with no arguments: the launcher shows
every shipped setting with its own help text. To get back to the shipped defaults, delete
`vcthunder.ini` and start again. This page is what the launcher cannot show; §8 lists every key.

---

## 1. The file model

One pack, one file, both cabinets:

```
[section]            both titles
[<title>.section]    one title only; wins key by key
[analog.<label>]     one CONTROLLER only; wins over both (§4.1.1)

built-in defaults  ->  [section]  ->  [<title>.section]  ->  command line
```

So `[offroad.audio] output = cabinet` leaves Hydro alone; anything a title section does not name
falls through to the plain one. There is no flat `[offroad]` section.

- **The log names where each value came from.** An illegal value is refused with a reason and
  the previous one stands. An unknown key is ignored; only the launcher names it.
- Inline `;` and `#` comments need whitespace before them.
- `log_file` is read only from the plain `[diagnostics]`, because the log is open before the title
  is known.

> **Never paste a second copy of a section or a key**; edit the one that is there. In `[analog]`,
> `[ffb]`, the bindings and `[devices]` the game reads only the **first** block and the **first**
> copy of a key, silently. The launcher lists both on *Help*.

### 1.1 The launcher

- **Apply writes only what changed**, keeping ordering, comments, unknown keys and line endings.
  Every save first copies the old file to `vcthunder.ini.bak`.
- **Restore default settings**, on *Help*, replaces `vcthunder.ini` with a clean copy
  of the shipped one, the file a fresh download starts with, keeping the old file as
  `vcthunder.ini.bak`. Deleting `vcthunder.ini` does the same on the next start.
- **Per-title values are shown, not edited**, except control bindings and the controller each
  game uses (§4.1.4). A key set in `[<title>.<section>]` says so on its row, as the shipped
  `[hydro.ffb]` rumble trims do.
- ***Help* lists lines nothing reads** (unknown keys, duplicate sections) and never
  changes them.
- **Set up... measures your axes**: move each control when asked. It listens on **every**
  DirectInput controller, so the accelerator can be a pedal on the wheel or a throttle lever on
  another controller; an axis on another controller is written `<label>:<axis>` (§4.1.3). A lever
  counts once it has moved and stays put. **Skip step** leaves that row as it was. Nothing is
  written until you confirm what it measured and press Apply.
- **Show advanced settings on every page** (on *Help*) reveals the rest of the keys, and three
  pages that are not listed without it: *Game System*, *Files & Logging* and *Link Play* (which
  stays listed while `[link] link` is not `off`).
- **Show the log files**, on *Help*, opens Explorer on `vcthunder.log`, the file a problem report
  needs.

### 1.2 The command line and the shipped file

```sh
VCThunder.exe                VCThunder.exe hydro          VCThunder.exe offroad
```

**The title is a command-line word; there is no `title` key.** These words override the file for
one run:

| Word | Overrides |
|---|---|
| `--link off\|lan\|local` | `[link] link` (§7) |
| `--save DIR` | `[paths] save_dir` |
| `--log FILE` | `[diagnostics] log_file` |
| `--glide-log FILE` | `[graphics] vcglide_log` |
| `--data DIR` / `--exe PATH` | `[paths] data_dir` / `exe` |

Two cabinets on one machine need their own save directory and logs:

```sh
VCThunder.exe hydro --link local --save save2 --log link2.log --glide-log vcglide2.log
```

`--trace` raises the log level for one run; `--dry-run` stops after loading, without starting the
game. See [`building.md`](building.md).

**The shipped `vcthunder.ini` is built into `VCThunder.dll`** and written out whenever the pack has
none; an existing file is never overwritten. That is why a release zip carries no ini: extracting
an update over your pack cannot replace your controller setup. The ini always sits beside the exe.

If it cannot be written, the log says why: usually a pack under `Program Files` or still inside a
zip viewer.

## 2. Paths

Paths are relative to `VCThunder.exe`. **The game dump is never written to**, so it can be
read-only: anything the game opens for writing goes to `save_dir`, and reads check `save_dir`
first, then `data_dir`.

## 3. The picture

Everything here is in `[graphics]`.

### 3.1 Raster and video mode

The cabinet's picture is 512x400. Keep `raster_height = 400` and `video_mode = auto`:

- `raster_height = auto` gives 512x384 and **cuts the top off every full-screen bitmap**.
- `video_mode = auto` is the only correct setting for a real cabinet. **Mode `2` is 640x480 and
  may damage an original arcade monitor.** `raster_height` is ignored under any other mode.

### 3.2 Window, fullscreen and aspect

- **Fullscreen is a borderless window** over the game's monitor; Alt+Enter toggles it. The display
  mode never changes; exclusive fullscreen would hang the game.
- `aspect`: `raster` keeps the game's own shape, `4:3` and `16:9` force that ratio, `stretch` fills
  the screen. The others add black bars.
- `window_scale` sizes the window as a multiple of the raster; it cannot be resized. `auto`, the
  default, takes the largest multiple that fits the screen.
- `render_scale` renders the 3D scene at a higher resolution on the GPU. HUD and menus stay native
  and are scaled up. It costs GPU time and does nothing on the CPU renderer.
- `upscale_2d` is how HUD and menus are scaled up when `render_scale` is above 1: `bilinear`
  (smooth), `catmull_rom` (slightly sharper, the default) or `fsr1` (AMD FSR 1: sharper edges, no
  halos).
- `texmag` is how 3D textures are drawn where they are stretched, which at `render_scale` 3 is
  most of the 3D: `bilinear` (as the cabinet) or `catmull_rom` (sharper, rounder texels; the
  default). Above `render_scale` 1 only.
- The launcher shows those two as on/off boxes, where off is `bilinear` and on is `catmull_rom`;
  *Show advanced settings* turns them back into the full choice, `fsr1` included.
- `bezel` frames the picture with bezel art: a PNG with a transparent screen hole, the kind made
  for MAME and RetroArch at 1920x1080. **Off by default.** The launcher's Graphics page has a
  Bezel box with a switch and **Choose...** for each game; Apply copies the chosen file into the
  pack's `bezels` folder and writes the line. By hand it is one line per title:

  ```ini
  [hydro.graphics]
  bezel = bezels/hydro.png
  [offroad.graphics]
  bezel = bezels/offroad.png
  ```

  The hole is found from the art itself: the transparent area around its centre. The art keeps
  its own shape and is fitted to the window or screen, with black outside it. The picture keeps
  its shape too: the cabinet's 4:3 grows until it fills the hole (what passes the hole's edge
  lies under the frame), and a `widescreen` picture spans the hole's width with black above and
  below it, inside the frame. `aspect` is not used while a bezel is on. The window takes the
  art's shape, with the hole as tall as the cabinet's picture at `window_scale` (a widescreen
  picture spans the hole's width, so it is shorter than that, at a scale that is not a whole
  number). A file that cannot be used is named in the log with the reason, and the game runs
  without it; so is art that frames nothing, a transparent area that reaches both sides of the
  image or three of its edges.

### 3.3 Widescreen

`widescreen` is **off by default** and is a hack: Hor+ widening the cabinet never had. With the
default raster it gives a 711x400 picture, with the HUD and menus left where the game put them.
A title missing an address it needs, or `aspect = 4:3`, turns it off with a warning.

`widescreen_hud` says where the HUD goes under `widescreen`. `centre` (the default) leaves it in
the cabinet's 4:3 field. `edges` moves what sits in the left or right third of that field out to
the widescreen's edges, at the distance the cabinet kept from its own: in a race the dials, the
map, the lap time and the place and lap text; in attract the logo and web address (Hydro) or the
demo logo (Offroad; its credit line stays put). The centre group stays put, and so does anything
drawn over a full-screen 4:3 page, menus and high-score tables included.

### 3.4 Frame rate

`frame_rate` is `30` (the cabinet) or `60`. The game always runs at 30 ticks a second, one picture
per tick, so its speed and its handling are the cabinet's whatever this says. At `60` an extra
picture is drawn between each two of the game's own: the game's own draw, run on a copy of its
state interpolated halfway between the two ticks and put back afterwards. Nothing is blended as an
image. Real frames reach the screen about 17 ms later. When an
in-between would make the game's tick late it is skipped, so a machine that cannot keep up shows
30 fps rather than slowing the game. Some motion stays at 30: particles, the water's own animation,
HUD animation.

### 3.5 GPU or CPU

| `vcglide_renderer` | Result |
|---|---|
| `auto` | **Default.** Use the GPU (Direct3D 12); fall back to the CPU if setup fails. |
| `gpu` | The same, but warns if it has to fall back. |
| `cpu` | CPU renderer only; never touches the GPU. |

GPU and CPU pictures match closely, not pixel for pixel, by design.

### 3.6 Captures and frame dumps

`glide_capture` and `glide_frames` are slow diagnostics: never time a run with one on. Under
`frame_rate = 60` a capture's own cost leaves no room for in-between pictures, so a capture holds
only the real frames.

## 4. Controls

The host builds the cabinet's I/O board packets and the game decodes them itself, so the operator
menus' switch and analogue tests work as they did on the cabinet. With
`input_background = true`, keys are read even when another window has focus.

### 4.1 Choosing a device

`[analog] provider = auto` tries **DirectInput, then XInput, then WinMM**. Unless you name a
device, DirectInput only takes a controller with force feedback, so a wheel gets DirectInput and an
Xbox pad falls through to XInput. **An Xbox pad is listed once, as XInput**: DirectInput sets aside
any controller Windows tags as an XInput device, as MAME does, unless it has force feedback, which
only DirectInput can drive. `dinput_name` (part of the controller's name or device id, see
§4.1.1) picks a specific device; there is no device number, because a number shifts whenever
anything is plugged in.

`pov_digital = auto` lets a D-pad steer on a pad but not on a force-feedback wheel, where a
bumped hat would jolt to full lock. `true` or `false` overrides it.

**If an axis does not work, turn on `[diagnostics] input_trace`** (launcher *Controls*, advanced).
The log then lists every axis and button the device reports, warns about a configured axis it
lacks, and shows the map in use. A missing axis is ignored, so it cannot silence the
pedal beside it.

#### 4.1.1 One section per controller: `[devices]` and `[analog.<label>]`

Name a controller and it gets its own section, so setting up a pad cannot overwrite a wheel's
pedals:

```ini
[devices]
; <label> = <provider>:<match>
tmx = dinput:Thrustmaster TMX
pad = xinput:0

[analog.tmx]
dinput_throttle_axis = rz
dinput_brake_axis    = y
dinput_pedal_mode    = separate
```

- **label**: yours to choose; up to 15 letters, digits, `_` or `-`.
- **match**: on DirectInput, part of the controller's **name**, or of its whole
  **device id** when the match holds `product_` or `instance_`. The log prints the id for each
  one: `io:   device id: Thrustmaster TMX Racing Wheel product_b67f044f-… instance_5be31ef0-…`,
  in [MAME's](https://docs.mamedev.org/advanced/devicemap.html) format. A GUID without its prefix
  matches nothing, because a few hex letters would otherwise match some GUID by chance. On XInput
  and WinMM, the device number.
- **Two controllers with one name** (two identical button boxes, or the two halves a Fanatec
  wheel presents) are told apart by the `instance_` part. A line holding `instance_` **pins**
  that controller: it belongs to the lines that pin it, and a line matching only the name opens the
  other one, whatever order Windows lists them in. The launcher writes the name when the name is
  enough and the full id when it is not. A pinned controller plugged into another USB port may get
  a new id; it then shows `(not connected)` and is added again once.
- **Lookup order**: `[<title>.analog.<label>]` → `[analog.<label>]` → `[<title>.analog]` →
  `[analog]` → default. So a profile holds only what is different about that controller.
- **What goes in a profile**: the `dinput_*`, `xinput_*` and `winmm_*` keys, except the four that
  choose the device (`dinput_name`, `xinput_device`, `winmm_device`).

The launcher writes the `[devices]` line for you. The log warns about an `[analog.<label>]` that
no `[devices]` line names, usually a typo.

#### 4.1.2 Extra controllers: shifters, button boxes

**A `dinput:` entry in `[devices]` also opens that controller**, so a USB shifter or button box
works next to the wheel. Put its label in front of the button number:

```ini
[devices]
tmx = dinput:Thrustmaster TMX
th8 = dinput:TH8A

[offroad.gamepad]
shift1 = th8:1
shift2 = th8:2
shift3 = th8:3
boost  = 1, th8:4      ; wheel button 1 OR the shifter's button 4
```

- **A bare number is the primary controller**, the one with force feedback and the hat. The
  primary is `dinput_name`, then the first controller with force feedback;
  never this table, so adding a shifter cannot move your axes onto it. `dinput_name` follows the
  same pins as the table: a name skips a controller a `[devices]` line pins.
- Extra controllers give their buttons, and any axis §4.1.3 names on them. They are `dinput:`
  controllers or **XInput pads**: `pad = xinput:0` beside a wheel makes `pad:1` the pad's A button
  and `pad:rt` its right trigger.
- **An Xbox pad has no DirectInput view here**: name it `xinput:0`, never `dinput:`.
- A binding naming a controller that is not open **does nothing** and the log says why.
- **Two labels for one controller are two names for it**: bindings on either reach it.
- **Two controllers with the same name each get a label**: the first by name, the second by its
  device id (§4.1.1).
- Limits: eight controllers, four buttons per action, 128 buttons per controller.

The launcher's *Add* writes the label for you on DirectInput devices.

#### 4.1.3 Steering, gas and brake from different controllers

Put a label and a colon in front of an axis to read it from that controller:

```ini
[devices]
fanatec = dinput:FANATEC
twcs    = dinput:TWCS Throttle

[analog.fanatec]
dinput_steering_axis = x        ; no label: the primary controller
dinput_throttle_axis = twcs:z   ; the TWCS's z axis
dinput_brake_axis    = rz

[analog.twcs]
dinput_throttle_invert = false  ; facts about the TWCS's own axis
```

- **The three axis keys and `dinput_pedal_mode` belong to the primary controller's section.** A
  file without labels reads exactly as before.
- **Invert, deadzone and steering range are read from the section of the controller the axis is
  on**: `[analog.twcs]` above, then `[analog]`. The launcher shows and writes them there.
- **A controller that is not plugged in, or stops answering, reads released**: steering centred,
  pedal up. It never falls back to the primary controller's axis of the same name. The log names
  the line and says why.
- **`combined` pedals with the brake on another controller** are read as separate, with a warning:
  combined means one axis carries both pedals.
- **On an XInput pad the axis names are XInput's**: `lx ly rx ry lt rt`. `dinput_throttle_axis =
  pad:rt` with `[analog.pad] dinput_throttle_invert = false` is the right trigger as gas; a trigger
  rests low, so the invert is false (*Set up...* measures it).
- Only the DirectInput role keys take a label; the `xinput_*` and `winmm_*` keys read one
  controller, the one driving.
- *Set up...* writes these for you: move the lever when it asks for the accelerator.
- The log prints one line saying where each axis is read from:
  `io: axis roles: steering x on "FANATEC Wheel", accelerator twcs:z on "TWCS Throttle", ...`.
  `--dry-run` prints it without starting a race.

#### 4.1.4 A different controller for each game

Tick **Separate controller per game** on the launcher's *Controls* page and the
**Game** row at the top of the page picks whose controller the list shows, as it always picks whose
buttons. Each game then keeps its own controller and its own steering, accelerator and brake axes.
In the file:

```ini
[devices]
tmx = dinput:Thrustmaster TMX
pad = xinput:0

[hydro.analog]
dinput_name = Thrustmaster TMX   ; the wheel and its force feedback in Hydro
provider    = dinput

[offroad.analog]
provider = xinput                ; the Xbox pad and its rumble in Offroad

[hydro.analog.tmx]
dinput_throttle_axis = twcs:z    ; Hydro only: accelerate with a lever
```

- **Per game**: `provider`, `dinput_name`, `xinput_device`, `winmm_device` and the
  `*_steering_axis`, `*_throttle_axis`, `*_brake_axis` keys. The selectors go in
  `[<title>.analog]`; an axis goes in **`[<title>.analog.<label>]`**, because the controller's own
  `[analog.<label>]` beats `[<title>.analog]` (§4.1.1's lookup order).
- **Shared by both games**: everything else about a controller (invert, deadzone, steering range,
  pedal mode), so a wheel is calibrated once. A line like that in a `[<title>.analog...]` section
  still works, and its row says which game sets it.
- **The file is split when any of those keys sits in a `[<title>.analog...]` section with a value.**
  Unticking the box **moves** `provider` and the device choice out of `[analog]` into both games;
  ticking it again keeps the shown game's choice as the shared one and comments out every per-game
  line.
- **An empty per-game value means "not set"**: the game falls through to `[analog]`, then the
  default. So a game cannot say "no wheel name" while `[analog]` names one; the launcher refuses that
  on Apply. A device choice under a controller's label (`[hydro.analog.tmx] provider`) is read by
  nothing and is listed on *Help*.
- `[devices]` stays shared, so every listed controller is opened in both games; only the driving
  one changes. A bare button number means the driving controller, which can now differ per game.
- The log says when a game made its own choice:
  `io: provider = xinput, from [offroad.analog]: this game has its own controller`.

### 4.2 The two control panels

Hydro has a throttle lever, three views, and Boost as start. Offroad has pedals, START, NITRO,
three cameras, and a three-speed shifter where **releasing all shift buttons is neutral**.

Because that shifter holds its position, a paddle cannot be wired to one gear. `shift_up` and
`shift_down` step through the gears instead and hold the one selected. **In Offroad they are on
LB/RB by default** (`shift_down = 5`, `shift_up = 6`); Hydro has no shifter. `shift1..3` are for
an H-pattern shifter and are unbound by default. Bind a wheel's paddles on the launcher's
*Controls* page.

### 4.3 Force feedback

**The game computes the force; the host only passes it on** to a DirectInput constant-force
effect. Nothing is invented (no canned rumble, no road texture), except rumble on a pad (§4.3.1).

Three switches must all be on: `[ffb] enabled`; `[analog] provider`, which has to allow a wheel;
and **the game's own F2 operator menu**, which the host leaves alone. `[diagnostics] ffb_trace`
logs what the game asked for, so you can tell "the wheel is not answering" from "the game is not
asking".

**Keep `autocenter` off.** The driver's centring spring would fight the game's force. The wheel
therefore goes limp when the game asks for nothing; nothing here adds a spring on purpose.

#### 4.3.1 A gamepad: `rumble`

A pad has no force motor, so the host turns the game's force into rumble. It is on by default,
needs `[ffb] enabled` too, and **a DirectInput wheel never gets it**. It works through XInput only.

The raw force mixes steering into everything, so the pad reads the game's own values:

| Channel | Hydro | Offroad |
|---|---|---|
| **impact** | collisions | the game's hit value |
| **texture** | n/a | four per-tyre jolts |
| **body** | n/a | the suspension |
| **engine** | the force's fast part, above ~8 Hz | the same |
| **boost** | firing while its boost seconds fall | firing while its nitro flag is set |

Boost is its own path. When the game starts boosting, a spark on both motors; while it runs, a
buzz on the light motor as strong as the boost's alternation in the force. Pressing the button
with an empty tank does nothing, as in the game. `rumble_boost_percent` sets it and the
surface trim does not touch it.

`rumble_events = false` filters the raw force instead, as Offroad's menus always do.

**The titles need different numbers**, which is why the shipped ini sets `[hydro.ffb]` and
`[offroad.ffb]` separately. Tune rumble in those sections, not in `[ffb]`.

## 5. Audio

Both cabinets sent the left channel to the front speakers and the right to a woofer in the seat.
That is two separate speakers, not stereo, and played faithfully it sounds wrong on a PC.

| `output` | What it does |
|---|---|
| `mix` | **Default.** Both channels to both speakers. |
| `cabinet` | The real wiring: front left, seat woofer right. |

`balance` turns one bus down. `50` is equal; below it the seat woofer gets quieter, above it
the front. **The default, 15, suits desk speakers; a cabinet wants 50.** The launcher's slider
labels its two ends with the level each plays at. In `mix` the two buses keep the same total
level, so `0` plays the front alone on both speakers at full level and `100` the seat woofer.

## 6. Diagnostics

`[diagnostics]` is off by default. Two rules for every switch in it:

- **Be sceptical of any number it produces.** Compare against a run without it.
- **Turn it off afterwards.** A switch left on skews everything measured later.

The shipped ini has only `log_file` and `log_level`. `stall_ms` and `input_trace` are in the
launcher under advanced.

**Everything else is a developer switch**, off by default and in no file; add the line to use it. The parser takes any key it knows, wherever it finds it, and **the log
says to move it**. The exception is `[analog]`, `[ffb]` and the binding sections: their keys are read only in their own
section, so a misplaced one does nothing.

## 7. The arcade link

`[link] link` chooses **how** two cabinets reach each other.

| `link` | Meaning |
|---|---|
| `off` | **Default.** Standalone. |
| `local` | Two instances of one pack on this machine. Tested. |
| `lan` | A second machine on the network. Implemented, not yet tested. |

**This is not the on/off switch for link play, and it is not a cabinet number.** Both are the
game's own operator settings, in its F2 service menu. `link` only makes those settings able to
work. A second instance on one machine: §1.2.

## 8. Key reference

Defaults are the built-in ones. The shipped ini changes none of them, except the per-title rumble
trims (§4.3.1).

### `[paths]`

| Key | Default | Meaning |
|---|---|---|
| `data_dir` | `data` | the folder holding each title's dump, as `<title>/` |
| `save_dir` | `save` | everything the game writes: settings, audits, high scores |
| `exe` | empty | developer key: an executable outside the pack |

### `[graphics]`

| Key | Default | Values |
|---|---|---|
| `vcglide_renderer` | `auto` | `auto` \| `gpu` \| `cpu` (§3.5) |
| `vcglide_threads` | `0` | CPU renderer threads; 0 = one per physical core, 1 = off |
| `vcglide_log` / `vcglide_verbose` | `vcglide.log` / `false` | the renderer's log |
| `fullscreen` | `false` | borderless; Alt+Enter toggles |
| `window_scale` | `auto` | 1..8 or `auto` |
| `render_scale` | `1` | 1..8, GPU only |
| `upscale_2d` | `catmull_rom` | `bilinear` \| `catmull_rom` \| `fsr1`; above `render_scale` 1 (§3.2) |
| `texmag` | `catmull_rom` | `bilinear` \| `catmull_rom`; 3D textures above `render_scale` 1 (§3.2) |
| `frame_rate` | `30` | `30` \| `60`; 60 draws an interpolated picture between the game's own (§3.4) |
| `aspect` | `raster` | `raster` \| `4:3` \| `16:9` \| `stretch`; `4:3` turns `widescreen` off |
| `bezel` | `off` | a PNG path, relative to the pack; usually per title (§3.2) |
| `widescreen` | `false` | 711x400 Hor+ (§3.3) |
| `widescreen_hud` | `centre` | `centre` \| `edges`; race HUD and attract overlay under `widescreen` (§3.3) |
| `raster_height` | `400` | `auto` gives 512x384 and **cuts the top off full-screen bitmaps** |
| `video_mode` | `auto` | **`2` is 640x480 and may damage an original monitor** |
| `glide_capture` / `glide_frames` | empty | a call-stream capture / a PNG per frame (§3.6) |
| `glide_capture_frames` | `60` | frames those cover; 0 = until exit |

### `[timing]`

| Key | Default | Values |
|---|---|---|
| `cpu_hz` | `333000000` | the cabinet CPU speed the game expects; **not** game speed |
| `speed_percent` | `100` | 25..400, the game clock's speed |

### `[audio]`

| Key | Default | Values |
|---|---|---|
| `audio` | `true` | sound on |
| `volume` | `100` | percent |
| `output` | `mix` | `mix` \| `cabinet` (§5) |
| `balance` | `15` | 0..100; 50 is equal (§5) |
| `dcs_us` | `5400` | 1000..100000, the sound thread's period in microseconds; 5400 is the cabinet's, and the game's fades are sized for it |

### `[controls]`

| Key | Default | Values |
|---|---|---|
| `dip_switches` | `0xFF` | the cabinet's DIP switches |
| `io_ms` | `16` | I/O board poll period (the cabinet ran ~60 Hz) |
| `input_background` | `true` | read keys when not focused (§4) |

`backend` and `port` are developer keys for a real I/O board.

### `[digital]`: the keyboard

| Action | Hydro | Offroad | | Action | Both |
|---|---|---|---|---|---|
| `steer_left` / `steer_right` | Left / Right | Left / Right | | `coin1` / `coin2` | 5 / 6 |
| `throttle` | Up | **LCtrl** | | `start1` | 1 |
| `brake` | Down | **LAlt** | | `service1` | 9 |
| `boost` | LCtrl | **Space** | | `service_mode` (F2 menu) | F2 |
| `view_high` / `view_low` / `view_pilot` | LAlt / Space / LShift | **V / B / N** | | `volume_down` / `volume_up` | - / = |
| `shift1` / `shift2` / `shift3` | n/a | **Z / X / C** | | | |

A key moves steering or throttle to full. Offroad needs a gear to leave the grid. Escape quits
and cannot be rebound. Key names are
MAME's (`A`-`Z`, `F1`-`F24`, `LEFT`, `LCTRL`, `SPACE`, `MINUS`, `NONE`, …), with an optional
`KEYCODE_` prefix.

### `[devices]`

`<label> = <provider>:<match>`: see §4.1.1 and §4.1.2.

### `[analog]`: wheels and pads

Most `dinput_`, `xinput_` and `winmm_` keys may also go in `[analog.<label>]` (§4.1.1).

| Key | Default | Values |
|---|---|---|
| `provider` | `auto` | `auto` \| `dinput` \| `xinput` \| `winmm` \| `none` (§4.1) |
| `deadzone_percent` | `12` | 0..49 |
| `pov_digital` | `auto` | `auto` \| `true` \| `false`: let a hat or D-pad steer (§4.1) |
| `xinput_device` / `winmm_device` | `0` | device number, from 0 |
| `dinput_name` | empty | part of the device name or id (§4.1.1); empty = the first controller with force feedback, or none |
| `dinput_deadzone_percent` | `0` | DirectInput's own deadzone |
| `dinput_steering_range_percent` | `100` | 100 for a wheel at ~270°, 30 for one at 900° |

Each backend has `_steering_axis`, `_throttle_axis` and `_brake_axis`, each with an `_invert`:

| Backend | Axis names | Defaults |
|---|---|---|
| XInput | `lx ly rx ry lt rt none` | `lx` / `rt` / `lt` |
| WinMM | `x y z r u v none` | `x` / `y` / `none`, `combined`, throttle inverted |
| DirectInput | `x y z rx ry rz slider0 slider1 none`, or `<label>:<axis>` on another controller (§4.1.3) | `x` / `y` / `rz`, `separate`, pedals inverted |

`dinput_pedal_mode` and `winmm_pedal_mode` are `combined` (one axis for both pedals) or
`separate`. **If the driver combines the pedals, `separate` leaves you with a permanent
half-brake.**

### `[ffb]`

| Key | Default | Values |
|---|---|---|
| `enabled` | `true` | (§4.3) |
| `direction` | `normal` | `normal` \| `reversed` |
| `strength_percent` | `100` | 0..200 |
| `device_gain_percent` | `100` | the device's overall gain |
| `autocenter` | `false` | **keep it off** (§4.3) |
| `rumble` | `true` | pad rumble (§4.3.1) |
| `rumble_events` | `true` | read the game's own values; off filters the raw force |
| `rumble_texture_percent` | `50` | 0..400, the background rumble |
| `rumble_texture_curve_percent` | `35` | 2..100, compression; 100 is none |
| `rumble_strength_percent` | `100` | 0..200, everything |
| `rumble_sustain_percent` | `100` | 0..200, the heavy motor |
| `rumble_impact_percent` | `100` | 0..200, the light motor |
| `rumble_spring_percent` | `0` | 0..100, adds the steering force back |
| `rumble_agc_percent` | `100` | 100..800, boosts quiet stretches; 100 is off |
| `rumble_boost_percent` | `100` | 0..200, the boost spark and buzz (§4.3.1) |

The shipped ini sets `[hydro.ffb]` texture 100 / curve 100 and `[offroad.ffb]` texture 8.

### `[gamepad]`: button numbers

Numbered as the device's own control panel numbers them. `0` unbinds; the maximum is 128 (32 on
WinMM). XInput: A=1 B=2 X=3 Y=4 LB=5 RB=6 Back=7 Start=8 LStick=9 RStick=10.

A value is a comma list of up to four `[<label>:]<number>` (§4.1.2).

| Action | Default | | Action | Default |
|---|---|---|---|---|
| `boost` | `1` | | `start1` | `8` |
| `view_high` | `2` | | `coin1` | `7` |
| `view_low` | `3` | | `coin2` | `0` |
| `view_pilot` | `4` | | `service1` / `service_mode` | `0` |
| `shift1` / `shift2` / `shift3` | `0` | | `volume_down` / `volume_up` | `0` |
| `shift_up` / `shift_down` | `0`; **Offroad `6` / `5`** | | | |

A binding written into the shared `[gamepad]` beats a per-title default, so setting `shift_up` there takes the paddles
away from Offroad.

#### A PlayStation pad

A native PlayStation pad is neither XInput nor force-feedback, so **WinMM** picks it up and Cross
is button `2`, not `1`. Either run it as an Xbox controller (the only way to get rumble), or bind
it on the launcher's *Controls* page, or merge this in (community-reported numbers, untested here):

```ini
[gamepad]
boost      = 2    ; Cross
view_high  = 3    ; Circle
view_low   = 1    ; Square
view_pilot = 4    ; Triangle
coin1      = 9    ; Share / Create
start1     = 10   ; Options

[offroad.gamepad]
shift_down = 5    ; L1
shift_up   = 6    ; R1
```

### `[link]`

`link` (`off`) and `link_rx_budget` (`64`, 1..4096, datagrams per frame): §7.

### `[diagnostics]`

| Key | Default | Values |
|---|---|---|
| `log_file` | `vcthunder.log` | read only from the plain section (§1) |
| `log_level` | `info` | `error` \| `warn` \| `info` \| `trace` |
| `stall_ms` | `60` | frame-stall threshold, for the log only |
| `input_trace` | `false` | log every axis and button (§4.1) |

Everything else here is a developer switch: §6.
