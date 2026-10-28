# Changelog

## 0.4

### Arctic Thunder

- **The third title.** Import it from MAME's `arctthnd` CHD like the other two; it runs from
  the launcher or as `VCThunder.exe arctic`.
- **Widescreen, HUD at the edges and pad rumble**.

### Ultimate Arctic Thunder

- **The fourth title.** Import its update CD (MAME's `040503_1309` CHD, or a `.cue`/`.bin`); it is
  laid over Arctic Thunder. It has its own Play card and needs no cabinet key.
- **Widescreen, HUD at the edges and pad rumble**.

### Controls

- **Select screens from the keyboard and a pad.** The arrow keys, the D-pad, a stick and LB/RB
  move one item a press on the track, boat, truck, driver and initials screens, and the choice
  stays put. A wheel takes the screen back when turned. `[analog] menu_wheel = off` restores the
  old behaviour. See `docs/configuration.md` §4.1.

### Launcher

- **Graphics page**: a table shows what each game gets.
- **Force feedback and rumble per game**: the *Force Feedback* page has the same game row as
  *Controls*, and each game keeps its own strength and rumble settings.
- **Works with the keyboard and a controller.** Tab reaches every control on every page; the
  D-pad moves, A presses, B goes back, LB/RB change page and Start plays the selected game.
  Escape closes the launcher. See `docs/configuration.md` §1.1.

### Performance

- **Processors with performance and efficiency cores** (Intel 12th generation and later): the game
  runs on the performance cores, automatically.
- **The CPU renderer uses only the cores it is allowed**, so `start /affinity` no longer slows it
  down.

## 0.3

### Download and play

- **A ready-to-run download.** Unzip the release and run `VCThunder.exe`; nothing to compile.
- **CHD Import**: press **Import...**, drop chd files onto `VCThunder.exe`, or leave them beside it.
- **Building from source no longer needs the games**: the address tables are in the repository.

### Graphics

- **60 fps.** `frame_rate = 60` draws a picture between each two of the game's own.
- **A tenth of the processor time.** The game sleeps between ticks instead of spinning.
- **Sharper scaling**: HUD and menus are drawn at 1x and scaled up in one pass (`upscale_2d`),
  and stretched 3D textures are sharper (`texmag`).
- **Bezel art.** The Graphics page's Bezel box takes a PNG with a transparent screen opening.
- **The widescreen HUD can sit at the screen's edges** (`widescreen_hud = edges`).
- **The window opens at the largest size that fits the screen.**
- **Hydro's race map shows the land**, where only the boats drew.
- **With the CPU renderer, `aspect = 4:3` keeps the window 4:3.**

### Sound

- **`output = front` is gone**: the balance slider does it.

### Launcher

- **Five pages in plain words**: the rest appear with *Show advanced settings*.
- **Sliders** for volume, balance, force-feedback strength and the rumble trims.
- **Each game's Play card says "Ready to play." or says why it's not.**
- **Help has Restore default settings, Show the log files and an About box.**
- **Settings that can stop a game working are now advanced mode**

### Controllers

- **Two slots per control** instead of Add and Clear: click a slot and press, and only that slot
  changes. **Reset to defaults** puts every control back.
- **A different controller for each game**: tick *Separate controller per game*.
- **Started from a console window, VCThunder no longer takes the console for the game window.**

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

## 0.1

First public release, 2026-09-04.
