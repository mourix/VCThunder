#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""stamp.py -- has anything this check reads changed since it last passed?

`make check` re-verifies two sets of inputs that cannot change between runs:
four CHDs against their own per-hunk checksums (8.1 s) and generated/ against
the two game dumps (10.4 s). That is 18.5 s of a 28 s loop spent re-reading
files that were last written months ago, and the cost is not the seconds -- it
is that a check nobody runs protects nothing.

So each of those two records a KEY when it passes, and re-reads its inputs only
when the key has moved. What goes into the key is named by the caller, in two
kinds, because the inputs are two kinds:

    --hash FILE...   content, SHA-256. For the checker itself and the files it
                     emits: small, in-tree, and a change to any of them must
                     invalidate the stamp or the check reports a stale pass on
                     a generator that no longer does the same thing.
    --stat FILE...   size and mtime. For the immutable multi-gigabyte inputs a
                     CHD or a dump is, where hashing to decide whether to read
                     costs more than reading.

**Say what --stat is worth.** Size plus mtime is not a content check: a file
rewritten to the same length with its mtime restored passes it, and so does
one on a filesystem with coarse timestamps. It is a cache key, not evidence.
Whatever it guards therefore stays runnable unconditionally -- `make
check-full` -- and the skip prints as a skip, never as a pass.

A named file that does not exist is part of the key too: `missing` is a state,
and a CHD appearing or a dump moving must both move the key.

    stamp.py check STAMP --hash A B --stat C     exit 0 fresh, 1 stale
    stamp.py write STAMP --hash A B --stat C     record; the check just passed

`check` prints when the stamp was last written, so the caller can say so.
"""
import hashlib
import os
import sys
import time

USAGE = "usage: stamp.py {check|write} STAMP [--hash FILE...] [--stat FILE...]"


def split_inputs(argv):
    """Two lists, in argument order, so the key is stable across invocations."""
    hashed, stat, cur = [], [], None
    for a in argv:
        if a == "--hash":
            cur = hashed
        elif a == "--stat":
            cur = stat
        elif cur is None:
            sys.exit(f"stamp.py: {a} comes before --hash or --stat\n{USAGE}")
        else:
            cur.append(a)
    return sorted(set(hashed)), sorted(set(stat))


def key(hashed, stat):
    """One line per input, digested. The lines are sorted, so the key does not
    depend on the order a wildcard happened to expand in."""
    h = hashlib.sha256()
    for p in hashed:
        try:
            with open(p, "rb") as fh:
                digest = hashlib.sha256(fh.read()).hexdigest()
        except OSError:
            digest = "missing"
        h.update(f"hash {p} {digest}\n".encode())
    for p in stat:
        try:
            st = os.stat(p)
            mark = f"{st.st_size} {st.st_mtime_ns}"
        except OSError:
            mark = "missing"
        h.update(f"stat {p} {mark}\n".encode())
    return h.hexdigest()


def main(argv):
    if len(argv) < 2 or argv[0] not in ("check", "write"):
        sys.exit(USAGE)
    action, path = argv[0], argv[1]
    hashed, stat = split_inputs(argv[2:])
    if not hashed and not stat:
        sys.exit("stamp.py: a stamp over no inputs would never go stale")
    k = key(hashed, stat)

    if action == "write":
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(k + "\n" + time.strftime("%Y-%m-%d %H:%M:%S") + "\n")
        return 0

    try:
        with open(path, encoding="utf-8") as fh:
            recorded = fh.read().splitlines()
    except OSError:
        return 1
    if not recorded or recorded[0] != k:
        return 1
    print(recorded[1] if len(recorded) > 1 else "an earlier run")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
