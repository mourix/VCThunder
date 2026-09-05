#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""ingest-chd.py -- turn a MAME CHD into the data directory this host reads.

THIS IS THE FRONT DOOR. A user who has the games as MAME CHDs has a compressed
hard-disk image; this host wants a directory of files. Everything between those
two is here, in one command:

    python3 tools/ingest-chd.py --title hydro --chd hydrthnd.chd --out data
    python3 tools/ingest-chd.py --title offroad --chd offrthnd.chd --out data

WHAT THE LAYOUT HAS TO BE, AND WHY IT IS FLATTER THAN IT LOOKS
    The cabinet's disk carries several partitions (C:, D:, sometimes E:), and
    the game asks for `C:\\HT.R2` and `D:\\SAMAZAA0.CSH` by absolute path. But
    `fs_map_path()` in src/fs_map.c strips ANY drive specifier and joins the rest
    onto the data directory, so every drive lands in the SAME place:

        C:\\HYDRO.EXE      ->  data/hydro/HYDRO.EXE
        D:\\SAMAZAA0.CSH   ->  data/hydro/SAMAZAA0.CSH

    So this merges all partitions into one flat directory per title. Directories
    INSIDE a partition are preserved: only the drive letter goes.

THE CHD IS READ HERE, AND chdman IS ONLY THE FALLBACK
    Leaning on chdman costs the user twice over: a 100 MB MAME download to run
    one command once, and the disk to hold a whole uncompressed drive, because
    chdman writes the entire image -- 3.2 GB for Hydro, 6.4 GB for Offroad --
    before this reads ~1 GB of files out of it and deletes the rest.

    `tools/chd.py` reads the CHD directly, decoding only the hunks the files
    occupy, with nothing but the Python standard library. The container is
    self-checking: every hunk carries a CRC-16 of its own decoded bytes and the
    header carries the SHA-1 of the whole image, so `python3 tools/chd.py
    --verify <chd>` proves the reader against the file itself, without MAME.
    Both of the user's CHDs pass, every hunk and both whole images, and the
    files this writes out of them are byte-identical to chdman's.

    chdman is still used, and is still the right thing to use, for anything this
    reader declines: a CHD older than v5, or one with a parent. Those raise
    ChdUnsupported, and the fallback below says so and runs chdman.

    The FAT reader below IS ours too, because the alternatives all cost the user
    an install (`7z`, `mtools`) or root (`mount -o loop`), and Python is already
    required to build.

NO GAME MATERIAL IS IN THIS REPOSITORY AND NONE EVER WILL BE. This reads the
user's own lawful dump and writes it to the user's own disk. It then hashes the
result against the build registered in scripts/titles.py and says plainly
whether it is the supported one: addresses are per-title DATA, so a different
build is not a patch away, it needs its own registered entry.
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from titles import TITLES, get, ets_titles                    # noqa: E402
from chd import ChdImage, ChdUnsupported, ChdError            # noqa: E402

SECTOR = 512

# MBR partition types this reader understands. Anything else in the table is
# reported and skipped rather than guessed at: a partition we cannot read is
# a fact worth printing, not a silent omission.
FAT_TYPES = {
    0x01: "FAT12",
    0x04: "FAT16 <32M",
    0x06: "FAT16",
    0x0B: "FAT32",
    0x0C: "FAT32 LBA",
    0x0E: "FAT16 LBA",
}

ATTR_VOLUME = 0x08
ATTR_DIR = 0x10
ATTR_LFN = 0x0F


# ---- the container -------------------------------------------------------

def is_wsl():
    """WSL, where a Windows .exe and this interpreter do NOT share a filesystem
    view."""
    if os.path.exists("/proc/sys/fs/binfmt_misc/WSLInterop"):
        return True
    try:
        return "microsoft" in os.uname().release.lower()
    except AttributeError:          # not POSIX at all
        return False


def host_path(p):
    """Translate a WSL path for a Windows program, via WSL's own translator.

    THIS IS NOT COSMETIC. `docs/building.md` tells you to build from WSL, and
    the chdman anyone has is a Windows binary, and a Windows binary handed
    `/mnt/c/...` reports "No such file or directory" for a file that is plainly
    there. Guessing the mapping ourselves would be wrong for anyone whose mount
    root is not /mnt; wslpath knows it and we do not.
    """
    r = subprocess.run(["wslpath", "-w", os.path.abspath(p)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"wslpath failed for {p}: {r.stderr.strip()}")
    return r.stdout.strip()


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHD_DIR = os.path.join(REPO_ROOT, "chd")


def find_chdman(explicit=None):
    """Locate chdman WITHOUT asking anyone to edit PATH.

    Order: --chdman, then the repository's own chd/, then PATH. chd/ is the
    "right place" the error below names -- the same folder the CHDs go in: it is
    gitignored and skipped by the contraband scanner precisely so that dropping
    a binary there is safe. An error that says "not on PATH" and stops is not
    help; this one says where to put the file and where to get it.
    """
    if explicit:
        if os.path.isfile(explicit):
            return os.path.abspath(explicit)
        raise SystemExit(f"--chdman: no such file: {explicit}")

    for name in ("chdman.exe", "chdman"):
        local = os.path.join(CHD_DIR, name)
        if os.path.isfile(local):
            return local

    found = shutil.which("chdman") or shutil.which("chdman.exe")
    if found:
        return found

    raise SystemExit(
        "chdman not found, and this CHD needs it.\n\n"
        "Most CHDs are read here directly, with nothing installed; this one is\n"
        "not one of those (the reason is printed above). chdman opens it, and\n"
        "it ships inside the MAME binary distribution.\n"
        "You do not need to change PATH or install MAME: copy the single\n"
        "file in beside the CHDs:\n\n"
        f"    {os.path.join(CHD_DIR, 'chdman.exe')}      (Windows)\n"
        f"    {os.path.join(CHD_DIR, 'chdman')}          (Linux)\n\n"
        "Get it from https://www.mamedev.org/release.html or\n"
        "https://github.com/mamedev/mame/releases; unpack the archive and\n"
        "take chdman only. If MAME is already installed, its copy works.\n\n"
        "Alternatives, if you would rather not:\n"
        "    --chdman <path>   point at it wherever it already is\n"
        "    --img <file>      skip chdman: extract the image yourself with\n"
        "                      `chdman extracthd -i <chd> -o game.img`\n"
        "See chd/README.md.\n")


def chdman_extract(chd, img, chdman=None):
    """Run MAME's own extractor. Its errors are surfaced verbatim: chdman knows
    far more about why a CHD will not open than we ever should."""
    exe = find_chdman(chdman)

    # A Windows chdman under WSL needs Windows paths for BOTH arguments. It
    # also cannot see /tmp at all, which is why the image is staged beside the
    # CHD rather than in a temporary directory; see main().
    args_chd, args_img = chd, img
    if exe.lower().endswith(".exe") and is_wsl():
        args_chd, args_img = host_path(chd), host_path(img)
        print(f"chdman is a Windows binary under WSL; passing host paths")

    print(f"chdman: {exe}")
    print(f"extracting {chd} -> {img}  (this writes the full uncompressed disk)")
    r = subprocess.run([exe, "extracthd", "-i", args_chd, "-o", args_img])
    if r.returncode != 0:
        raise SystemExit(
            f"chdman exited {r.returncode}. Its output is above.\n"
            "If your chdman spells the subcommand differently (older builds use\n"
            "`extractraw`), run it yourself and pass the result with --img.")
    return img


def _as_file(src):
    """(file object, whether we opened it). A path opens; anything that already
    reads is passed straight through, so one open CHD serves identify_title()
    and from_image() without decoding and CRC-checking its map twice -- two
    seconds each time for Offroad's 1.6 million hunks."""
    if hasattr(src, "read"):
        return src, False
    return open(src, "rb"), True


def _decoded(src):
    """How much of the disk was actually decoded, when it was read as a CHD.

    Worth printing: it is the difference between this and chdman, and it is the
    number to look at if an ingest is slower than it should be -- a hunk that
    falls out of the cache before the file that shares it is copied is decoded
    twice."""
    if not isinstance(src, ChdImage):
        return ""
    mb = src.chd.hunks_decoded * src.chd.hunkbytes / (1 << 20)
    return f"; {src.chd.hunks_decoded:,} hunks decoded ({mb:,.0f} MB of the disk)"


def open_chd(path, chdman=None, force_chdman=False, jobs=None):
    """Open a CHD for reading, by whichever route works.

    Returns (file object, image path or None). The image path is not None only
    when chdman had to run, and it is then the caller's to delete.

    The native reader is tried first and is what should normally serve. chdman
    is the fallback for what it declines -- a pre-v5 CHD, or one with a parent
    -- and forcing it with --chdman remains available for anyone who would
    rather trust MAME's own code than ours.
    """
    if not force_chdman:
        try:
            image = ChdImage(path, jobs=jobs)
            cores = image.chd.jobs
            print(f"reading {os.path.basename(path)} directly "
                  f"({image.size / (1 << 30):.1f} GB image, no extraction) "
                  f"on {cores} core{'' if cores == 1 else 's'}")
            return image, None
        except ChdUnsupported as exc:
            print(f"this CHD needs chdman: {exc}")
        except ChdError as exc:
            print(f"reading this CHD failed: {exc}")
            print("falling back to chdman.")
    img = path + ".img"
    chdman_extract(path, img, chdman)
    return open(img, "rb"), img


# ---- the partition table -------------------------------------------------

# Containers, not filesystems: an entry of this type holds a chain of logical
# volumes rather than data of its own.
EXTENDED_TYPES = {0x05, 0x0F, 0x85}


def read_extended(f, ext_base):
    """Walk the EBR chain of an extended partition.

    The two entries in an EBR are relative to DIFFERENT bases, and that is the
    classic way to read this structure wrongly: entry 0 is a logical volume at
    an offset relative to THIS EBR, while entry 1 points at the next EBR at an
    offset relative to the extended partition's own start.
    """
    out = []
    cur, seen = ext_base, set()
    while cur and cur not in seen:
        seen.add(cur)                    # a malformed chain must not loop forever
        f.seek(cur * SECTOR)
        ebr = f.read(SECTOR)
        if len(ebr) < SECTOR or ebr[510:512] != b"\x55\xaa":
            break
        ptype = ebr[446 + 4]
        lba, count = struct.unpack_from("<II", ebr, 446 + 8)
        if ptype and count:
            out.append((ptype, cur + lba))
        ntype = ebr[462 + 4]
        nlba, ncount = struct.unpack_from("<II", ebr, 462 + 8)
        cur = ext_base + nlba if ntype and ncount else None
    return out


def read_partitions(f):
    """Every partition entry: the four primary slots, and the logical volumes
    inside any extended one.

    THE CABINET DISKS ARE NOT PLAIN PRIMARY FAT VOLUMES, however much chasing
    an extended partition looks like support for a layout no dump has. Hydro's
    own dump settles it: drive C is a primary FAT16, and drives D and E are
    logical volumes inside a type-0x05 extended partition. A reader that skips
    the chain writes 5 files, leaves every audio sample behind, and still
    reports the executable's hash as correct: a data directory that passes
    verification and cannot play. Do not un-chase this.
    """
    f.seek(0)
    mbr = f.read(SECTOR)
    if len(mbr) < SECTOR or mbr[510:512] != b"\x55\xaa":
        # A partitionless image is legal and some dumps are exactly that.
        return [(None, 0)]
    out = []
    for i in range(4):
        e = mbr[446 + i * 16: 446 + (i + 1) * 16]
        ptype = e[4]
        lba, count = struct.unpack_from("<II", e, 8)
        if ptype == 0 or count == 0:
            continue
        if ptype in EXTENDED_TYPES:
            out.extend(read_extended(f, lba))
            continue
        out.append((ptype, lba))
    if not out:
        return [(None, 0)]
    return out


# ---- FAT -----------------------------------------------------------------

class Fat:
    """A FAT12/16/32 volume, read-only, streaming.

    Files here reach 1 GB (Offroad's GD3.R2), so nothing is ever held whole in
    memory: chains are walked cluster by cluster straight into the output file.
    """

    def __init__(self, f, part_lba):
        self.f = f
        self.base = part_lba * SECTOR
        b = self._sec(0)

        self.bps = struct.unpack_from("<H", b, 0x0B)[0]
        self.spc = b[0x0D]
        self.reserved = struct.unpack_from("<H", b, 0x0E)[0]
        self.nfats = b[0x10]
        self.root_entries = struct.unpack_from("<H", b, 0x11)[0]
        total16 = struct.unpack_from("<H", b, 0x13)[0]
        spf16 = struct.unpack_from("<H", b, 0x16)[0]
        total32 = struct.unpack_from("<I", b, 0x20)[0]
        spf32 = struct.unpack_from("<I", b, 0x24)[0]

        if not self.bps or not self.spc:
            raise ValueError("not a FAT volume (zero bytes/sector or /cluster)")

        self.spf = spf16 or spf32
        self.total = total16 or total32
        self.root_sectors = ((self.root_entries * 32) + self.bps - 1) // self.bps
        self.first_data = self.reserved + self.nfats * self.spf + self.root_sectors
        clusters = (self.total - self.first_data) // self.spc

        # The standard determination. It is by CLUSTER COUNT, not by the
        # partition type byte: the type byte is a hint the formatter wrote and
        # is wrong often enough to matter.
        self.bits = 12 if clusters < 4085 else (16 if clusters < 65525 else 32)
        self.root_cluster = struct.unpack_from("<I", b, 0x2C)[0] if self.bits == 32 else 0
        self.label = b[0x2B:0x36].decode("latin1").strip() if self.bits != 32 \
            else b[0x47:0x52].decode("latin1").strip()
        self._fat = self._read_fat()

    def _sec(self, n, count=1):
        self.f.seek(self.base + n * SECTOR)
        return self.f.read(count * SECTOR)

    def _read_fat(self):
        return self._sec(self.reserved, self.spf)

    def _next(self, cl):
        if self.bits == 16:
            v = struct.unpack_from("<H", self._fat, cl * 2)[0]
            return None if v >= 0xFFF8 else v
        if self.bits == 32:
            v = struct.unpack_from("<I", self._fat, cl * 4)[0] & 0x0FFFFFFF
            return None if v >= 0x0FFFFFF8 else v
        # FAT12: 1.5 bytes per entry, nibble-packed by parity.
        off = cl + (cl // 2)
        v = struct.unpack_from("<H", self._fat, off)[0]
        v = (v >> 4) if (cl & 1) else (v & 0x0FFF)
        return None if v >= 0xFF8 else v

    def _chain(self, cl):
        seen = set()
        while cl is not None and cl >= 2:
            if cl in seen:                       # a corrupt image must not hang
                raise ValueError(f"cluster loop at {cl}")
            seen.add(cl)
            yield cl
            cl = self._next(cl)

    def _cluster_bytes(self, cl):
        sec = self.first_data + (cl - 2) * self.spc
        return self._sec(sec, self.spc)

    def _root_bytes(self):
        if self.bits == 32:
            return b"".join(self._cluster_bytes(c)
                            for c in self._chain(self.root_cluster))
        return self._sec(self.reserved + self.nfats * self.spf, self.root_sectors)

    def _dir_bytes(self, cl):
        return b"".join(self._cluster_bytes(c) for c in self._chain(cl))

    @staticmethod
    def _short_name(raw):
        stem = raw[0:8].decode("latin1").rstrip()
        ext = raw[8:11].decode("latin1").rstrip()
        if raw[0] == 0x05:                        # 0xE5 escaped as 0x05
            stem = "\xe5" + stem[1:]
        return f"{stem}.{ext}" if ext else stem

    def entries(self, blob):
        """Yield (name, attr, cluster, size) for one directory's bytes.

        Long filenames are assembled when present, because a dump made by a
        Windows tool can carry them and the game's own 8.3 name is then only
        half the truth. The arcade's own files are all 8.3, so this is
        insurance, not the common path.
        """
        lfn = []
        for i in range(0, len(blob), 32):
            e = blob[i:i + 32]
            if len(e) < 32 or e[0] == 0x00:
                break
            if e[0] == 0xE5:                      # deleted
                lfn = []
                continue
            attr = e[0x0B]
            if attr == ATTR_LFN:
                seq = e[0] & 0x3F
                chars = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le",
                                                               "ignore")
                chars = chars.split("\uffff")[0].split("\x00")[0]
                lfn.append((seq, chars))
                continue
            if attr & ATTR_VOLUME and not attr & ATTR_DIR:
                lfn = []
                continue
            name = ("".join(c for _, c in sorted(lfn)) if lfn
                    else self._short_name(e[0:11]))
            lfn = []
            cl = (struct.unpack_from("<H", e, 0x1A)[0] |
                  (struct.unpack_from("<H", e, 0x14)[0] << 16
                   if self.bits == 32 else 0))
            size = struct.unpack_from("<I", e, 0x1C)[0]
            yield name, attr, cl, size

    def walk(self, blob=None, prefix=""):
        """Yield (relative path, cluster, size) for every file in the volume."""
        if blob is None:
            blob = self._root_bytes()
        for name, attr, cl, size in self.entries(blob):
            if name in (".", ".."):
                continue
            path = f"{prefix}{name}"
            if attr & ATTR_DIR:
                if cl >= 2:
                    yield from self.walk(self._dir_bytes(cl), path + "/")
            else:
                yield path, cl, size

    def _cluster_range(self, cl):
        sec = self.first_data + (cl - 2) * self.spc
        return self.base + sec * SECTOR, self.spc * SECTOR

    def sha256_of(self, cl, size):
        """Hash one file's chain without writing it anywhere. This is how a
        CHD's build is known before anything is extracted out of it."""
        h = hashlib.sha256()
        left = size
        if size == 0 or cl < 2:
            return h.hexdigest()
        for c in self._chain(cl):
            chunk = self._cluster_bytes(c)
            if left < len(chunk):
                chunk = chunk[:left]
            h.update(chunk)
            left -= len(chunk)
            if left <= 0:
                break
        return h.hexdigest()

    def copy_out(self, cl, size, dst):
        """Stream one file's chain into dst. Never buffers the whole file.

        The chain is known before any of it is read, so a source that can
        decode ahead is told the whole plan first. For a CHD that is the
        difference between one core and all of them: the copy below asks for
        one cluster at a time, and nothing about that loop can be parallel.
        """
        plan = getattr(self.f, "plan_reads", None)
        if plan is not None and size and cl >= 2:
            plan(self._cluster_range(c) for c in self._chain(cl))
        with open(dst, "wb") as o:
            left = size
            if size == 0 or cl < 2:
                return
            for c in self._chain(cl):
                chunk = self._cluster_bytes(c)
                if left < len(chunk):
                    chunk = chunk[:left]
                o.write(chunk)
                left -= len(chunk)
                if left <= 0:
                    break


# ---- the three input stages ----------------------------------------------

def from_image(img, out):
    """Every readable FAT partition, merged flat into `out`.

    `img` is a path, or any object that seeks and reads -- which is how a CHD
    is ingested with no image on disk at all: ChdImage is exactly that object.

    Returns (files written, problems). A partition this reader could not handle
    is a PROBLEM and not a footnote: the run that found the extended-partition
    bug printed "skipped" for the volume holding every audio sample and then
    reported success, because only the executable is hashed. Anything unread is
    now surfaced to the exit status.
    """
    n, problems = 0, []
    f, opened = _as_file(img)
    try:
        parts = read_partitions(f)
        print(f"partition table: {len(parts)} entr{'y' if len(parts)==1 else 'ies'}")
        for ptype, lba in parts:
            label = FAT_TYPES.get(ptype, f"type 0x{ptype:02X}" if ptype else "no MBR")
            if ptype is not None and ptype not in FAT_TYPES:
                print(f"  lba {lba:>10}  {label}; not a FAT type, skipped")
                problems.append(f"partition at lba {lba} ({label}) was not read")
                continue
            try:
                v = Fat(f, lba)
            except Exception as exc:              # noqa: BLE001 (reported, not raised
                print(f"  lba {lba:>10}  {label}) not readable: {exc}")
                problems.append(f"partition at lba {lba} ({label}) was not readable: {exc}")
                continue
            files = list(v.walk())
            print(f"  lba {lba:>10}  {label} -> FAT{v.bits}"
                  f"{', label ' + v.label if v.label else ''}"
                  f"  {len(files)} files")
            for path, cl, size in files:
                dst = os.path.join(out, path.replace("/", os.sep))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                v.copy_out(cl, size, dst)
                n += 1
    finally:
        if opened:
            f.close()
    return n, problems


def from_tree(tree, out):
    """Merge an already-extracted drive tree: <tree>/C, <tree>/D, ...

    This is the path for someone who extracted by hand years ago, and it is how
    the merge and the flattening are regression-tested against a known-good
    result; see docs/building.md.
    """
    n = 0
    roots = sorted(d for d in os.listdir(tree)
                   if os.path.isdir(os.path.join(tree, d)))
    print(f"drive tree: {', '.join(roots) or '(none)'}")
    for drive in roots:
        src = os.path.join(tree, drive)
        count = 0
        for dirpath, _dirs, names in os.walk(src):
            for name in names:
                rel = os.path.relpath(os.path.join(dirpath, name), src)
                dst = os.path.join(out, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(os.path.join(dirpath, name), dst)
                count += 1
        print(f"  {drive}: {count} files")
        n += count
    return n


# ---- verification --------------------------------------------------------

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(out, tid, offer_any=True, said_where="extracted"):
    """Hash the executable against the registered build.

    This is house rule 2 applied at the front door. Addresses and sizes are
    per-title DATA derived from one exact file; against another build they are
    plausible numbers pointing at the wrong instructions. So a hash miss is
    reported as an unsupported BUILD, not as a broken download.
    """
    meta = TITLES[tid]
    leaf = os.path.basename(meta["exe"])
    path = os.path.join(out, leaf)
    if not os.path.isfile(path):
        # Case-insensitive retry: FAT is case-preserving and dumps disagree.
        for name in os.listdir(out):
            if name.lower() == leaf.lower():
                path = os.path.join(out, name)
                break
    if not os.path.isfile(path):
        print(f"\nFAILED: no {leaf} in {out}")
        print("The image was read, but the game executable is not in it.")
        return False

    got = sha256(path)
    want = meta.get("sha256")
    print(f"\n{os.path.basename(path)}  {got}")
    if want and got == want:
        print(f"OK: this is the registered build ({meta['build']}).")
        parent = os.path.dirname(os.path.abspath(out))
        if os.path.realpath(parent) != os.path.realpath(os.path.join(REPO_ROOT, "data")):
            # Only when it is somewhere the build will not look by itself.
            print(f"Point the build at it with:  VCT_DUMPS={parent}")
        return True
    if want:
        print(f"expected  {want}")
        print(unsupported_build(tid, said_where, offer_any=offer_any))
    return False


SAID_WHERE = {
    # Before an ingest, off the executable hashed inside the CHD.
    "chd": "Nothing was extracted, and nothing is wrong\nwith this CHD. It holds",
    # After one: the backstop, and the only wording that existed at first.
    "extracted": "The files extracted cleanly: this is not a\ncorrupt dump. It is",
    # A directory that was already there, which nothing in this run wrote.
    "existing": "These files were already there and this run did\nnot write them. They are",
}


def unsupported_build(tid, said_where, offer_any=True):
    """The one refusal, worded for where it is said.

    Three places, and the difference between them is not decoration: telling
    someone their files "extracted cleanly" when nothing was extracted, or
    offering --any-build when the CHD beside them is the right build and it is
    the DIRECTORY that is stale, is advice that sends them the wrong way.
    """
    meta = TITLES[tid]
    return (f"\nUNSUPPORTED BUILD. "
            + SAID_WHERE[said_where]
            + f" a different build of {meta['name']} from the\n"
              f"one this host has address tables for ({meta['build']}).\n\n"
              f"Every address VCThunder patches is an offset into that exact\n"
              f"file, so another build is not a configuration away: it needs its\n"
              f"own entry in scripts/titles.py and its own `make defs` run.\n"
              f"Please open an issue quoting the hash above."
            + _trailer(said_where, offer_any))


def _trailer(said_where, offer_any):
    """What to do about it, which differs by which of the three this is.

    Nothing here is cosmetic. Offering --any-build to someone who just used it,
    or to someone whose CHD is fine and whose data directory is stale, is the
    wrong instruction in both cases.
    """
    if said_where == "existing":
        return ""                                 # the caller says what to do
    if offer_any:
        return ("\n\nTo unpack it anyway -- which is what registering a new "
                "build\nstarts with -- pass --any-build.")
    return ("\n\nThe files are unpacked, because you asked for them with "
            "--any-build.\nThey are not playable by this host until that build "
            "is registered.")


def build_gate(tid, exe_sha, any_build):
    """Decide whether to ingest at all, from the executable read in place.

    THE POINT IS THAT NOTHING IS WRITTEN. The hash comes out of the CHD in
    about a second; extracting first and refusing afterwards left a data
    directory that failed verification, and `--auto` then read that directory
    as done on the next run.
    """
    want = TITLES[tid].get("sha256")
    if not want or exe_sha == want or any_build:
        if want and exe_sha != want and any_build:
            print(f"  --any-build: unpacking a build this host has no tables "
                  f"for ({exe_sha[:16]}...)")
        return True
    print(f"\n  {os.path.basename(TITLES[tid]['exe'])}  {exe_sha}")
    print(f"  expected  {want}")
    print(unsupported_build(tid, "chd"))
    return False


# ---- the zero-knowledge path ---------------------------------------------

_PARENT = os.path.dirname(REPO_ROOT)
# The last two are the same convention one level out: a clone that lives beside
# the CHDs rather than above them is the normal shape when the repository is
# checked out inside a larger working directory, and this project's own is
# exactly that. Named directories only -- nothing here walks the parent.
CHD_SEARCH_ROOTS = [CHD_DIR, REPO_ROOT, os.path.join(REPO_ROOT, "chds"),
                    _PARENT, os.path.join(_PARENT, "chd"),
                    os.path.join(_PARENT, "chds")]


def find_chds():
    """Every .chd at or just below the repository root.

    chd/ is where chd/README.md asks for them, but a user who downloads two
    CHDs drops them wherever felt natural: the repo root, or the folder the
    browser made. Look in all of those rather than making them learn where we
    wanted them.
    """
    seen, out = set(), []
    for root in CHD_SEARCH_ROOTS:
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            full = os.path.join(root, name)
            if os.path.isfile(full) and name.lower().endswith(".chd"):
                real = os.path.realpath(full)
                if real not in seen:
                    seen.add(real)
                    out.append(full)
            elif os.path.isdir(full) and root == REPO_ROOT and name not in SKIP_SCAN:
                for sub in sorted(os.listdir(full)):
                    f2 = os.path.join(full, sub)
                    if os.path.isfile(f2) and sub.lower().endswith(".chd"):
                        real = os.path.realpath(f2)
                        if real not in seen:
                            seen.add(real)
                            out.append(f2)
    return out


SKIP_SCAN = {"build", "generated", ".git", "src", "renderer", "docs", "tests",
             "scripts", "tools", "res", "__pycache__"}


def identify(img):
    """(title, executable's SHA-256) for an image. Decided by what is ON it,
    never by its name.

    A downloaded CHD is called whatever the uploader called it. The executable
    leaf is the one thing that cannot be renamed without breaking the game, so
    that is what we look for -- and while we are there we hash it, because the
    build is the other thing a filename cannot be trusted for. Reading one
    executable out of a CHD costs a second; ingesting the wrong build costs the
    user the whole run and a confusing failure at the end of it.
    """
    want = {os.path.basename(TITLES[t]["exe"]).lower(): t for t in ets_titles()}
    f, opened = _as_file(img)
    try:
        for ptype, lba in read_partitions(f):
            if ptype is not None and ptype not in FAT_TYPES:
                continue
            try:
                vol = Fat(f, lba)
            except Exception:                     # noqa: BLE001
                continue
            try:
                entries = list(vol.walk())
            except Exception:                     # noqa: BLE001
                continue
            for path, cl, size in entries:
                leaf = os.path.basename(path).lower()
                if leaf in want:
                    return want[leaf], vol.sha256_of(cl, size)
    finally:
        if opened:
            f.close()
    return None, None


def identify_title(img):
    """Just the title, for callers that do not care which build it is."""
    return identify(img)[0]


def survey(chds, a):
    """What each CHD actually is: (path, title, exe hash, registered?).

    Opening every CHD before ingesting any of them costs a second each and
    buys the one decision that cannot be made from filenames: WHICH CHD OF A
    TITLE TO USE. Sorted order is not a fact about the disks: with `hydro.chd`
    and `hydro_100d.chd` in one directory, taking the first would ingest
    whichever sorted first and then skip the other, because the output
    directory would no longer be empty. If that was the unsupported build, the
    run ends with a data directory that fails verification and a perfectly good
    CHD sitting next to it, unread.
    """
    out = []
    for path in chds:
        src = img = None
        try:
            src, img = open_chd(path, a.chdman, force_chdman=bool(a.chdman),
                                jobs=a.jobs)
            title, exe = identify(src)
            registered = bool(title and exe == TITLES[title].get("sha256"))
            if title:
                build = TITLES[title]["build"]
                print(f"    {os.path.basename(path):<24} {TITLES[title]['name']}"
                      + (f", the registered build ({build})" if registered
                         else f", a DIFFERENT build (registered is {build})"))
            else:
                print(f"    {os.path.basename(path):<24} no game executable "
                      f"inside it")
            out.append((path, title, exe, registered))
        except ChdError as exc:
            print(f"    {os.path.basename(path):<24} not readable: {exc}")
            out.append((path, None, None, False))
        finally:
            if src is not None:
                src.close()
            if img and not a.keep_img and os.path.exists(img):
                os.unlink(img)
    return out


def choose(surveyed):
    """One CHD per title: the registered build if any of them is it.

    Returns (chosen, passed over) where passed over says why, because a CHD
    the run does not read is exactly the thing a user needs told."""
    chosen, skipped = {}, []
    for path, title, exe, registered in surveyed:
        if not title:
            skipped.append((path, "no game executable inside it"))
            continue
        have = chosen.get(title)
        if have is None or (registered and not have[3]):
            if have is not None:
                skipped.append((have[0], f"{TITLES[title]['name']}, superseded by "
                                         f"{os.path.basename(path)}"))
            chosen[title] = (path, title, exe, registered)
        else:
            same = exe == have[2]
            skipped.append((path, f"{TITLES[title]['name']}, "
                                  + ("the same build as " if same else
                                     "a different build; using ")
                                  + os.path.basename(have[0])))
    return chosen, skipped


def run_auto(a):
    """Find the CHDs, work out which game each one is, and ingest them all."""
    chds = find_chds()
    if not chds:
        raise SystemExit(
            "No .chd files found.\n\n"
            "Put your game CHDs in chd/ and run this again.\n"
            "Looked in:\n"
            + "".join(f"    {r}\n" for r in CHD_SEARCH_ROOTS)
            + f"    and one folder below {REPO_ROOT}\n")

    if a.chdman:
        find_chdman(a.chdman)                      # fail before any extraction
    print(f"found {len(chds)} CHD file(s); looking inside each one:")
    surveyed = survey(chds, a)
    chosen, skipped = choose(surveyed)
    if skipped:
        print("\nnot reading:")
        for path, why in skipped:
            print(f"    {os.path.basename(path):<24} {why}")
    print()

    done, failed = [], []
    for title in sorted(chosen):
        path, _t, exe, registered = chosen[title]
        print(f"=== {os.path.basename(path)}: {TITLES[title]['name']} ===")
        if not build_gate(title, exe, a.any_build):
            failed.append((path, f"not the registered build of "
                                 f"{TITLES[title]['name']}"))
            print()
            continue
        src = img = None
        try:
            out = os.path.join(os.path.abspath(a.out), title)
            if os.path.isdir(out) and os.listdir(out):
                # NOT "leaving it alone", which counted whatever was there as
                # done -- including an unsupported build a previous run had
                # written before refusing it. What is there gets hashed.
                print(f"  {out} already has files; checking what they are")
                if verify(out, title, offer_any=False, said_where="existing"):
                    done.append(title)
                else:
                    print(f"\n  This directory was not ingested by this run and "
                          f"is not the\n  registered build. Remove it and run "
                          f"again to replace it.")
                    failed.append((path, f"{out} holds files that do not verify"))
                print()
                continue
            src, img = open_chd(path, a.chdman, force_chdman=bool(a.chdman),
                                jobs=a.jobs)
            os.makedirs(out, exist_ok=True)
            n, problems = from_image(src, out)
            print(f"  {n} files written{_decoded(src)}")
            ok = verify(out, title, offer_any=not a.any_build)
            if problems:
                print("  INCOMPLETE: part of the disk was not read:")
                for w in problems:
                    print(f"    {w}")
                ok = False
            (done if ok else failed).append(title if ok else (path, "failed"))
        except ChdError as exc:
            print(f"  {exc}")
            failed.append((path, str(exc)))
        finally:
            if src is not None:
                src.close()
            if img and not a.keep_img and os.path.exists(img):
                os.unlink(img)
        print()

    print("=" * 62)
    if failed:
        print("Some CHDs did not ingest:")
        for c, why in failed:
            print(f"    {os.path.basename(str(c))}: {why}")
    if done:
        print(f"Ready: {', '.join(sorted(set(done)))}")
        print("\nNow build it. One command does the rest:\n")
        print("    make setup")
        print("\nThat builds the address tables from your dump, builds the")
        print("host, and stages a run directory you can launch.")
        # This script runs happily under a native Windows Python in PowerShell,
        # and then hands the reader a command that cannot work there: the
        # Makefile is POSIX shell throughout. Say so at the handoff rather than
        # letting make say it with CreateProcess(NULL, echo, ...) failed.
        if os.name == "nt" and "MSYSTEM" not in os.environ:
            print("\nRun it from WSL or an MSYS2 MINGW32 shell, not PowerShell")
            print("or cmd: the build needs a POSIX shell. See docs/building.md.")
    return 0 if done and not failed else 1


# ---- cli -----------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Extract a game's data directory from a MAME CHD.",
        epilog="One of --chd, --img or --tree is required.")
    ap.add_argument("--list-chds", action="store_true",
                    help="print every CHD --auto would find, one per line, and "
                         "stop. This is how `make check` finds something to "
                         "check the reader against.")
    ap.add_argument("--auto", action="store_true",
                    help="find the CHDs yourself, work out which game each is, "
                         "and ingest them all. Needs no other argument.")
    ap.add_argument("--title", choices=ets_titles())
    ap.add_argument("--chd", help="a MAME CHD, read directly; needs nothing installed")
    ap.add_argument("--chdman",
                    help="extract with MAME's chdman at this path instead of "
                         "reading the CHD here")
    ap.add_argument("--img", help="an already-extracted raw disk image")
    ap.add_argument("--tree", help="an already-extracted drive tree (<dir>/C, <dir>/D, ...)")
    ap.add_argument("--out", default="data",
                    help="parent directory; writes <out>/<title>/ (default: data)")
    ap.add_argument("--any-build", action="store_true",
                    help="unpack a build this host has no address tables for. "
                         "It will not run, and this is the first step of "
                         "registering one, not a way to play it.")
    ap.add_argument("--jobs", type=int, metavar="N",
                    help="decode on N cores (default: all of them). The codecs "
                         "are the whole cost of reading a CHD and hunks are "
                         "independent, so this is what makes the reader fast; "
                         "--jobs 1 is the serial path.")
    ap.add_argument("--keep-img", action="store_true",
                    help="if chdman had to run, keep its image instead of "
                         "deleting it")
    a = ap.parse_args()

    if a.list_chds:
        for c in find_chds():
            print(c)
        sys.exit(0)

    if a.auto:
        if a.chd or a.img or a.tree or a.title:
            ap.error("--auto takes no --title, --chd, --img or --tree")
        sys.exit(run_auto(a))

    if not a.title:
        ap.error("--title is required (or use --auto)")
    if sum(x is not None for x in (a.chd, a.img, a.tree)) != 1:
        ap.error("give exactly one of --chd, --img, --tree")

    # Resolve chdman BEFORE touching the filesystem, when it is going to be
    # used at all. Creating the output directory and then failing on a missing
    # tool leaves debris and reads as though the run got further than it did.
    if a.chd and a.chdman:
        find_chdman(a.chdman)

    out = os.path.join(os.path.abspath(a.out), a.title)
    if os.path.exists(out) and os.listdir(out):
        raise SystemExit(f"refusing to write into a non-empty directory: {out}")
    print(f"{TITLES[a.title]['name']} -> {out}\n")

    tmp = src = None
    try:
        if a.tree:
            os.makedirs(out, exist_ok=True)
            n, problems = from_tree(a.tree, out), []
        else:
            # A CHD normally needs no image at all. If chdman does have to run,
            # its image goes beside the CHD, always: an extracted disk is
            # multiple gigabytes, /tmp is tmpfs (RAM) on many systems, and a
            # Windows chdman cannot see it there at all. --keep-img only decides
            # whether it is deleted afterwards, not where it goes.
            if a.chd:
                src, tmp = open_chd(a.chd, a.chdman, force_chdman=bool(a.chdman),
                                    jobs=a.jobs)
            else:
                src = open(a.img, "rb")
            # Which build this is comes out of the image in about a second, so
            # it is answered BEFORE the output directory exists. Extracting a
            # gigabyte and then refusing it is the same answer, an hour later
            # and with debris.
            found, exe = identify(src)
            if found and found != a.title:
                raise SystemExit(
                    f"this holds {TITLES[found]['name']}, not "
                    f"{TITLES[a.title]['name']}. Use --title {found}, or --auto.")
            if exe and not build_gate(a.title, exe, a.any_build):
                sys.exit(1)
            os.makedirs(out, exist_ok=True)
            n, problems = from_image(src, out)
    finally:
        if src is not None:
            src.close()
        if tmp and not a.keep_img and os.path.exists(tmp):
            os.unlink(tmp)

    print(f"\n{n} files written{_decoded(src)}")

    ok = verify(out, a.title, offer_any=not a.any_build)
    if problems:
        print("\nINCOMPLETE: part of this disk was not read:")
        for w in problems:
            print(f"  {w}")
        print("\nThe executable's hash says nothing about the rest of the disk,\n"
              "so this directory may be missing game data even if it verified.")
        ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
