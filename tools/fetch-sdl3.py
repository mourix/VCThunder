#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""fetch-sdl3.py -- fetch the exact 32-bit MinGW SDL3 SDK used by vcglide.

The package goes under build/deps/ in this repository, always. Its digest is
checked before extraction; an existing valid installation is left untouched.
"""

import argparse
import hashlib
import pathlib
import shutil
import tarfile
import tempfile
import urllib.request


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

VERSION = "3.4.10"
ARCHIVE = f"SDL3-devel-{VERSION}-mingw.tar.gz"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/{ARCHIVE}"
SHA256 = "39dd2ac370bf33d6332a21ed768d8d49c37cc6f3211d788ead765102722639a8"


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    # --into is the directory that will CONTAIN SDL3/, and it defaults to this
    # repository's build/deps. A dependency must never come from whatever
    # directory happens to sit beside the clone, so there is no probe and no
    # fallback: the default is anchored to REPO_ROOT, not to the caller's cwd,
    # so running this from anywhere lands it where the Makefile expects.
    parser.add_argument("--into", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "deps",
                        help="directory to place SDL3/ under (default: build/deps)")
    args = parser.parse_args()
    root = args.into.resolve() / "SDL3"
    sdk = root / f"SDL3-{VERSION}" / "i686-w64-mingw32"
    runtime = sdk / "bin" / "SDL3.dll"
    if runtime.is_file():
        print(f"SDL3 {VERSION} x86 is already present at {sdk}")
        return 0

    root.mkdir(parents=True, exist_ok=True)
    archive = root / ARCHIVE
    if not archive.is_file() or digest(archive) != SHA256:
        with tempfile.NamedTemporaryFile(dir=root, delete=False) as tmp:
            temp = pathlib.Path(tmp.name)
        try:
            print(f"downloading {URL}")
            with urllib.request.urlopen(URL) as response, temp.open("wb") as output:
                shutil.copyfileobj(response, output)
            actual = digest(temp)
            if actual != SHA256:
                raise SystemExit(f"SDL3 digest mismatch: expected {SHA256}, got {actual}")
            temp.replace(archive)
        finally:
            temp.unlink(missing_ok=True)
    with tarfile.open(archive, "r:gz") as bundle:
        resolved_root = root.resolve()
        for member in bundle.getmembers():
            target = (root / member.name).resolve()
            if target != resolved_root and resolved_root not in target.parents:
                raise SystemExit(f"refusing archive path outside SDL3 root: {member.name}")
        bundle.extractall(root)
    if not runtime.is_file():
        raise SystemExit(f"archive did not produce expected runtime {runtime}")
    print(f"installed SDL3 {VERSION} x86 at {sdk}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
