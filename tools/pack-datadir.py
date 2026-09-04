#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""pack-datadir.py -- point a freshly seeded pack at data the user already has.

`make pack` stages a run directory at run/vcthunder/. When someone has just
ingested their CHDs into the repository's own data/, that directory holds
several gigabytes, and copying it into the pack would duplicate all of it for
no reason. The pack's ini has a `data_dir` key; setting it is the whole job.

Two things here are deliberate and were both learned by getting them wrong:

  * the replacement is NOT a regex replacement. A Windows path is full of
    backslashes and `re.sub` reads those as escapes in the replacement string:
    "C:\\claude\\..." raises `bad escape \\c`.
  * the file is written only after the new text exists. The first version
    opened it for writing and THEN built the replacement, so the exception
    above left a zero-byte ini behind.
"""

import os
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: pack-datadir.py <pack.ini> <data-dir>")
    ini, data = sys.argv[1], sys.argv[2]
    if not os.path.isfile(ini):
        raise SystemExit(f"no such ini: {ini}")

    out, seen = [], False
    with open(ini, encoding="utf-8") as f:
        for line in f:
            stripped = line.lstrip()
            if stripped.startswith("data_dir") and "=" in stripped:
                out.append(f"data_dir = {data}\n")
                seen = True
            else:
                out.append(line)
    if not seen:
        raise SystemExit(f"{ini} has no data_dir key to set")

    with open(ini, "w", encoding="utf-8", newline="") as f:
        f.writelines(out)
    print(f"  data_dir -> {data}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
