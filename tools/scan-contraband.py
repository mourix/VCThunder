#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""scan-contraband.py -- refuse to let anything that is not ours into the repo.

The primary boundary is the directory layout: game dumps, wrappers, VM images
and research caches live outside this repository, where no `git add` can reach
them. This is the second backstop, and it is built on a rule that needs no
corpus to maintain:

    THIS REPOSITORY CONTAINS UTF-8 TEXT AND ONE ICON. NOTHING ELSE.

Every kind of contraband anyone has actually worried about here (a game
executable, an asset archive, a disk image, a Glide wrapper, a NVRAM blob, a
screenshot with a texture in it) fails that rule without anyone having to
predict it, where a keyword list would produce false positives on our own
prose and still miss the actual risk.

Exit status is 1 on any finding, so `make check` and a pre-commit hook can use
it directly.
"""

import argparse
import sys
from pathlib import Path

# Not tracked, not published: build output, the profiles generated from the
# user's own dump, and logs.
# 'chd' holds what the user supplies: their own CHDs and chdman; see
# chd/README.md. It is documented, gitignored, and not ours, so it is not
# scanned.
#
# 'run' and 'data' are the user's too, and BOTH ARE FULL OF CONTRABAND BY
# DESIGN: run/ is the playable pack (three PE images, both game dumps, NVRAM)
# and data/ is the unpacked games. Both live inside this tree, so without
# these two entries the scan reports every file in them and `make check` fails
# on a repository that is perfectly clean.
SKIP_DIRS = {'build', 'generated', '__pycache__', '.git', 'chd', 'run', 'data'}

# The one binary this repository is allowed to contain. It is project-authored
# (tools/make-icon.py) and it has to be binary to be an icon.
BINARY_ALLOWLIST = {'res/vcthunder.ico'}

# Belt over the text rule: these never belong here whatever their content.
FORBIDDEN_SUFFIXES = {
    '.exe', '.dll', '.img', '.iso', '.rar', '.zip', '.7z', '.r2', '.csh',
    '.nvr', '.omf', '.bin', '.pdf', '.obj', '.lib', '.so', '.dylib',
    # A MAME CHD is the front door's INPUT and belongs in chd/, which is not
    # scanned. 1.5 GB of them were dropped in the tree once, untracked, one
    # `git add -A` from a public push; the text rule caught them, but name the
    # type as well.
    '.chd',
}

MAX_BYTES = 1 << 20      # 1 MiB. The largest legitimate file here is ~100 KB.


def walk(root):
    for path in sorted(root.rglob('*')):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.relative_to(root).parts):
            continue
        yield path


def check(path, rel):
    """Every reason this file may not be here, as a list of strings."""
    findings = []
    if path.suffix.lower() in FORBIDDEN_SUFFIXES:
        findings.append(f'forbidden type "{path.suffix}"')

    size = path.stat().st_size
    if size > MAX_BYTES:
        findings.append(f'{size / 1024:.0f} KiB exceeds the {MAX_BYTES // 1024} KiB limit')

    if rel in BINARY_ALLOWLIST:
        return findings

    raw = path.read_bytes()
    try:
        raw.decode('utf-8')
    except UnicodeDecodeError as e:
        what = 'a PE/COFF image' if raw[:2] == b'MZ' else 'not UTF-8 text'
        findings.append(f'{what} (byte {e.start}); this repository holds text '
                        f'and {"/".join(sorted(BINARY_ALLOWLIST))} only')
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root', nargs='?', type=Path,
                    default=Path(__file__).resolve().parent.parent,
                    help='directory to scan (default: the repository)')
    ap.add_argument('--quiet', action='store_true',
                    help='print findings only')
    args = ap.parse_args()

    root = args.root.resolve()
    n = bad = 0
    for path in walk(root):
        rel = path.relative_to(root).as_posix()
        n += 1
        for finding in check(path, rel):
            print(f'CONTRABAND  {rel}: {finding}')
            bad += 1

    if bad:
        print(f'\n{bad} finding(s) in {n} files. Nothing here may be published.')
        return 1
    if not args.quiet:
        print(f'clean: {n} files, all project-authored text'
              f' (+{len(BINARY_ALLOWLIST)} allow-listed icon)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
