#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""embed-text.py -- embed a UTF-8 text file as a NUL-terminated C byte array."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("symbol")
    args = ap.parse_args()

    data = args.input.read_bytes() + b"\0"
    lines = [
        "/* Generated from %s; do not edit. */" % args.input.as_posix(),
        "static const unsigned char %s[] = {" % args.symbol,
    ]
    for off in range(0, len(data), 16):
        chunk = data[off : off + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.extend(("};", ""))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="ascii", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
