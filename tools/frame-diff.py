#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""frame-diff.py -- compare two directories of frames, per pixel.

The scoring half of the conformance method. `glide-replay` produces the
frames; this says whether two sets of them are the same, and if not, how
different and where.

    python3 tools/frame-diff.py frames-live/ frames-replay/
    python3 tools/frame-diff.py golden/ vcglide/ --threshold 2 --report d.txt

Three numbers per frame, because one is never enough:

    identical   byte-for-byte. The only acceptable answer for a passthrough.
    max         the largest single-channel difference anywhere in the frame.
                A handful of pixels off by one is a rounding difference; one
                pixel off by 200 is a missing triangle.
    mean        the average absolute difference over every channel of every
                pixel. Says how MUCH of the frame moved, which `max` cannot.

A mean alone would hide one catastrophically wrong pixel in 256,000 right
ones; a max alone would fail a frame whose every pixel is one step off in a
way no one can see, so both are reported.

No third-party imaging library, deliberately: this has to run wherever the
build runs. It reads the 8-bit truecolour non-interlaced PNGs the harness
writes and refuses anything else rather than guessing.
"""
import argparse
import os
import struct
import sys
import zlib


def read_png(path):
    """Return (width, height, bytes) as packed RGB, or raise ValueError."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, w, h, idat = 8, 0, 0, []
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, colour, comp, filt, inter = struct.unpack(">IIBBBBB",
                                                                   body)
            if (depth, colour, inter) != (8, 2, 0):
                raise ValueError(
                    f"{path}: depth {depth}, colour type {colour}, interlace "
                    f"{inter}: this tool reads 8-bit truecolour only")
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        pos += 12 + length
    raw = zlib.decompress(b"".join(idat))
    stride = w * 3
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        # The harness writes filter 0 throughout; the others are implemented
        # so a frame from some other producer can still be read.
        if ft == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError(f"{path}: unknown row filter {ft}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out)


def compare(a_path, b_path):
    aw, ah, a = read_png(a_path)
    bw, bh, b = read_png(b_path)
    if (aw, ah) != (bw, bh):
        return None, f"size {aw}x{ah} vs {bw}x{bh}"
    if a == b:
        return (0, 0.0, 0), None
    worst = 0
    total = 0
    moved = 0
    for i in range(0, len(a), 3):
        dr = abs(a[i] - b[i])
        dg = abs(a[i + 1] - b[i + 1])
        db = abs(a[i + 2] - b[i + 2])
        if dr or dg or db:
            # A PIXEL, not a channel. Dividing a channel count by three
            # reports "0 pixels" for a frame whose one differing pixel moved
            # in only one or two channels, which is a report that contradicts
            # itself: max says 8, pixels says none.
            moved += 1
            total += dr + dg + db
            w = dr if dr > dg else dg
            if db > w:
                w = db
            if w > worst:
                worst = w
    return (worst, total / len(a), moved), None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("first")
    ap.add_argument("second")
    ap.add_argument("--threshold", type=int, default=0,
                    help="largest per-channel difference still counted as a "
                         "pass (default 0: byte-identical)")
    ap.add_argument("--report", help="also write the table here")
    args = ap.parse_args()

    names = sorted(n for n in os.listdir(args.first)
                   if n.lower().endswith(".png"))
    if not names:
        sys.exit(f"{args.first}: no PNGs")

    lines = []
    identical = passed = failed = missing = 0
    for n in names:
        b = os.path.join(args.second, n)
        if not os.path.exists(b):
            missing += 1
            lines.append(f"{n}  MISSING from {args.second}")
            continue
        res, err = compare(os.path.join(args.first, n), b)
        if err:
            failed += 1
            lines.append(f"{n}  {err}")
            continue
        worst, mean, px = res
        if worst == 0:
            identical += 1
            continue
        if worst <= args.threshold:
            passed += 1
            lines.append(f"{n}  max {worst:3d}  mean {mean:7.4f}  "
                         f"{px} pixels  (within threshold)")
        else:
            failed += 1
            lines.append(f"{n}  max {worst:3d}  mean {mean:7.4f}  "
                         f"{px} pixels  DIFFERS")

    head = (f"{args.first} vs {args.second}: {len(names)} frames, "
            f"{identical} identical, {passed} within threshold "
            f"{args.threshold}, {failed} differing, {missing} missing")
    out = "\n".join([head] + lines)
    print(out)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write(out + "\n")
    return 1 if (failed or missing) else 0


if __name__ == "__main__":
    sys.exit(main())
