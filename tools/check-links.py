#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""check-links.py -- every relative link in the tree's markdown resolves.

Runs over a tree as it was actually assembled and is closed: a link either
resolves against the files that are there, or this fails. Image embeds are
checked the same way, because a missing image is a broken link too.

Exit status is 1 on any finding.
"""
import os
import re
import sys
import urllib.parse

# [text](target). Image embeds are checked the same way, which is what we want:
# a missing image is a broken link too.
LINK = re.compile(r'\[[^\]]*\]\(([^)]+)\)')

SKIP_DIRS = {'.git', 'build', 'node_modules', '__pycache__', 'archive',
             'data', 'run', 'generated'}

# A tree may hold documents authored for a DIFFERENT tree's root, kept in place
# only so they are version-controlled. Their links are relative to where they
# LAND, not to where they sit, so they cannot resolve in place and are
# meaningless to check here. They are checked where it counts: after staging,
# at their real paths, by this same script.
SKIP_RELDIRS = {os.path.join('docs', 'public')}


def targets(text):
    for m in LINK.finditer(text):
        t = m.group(1).strip()
        # An inline title: [x](path "title").
        if ' ' in t and not t.startswith('<'):
            t = t.split(' ', 1)[0]
        t = t.strip('<>')
        if not t or t.startswith('#'):
            continue
        if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', t):   # http:, mailto:, ...
            continue
        yield m.group(0), t


def main(root):
    root = os.path.abspath(root)
    broken = []
    checked = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        reldir = os.path.relpath(dirpath, root)
        if any(reldir == s or reldir.startswith(s + os.sep)
               for s in SKIP_RELDIRS):
            continue
        for fn in filenames:
            if not fn.endswith('.md'):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root)
            with open(path, encoding='utf-8') as fh:
                text = fh.read()
            for whole, target in targets(text):
                checked += 1
                # Strip the fragment, then percent-decode: a link may be
                # written to a path containing a space.
                p = urllib.parse.unquote(target.split('#', 1)[0])
                if not p:
                    continue
                if not os.path.exists(os.path.join(dirpath, p)):
                    broken.append((rel, whole))

    if broken:
        print(f"BROKEN LINKS: {len(broken)} of {checked} relative links do not "
              f"resolve under {root}", file=sys.stderr)
        for rel, whole in broken:
            print(f"  {rel}: {whole}", file=sys.stderr)
        print(file=sys.stderr)
        print("Either publish the document, or make the citation plain text so "
              "it survives as an attribution.", file=sys.stderr)
        return 1

    print(f"links: {checked} relative link(s), all resolve")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else '.'))
