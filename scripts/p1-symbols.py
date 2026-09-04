#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""p1-symbols.py -- recover the CodeView symbol table embedded in HYDRO.EXE.

READ-ONLY. HYDRO.EXE is statically linked and has no PE symbol table (`rabin2 -s` yields 2),
but LinkLoc left CodeView S_PUB32-style records in the image. Each record is:

    u16 len | u16 rectype(0x1009) | u32 typeind | u32 segoffset | u16 segment | u8 namelen | name

Segment 1 == CODE, segment 2 == DATA. Segment bases are calibrated at runtime against a
known anchor rather than assumed, so this stays correct if the assumption is ever wrong.

Emits <analysis dir>/p1/json/symbols.json (build/analysis/<title>/ in a clone):
{"name": {"va": int, "seg": int, "off": int}, ...}

The output path carries the title, because a constant one would let a run
against the second binary overwrite the first title's map with no warning.

Usage:
    python3 scripts/p1-symbols.py [--title hydro|offroad]
    python3 scripts/p1-symbols.py --title offroad --lookup 0x136a18   # nearest symbol
    python3 scripts/p1-symbols.py --grep diego                        # name search
"""
import json
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from titles import pick                                   # noqa: E402

REC_TYPE = 0x1009

# Segment 1 == CODE, 2 == DATA, 3 == .bss; the bases are those sections' VAs,
# read from the PE headers per title rather than written down. The Hydro values
# were confirmed independently once: _CoinopVidTiming512x400_Voodoo{1,2} live at
# seg2 offsets 0xd440/0xd488 and _init3dfx_Init pushes them as 0x2aa080/0x2aa0c8,
# giving 0x2aa080 - 0xd440 == 0x29cc40 == the DATA section VA, and the header
# agrees, so the header is trusted for every title.

# Prologue bytes an MSVC 11.00 function can plausibly start with. Used only to
# score a candidate segment base, never to classify a symbol.
PROLOGUES = (b"\x55\x8b\xec", b"\x53", b"\x56", b"\x57", b"\x83\xec", b"\x81\xec",
             b"\x8b\x44\x24", b"\x8b\x4c\x24", b"\xa1", b"\x33\xc0", b"\xd9",
             b"\x6a", b"\x68", b"\xb8", b"\xc3")


def parse_records(d):
    """Scan the whole image for chains of valid CodeView records."""
    syms, seen, i, n = {}, set(), 0, len(d)
    while i + 4 <= n:
        ln, rt = struct.unpack_from("<HH", d, i)
        if rt != REC_TYPE or ln < 12 or ln > 512 or i + 2 + ln > n:
            i += 1
            continue
        body = d[i + 4:i + 2 + ln]
        if len(body) < 11:
            i += 1
            continue
        _typ, off, seg = struct.unpack_from("<IIH", body, 0)
        nl = body[10]
        name = body[11:11 + nl]
        if nl == 0 or len(name) != nl or not all(32 <= c < 127 for c in name):
            i += 1
            continue
        key = (i, ln)
        if key not in seen:
            seen.add(key)
            syms[name.decode()] = {"seg": seg, "off": off}
        i += ln + 2
    return syms


def calibrate(d, syms, code_off):
    """Confirm the CODE segment base by scoring candidates against the corpus.

    Scoring hundreds of symbols, rather than testing one named function's
    prologue: any single anchor is a symbol of ONE title (_diego_io_init is
    Hydro's; Offroad uses the successor MagicBus board), so on the other title
    such a check could only ever report UNVERIFIED, which is a silent downgrade
    dressed up as a result. A wrong base makes essentially every symbol land on
    a byte that is not a plausible function start.
    """
    sample = [s["off"] for s in syms.values() if s["seg"] == 1][:400]
    if not sample:
        return code_off, False
    best, best_hits = code_off, -1
    for base in (code_off, 0x0):
        hits = sum(1 for off in sample
                   if any(d[base + off:base + off + len(p)] == p for p in PROLOGUES))
        if hits > best_hits:
            best, best_hits = base, hits
    return best, best_hits >= len(sample) // 2


def build(t):
    d = t.read()
    syms = parse_records(d)
    seg_va = {2: t.data_va, 3: t.bss_va}
    base, ok = calibrate(d, syms, t.code_file_off)
    out = {}
    for name, s in syms.items():
        if s["seg"] == 1:
            va = t.image_base + base + s["off"]
        else:
            sb = seg_va.get(s["seg"])
            va = sb + s["off"] if sb else None
        out[name] = {"va": va, "seg": s["seg"], "off": s["off"]}
    return d, out, base, ok


def main():
    t = pick()
    # Asking what this does must not RUN it: several of these sweep a whole
    # dump, and p1-symbols.py writes into analysis/ as a side effect.
    if "--help" in sys.argv[1:] or "-h" in sys.argv[1:]:
        print(__doc__.strip())
        return 0
    args = [a for a in sys.argv[1:]]
    if args and not args[0].startswith("--"):
        args.pop(0)          # legacy positional path; the title decides the file

    d, syms, base, ok = build(t)
    code = {k: v for k, v in syms.items() if v["seg"] == 1}
    data = {k: v for k, v in syms.items() if v["seg"] in (2, 3)}

    if "--lookup" in args:
        target = int(args[args.index("--lookup") + 1], 0)
        pool = {k: v for k, v in syms.items() if v["va"] and v["va"] <= target}
        best = max((v["va"], k) for k, v in pool.items())
        print(f"{target:#x}  ->  {best[1]} + {target - best[0]:#x}   (fn at {best[0]:#x})")
        return
    if "--grep" in args:
        pat = args[args.index("--grep") + 1].lower()
        for k in sorted(k for k in syms if pat in k.lower()):
            v = syms[k]
            print(f'{v["va"]:#010x}  {k}' if v["va"] else f'  seg{v["seg"]}:{v["off"]:#09x}  {k}')
        return

    print(f"title   {t.id} ({t.name})")
    print(f"input   {t.exe}")
    print(f"sha256  {t.sha256()}")
    t.verify()
    print(f"CODE segment base {base:#x}  (calibration {'verified' if ok else 'UNVERIFIED'})")
    print(f"symbols {len(syms)} total, {len(code)} in CODE, {len(data)} in DATA/.bss")
    outp = t.symbols_path
    os.makedirs(os.path.dirname(outp), exist_ok=True)
    with open(outp, "w") as f:
        json.dump(syms, f, indent=0, sort_keys=True)
    print(f"wrote   {outp}")


if __name__ == "__main__":
    main()
