#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""regress.py -- did this renderer change move a pixel? Over whole captures.

`make check` gates the renderer on SEVEN synthetic frames. That is the most it
can gate on: every capture of a real title is game material and
tools/scan-contraband.py refuses to let one into this repository, correctly.
The real instrument has therefore always been a hand-run A/B -- build a
control, replay both titles through both backends, --hash, cmp -- described in
prose and executed by hand, one session at a time, which produced sixteen
hand-made .hash files in a scratch directory.

This is that procedure, as one command.

    make regress                 control = HEAD, current = the working tree
    make regress TIER=full       the three long captures as well
    make regress REV=abc123      control = that revision

WHAT IT COMPARES, AND WHAT IT DOES NOT. Two providers, one harness: the SAME
build/glide-replay.exe replays both arms, because a comparison whose measuring
instrument also changed is not a comparison. Identical per-frame hashes over a
whole capture mean the change moved no pixel in it. They do NOT mean the change
is correct -- only an emulated Voodoo2 can say what a Voodoo2 drew -- and they
say nothing at all about speed, which is a separate question that needs a
quiet machine.

THE CONTROL IS BUILT FROM A GIT REVISION, never from archive/ and never from
the run directory: `make pack` has overwritten a control there once already and
the change was scored against itself. It is extracted with `git archive` into
build/regress/, so the working tree is never touched and no stash is involved.
Only the renderer is built there; renderer/ deliberately depends on nothing
under generated/, so a control needs no dumps.

A CLEAN WORKING TREE IS NOT AN ERROR. Control and current are then the same
source, and the run becomes a self-test of the harness: it must report every
frame identical. That is the "the control is the same binary run twice" case,
and it is worth having.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "scripts"))
import titles                                            # noqa: E402

MANIFEST = os.path.join(REPO, "tests", "regress-captures.def")
WORK     = os.path.join(REPO, "build", "regress")
REPLAY   = os.path.join(REPO, "build", "glide-replay.exe")
CURRENT  = os.path.join(REPO, "build", "vcglide.dll")
SDL3     = os.path.join(REPO, "build", "SDL3.dll")

# The replay harness is a Windows binary. Under WSL its environment reaches it
# only through WSLENV, and a dropped variable is silent: the run then uses the
# default backend, which for a pixel claim is a comparand of the wrong thing.
PASS_THROUGH = ["VCGLIDE_RENDERER", "VCGLIDE_PRESENT",
                "VCGLIDE_RENDER_SCALE", "VCGLIDE_LOG"]


def say(*a):
    print(*a, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


# ---- the manifest -------------------------------------------------------
def read_manifest(tier):
    rows = []
    with open(MANIFEST, encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            row_tier, title, frames, path = line.split(None, 3)
            if tier == "quick" and row_tier != "quick":
                continue
            rows.append((title, int(frames), path.strip()))
    return rows


def resolve(rows):
    """Which captures are actually on this machine. A missing one is named:
    a regression suite that quietly shrinks is the failure mode here."""
    found, missing = [], []
    for title, frames, rel in rows:
        full = os.path.join(titles.WORKSPACE, rel)
        (found if os.path.isfile(full) else missing).append(
            (title, frames, rel, full))
    return found, missing


# ---- the control --------------------------------------------------------
def sdl3_version(makefile):
    for line in open(makefile, encoding="utf-8"):
        if line.startswith("SDL3_VERSION"):
            return line.split(":=")[1].strip()
    return None


def resolve_rev(rev):
    """The revision, resolved BEFORE anything is said about it. `git diff`
    against a name git cannot resolve fails with empty output, which reads
    exactly like "no changes" -- so an unknown revision used to be announced as
    identical to the working tree and only then reported as unknown."""
    head = run(["git", "-C", REPO, "rev-parse", "--verify", rev + "^{commit}"])
    if head.returncode:
        say(f"SKIPPED: '{rev}' is not a revision in this repository.")
        say("         This check did NOT run.")
        return None
    return head.stdout.strip()


def build_control(sha):
    """`git archive` the revision into build/regress/control and build only its
    renderer. Extraction rather than a worktree or a stash: this must not be
    able to touch the tree the user is working in."""
    src = os.path.join(WORK, "control")
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    archive = subprocess.Popen(["git", "-C", REPO, "archive", sha],
                               stdout=subprocess.PIPE)
    tar = subprocess.Popen(["tar", "-x", "-C", src], stdin=archive.stdout)
    archive.stdout.close()
    tar.communicate()
    if tar.returncode or archive.wait():
        say("regress: could not extract the control revision")
        return None

    # The SDK is pinned per revision. Sharing this tree's download is right
    # when the pin matches and WRONG when it does not, so check rather than
    # hope: a control built against a different SDL3 is a second variable.
    want = sdl3_version(os.path.join(src, "Makefile"))
    have = sdl3_version(os.path.join(REPO, "Makefile"))
    if want != have:
        say(f"SKIPPED: {sha[:12]} pins SDL3 {want}, this tree has {have}.")
        say("         Building the control against a different SDK would add a")
        say("         second variable. This check did NOT run.")
        return None

    say(f"building the control from {sha[:12]} (renderer only)")
    t0 = time.time()
    made = run(["make", "-C", src, "renderer",
                "SDL3_HOME=" + os.path.join(REPO, "build", "deps")])
    if made.returncode:
        say("regress: THE CONTROL DID NOT BUILD")
        say(made.stdout[-2000:])
        say(made.stderr[-2000:])
        return None
    say(f"  built in {time.time() - t0:.1f}s")
    return os.path.join(src, "build")


def arm_dir(name, provider_build):
    """One directory per arm holding the provider under test and the SAME
    harness. glide-replay resolves --provider against the working directory, so
    the two arms cannot pick up one another's dll by accident."""
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    shutil.copy2(os.path.join(provider_build, "vcglide.dll"), d)
    for extra in (os.path.join(provider_build, "SDL3.dll"), SDL3):
        if os.path.isfile(extra):
            shutil.copy2(extra, d)
            break
    shutil.copy2(REPLAY, d)
    return d


# ---- the replay ---------------------------------------------------------
def replay(arm, capture_win, backend, hash_path, log_path):
    env = dict(os.environ)
    env["VCGLIDE_RENDERER"] = backend
    env["VCGLIDE_PRESENT"] = "0"
    # The user's own tuned render_scale is 3. It is not read here -- this
    # harness takes no ini -- but naming it costs nothing and a scale that
    # differed between the arms would be a silent second variable.
    env["VCGLIDE_RENDER_SCALE"] = "1"
    env["VCGLIDE_LOG"] = os.path.basename(log_path)
    if os.path.exists("/proc/sys/fs/binfmt_misc/WSLInterop"):
        env["WSLENV"] = ":".join(v + "/p" if v == "VCGLIDE_LOG" else v
                                 for v in PASS_THROUGH)
    r = subprocess.run([os.path.join(arm, "glide-replay.exe"), capture_win,
                        "--provider", "vcglide.dll",
                        "--hash", os.path.basename(hash_path)],
                       cwd=arm, env=env, capture_output=True, text=True)
    return r


def to_windows(path):
    """A /mnt/c path handed to a Windows binary fails to open SILENTLY: the
    harness prints its per-frame line only on success, so the run looks
    complete and produces nothing."""
    if not os.path.exists("/proc/sys/fs/binfmt_misc/WSLInterop"):
        return path
    return run(["wslpath", "-w", path]).stdout.strip()


def read_hashes(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return [ln.rstrip("\n") for ln in fh if ln.strip()]
    except OSError:
        return []


def compare(name, frames, control_hash, current_hash):
    a, b = read_hashes(control_hash), read_hashes(current_hash)
    if not a or not b:
        say(f"  {name}: FAILED -- one arm produced no hashes at all")
        return False
    if len(a) != frames or len(b) != frames:
        say(f"  {name}: FAILED -- the manifest says {frames} frames, the "
            f"control replayed {len(a)} and the current {len(b)}")
        return False
    if a == b:
        say(f"  {name}: {len(a)} frames, every one identical")
        return True
    differing = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    say(f"  {name}: MOVED -- {len(differing)} of {len(a)} frames differ, "
        f"first at frame {differing[0]}")
    say(f"      control {a[differing[0]]}")
    say(f"      current {b[differing[0]]}")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tier", default="quick", choices=("quick", "full"))
    ap.add_argument("--rev", default="HEAD")
    ap.add_argument("--backends", default="cpu,gpu")
    args = ap.parse_args()

    backends = [b.strip() for b in args.backends.split(",") if b.strip()]

    for needed, what in ((REPLAY, "build/glide-replay.exe (make tools)"),
                         (CURRENT, "build/vcglide.dll (make renderer)")):
        if not os.path.isfile(needed):
            say(f"regress: {what} is not built")
            return 1

    rows = read_manifest(args.tier)
    found, missing = resolve(rows)
    for _, _, rel, _ in missing:
        say(f"  absent: {rel}")
    if not found:
        say("SKIPPED: no capture in tests/regress-captures.def is on this")
        say("         machine. They are game material and cannot live in this")
        say("         repository. This check did NOT run.")
        return 0
    if missing:
        say(f"regress: {len(found)} of {len(rows)} captures present; the rest "
            f"were NOT replayed")

    sha = resolve_rev(args.rev)
    if sha is None:
        return 0

    # Is there anything to compare at all?
    dirty = run(["git", "-C", REPO, "diff", "--stat", sha, "--",
                 "renderer", "tools/glide-replay.c"])
    if dirty.returncode:
        say("regress: git could not diff the working tree against "
            f"{sha[:12]}; continuing")
    elif not dirty.stdout.strip():
        say(f"NOTE: renderer/ is identical to {args.rev}. Control and current "
            f"are the same")
        say("      source, so this run is a self-test: every frame must match.")

    control_build = build_control(sha)
    if not control_build:
        return 1

    control = arm_dir("arm-control", control_build)
    current = arm_dir("arm-current", os.path.join(REPO, "build"))

    ok = True
    t0 = time.time()
    for title, frames, rel, full in found:
        win = to_windows(full)
        for backend in backends:
            name = f"{title} {backend:3s} {os.path.basename(rel)}"
            # Per capture, not accumulated. A suite that stops comparing after
            # its first finding reports one moved capture and stays silent
            # about the other four, which is worse than not running: it looks
            # like a complete answer. (Written that way once, on 2026-08-31,
            # and caught by the deliberate one-line renderer change that this
            # harness was being falsified against.)
            replayed = True
            for arm, tag in ((control, "control"), (current, "current")):
                r = replay(arm, win, backend,
                           os.path.join(arm, "regress.hash"),
                           os.path.join(arm, "regress.log"))
                if r.returncode:
                    say(f"  {name}: the {tag} replay itself failed")
                    say(r.stdout[-1500:])
                    replayed = False
            if not replayed:
                ok = False
                continue
            ok &= compare(name, frames,
                          os.path.join(control, "regress.hash"),
                          os.path.join(current, "regress.hash"))
    say(f"regress: {time.time() - t0:.1f}s of replay")

    if not ok:
        say("")
        say("A MOVED PIXEL IS NOT AUTOMATICALLY A BUG. Look at the frames before")
        say("deciding: glide-replay --out FRAMES/ writes a PNG per frame, and")
        say("--from N gates WRITING while --frames N caps the whole replay, so")
        say("a late window needs --from N with no --frames.")
        return 1
    say("regress: the change moved no pixel in any capture replayed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
