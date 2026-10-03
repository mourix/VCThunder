#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""pack-datadir.py -- point a freshly seeded pack at data the user already has.

`make pack` copies the ingested data/ into the pack by default, so the pack
stands alone. `make pack LINK_DATA=1` on the first pack runs this instead: the
data stays where the ingest wrote it and the pack's ini points at it, which
saves its 1.4 GB but breaks the pack if the clone moves. The ini has a
`data_dir` key; setting it is the whole job.
"""

import os
import sys


# TWO THINGS HERE ARE DELIBERATE. The replacement is NOT a regex replacement: a
# Windows path is full of backslashes and re.sub reads those as escapes in the
# replacement string, so "C:\\data\\..." raises `bad escape \\d`. And the file
# is written only after the new text exists, or that exception leaves a
# zero-byte ini behind.
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
