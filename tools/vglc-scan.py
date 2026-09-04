#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""vglc-scan.py -- read a .vglc capture and say what is actually in it.

`glide-replay --list` counts calls; this reads the stream itself, so it can
answer the questions a count cannot: which frame a call landed in, what its
arguments were, which payload blobs came with it and how big they were, and
(the reason it was written) **which calls carry a pointer whose contents the
capture did not record**.

That last one is the whole point. A capture is only as good as its payload
rules, and a rule that silently declines to fire looks exactly like a rule
that fired correctly: the replay is short of data it never knew it was
missing, and nothing in the capture flags it, because the capture is what did
not write it.

    vglc-scan.py CAP.vglc                       header, per-entry totals
    vglc-scan.py CAP.vglc --entry grLfbLock     every call, with args, by frame
    vglc-scan.py CAP.vglc --lfb                 lock/unlock/region, by frame
    vglc-scan.py CAP.vglc --blobless            calls whose payload rule
                                                produced NO blob: the gap
    vglc-scan.py CAP.vglc --frames 0:500        bound any of the above

It reads the file, it does not run anything, so it works on Linux against a
capture taken on Windows. It is deliberately independent of the C reader in
tools/glide-replay.c: two implementations of one format disagreeing is a
finding, and one of them is this file.
"""
import argparse
import mmap
import struct
import sys

VGLC_MAGIC = 0x434C4756
VGLC_CONTROL = 0xFFFF
VGLC_CONTROL_ARGS = 16
VGLC_FRAME, VGLC_LFB, VGLC_END = 1, 2, 3
TAG_TEXDATA, TAG_LFB = 0x100, 0x101

# capture.h's payload enum, in its declared order. Kept here as names only:
# this tool never acts on a rule, it only reports which one applied.
PAYLOAD = ["PL_NONE", "PL_V2", "PL_V3", "PL_VLIST", "PL_FOG", "PL_TEXDOWN",
           "PL_TEXINFO3", "PL_TEXINFO1", "PL_TEXPART", "PL_TEXTABLE",
           "PL_STATE_IN", "PL_STATE_OUT", "PL_HWCFG", "PL_CALLBACK",
           "PL_LFBINFO", "PL_LFBUNLOCK", "PL_WINOPEN", "PL_SWAP",
           "PL_VIDTIMING"]

# The rules that mean "this call references memory the capture is supposed to
# have recorded". A call with one of these and no blob is a hole.
WANTS_BLOB = {"PL_V2", "PL_V3", "PL_VLIST", "PL_FOG", "PL_TEXDOWN",
              "PL_TEXINFO3", "PL_TEXINFO1", "PL_TEXPART", "PL_TEXTABLE",
              "PL_VIDTIMING"}


class Capture:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        (magic, version, header_bytes, entry_count, vertex_bytes, flags,
         _r0, _r1) = struct.unpack_from("<8I", self.m, 0)
        if magic != VGLC_MAGIC:
            sys.exit(f"{path}: not a .vglc capture")
        self.version = version
        self.vertex_bytes = vertex_bytes
        self.title = self.m[32:64].split(b"\0")[0].decode("latin-1")
        self.backend = self.m[64:128].split(b"\0")[0].decode("latin-1")
        self.entries = []
        p = 128
        for _ in range(entry_count):
            argbytes, payload, namelen = struct.unpack_from("<HBB", self.m, p)
            name = self.m[p + 4:p + 4 + namelen].decode("latin-1")
            self.entries.append((name, argbytes, payload))
            p += (4 + namelen + 3) & ~3
        self.header_bytes = header_bytes
        self.size = self.m.size()

    def records(self):
        """Yield (idx, name, payload_name, args, blobs) in stream order.

        `blobs` is a list of (tag, size, offset). Control records come back
        with idx == VGLC_CONTROL and name 'control'.
        """
        m, p, end = self.m, self.header_bytes, self.size
        while p + 4 <= end:
            idx, nblobs = struct.unpack_from("<HH", m, p)
            p += 4
            if idx == VGLC_CONTROL:
                name, pay, nargs = "control", "-", VGLC_CONTROL_ARGS // 4
            else:
                if idx >= len(self.entries):
                    print(f"  stream corrupt at 0x{p:x}: entry {idx}",
                          file=sys.stderr)
                    return
                name, argbytes, payload = self.entries[idx]
                pay = PAYLOAD[payload] if payload < len(PAYLOAD) else str(payload)
                nargs = argbytes // 4
            if p + nargs * 4 > end:
                return
            args = struct.unpack_from(f"<{nargs}I", m, p) if nargs else ()
            p += nargs * 4
            blobs = []
            for _ in range(nblobs):
                if p + 8 > end:
                    return
                tag, size = struct.unpack_from("<II", m, p)
                p += 8
                blobs.append((tag, size, p))
                p += (size + 3) & ~3
            yield idx, name, pay, args, blobs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture")
    ap.add_argument("--entry", action="append", default=[],
                    help="dump every call to this entry point (repeatable)")
    ap.add_argument("--lfb", action="store_true",
                    help="every grLfbLock/Unlock and every captured region")
    ap.add_argument("--blobless", action="store_true",
                    help="calls whose payload rule produced no blob at all")
    ap.add_argument("--frames", default=None, metavar="A:B",
                    help="restrict output to frames [A,B)")
    args = ap.parse_args()

    lo, hi = 0, 1 << 30
    if args.frames:
        a, _, b = args.frames.partition(":")
        lo = int(a) if a else 0
        hi = int(b) if b else hi

    cap = Capture(args.capture)
    print(f"capture   {args.capture}")
    print(f"  title   {cap.title}, taken through {cap.backend}")
    print(f"  v{cap.version}, {len(cap.entries)} entries, "
          f"GrVertex {cap.vertex_bytes} bytes, "
          f"{cap.size / (1024 * 1024):.1f} MB")

    frame = 0
    calls = {}
    blobbed = {}
    payload_bytes = {}
    blobless = {}
    n_records = n_frames = 0
    lfb_lock = lfb_unlock = lfb_region = 0
    lfb_lines = []
    ended = None

    for idx, name, pay, a, blobs in cap.records():
        n_records += 1
        if idx == VGLC_CONTROL:
            kind = a[0]
            if kind == VGLC_FRAME:
                frame = a[1]
                n_frames += 1
            elif kind == VGLC_LFB:
                lfb_region += 1
                if args.lfb and lo <= frame < hi:
                    sz = blobs[0][1] if blobs else 0
                    lfb_lines.append(
                        f"  f{frame:<6d} REGION buffer={a[1]} stride={a[2]} "
                        f"origin={a[3]} {sz} bytes")
            elif kind == VGLC_END:
                ended = (a[1], a[2])
            continue

        calls[name] = calls.get(name, 0) + 1
        if blobs:
            blobbed[name] = blobbed.get(name, 0) + 1
            payload_bytes[name] = payload_bytes.get(name, 0) + sum(
                b[1] for b in blobs)
        elif pay in WANTS_BLOB:
            blobless[name] = blobless.get(name, 0) + 1

        if lo <= frame < hi:
            if name in args.entry:
                bl = " ".join(f"[tag {t:#x} {s}B]" for t, s, _ in blobs)
                av = " ".join(f"{v:#010x}" for v in a)
                print(f"  f{frame:<6d} {name}({av}) {bl}")
            if args.lfb and name in ("grLfbLock", "grLfbUnlock"):
                av = " ".join(f"{v:#010x}" for v in a)
                lfb_lines.append(f"  f{frame:<6d} {name}({av})")
        if name == "grLfbLock":
            lfb_lock += 1
        elif name == "grLfbUnlock":
            lfb_unlock += 1

    if args.lfb:
        print("\nLFB traffic:")
        for line in lfb_lines:
            print(line)

    print(f"\n{n_records} records, {n_frames} frame marks, "
          f"{'ends cleanly' if ended else 'NO END RECORD: truncated'}"
          + (f" ({ended[0]} records, {ended[1]} frames)" if ended else ""))
    print(f"grLfbLock {lfb_lock}, grLfbUnlock {lfb_unlock}, "
          f"captured regions {lfb_region}"
          + (f": {lfb_lock - lfb_region} locks left NO region"
             if lfb_lock != lfb_region else ""))

    print("\ncalls, and how many of them carried a payload blob:")
    for name in sorted(calls):
        b = blobbed.get(name, 0)
        note = ""
        if name in blobless:
            note = f"   <<< {blobless[name]} call(s) with NO blob"
        pb = payload_bytes.get(name, 0)
        print(f"  {calls[name]:9d}  {name:<34s} blobs {b:9d}  "
              f"{pb / (1024 * 1024):9.2f} MB{note}")

    if args.blobless:
        print("\nPayload rules that produced nothing: each is a call whose "
              "referenced memory the replay will not have:")
        if not blobless:
            print("  (none)")
        for name in sorted(blobless):
            print(f"  {blobless[name]:9d}  {name}")


if __name__ == "__main__":
    main()
