#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""check-launcher-settings.py -- hold settings.def and vcthunder.ini to each other.

settings.def is the one place a configuration key's section, type, default,
range and choice set is written down, and bindings.def the one place a control's
per-title default, switch line and label is. This checks the two things a C
compiler cannot: that the shipped ini and the launcher's catalogue agree in both
directions, and that key names are unique across sections.
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

# THE THIRD CATEGORY. A key the launcher shows and the shipped ini deliberately
# does not carry: its default is settings.def's, the launcher writes the line
# the moment its value changes, and vcthunder.ini stays a file about the
# handful of things an owner actually set.
# Read by splitting the table into rows, because the flag sits inside the SHOW
# half and a file-wide search cannot say which row it belongs to.
unshipped_keys: set[tuple[str, str]] = set()
_def_lines = DEF.splitlines()
for _n, _line in enumerate(_def_lines):
    _m = re.match(r'^SETTING\("([^"]+)",\s*"([^"]+)",', _line)
    if not _m:
        continue
    # THE ROW ENDS WHERE THE MACRO DOES, not where the next one starts: the
    # comment blocks between rows belong to the row BELOW, and one of them
    # explains SF_UNSHIPPED in prose. Reading to the next SETTING therefore
    # marked the preceding key unshipped, a checker made wrong by a comment,
    # which is the shape of defect this file exists to catch elsewhere.
    _row = []
    for _line2 in _def_lines[_n:]:
        _row.append(_line2)
        if _line2.rstrip().endswith("))"):
            break
    if "SF_UNSHIPPED" in "\n".join(_row):
        unshipped_keys.add((_m.group(1), _m.group(2)))
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

# A LABEL WIDER THAN ITS COLUMN WRAPS INTO THE HELP CAPTION UNDERNEATH IT, and
# the two then overlap on screen. Only a row with a control beside it is
# constrained: a checkbox's label is the whole width of the page. The launcher
# gives the label column 215 logical units, which is about 38 characters of the
# system font; found on "Wheel: how much of its rotation to use (%)".
LABEL_MAX = 38
wide_labels = []
for _n, _line in enumerate(_def_lines):
    _m = re.match(r'^SETTING\("([^"]+)",\s*"([^"]+)",', _line)
    if not _m:
        continue
    _row = []
    for _line2 in _def_lines[_n:]:
        _row.append(_line2)
        if _line2.rstrip().endswith("))"):
            break
    _text = "\n".join(_row)
    _show = re.search(r'SHOW\(\s*PAGE_\w+,\s*"((?:[^"\\]|\\.)*)",\s*(ST_\w+)',
                      _text, re.S)
    if not _show or _show.group(2) == "ST_BOOL":
        continue
    if len(_show.group(1)) > LABEL_MAX:
        wide_labels.append(f"{len(_show.group(1))} chars: {_show.group(1)}")
if wide_labels:
    raise SystemExit(
        f"a setting label is wider than its column ({LABEL_MAX} characters); "
        f"the launcher would wrap it into the help caption below it:\n  "
        + "\n  ".join(wide_labels))

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

# AND THE DEFAULTS THEMSELVES, NOT ONLY THE KEY NAMES. Matching by name alone
# lets the shipped INI carry a different default from the table, "two spellings
# of one default", which bindings.def's header forbids and which is invisible
# because both files look right on their own. It matters most for a control
# whose two cabinets DISAGREE: a value in the shared section beats a per-title
# default rather than losing to it, so a row with per-title defaults that ships
# only a shared line silently gives both cabinets the wrong one. The rule,
# matching what [digital]/[offroad.digital] already does:
#
#   the shared section carries HYDRO's default;
#   [offroad.<section>] carries Offroad's, and only where the two differ.
# BOTH DIRECTIONS, because one is not enough: a setting must not be added to the
# launcher without being shipped, or shipped without being editable, unless the
# control is marked SF_UNSHIPPED, which is the third category and is
# deliberately absent from the file. So an unshipped control must not appear in
# the ini AND a shipped key must not be marked unshipped.
#
# UNIQUE ACROSS SECTIONS, because config.c accepts a known key found in the
# wrong section (docs/configuration.md §6 promises it does) and that fallback is
# only unambiguous while no two sections share a key name. A developer switch
# (SETTING_DEV) is the deliberate exception: parsed, shown nowhere, absent from
# the shipped file.
def binding_defaults(macro, columns):
    """{action: (hydro, offroad)} for one macro, by column index."""
    out = {}
    for m in re.finditer(
            r"^" + macro + r"\s*\(([^;]*?)\)\s*$", BINDINGS, re.M | re.S):
        parts = [p.strip() for p in m.group(1).split(",")]
        out[parts[0]] = (parts[columns[0]], parts[columns[1]])
    return out

# BINDING(key, hydro, offroad, pad_hydro, pad_offroad, button, hl, ol)
# BINDING_AXIS(key, hydro, offroad, hl, ol)
# BINDING_PAD(key, pad_hydro, pad_offroad, hl, ol)
expected: dict[tuple[str, str], tuple[str, str]] = {}
for action, pair in binding_defaults("BINDING", (1, 2)).items():
    expected[("digital", action)] = tuple(v.strip('"') for v in pair)
for action, pair in binding_defaults("BINDING_AXIS", (1, 2)).items():
    expected[("digital", action)] = tuple(v.strip('"') for v in pair)
for action, pair in binding_defaults("BINDING", (3, 4)).items():
    expected[("gamepad", action)] = pair
for action, pair in binding_defaults("BINDING_PAD", (1, 2)).items():
    expected[("gamepad", action)] = pair
n_both = len(binding_defaults("BINDING", (1, 2)))
n_axis = len(binding_defaults("BINDING_AXIS", (1, 2)))
n_pad = len(binding_defaults("BINDING_PAD", (1, 2)))
if (n_both + n_axis + n_pad != len(bindings) or
        len(expected) != (n_both + n_axis) + (n_both + n_pad)):
    raise SystemExit(
        "bindings.def defaults parsed to the wrong count: has a macro's "
        "column order changed?")

settings = set(shown)
dev_keys = {key for _, key in dev}

section = ""
missing: list[str] = []
shipped_dev: list[str] = []
shipped_unshipped: list[str] = []
shipped_settings: set[tuple[str, str]] = set()
shipped_bindings: dict[tuple[str, str, str], str] = {}
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
    # THE BASE SECTION, FOR SETTINGS AS WELL AS FOR BINDINGS. ini_read_scoped()
    # tries `<title>.<section>` before `<section>` for every key, not only for
    # the two binding sections, so `[hydro.ffb] rumble_texture_percent` is the
    # SAME setting as `[ffb] rumble_texture_percent` and settings.def has one
    # row for it. Matching on the full section would read a per-title trim as a
    # key with no control at all.
    base = section.split(".", 1)[-1]
    matches = int((base, key) in settings)
    if base in {"digital", "gamepad"}:
        matches += int(key in bindings)
        title = section[: -len(base) - 1].lower() if "." in section else ""
        shipped_bindings[(title, base, key)] = line.split("=", 1)[1].strip()
    if matches != 1:
        missing.append(f"[{section}] {key} ({matches} controls)")
    elif (base, key) in unshipped_keys:
        shipped_unshipped.append(f"[{section}] {key}")
    if key in dev_keys:
        shipped_dev.append(f"[{section}] {key}")
    if base not in {"digital", "gamepad"}:
        shipped_settings.add((base, key))

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

# AND THE OTHER DIRECTION, which is the one that rots quietly: a key marked
# SF_UNSHIPPED and shipped anyway reads as configuration the user chose and is
# the launcher's own compiled default written out in longhand.
if shipped_unshipped:
    raise SystemExit(
        "SF_UNSHIPPED settings are deliberately absent from the shipped "
        "INI; these are in it:\n  "
        + "\n  ".join(shipped_unshipped)
    )

# Hold every shipped binding value to bindings.def's own column.
wrong: list[str] = []
for (base, action), (hydro, offroad) in sorted(expected.items()):
    shared = shipped_bindings.get(("", base, action))
    scoped = shipped_bindings.get(("offroad", base, action))
    # AN ABSENT KEY IS NOT A DEFECT, and saying so was this check's own first
    # mistake: it flagged shift1..3, which the file deliberately ships only
    # under [offroad.digital] because Hydro has no shifter. Work the fallback
    # chain instead of the file's shape. Absent everywhere, each cabinet lands
    # on its OWN bindings.def column and both are right. The one arrangement
    # that is wrong is a shared line without the scoped line beside it, because
    # the shared value is tried second and beats the per-title DEFAULT, which is
    # tried third and never reached.
    if shared is not None and shared != hydro:
        wrong.append(
            f"[{base}] {action} = {shared}, but bindings.def says {hydro}")
    if scoped is not None and scoped != offroad:
        wrong.append(
            f"[offroad.{base}] {action} = {scoped}, but bindings.def "
            f"says {offroad}")
    if offroad != hydro and shared is not None and scoped is None:
        wrong.append(
            f"[offroad.{base}] {action} is not shipped, so Offroad reads "
            f"[{base}]'s {shared} and never bindings.def's {offroad}")
    if offroad == hydro and scoped is not None:
        wrong.append(
            f"[offroad.{base}] {action} = {scoped} restates the shared "
            f"value; bindings.def gives both cabinets {hydro}")
if wrong:
    raise SystemExit(
        "shipped INI bindings disagree with src/bindings.def:\n  "
        + "\n  ".join(wrong))

missing_from_ini = sorted(settings - shipped_settings - unshipped_keys)
if missing_from_ini:
    rendered = "\n  ".join(f"[{section}] {key}"
                           for section, key in missing_from_ini)
    raise SystemExit(
        "settings the launcher shows that the INI does not carry. Ship the "
        "key, or mark the row SF_UNSHIPPED if it is meant to be absent:\n  "
        + rendered)

print(
    f"settings.def: {len(settings)} shipped settings + {len(dev)} developer "
    f"switches; bindings.def: {len(bindings)} actions and "
    f"{len(expected)} defaults, all matching the INI; "
    f"{len(unshipped_keys)} deliberately unshipped; "
    f"INI covered, key names unique"
)
