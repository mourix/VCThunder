/* rumble_model_test.c -- does the derived pad rumble describe the right thing?
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * src/rumble.c's whole claim is a DISCRIMINATION: a force the game HOLDS
 * through a corner must reach the heavy motor and not the light one, and a
 * force that SHAKES must reach the light one and not the heavy one. Neither
 * half can be read off a screenshot and a hand on a pad cannot tell "the model
 * works" from "the motors are on", so the arithmetic is checked here on a
 * pinned timeline. No game data, no dump, no profile: rumble.c is included
 * whole so the test can drive rumble_step() directly.
 */
#include <direct.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/rumble.c"

/* ---- the collaborators rumble.c reaches for ----------------------------- */

shim_config_t g_cfg;
/* A profile with only an id: ini_read_scoped's scoped lookup uses it, and
 * rumble.c names the title when it reports which source it took. Every anchor
 * stays 0, which is the "this title has no force-feedback cells" case and is
 * what makes the DAC-path cases below the fallback path they are testing. */
static const game_profile_t k_profile = { .id = "test" };
const game_profile_t *g_game = &k_profile;
log_level_t g_log_level = LOG_ERR;

static int s_show_log;

void log_printf(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    if (!s_show_log)
        return;
    (void)level;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static controls_config_t s_controls;
const controls_config_t *io_config(void) { return &s_controls; }

static int s_active = 1;
int io_host_active(void) { return s_active; }

/* The wheel, as di_preload() would have left it. rumble.c asks the DEVICE THAT
 * ANSWERED rather than the configured provider name, because the shipped
 * provider is `auto` and a named one is not what a wheel arrives as. */
static int s_wheel;
int di_present(void) { return s_wheel; }
const char *di_device_name(void) { return "a force-feedback wheel"; }

/* The game's own force-feedback channels, as ffb_events.c would have supplied
 * them. `have` 0 is a title with no anchors, which is every case here except
 * the four at the end: those are the ones that test the event path. */
static ffb_event_state_t g_ev;
int ffb_events_take(ffb_event_state_t *out)
{
    *out = g_ev;
    g_ev.impact = 0.0f;                 /* one-shot, exactly as the real one */
    return g_ev.have;
}

/* The motors, as a device would have received them.
 *
 * `g_no_sink` is the machine with no XInput device on it. io_host_rumble()
 * answers 0 there for every command; the rumble sink is XInput and only
 * XInput, and the whole model still runs. Case 38 is about what the census
 * says in that state, which for a long time was the opposite of the truth. */
static unsigned g_low, g_high, g_writes;
static int      g_no_sink;
int io_host_rumble(unsigned low_motor, unsigned high_motor)
{
    if (g_no_sink)
        return 0;
    g_low = low_motor;
    g_high = high_motor;
    g_writes++;
    return 1;
}

/* ---- the test ----------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s (low=%u high=%u)\n", what, g_low, g_high);
        failures++;
    }
}

/* Full scale as a percentage, so an expectation reads in the units the ini is
 * written in rather than in sixteenths of a motor. */
static unsigned pct(unsigned motor)
{
    return (unsigned)((float)motor * 100.0f / RUMBLE_FULL + 0.5f);
}

/* A 15.6 MS TIMER'S GRAIN against a caller that wants 33.333 ms: 31, 31, 31,
 * 32, 31, 31, 31, 47 ... averaging 33.3. Only case 18 needs it, the rest of
 * this file pins levels and releases, where a tidy 33 is the clearer
 * instrument. */
static unsigned tick_dt(unsigned frame)
{
    double now  = (double)(frame + 1) * 33.3333;
    double prev = (double)frame * 33.3333;

    return (unsigned)((unsigned long)(now / 15.625) * 15.625) -
           (unsigned)((unsigned long)(prev / 15.625) * 15.625);
}

/* Hold a force for a while at the board's own cadence (~30 Hz). */
static void hold(float f, unsigned ms)
{
    unsigned t;

    for (t = 0; t < ms; t += 33)
        rumble_step(f, 33);
}

/* Wind a force on the way a corner is wound on, rather than stepping it. A
 * step is a transient by definition and both motors are right to answer one;
 * what separates them is RATE, so the case that claims the split has to make
 * the change slow. */
static void ramp(float from, float to, unsigned ms)
{
    unsigned t;

    for (t = 0; t < ms; t += 33)
        rumble_step(from + (to - from) * (float)t / (float)ms, 33);
}

/* A byte the game HOLDS: a large steady level with a little wobble on it,
 * which is what a menu selection and a corner both look like and what hold()
 * deliberately is not. The timeline is a function of the elapsed time alone,
 * so two runs of it are the same signal and can be read against each other. */
static void hold_loaded(unsigned ms)
{
    unsigned t;

    for (t = 0; t < ms; t += 33)
        rumble_step(0.7f + 0.05f *
                    (float)sin(6.2831853 * (double)t / 200.0), 33);
}

static void write_ini(const char *name, const char *body)
{
    FILE *f = fopen(name, "wb");

    if (!f) {
        fprintf(stderr, "FAIL: cannot write %s\n", name);
        exit(1);
    }
    fputs(body, f);
    fclose(f);
}

/* Start a case: a fresh ini, a fresh model, and no motor history.
 *
 * AND HALF A SECOND OF RESTING DAC, which is where a race starts. The filters
 * seed on their first sample, so a force applied to a COLD model reads as "it
 * was always like that" and carries no arrival at all, correct behaviour
 * (it is what stops a phantom transient when the window regains focus mid
 * corner) and a trap for a test that skips the rest. */
static char s_folder[MAX_PATH];

static void remove_folder(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("rumble-test*.ini", &fd);

    if (h != INVALID_HANDLE_VALUE) {
        do
            DeleteFileA(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    _chdir("..");
    RemoveDirectoryA(s_folder);
}

static void begin(const char *name, const char *body)
{
    write_ini(name, body);
    g_low = g_high = g_writes = 0;
    g_no_sink = 0;
    s_active = 1;
    s_wheel = 0;
    memset(&g_ev, 0, sizeof g_ev);
    rumble_preload(name);
    hold(0.0f, 500);
}

/* The shaping off: spring subtraction keeps nothing back, no boost, no grain.
 * Every case that predicts an exact motor level uses this, because a learned
 * spring and a decaying peak are exactly the two things that make a level
 * depend on what came before it. The four cases at the end turn them on one
 * at a time, which is the only way to say what each one does. */
#define INI_ON \
    "[ffb]\n" \
    "enabled = true\n" \
    "rumble = true\n" \
    "rumble_strength_percent = 100\n" \
    "rumble_sustain_percent = 100\n" \
    "rumble_impact_percent = 100\n" \
    "rumble_spring_percent = 0\n" \
    "rumble_agc_percent = 100\n"

int main(int argc, char **argv)
{
    char root[MAX_PATH];

    s_show_log = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v';
    /* Case 1 hands rumble_preload a BARE filename, which is the whole point of
     * it, so the working directory has to be one this test may write to. It
     * is under %TEMP% rather than beside the exe because the profile API
     * cannot read an ini on a WSL path, where a tree built under WSL lives. */
    GetTempPathA(sizeof root, root);
    snprintf(s_folder, sizeof s_folder, "%.*sVCThunder-rumble-%lu",
             (int)(sizeof s_folder - sizeof "VCThunder-rumble-" - 12),
             root, GetCurrentProcessId());
    CreateDirectoryA(s_folder, NULL);
    if (_chdir(s_folder) != 0) {
        fprintf(stderr, "FAIL: cannot enter %s\n", s_folder);
        return 1;
    }
    atexit(remove_folder);

    /* 1. A bare filename must still be read. The profile API resolves a bare
     *    name against the Windows directory, so `rumble = true` reads as false
     *    and the module stays silent with the file plainly saying otherwise. */
    begin("rumble-test.ini", INI_ON);
    check(rumble_active(), "a bare ini filename is resolved, not defaulted");
    /* Since `rumble` defaults ON, the check above passes against an unread
     * file too. Only the OFF spelling still tells a resolved path from a
     * defaulted one, so it is the half that carries case 1 now. */
    begin("rumble-test.ini", "[ffb]\nenabled = true\nrumble = false\n");
    check(!rumble_active(), "a bare ini filename's `rumble = false` is honoured");
    /* And a file that does not mention the key gets the pad model. */
    begin("rumble-test.ini", "[ffb]\nenabled = true\n");
    check(rumble_active(), "`rumble` absent defaults to on");

    /* 2. `enabled = false` turns off BOTH kinds of force feedback. */
    begin("rumble-test-off.ini",
          "[ffb]\nenabled = false\nrumble = true\n");
    check(!rumble_active(), "[ffb] enabled = false silences the pad too");

    /* 3. A wheel never gets the derived effect, whatever the ini says. */
    s_controls.provider = PROVIDER_DINPUT;
    begin("rumble-test.ini", INI_ON);
    check(!rumble_active(), "a DirectInput wheel is refused the derived effect");
    s_controls.provider = PROVIDER_XINPUT;

    /* 4. A resting DAC is a resting pad: no force, no motors, and no traffic
     *    to the driver at all. */
    begin("rumble-test.ini", INI_ON);
    hold(0.0f, 2000);
    check(g_low == 0 && g_high == 0, "a resting DAC leaves both motors still");
    /* One write, not none: the first push asserts rest on a pad that may have
     * been left buzzing by the launcher's test pulse or a previous process.
     * After that a still DAC costs the driver nothing, which matters because
     * this runs on the frame-boundary thread. */
    check(g_writes == 1, "a resting DAC is asserted once and then left alone");

    /* 5. A force WOUND ON is the heavy motor's, and the light one must not
     *    answer it: this is the discrimination the module exists for. Two
     *    seconds is a corner, not an impact, and note that the claim is
     *    about the RATE, not the level. Where the level goes once it stops
     *    changing is case 13. */
    begin("rumble-test.ini", INI_ON);
    ramp(0.0f, 0.5f, 2000);
    check(pct(g_low) >= 10, "a force wound on over two seconds reaches the low motor");
    /* Overwhelmingly, not absolutely. "high == 0" was true only because the
     * motor floor gated the ramp's small residue away, so lowering that floor
     * broke it, a test that passes because of a gate is a test of the gate.
     * The claim the split actually makes is about proportion. */
    check(g_high * 3 < g_low, "and goes overwhelmingly to the low motor");

    /* 6. A SHAKING force is the light motor's, and the heavy one must not
     *    answer it. The mean of +-0.4 is zero, so a model that only measured
     *    magnitude would fail this and a model that only measured the mean
     *    would fail case 5. */
    begin("rumble-test.ini", INI_ON);
    {
        unsigned t;
        for (t = 0; t < 1000; t += 33)
            rumble_step((t / 33) % 2 ? 0.4f : -0.4f, 33);
    }
    check(pct(g_high) >= 25, "an alternating force reaches the high motor");
    check(g_low * 2 < g_high,
          "an alternating force goes mostly to the high motor");

    /* 7. A single frame of force is still felt: the attack is shorter than one
     *    33 ms sample and the release is longer than several, because a mass
     *    that has not spun up has not been told anything. */
    begin("rumble-test.ini", INI_ON);
    hold(0.0f, 500);
    rumble_step(0.8f, 33);
    check(pct(g_high) >= 20, "one frame of force reaches the high motor");
    {
        unsigned after_hit = g_high;
        hold(0.0f, 66);
        check(g_high > after_hit / 4, "a one-frame jolt outlives the frame");
    }

    /* 8. And it stops. Whatever the history, a DAC that goes quiet leaves the
     *    motors at rest, and the write that stops them is not optional, so
     *    it must survive the change threshold. */
    begin("rumble-test.ini", INI_ON);
    hold(0.6f, 500);
    check(g_low > 0, "the corner is felt before the release is checked");
    hold(0.0f, 3000);
    check(g_low == 0 && g_high == 0, "a DAC that goes quiet stops both motors");

    /* 9. Below the motors' own floor a command is only a buzz, so it is not
     *    sent at all. */
    begin("rumble-test.ini", INI_ON);
    hold(0.02f, 1000);
    check(g_low == 0 && g_high == 0, "a force under the motor floor is dropped");

    /* 10. sustain = 0 is a supported taste, not a broken configuration:
     *     impacts only, and silence in a long corner. */
    begin("rumble-test-impacts.ini",
          "[ffb]\nenabled = true\nrumble = true\n"
          "rumble_sustain_percent = 0\nrumble_impact_percent = 100\n"
          "rumble_spring_percent = 0\nrumble_agc_percent = 100\n");
    hold(0.6f, 1000);
    check(g_low == 0, "sustain = 0 leaves a held corner silent");
    rumble_step(0.0f, 33);
    check(g_high > 0, "sustain = 0 still reports the moment the force drops");

    /* 11. Losing focus is nobody holding the pad, and the model forgets: a
     *     resume must not read as a step against a sample minutes old. */
    begin("rumble-test.ini", INI_ON);
    hold(0.6f, 500);
    s_active = 0;
    rumble_set_force(0.6f, 0.0, 0);
    check(g_low == 0 && g_high == 0, "losing focus stops the motors");
    s_active = 1;
    rumble_step(0.6f, 33);
    check(g_high == 0, "the first sample after a resume is not a transient");

    /* 12. A gap is a gap, not a step: a frame the machine took five seconds
     *     over must not integrate as one interval. */
    begin("rumble-test.ini", INI_ON);
    hold(0.0f, 500);
    rumble_step(1.0f, 5000);
    check(g_low <= RUMBLE_FULL && g_high <= RUMBLE_FULL,
          "a five-second interval stays inside full scale");

    /* 13. A HELD force falls silent. Most of the force is a spring on the wheel
     *     position; a pad has no loop to feel a spring in. Nothing here tells
     *     the model what the steering was doing, which is the point. */
    begin("rumble-test.ini", INI_ON);
    hold(0.5f, 3000);
    check(g_low == 0 && g_high == 0,
          "a force held for three seconds leaves both motors still");
    /* Longer, because this one is a step of 1.4 full-scale units rather than
     * 0.5, and the 700 ms pole takes about four of its time constants to put
     * that under the floor's gate. "Falls silent in about a second" is the
     * claim for a corner, not for a lock-to-lock reversal. */
    hold(-0.9f, 5000);
    check(g_low == 0 && g_high == 0,
          "and so does a bigger one, at the other sign");

    /* 14. And the road on top of it is still felt. A model that removed the
     *     held level by simply going quiet would pass 13 and fail here. */
    {
        unsigned quiet = g_high;
        rumble_step(-0.9f + 0.5f, 33);
        check(g_high > quiet, "an impact inside a held force is still felt");
    }

    /* 15. The silence is not instant: a corner has a body, and the force
     *     arriving has to be felt before it settles out. */
    begin("rumble-test.ini", INI_ON);
    hold(0.6f, 400);
    check(g_low > 0, "a force that has just arrived has not gone quiet yet");

    /* 16. rumble_spring_percent adds a little of the held level back on
     *     purpose, for a driver who wants the corner present under the road.
     *     Same held force as 13, and now it must NOT be silent. */
    begin("rumble-test-spring.ini",
          "[ffb]\nenabled = true\nrumble = true\n"
          "rumble_spring_percent = 60\nrumble_agc_percent = 100\n");
    hold(0.5f, 3000);
    check(g_low > 0, "keeping 60% of the held level leaves the corner present");

    /* 17. What is left after the held level is removed is narrow, so it is
     *     normalised. The same small shaking force reaches the motors harder
     *     with the boost on, and a big one is not scaled DOWN by it;
     *     attenuating a hit would take the size off exactly the events worth
     *     feeling. */
    {
        unsigned quiet_off, quiet_on, loud_off, loud_on;
        unsigned t;

        begin("rumble-test.ini", INI_ON);
        for (t = 0; t < 2000; t += 33)
            rumble_step((t / 33) % 2 ? 0.06f : -0.06f, 33);
        quiet_off = g_high;
        begin("rumble-test-agc.ini",
              "[ffb]\nenabled = true\nrumble = true\n"
              "rumble_spring_percent = 0\nrumble_agc_percent = 400\n"
              "rumble_impact_percent = 100\n");
        for (t = 0; t < 2000; t += 33)
            rumble_step((t / 33) % 2 ? 0.06f : -0.06f, 33);
        quiet_on = g_high;
        check(quiet_on > quiet_off * 2,
              "the boost lifts a quiet signal towards the motors' range");

        begin("rumble-test.ini", INI_ON);
        for (t = 0; t < 2000; t += 33)
            rumble_step((t / 33) % 2 ? 0.9f : -0.9f, 33);
        loud_off = g_high;
        begin("rumble-test-agc.ini",
              "[ffb]\nenabled = true\nrumble = true\n"
              "rumble_spring_percent = 0\nrumble_agc_percent = 400\n"
              "rumble_impact_percent = 100\n");
        for (t = 0; t < 2000; t += 33)
            rumble_step((t / 33) % 2 ? 0.9f : -0.9f, 33);
        loud_on = g_high;
        check(loud_on >= loud_off, "and never scales a loud one down");
    }

    /* 18. NO MOTOR IN THIS MODEL IS CHOPPED: a rule, not a statement about
     *     the current tuning (rumble.c). A CONTINUING swell holds the heavy
     *     motor at a level, and every direction change the motor makes must be
     *     one the swell itself made, because a square wave built from `dt`
     *     cannot help showing up as extra ones.
     *
     *     AND THE CLOCK IS NOT A TIDY 33: on a 15.6 ms grain a ~30 Hz caller
     *     hands the model 31, 31, 31, 32, 31, 31, 31, 47 ... The irregularity
     *     is the whole defect. */
    {
        unsigned t, n, turns = 0, prev = 0, held = 0;
        int falling = 0, was_falling = 0;
        /* A 1 Hz swell: continuing, so the heavy motor holds a level, which a
         * held force deliberately would not. */
#define SWELL(t) (0.5f * (float)sin(6.2831853 * (double)(t) / 1000.0))

        begin("rumble-test.ini", INI_ON);
        for (t = 0; t < 4000; t += 33)
            rumble_step(SWELL(t), 33);
        check(g_low > 0, "a continuing swell holds the heavy motor up");

        /* THE EVENTS PATH, and a body channel rather than the byte. On the
         * fallback the heavy motor follows |s_mid - s_slow|, and |sine at f|
         * has its own rhythm at 2f, a recorded trap, so the signal
         * contributes twice the turns there and the bar would be measuring
         * that instead of the chop. `ev.body` arrives as a magnitude the game
         * already normalised, so its envelope turns round exactly when the
         * swell does: four seconds of a 0.5 Hz swell is four turns, and an
         * 8 Hz chop on top would be sixty. */
        begin("rumble-test.ini", INI_ON);
        g_ev.have = 1;
        g_ev.anchored = 1;
        for (t = 0, n = 0; n < 120; n++) {
            unsigned dt = tick_dt(n);

            g_ev.body = (float)(0.55 + 0.25 *
                                sin(6.2831853 * (double)t / 2000.0));
            rumble_step(0.0f, dt);
            t += dt;
            if (n > 30 && g_low != prev) {
                falling = g_low < prev;
                if (falling != was_falling)
                    turns++;
                was_falling = falling;
            }
            if (g_low)
                held++;
            prev = g_low;
        }
        check(held > 100, "and keeps holding it on the host's own clock");
        /* MEASURED 4, and the bar is 8. The same case with an 8 Hz chop
         * spliced back into rumble_step() measures 48, so this discriminates
         * by 12:1 rather than by a margin. */
        check(turns <= 8,
              "no motor is chopped: every turn at the motor is the signal's");
#undef SWELL
    }

    /* 19. A constant, at any level, for as long as you like, is silence. A
     *     model with per-position state fails exactly here; no case in this
     *     file may take a steering position again. */
    {
        float level;
        for (level = -1.0f; level <= 1.0f; level += 0.25f) {
            begin("rumble-test.ini", INI_ON);
            hold(level, 4000);
            check(g_low == 0 && g_high == 0,
                  "a constant force of any level ends in silence");
        }
    }

    /* 20. The shipped gains leave headroom, measured against a real race: the
     *     motors were pinned at 65535 in 10 of 24 windows while the source
     *     ranged 1.7% to 30.6% mean. The ini names only the two switches, so
     *     every gain comes from the compiled default and cannot drift. */
    {
        unsigned typical, loud, t;
#define RACE_MEAN 0.25f      /* mean |force| in the busy windows of the race */
#define RACE_PEAK 0.80f      /* dac -102..102 of 128                         */

        begin("rumble-test-shipped.ini",
              "[ffb]\nenabled = true\nrumble = true\n");
        for (t = 0; t < 3000; t += 33)
            rumble_step((t / 33) % 2 ? RACE_MEAN : -RACE_MEAN, 33);
        typical = g_high;
        check(pct(typical) >= 10 && pct(typical) <= 70,
              "the race's typical level lands mid-range at the shipped gains");

        begin("rumble-test-shipped.ini",
              "[ffb]\nenabled = true\nrumble = true\n");
        for (t = 0; t < 3000; t += 33)
            rumble_step((t / 33) % 2 ? RACE_PEAK : -RACE_PEAK, 33);
        loud = g_high;
        check(loud > typical,
              "and the race's loudest is still louder, not clipped down to it");
#undef RACE_MEAN
#undef RACE_PEAK
    }

    /* 21. The game's own units, where the title has them. The byte is a sum and
     *     no filter un-mixes it; these are typed channels instead. */
    {
        /* An impact arrives already separated from the spring, so it goes in
         * at full size rather than being inferred from a slope. */
        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 1000);
        check(g_high == 0, "no events, no motors");
        g_ev.have = 1;
        g_ev.impact = 0.9f;
        rumble_step(0.0f, 33);
        check(pct(g_high) >= 70, "an impact event reaches the high motor whole");
        check(g_low > 0, "and gives the heavy motor its share of the weight");

        /* ONE-SHOT. A burst that fired twice because two readers took it would
         * be a lie about the game. */
        {
            unsigned after = g_high;
            hold(0.0f, 400);
            check(g_high < after, "and then decays, rather than re-firing");
        }

        /* Texture is continuous and already normalised by the game. */
        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.texture = 0.5f;
        hold(0.0f, 500);
        /* A LEVEL, NOT A NUMBER. The texture curve lifts the quiet end and
         * rumble_texture_percent trims the whole bed, so any absolute
         * expectation here breaks the moment either is retuned. The claims
         * worth pinning are the ones a retune must NOT break. */
        check(g_high > 0, "texture reaches the high motor");
        check(g_low == 0, "and does not reach the heavy motor");

        /* Body is the slow bed and belongs to the heavy motor alone. */
        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.body = 0.6f;
        hold(0.0f, 500);
        /* A level, not a number: the terrain trim governs this now (case 30),
         * so an absolute expectation here would only be a restatement of the
         * default. */
        check(g_low > 0, "body holds the heavy motor");
        check(g_high == 0, "and does not reach the light one");

        /* And a title with no anchors is unaffected: `have` 0 means the DAC
         * path, which is every other case in this file. */
        begin("rumble-test.ini", INI_ON);
        g_ev.have = 0;
        g_ev.impact = 1.0f;
        g_ev.texture = 1.0f;
        hold(0.0f, 1000);
        check(g_low == 0 && g_high == 0,
              "a profile with no event anchors ignores the channels entirely");
    }

    /* 22. (a) With the game's channels live, the DAC alone moves nothing. The
     *     first build combined them with max() and the spring came back. */
    begin("rumble-test.ini", INI_ON);
    hold(0.0f, 500);
    g_ev.have = 1;
    {
        unsigned t;
        /* Spring-shaped means SLOW: the force follows a hand, and a driver
         * waggling the wheel on the spot manages a few hertz. 2 Hz here. The
         * engine channel is separated from this by frequency, not by kind, so
         * the test has to use the frequency a hand actually produces. */
        for (t = 0; t < 3000; t += 33)
            rumble_step(0.9f * (float)sin(6.2831853 * (double)t / 500.0), 33);
    }
    check(g_low == 0 && g_high == 0,
          "with the game's channels live, hand-speed steering moves nothing");

    /*     (b) A CONSTANT CHANNEL VALUE IS REST, WHATEVER ITS MAGNITUDE. The
     *         suspension cells sit at about -10 at rest, so an absolute
     *         magnitude read ~1.0 for ever and pinned the heavy motor while
     *         parked. Here the channel is already normalised by ffb_events.c,
     *         so the test states the contract it must deliver: a value that is
     *         not changing is silence. */
    begin("rumble-test.ini", INI_ON);
    hold(0.0f, 500);
    g_ev.have = 1;
    g_ev.body = 0.0f;                   /* a baselined channel at rest */
    g_ev.texture = 0.0f;
    hold(0.0f, 3000);
    check(g_low == 0 && g_high == 0,
          "channels resting leave both motors still, however long you wait");

    /* 23. AND THE FAST HALF OF THE SAME BYTE MUST STILL GET THROUGH. Hydro's
     *     MOTOR unit is a table-driven oscillator and its BOOSTER/FWDCAV/REVCAV
     *     units are RandomFloat flipped on frame parity; none is stored in any
     *     cell, so the engine and the boost reached the pad not at all and a
     *     race reported "static racing has zero rumble". They are fast by
     *     construction and the spring is slow by construction, which is the
     *     whole separation, so an alternation at the frame rate must pass
     *     where case 22's 2 Hz sweep did not. ffb_events.c derives this
     *     channel; here it arrives already extracted. */
    begin("rumble-test.ini", INI_ON);
    hold(0.0f, 500);
    g_ev.have = 1;
    g_ev.engine = 0.5f;
    hold(0.0f, 500);
    check(g_high > 0, "the engine channel reaches the high motor");
    check(g_low == 0, "and not the heavy one");

    /* 24. The texture curve. Dirt was overwhelming and tarmac silent: the same
     *     channel, a range wider than two motors can render. A quiet reading
     *     must land somewhere a hand can feel, and a loud one must not have
     *     been flattened to reach it. */
    {
        unsigned quiet, loud;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.texture = 0.10f;
        hold(0.0f, 500);
        quiet = g_high;
        /* Above the motors' own floor is the whole claim: below it an
         * eccentric mass does not turn at all, which is what "tarmac gave
         * nothing" was. */
        check(quiet > 0, "a quiet surface clears the motor floor");

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.texture = 0.90f;
        hold(0.0f, 500);
        loud = g_high;
        /* THE ROUND TRIP IS THE POINT. Companding the whole bed onto
         * [MOTOR_MIN_SPIN, 1] is an affine lift, and an affine lift compresses
         * every ratio it touches, including this one, at the top of the
         * range, where nothing needed lifting. The model that replaced it
         * spends the mass's start level only on BREAKING AWAY, so a level
         * already above it is untouched and this case measures what it did
         * before any of it. A bar that has to move for a change to the BOTTOM
         * of the range is a bar reporting the change did not stay there. */
        check(loud > quiet * 2, "and a rough one is still much rougher");
    }

    /* 25. AND A HIT MUST STAND OUT FROM THE GROUND IT HAPPENS ON. Both reach
     *     the light motor, so a shared gain makes the terrain too strong while
     *     the nitro underneath it cannot be picked out. The bed is trimmed
     *     below the impacts, and this is the claim that trim exists to make:
     *     the SAME number, as an impact, is louder than it is as surface. */
    {
        unsigned as_bed, as_hit;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.texture = 0.6f;
        hold(0.0f, 500);
        as_bed = g_high;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.impact = 0.6f;
        rumble_step(0.0f, 33);
        as_hit = g_high;

        check(as_hit > as_bed,
              "0.6 as an impact is louder than 0.6 as surface");
    }

    /* 26. The bed is a level and nothing takes it away. Ducking it under an
     *     impact was tried and withdrawn; the gaps were not wanted. */
    {
        unsigned steady, after;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.texture = 0.8f;
        hold(0.0f, 1000);
        steady = g_high;
        check(steady > 0, "the surface is felt");

        g_ev.impact = 1.0f;
        rumble_step(0.0f, 33);
        hold(0.0f, 990);                /* well past the hit's own release */
        after = g_high;
        check(after >= steady - RUMBLE_STEP,
              "and an impact leaves it where it was, rather than punching a "
              "hole in it");
    }

    /* 27. Having channels is not the same as them meaning anything. Offroad's
     *     own path is gated on a mode cell and is off in the menus, where the
     *     byte still carries a per-selection force. */
    {
        unsigned t;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        /* Channels present but the game's path down, exactly as a menu. */
        g_ev.anchored = 1;
        g_ev.have = 0;
        for (t = 0; t < 1500; t += 33)
            rumble_step(0.6f * (float)sin(6.2831853 * (double)t / 700.0), 33);
        check(g_low > 0 || g_high > 0,
              "with the game's own path down, the byte is rendered again");
    }

    /* 28. The fallback is capped: the events path runs through a per-title trim
     *     and the fallback renders the byte, so Offroad's menus came out three
     *     to four times Hydro's. A ceiling, not a scale. */
    {
        unsigned t, loud, gentle;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.anchored = 1;              /* has cells; its path is just down */
        g_ev.have = 0;
        for (t = 0; t < 2000; t += 33)
            rumble_step(0.9f * (float)sin(6.2831853 * (double)t / 700.0), 33);
        loud = g_high;
        check(pct(loud) <= 11, "a shouting fallback is held at the ceiling");

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.anchored = 1;
        g_ev.have = 0;
        gentle = 0;
        for (t = 0; t < 2000; t += 33) {
            rumble_step(0.06f * (float)sin(6.2831853 * (double)t / 700.0), 33);
            /* The PEAK over the window, not one sample: a gentle level is
             * below what a mass will start on and is therefore delivered as
             * pulses, so any single reading may land in a gap. */
            if (g_high > gentle)
                gentle = g_high;
        }
        check(gentle > 0 && gentle < loud,
              "and a gentle one keeps its own level, below it");
    }

    /* 29. (a) A mass will not hold a level below about 5%. A sub-threshold bed
     *     is COMPANDED onto the range the mass has, never delivered as pulses:
     *     the model is commanded at ~30 Hz, so a 12 Hz square gets two and a
     *     half commands per cycle and no duty cycle at that rate is a level;
     *     it is a knock (rumble.c). What this case pins is that the bed arrives
     *     as a level and never lets go of the motor; `seen_off` is the defect. */
    {
        unsigned t, seen_off = 0, seen_on = 0;

        /* The threshold is about the MOTOR level, not the channel value, so
         * the trim has to be the one that puts it there; these are the
         * numbers the user's own pack runs. */
        begin("rumble-test-quiet.ini",
              "[ffb]\nenabled = true\nrumble = true\n"
              "rumble_texture_percent = 4\n"
              "rumble_texture_curve_percent = 12\n"
              "rumble_agc_percent = 100\nrumble_spring_percent = 0\n");
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.texture = 0.03f;           /* lands near 2.6%: no mass starts there */
        /* THE ENVELOPE FIRST. s_env_high has a 20 ms attack and the bed gate is
         * shut until it arrives, so the first sample or two after a surface
         * appears are legitimately at rest; that is the bed starting, not the
         * bed chopping. The claim being pinned is about the level it SETTLES
         * to, so the count starts once it has. Counting from the transition is
         * what the old case did, and it could afford to: it required gaps. */
        hold(0.0f, 200);
        for (t = 0; t < 1000; t += 33) {
            rumble_step(0.0f, 33);
            if (g_high == 0)            seen_off++;
            if (pct(g_high) >= 2)       seen_on++;
        }
        /* THE BAR IS 2%, NOT 4%. 4% is half of MOTOR_MIN_SPIN and only start
         * pulses would meet it; this bed settles at its own 2.6%, which is what
         * the trim asks for. A quiet surface must not be inflated to the one
         * level a mass can start at. */
        check(seen_on > 0, "a sub-threshold surface is delivered at all");
        check(seen_off == 0,
              "as a LEVEL the mass can hold, never chopped back to rest");
        /* And it really is sub-threshold: the case is worthless if the trims
         * have drifted to somewhere the mass could have started on its own. */
        check((float)g_high / RUMBLE_FULL < MOTOR_MIN_SPIN,
              "and it is still below what the mass could have started at");
    }

    /*     (a2) AND IT STILL STOPS. The compander maps everything above the gate
     *          to at least the mass's start level, so RUMBLE_FLOOR, which is
     *          applied to the motor after the trims, cannot be what
     *          silences the bed. The engine channel's release is ~1500 ms and
     *          decays towards zero without reaching it, so without a gate of
     *          its own the pad would hum for ever after the last engine sample.
     *          This is that gate. */
    {
        unsigned t, ran = 0;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.engine = 0.5f;
        hold(0.0f, 500);
        check(g_high > 0, "an engine bed runs");
        /* Cut it off and give the envelope a second to fall. */
        g_ev.engine = 0.0f;
        for (t = 0; t < 2000; t += 33) {
            rumble_step(0.0f, 33);
            ran = g_high;
        }
        check(ran == 0, "and stops when the engine does, rather than humming on");
    }

    /*     (b) It cannot spin UP inside 120 ms, so an impact that decayed with
     *         the bed was gone before the mass got there, a 25:1 command
     *         ratio that could not be felt. The hit has its own, longer
     *         release, and must still be up well after the bed's has passed. */
    {
        unsigned at_120, at_250;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.impact = 1.0f;
        rumble_step(0.0f, 33);
        hold(0.0f, 99);
        at_120 = g_high;
        hold(0.0f, 132);
        at_250 = g_high;
        check(pct(at_250) >= 30,
              "an impact is still commanding the motor a quarter-second later");
        check(at_250 < at_120, "and is decaying, not held");
    }

    /* 30. The terrain trim governs BOTH motors. It shaped only the light one
     *     while the suspension drove the heavy one at full scale, so every
     *     adjustment moved a number the driver could barely hear. */
    {
        unsigned loud_trim, quiet_trim;

        begin("rumble-test-bed100.ini",
              "[ffb]\nenabled = true\nrumble = true\n"
              "rumble_texture_percent = 100\nrumble_agc_percent = 100\n"
              "rumble_spring_percent = 0\n");
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.body = 1.0f;
        hold(0.0f, 1000);
        loud_trim = g_low;

        begin("rumble-test-bed10.ini",
              "[ffb]\nenabled = true\nrumble = true\n"
              "rumble_texture_percent = 10\nrumble_agc_percent = 100\n"
              "rumble_spring_percent = 0\n");
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.body = 1.0f;
        hold(0.0f, 1000);
        quiet_trim = g_low;

        check(loud_trim > 0, "the suspension reaches the heavy motor");
        check(quiet_trim * 4 < loud_trim,
              "and the terrain trim governs it, not only the light motor");
    }

    /* 31. The impact envelope decays on BOTH paths. It was decayed only inside
     *     the events branch while being consulted after both, so a title
     *     sitting on the fallback held its last crash for ever. */
    {
        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.impact = 1.0f;
        rumble_step(0.0f, 33);
        check(g_high > 0, "the impact lands");
        g_ev.anchored = 1;
        g_ev.have = 0;                  /* the game's path goes down */
        hold(0.0f, 3000);
        check(g_high == 0, "and does not survive the switch to the fallback");
    }

    /* 32. There is no boost channel. The boat's booster state is availability,
     *     not activity, so it must move nothing. */
    {
        unsigned before, after;
        int state;

        begin("rumble-test.ini", INI_ON);
        hold(0.0f, 500);
        g_ev.have = 1;
        g_ev.engine = 0.05f;
        hold(0.0f, 500);
        before = g_high;
        for (state = 0; state <= 3; state++) {
            g_ev.boost = state;
            hold(0.0f, 330);
            after = g_high;
            check(after == before, "no boost state moves the motors");
        }
    }

    /* 33. THE HANDOVER TO THE FALLBACK CARRIES NO STEP OF ITS OWN. An events
     *     branch that forces s_slow to 0 and never advances s_mid leaves the
     *     fallback resuming from rest, reading a HELD byte as a step from
     *     nothing: the heavy motor then pins at FALLBACK_CEIL flat for
     *     1.98 s where the same byte with no path switch gives 0 throughout.
     *     Every race ends with that transition. Case 31 crosses the same
     *     boundary with the byte at REST, where both filters are already
     *     where they belong and nothing shows. */
    {
        unsigned switched, control;

        begin("rumble-test.ini", INI_ON);
        g_ev.anchored = 1;
        g_ev.have = 1;
        g_ev.texture = 0.3f;
        g_ev.body = 0.2f;
        hold_loaded(3000);
        g_ev.have = 0;                  /* _ffbnew_curMode 1 -> 0, the menus */
        hold_loaded(990);
        switched = g_low;

        /* The control is the same byte for the same time with no switch in it,
         * which is the only thing that separates a stale filter from a signal:
         * a held level has no AC and must reach neither motor either way. */
        begin("rumble-test.ini", INI_ON);
        g_ev.anchored = 1;
        g_ev.have = 0;
        hold_loaded(3000);
        hold_loaded(990);
        control = g_low;

        check(switched <= control + RUMBLE_STEP,
              "the switch to the fallback carries no step of its own");
    }

    /* 34. `rumble_events = false` puts the pad on the byte PERMANENTLY, and
     *     that is the anchor-less case rather than the menu one: capping a
     *     title the driver deliberately moved onto the byte would hold it at
     *     a menu's level for the whole run. The branch that decides read
     *     `ev.anchored` UNINITIALISED to answer it, because the short circuit
     *     had left ffb_events_take() uncalled, so this case was decided off
     *     the stack. gcc says so at -O2 and cannot at the -O1 the build uses,
     *     and no case here had ever turned the key off. */
    {
        unsigned t, loud = 0;

        begin("rumble-test-dac.ini",
              "[ffb]\n"
              "enabled = true\n"
              "rumble = true\n"
              "rumble_events = false\n"
              "rumble_strength_percent = 100\n"
              "rumble_sustain_percent = 100\n"
              "rumble_impact_percent = 100\n"
              "rumble_spring_percent = 0\n"
              "rumble_agc_percent = 100\n");
        g_ev.anchored = 1;              /* the title HAS its own cells ... */
        g_ev.have = 1;                  /* ... and its own path is up */
        for (t = 0; t < 2000; t += 33) {
            rumble_step(0.9f * (float)sin(6.2831853 * (double)t / 700.0), 33);
            if (g_high > loud)
                loud = g_high;
        }
        /* Case 28 holds the same 0.9 signal at 11% when the ceiling applies. */
        check(pct(loud) > 11,
              "rumble_events = false renders the byte at its own range, not "
              "at the menu ceiling");
    }

    /* 35. A PULSE REACHES THE MASS'S START LEVEL WHATEVER THE TRIMS ARE.
     *     MOTOR_MIN_SPIN is a fact about the motor, so it is spent in the
     *     units the motor is commanded in; it was compared and emitted BEFORE
     *     scale_motor()'s two trims. At `rumble_impact_percent = 50` the
     *     pulses arrived at half of it; the one level pulsing exists to
     *     avoid, and at 20% they fell under RUMBLE_FLOOR and the bed
     *     vanished instead of pulsing. RUMBLE_FLOOR was already on the right
     *     side of the trims; this was the only constant that was not. */
    {
        unsigned t, top = 0;

        begin("rumble-test-half.ini",
              "[ffb]\n"
              "enabled = true\n"
              "rumble = true\n"
              "rumble_strength_percent = 100\n"
              "rumble_sustain_percent = 100\n"
              "rumble_impact_percent = 50\n"
              "rumble_spring_percent = 0\n"
              "rumble_agc_percent = 100\n");
        g_ev.have = 1;
        g_ev.texture = 0.03f;           /* under the mass at this trim */
        for (t = 0; t < 2000; t += 33) {
            rumble_step(0.0f, 33);
            if (g_high > top)
                top = g_high;
        }
        check(top + RUMBLE_STEP >= (unsigned)(MOTOR_MIN_SPIN * RUMBLE_FULL),
              "a pulse reaches the mass's start level whatever the trim");
    }

    /* 36. A SAMPLE THIS MODEL DISCARDS MUST NOT SPEND THE IMPACT LATCH.
     *     ffb_events_take() consumes the impact by READING it, and it was
     *     taken at the top of rumble_step(), ahead of the seeding return and
     *     ahead of the `dt == 0` return, so a collision arriving on either
     *     was gone without ever reaching a motor. The clock does stand still:
     *     a tick stamped at or behind the last one carries no interval. */
    {
        begin("rumble-test.ini", INI_ON);
        g_ev.have = 1;
        g_ev.impact = 1.0f;
        rumble_step(0.0f, 0);           /* the clock has not moved */
        check(g_high == 0, "a sample with no interval moves nothing");
        rumble_step(0.0f, 33);
        check(g_high > 0,
              "and does not spend the impact that arrived on it");
    }

    /* 37. THE WHEEL THAT ANSWERED REFUSES IT, not the one that was named.
     *     Case 3 names `dinput`; this is the shipped configuration, where
     *     `auto` opens a wheel and the provider still reads AUTO, so the
     *     guard was off for every default install, the model ran all race and
     *     io_host_rumble() dropped every command it produced. */
    s_controls.provider = PROVIDER_AUTO;
    begin("rumble-test.ini", INI_ON);
    s_wheel = 1;
    rumble_preload("rumble-test.ini");
    check(!rumble_active(),
          "a wheel found by `auto` is refused the derived effect too");
    s_controls.provider = PROVIDER_XINPUT;

    /* 38. THE CENSUS MEASURES THE MODEL, NOT THE TRANSPORT, INCLUDING ITS
     *     PEAK. Raising `peak motor` inside push(), after io_host_rumble() has
     *     returned success, counts it differently from every other figure on
     *     the same line. On a machine with no XInput sink the line then reads
     *     `peak motor 0 of 65535` next to `bed spinning N% at mean M%`: the
     *     model asked for nothing and asked for a great deal, in one sentence.
     *
     *     The control is the same signal with a working sink. The two censuses
     *     must agree about the MODEL and differ only about the sink. */
    {
        char with_sink[320], without[320];
        unsigned peak_with, peak_without, t;

        begin("rumble-test.ini", INI_ON);
        g_ev.have = 1;
        g_ev.anchored = 1;
        for (t = 0; t < 2000; t += 33) {
            g_ev.texture = 0.6f;
            rumble_step(0.0f, 33);
        }
        peak_with = s_peak;
        rumble_census(with_sink, sizeof with_sink);
        check(peak_with > 0, "with a sink, the census sees a peak");
        check(!strstr(with_sink, "REFUSED"),
              "and says nothing about the sink when the sink took it all");

        begin("rumble-test.ini", INI_ON);
        g_no_sink = 1;                  /* every command refused, all run */
        g_ev.have = 1;
        g_ev.anchored = 1;
        for (t = 0; t < 2000; t += 33) {
            g_ev.texture = 0.6f;
            rumble_step(0.0f, 33);
        }
        peak_without = s_peak;
        rumble_census(without, sizeof without);

        /* THE CLAIM. The same model on the same signal produced the same peak;
         * only the delivery differed. */
        check(peak_without == peak_with,
              "with no sink at all, the peak is the model's just the same");
        check(strstr(without, "REFUSED") != NULL,
              "and the line says so, rather than reading like a quiet game");
    }

    /* 39. THE STAMP IS THE CLOCK. A frame that owes two control ticks runs
     *     them back to back, so no time passes between the calls; each carries
     *     its tick's stamp, and the model must integrate those. Offroad's
     *     16 ms ticks at 30 fps, two per frame. */
    {
        unsigned frame;

        begin("rumble-test.ini", INI_ON);
        g_ev.have = 1;
        g_ev.anchored = 1;
        g_ev.texture = 0.6f;
        for (frame = 0; frame < 60; frame++) {
            double t = frame * 33.333;

            rumble_set_force(0.0f, t, 0);
            rumble_set_force(0.0f, t + 16.0, 0);
        }
        check(g_low || g_high, "two ticks a frame, stamped apart, drive the motors");

        begin("rumble-test.ini", INI_ON);
        g_ev.have = 1;
        g_ev.anchored = 1;
        g_ev.texture = 0.6f;
        for (frame = 0; frame < 60; frame++)
            rumble_set_force(0.0f, 100.0, 0);
        check(!g_low && !g_high, "and a stamp that never moves moves nothing");
    }

    /* 40. BOOST IS ITS OWN PATH, here on the BUTTON FALLBACK (a profile with
     *     no boost chain: g_ev.boost_known is 0). Both titles synthesise boost
     *     as an alternation in the force, and Offroad's bed is trimmed to 8%,
     *     which held its nitro under the mass's start level. Measured on
     *     recorded races: Hydro's booster flips +-0.27 every tick, Offroad's
     *     nitro +-0.5 every two. */
    {
        static const char *const k_offroad =
            INI_ON "rumble_texture_percent = 8\n";
        unsigned t, spark, held;

        /* Held, with the buzz in the force: a spark, then a held buzz. */
        begin("rumble-test.ini", k_offroad);
        g_ev.have = 1;
        g_ev.anchored = 1;
        g_ev.engine = 0.15f;
        for (t = 0; t < 1000; t += 16)
            rumble_step(0.05f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
        s_boost_held = 1;
        spark = 0;
        for (t = 0; t < 300; t += 16) {
            rumble_step(0.5f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
            if (g_high > spark)
                spark = g_high;
        }
        for (t = 0; t < 700; t += 16)
            rumble_step(0.5f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
        held = g_high;
        check(pct(spark) >= 90, "boost catching is a spark on the light motor");
        check(pct(held) >= 40, "and a held buzz while it runs, past an 8% bed trim");

        /* The same force with the button up is surface, not boost. */
        begin("rumble-test.ini", k_offroad);
        g_ev.have = 1;
        g_ev.anchored = 1;
        g_ev.engine = 0.15f;
        for (t = 0; t < 1000; t += 16)
            rumble_step(0.5f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
        check(pct(g_high) < 15, "the alternation alone, button up, stays in the bed");

        /* Held, with nothing in the force: no fuel, or airborne. */
        begin("rumble-test.ini", k_offroad);
        g_ev.have = 1;
        g_ev.anchored = 1;
        s_boost_held = 1;
        for (t = 0; t < 1000; t += 16)
            rumble_step(0.0f, 16);
        s_boost_held = 0;
        check(!g_high, "a press with no buzz in the force moves nothing");

        /* And the key turns it off. */
        begin("rumble-test.ini", INI_ON "rumble_texture_percent = 8\n"
                                 "rumble_boost_percent = 0\n");
        g_ev.have = 1;
        g_ev.anchored = 1;
        s_boost_held = 1;
        spark = 0;
        for (t = 0; t < 1000; t += 16) {
            rumble_step(0.5f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
            if (g_high > spark)
                spark = g_high;
        }
        s_boost_held = 0;
        check(pct(spark) < 15, "rumble_boost_percent = 0 leaves boost in the bed");
    }

    /* 41. THE GAME SAYS WHEN BOOST FIRES, NOT THE BUTTON. Both titles burn
     *     fuel the player collects, so a press on an empty tank is no boost;
     *     a harness that pressed blindly took those for boosts that failed to
     *     spark. With the chain (boost_known) the button is not consulted. */
    {
        static const char *const k_offroad =
            INI_ON "rumble_texture_percent = 8\n";
        unsigned t, spark, held;

        /* Firing, with the buzz: a spark at once, then a held buzz. */
        begin("rumble-test.ini", k_offroad);
        g_ev.have = 1;
        g_ev.anchored = 1;
        g_ev.boost_known = 1;
        for (t = 0; t < 1000; t += 16)
            rumble_step(0.05f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
        g_ev.boosting = 1;
        rumble_step(0.05f, 16);
        spark = g_high;
        for (t = 0; t < 1000; t += 16)
            rumble_step(0.5f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
        held = g_high;
        check(pct(spark) >= 90, "boost firing sparks on the tick it starts");
        check(pct(held) >= 40, "and buzzes while the game says it fires");

        /* The button held on an empty tank, on a rough surface. */
        begin("rumble-test.ini", k_offroad);
        g_ev.have = 1;
        g_ev.anchored = 1;
        g_ev.boost_known = 1;
        s_boost_held = 1;
        spark = 0;
        for (t = 0; t < 1000; t += 16) {
            rumble_step(0.5f * ((t / 16) & 1 ? 1.0f : -1.0f), 16);
            if (g_high > spark)
                spark = g_high;
        }
        s_boost_held = 0;
        check(pct(spark) < 15, "a press on an empty tank is not boost, whatever the force does");

        /* Firing with nothing in the force (airborne): the spark, then quiet. */
        begin("rumble-test.ini", k_offroad);
        g_ev.have = 1;
        g_ev.anchored = 1;
        g_ev.boost_known = 1;
        g_ev.boosting = 1;
        rumble_step(0.0f, 16);
        spark = g_high;
        for (t = 0; t < 1500; t += 16)
            rumble_step(0.0f, 16);
        check(pct(spark) >= 90, "boost firing sparks even with no buzz to follow");
        check(pct(g_high) < 15, "and then has nothing to hold");
    }

    if (failures) {
        fprintf(stderr, "rumble model: %d failure(s)\n", failures);
        return 1;
    }
    printf("rumble model tests: all passed\n");
    return 0;
}
