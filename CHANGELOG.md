# Changelog

## 0.2

### Controllers

- **Several controllers at once.** Steering, gas and brake can each come from a different
  controller: a wheel with separate pedals, or a pad's triggers as pedals.
- **Xbox pads work beside DirectInput**, and a pad is listed once instead of twice.
- **Wheel axes are read correctly.** An axis is read where its data actually lands, not where the
  driver says it is. Confirmed on a Logitech G29, a Thrustmaster TMX and a Fanatec.
- **Fanatec wheels that appear as two devices** now open the wheel, not the pedals.
- **Offroad's gear shift works on a controller out of the box.** It had no default before.
- **Up to 128 buttons per controller**, up from 32.
- **The log says when a controller stops answering**, and when it comes back.

### Launcher

- **One Controls page.** Press a button to bind it (Add/Clear), and each binding names its
  controller. A guided **Set up** finds steering, gas and brake by having you move them.
- **A Force Feedback tab.** Settings are grouped into cards, with one Advanced switch.
- **Only deliberate changes are saved.** Trying a controller writes nothing, and Apply writes only
  what changed.
- **Graphics:** the resolution scale is a dropdown that names the resolution it gives. When Play
  is greyed out, the page says why.

### Force feedback and rumble

- **Pad rumble is on by default**, driven by the games' own force-feedback effects (impacts, road
  surface, engine) rather than by the raw wheel force.
- **Boost and nitro are felt on the pad:** a spark when it catches, then a buzz while it runs.
  Pressing with an empty tank does nothing, as in the game. `rumble_boost_percent` sets the
  strength.
- **Each game's control loop runs at its own rate**, Hydro 30 Hz and Offroad 62.5 Hz. Both ran at
  half that, which halved the frequency of Offroad's wheel effects.
- **Pad motors no longer stutter on and off.**

### Graphics

- **Hydro's lens flare shows with the GPU renderer.**
- **A GPU hang at render scale 7x and 8x is fixed.**
- **Hydro's boat select is letterboxed in widescreen**, so the 3D world no longer shows beside its
  frame.
- **No more line along the bottom of the screen during Hydro's fades.** Triangle filling now
  follows Glide's rule for edges that fall exactly on a pixel.

### Audio

- **Hydro's race music no longer stops after one play.**
- **No more burst of loud music in attract mode.** The sound timer ticks at the cabinet's 5.4 ms
  instead of 5 ms.

### Stability and setup

- **A crash while loading textures is fixed.** It came from VCThunder's own file-name cache,
  which is removed.
- **The default settings are built into the program**, so a release ships no `vcthunder.ini`.
- **A warning when the pack is on a WSL path** (`\\wsl.localhost\...`), where Windows cannot read
  the ini and controller and force-feedback settings silently fell back to their defaults.
- **`make pack` copies the game data into the pack.**

### Upgrading from 0.1

These 0.1 settings were removed with no conversion. An ini that still uses them loses them:

| 0.1 | 0.2 |
|---|---|
| `[analog] dinput_device` | `[analog] dinput_name`, or pick the controller on the launcher's Controls page |
| `[audio] dcs_ms` | `[audio] dcs_us`, default `5400` |
| `[ffb] invert` | `[ffb] direction = normal` or `reversed` |

## 0.1

First public release, 2026-09-04.
