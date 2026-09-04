#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""check-launcher-settings.py -- hold settings.def and vcthunder.ini to each other.

settings.def is the one place a configuration key's section, type, default,
range and choice set is written down, and bindings.def is the one place a
control's per-title default, switch line and label is. config.c, io_config.c,
io_board.c and launcher.c expand them. This checks the two things a C compiler
cannot:

  * every key the shipped ini carries has exactly one control, and every
    control is a key the shipped ini carries, so a setting cannot be added
    to the launcher without shipping it, or shipped without being editable;
  * key names are unique across sections. config.c accepts a known key found
    in the wrong section (docs/configuration.md §6 promises it does), and that
    fallback is only unambiguous while no two sections share a key name.

A developer switch (SETTING_DEV) is the deliberate exception: parsed, shown
nowhere, and absent from the shipped file.
"""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEF = (ROOT / "src" / "settings.def").read_text(encoding="utf-8")
BINDINGS = (ROOT / "src" / "bindings.def").read_text(encoding="utf-8")

# Only a line that STARTS with the macro is an entry; the file's own header
# comment describes the macros with unquoted placeholders and is indented.
shown = re.findall(r'^SETTING\("([^"]+)",\s*"([^"]+)"', DEF, re.M)
dev = re.findall(r'^SETTING_DEV\("([^"]+)",\s*"([^"]+)"', DEF, re.M)

if not shown:
    raise SystemExit("settings.def parsed to nothing: has the macro changed?")

table = shown + dev
if len(set(table)) != len(table):
    raise SystemExit("settings.def contains a duplicate [section] key")

# A help string longer than the launcher's caption buffer is truncated MID-WORD
# on screen and reports nothing: caught once, on the longest string in the
# table. src/launcher.c sizes its buffer from VCT_HELP_MAX and this is the only
# thing that can hold the table to it.
HELP_MAX = 400
long_help = []
for m in re.finditer(r'SHOW\((.*?)\)\)', DEF, re.S):
    tail = m.group(1)
    tail = tail[tail.rfind("WARNING_"):]
    text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', tail))
    if len(text) > HELP_MAX:
        long_help.append((len(text), text[:60]))
if long_help:
    raise SystemExit(
        f"a help string exceeds VCT_HELP_MAX ({HELP_MAX}); the launcher would "
        f"truncate it on screen:\n  "
        + "\n  ".join(f"{n} chars: {t}..." for n, t in long_help))

# config.c's cross-section fallback resolves a key by name alone.
by_name: dict[str, list[str]] = {}
for section, key in table:
    by_name.setdefault(key, []).append(section)
ambiguous = {k: v for k, v in by_name.items() if len(v) > 1}
if ambiguous:
    raise SystemExit(
        "settings.def key names must be unique across sections, because "
        "config.c resolves a misplaced key by name:\n  "
        + "\n  ".join(f"{k} in {', '.join(v)}" for k, v in sorted(ambiguous.items()))
    )

# The control bindings have their own one table, read here for the same reason
# settings.def is: a shipped [digital] or [gamepad] key with no row is a key
# nothing resolves, and a row with no shipped key is a control the user cannot
# reach. Only a line that STARTS with the macro is a row; the header comment
# describes them with unquoted placeholders and is indented.
binding_items = re.findall(r"^BINDING(?:_AXIS|_PAD)?\s*\(\s*(\w+)", BINDINGS, re.M)
bindings = set(binding_items)
if not bindings:
    raise SystemExit("bindings.def parsed to nothing: has the macro changed?")
if len(bindings) != len(binding_items):
    raise SystemExit("bindings.def contains a duplicate binding action")

settings = set(shown)
dev_keys = {key for _, key in dev}

section = ""
missing: list[str] = []
shipped_dev: list[str] = []
shipped_settings: set[tuple[str, str]] = set()
for raw in (ROOT / "vcthunder.ini").read_text(encoding="utf-8").splitlines():
    line = raw.strip()
    if not line or line.startswith((";", "#")):
        continue
    if line.startswith("[") and "]" in line:
        section = line[1 : line.index("]")].strip().lower()
        continue
    if "=" not in line:
        continue
    key = line.split("=", 1)[0].strip().lower()
    base = section.split(".", 1)[-1]
    matches = int((section, key) in settings)
    if base in {"digital", "gamepad"}:
        matches += int(key in bindings)
    if matches != 1:
        missing.append(f"[{section}] {key} ({matches} controls)")
    if key in dev_keys:
        shipped_dev.append(f"[{section}] {key}")
    if base not in {"digital", "gamepad"}:
        shipped_settings.add((section, key))

if missing:
    raise SystemExit(
        "shipped INI keys without exactly one launcher control:\n  "
        + "\n  ".join(missing)
    )

if shipped_dev:
    raise SystemExit(
        "SETTING_DEV keys are developer switches and must not ship in the "
        "INI (docs/configuration.md §6):\n  " + "\n  ".join(shipped_dev)
    )

unshipped = sorted(settings - shipped_settings)
if unshipped:
    rendered = "\n  ".join(f"[{section}] {key}" for section, key in unshipped)
    raise SystemExit("unshipped settings exposed by launcher:\n  " + rendered)

print(
    f"settings.def: {len(settings)} shipped settings + {len(dev)} developer "
    f"switches; bindings.def: {len(bindings)} actions; "
    f"INI covered, key names unique"
)
