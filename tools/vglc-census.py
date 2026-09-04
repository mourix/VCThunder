#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""vglc-census.py -- what a capture actually ASKS a provider to do.

`vglc-scan.py` says how often each entry point was called, which bounds the
*interface*. This says which distinct ARGUMENT TUPLES each one was called
with, which bounds the *implementation*, and those are very different numbers:
Hydro calls grAlphaCombine 177,725 times, but if it does so with four distinct
argument tuples then a renderer owes four combine modes, not the whole Glide
combine space. Scoping a renderer by a call count instead would implement the
entire specification of a chip the games drive in a handful of ways.

    vglc-census.py CAP.vglc                  every entry, its distinct tuples
    vglc-census.py CAP.vglc --top N          show at most N tuples per entry
    vglc-census.py CAP.vglc --entry NAME     only these (repeatable)
    vglc-census.py CAP.vglc --tex            GrTexInfo fields, by value
    vglc-census.py CAP.vglc --vertex         GrVertex component ranges

It reads the file and runs nothing, so it works on Linux against a capture
taken on Windows. It shares vglc-scan.py's reader deliberately: that reader is
already the second implementation of the format (glide-replay.c is the first),
and a THIRD would be a third thing to keep in step for no extra check.
"""
import argparse
import importlib.util
import struct
import sys
from pathlib import Path

# vglc-scan.py is not an importable module name (the hyphen), so it is loaded
# by path. Sharing its reader is deliberate; see the docstring.
_spec = importlib.util.spec_from_file_location(
    "vglc_scan", Path(__file__).resolve().parent / "vglc-scan.py")
vglc_scan = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vglc_scan)
Capture = vglc_scan.Capture
VGLC_CONTROL = vglc_scan.VGLC_CONTROL
VGLC_FRAME = vglc_scan.VGLC_FRAME
TAG_TEXDATA = vglc_scan.TAG_TEXDATA

# GrTexInfo, as five 32-bit fields. renderer/api/glide2.h carries the same
# offsets and its derivation; this tool only reads them.
TEXINFO_FIELDS = ("smallLod", "largeLod", "aspect", "format", "data")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture")
    ap.add_argument("--top", type=int, default=16,
                    help="show at most this many distinct tuples per entry")
    ap.add_argument("--entry", action="append", default=[],
                    help="restrict to these entry points (repeatable)")
    ap.add_argument("--tex", action="store_true",
                    help="census GrTexInfo fields rather than raw arguments")
    ap.add_argument("--vertex", action="store_true",
                    help="report GrVertex component ranges")
    ap.add_argument("--frames", default=None, metavar="A:B")
    args = ap.parse_args()

    lo, hi = 0, 1 << 30
    if args.frames:
        a, _, b = args.frames.partition(":")
        lo = int(a) if a else 0
        hi = int(b) if b else hi

    cap = Capture(args.capture)
    print(f"capture   {args.capture}")
    print(f"  title   {cap.title}, taken through {cap.backend}")

    frame = 0
    tuples = {}          # name -> {argtuple: count}
    texinfo = {}         # name -> {(smallLod,largeLod,aspect,format): count}
    vmin = [None] * 15
    vmax = [None] * 15
    nvert = 0

    for idx, name, pay, a, blobs in cap.records():
        if idx == VGLC_CONTROL:
            if a[0] == VGLC_FRAME:
                frame = a[1]
            continue
        if not (lo <= frame < hi):
            continue
        if args.entry and name not in args.entry:
            continue

        if args.tex and pay in ("PL_TEXDOWN", "PL_TEXINFO3", "PL_TEXINFO1"):
            # The GrTexInfo blob is tagged with its argument index; the
            # texture bytes themselves are TAG_TEXDATA and are not read here.
            for tag, size, off in blobs:
                if tag != TAG_TEXDATA and size >= 20:
                    f = struct.unpack_from("<5I", cap.m, off)
                    key = (f[0], f[1], f[2], f[3])
                    d = texinfo.setdefault(name, {})
                    d[key] = d.get(key, 0) + 1
        elif args.vertex and pay in ("PL_V2", "PL_V3", "PL_VLIST"):
            for tag, size, off in blobs:
                n = size // cap.vertex_bytes
                for v in range(n):
                    f = struct.unpack_from("<15f", cap.m,
                                           off + v * cap.vertex_bytes)
                    nvert += 1
                    for i, x in enumerate(f):
                        if vmin[i] is None or x < vmin[i]:
                            vmin[i] = x
                        if vmax[i] is None or x > vmax[i]:
                            vmax[i] = x
        elif not args.tex and not args.vertex:
            d = tuples.setdefault(name, {})
            d[a] = d.get(a, 0) + 1

    if args.tex:
        print("\nGrTexInfo, by distinct (smallLod, largeLod, aspect, format):")
        for name in sorted(texinfo):
            d = texinfo[name]
            print(f"\n  {name}: {len(d)} distinct")
            for key, n in sorted(d.items(), key=lambda kv: -kv[1])[:args.top]:
                print(f"    {n:9d}  smallLod={key[0]:2d} largeLod={key[1]:2d} "
                      f"aspect={key[2]:2d} format={key[3]:2d}")
        return

    if args.vertex:
        names = ("x", "y", "z", "r", "g", "b", "ooz", "a", "oow",
                 "t0.s", "t0.t", "t0.w", "t1.s", "t1.t", "t1.w")
        print(f"\nGrVertex components over {nvert} vertices "
              f"({cap.vertex_bytes} bytes each):")
        for i, nm in enumerate(names):
            if vmin[i] is None:
                continue
            print(f"  {nm:>6}  {vmin[i]:16.4f} .. {vmax[i]:16.4f}")
        return

    print("\ndistinct argument tuples per entry "
          "(count, then the arguments as hex):")
    for name in sorted(tuples, key=lambda k: (len(tuples[k]), k)):
        d = tuples[name]
        total = sum(d.values())
        print(f"\n  {name}: {len(d)} distinct of {total} calls")
        for tup, n in sorted(d.items(), key=lambda kv: -kv[1])[:args.top]:
            av = " ".join(f"{v:#010x}" for v in tup)
            print(f"    {n:9d}  {av}")
        if len(d) > args.top:
            print(f"    ... {len(d) - args.top} more")


if __name__ == "__main__":
    main()
