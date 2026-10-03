#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""titles.py -- the one place that knows which game binary a script reads.

Both the input path and the output path are derived from the title, never
written into a script: with two titles a constant output path is actively
dangerous, because a run against the second binary would overwrite the first
title's symbols.json with no warning. Layout facts are read out of the PE
section table and the binary's own code, never written down here.

    from titles import pick
    t = pick()                       # honours --title, defaults to hydro
    data = t.read()                  # the image bytes
    syms = t.symbols()               # the CodeView map, per-title cache
"""
import hashlib
import json
import os
import struct
import sys

# The repository root: scripts/ lives inside it.
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where derived per-title data goes (the symbol cache above all), and any
# directories searched for a dump after data/. Inside the checkout by default,
# so a clone creates nothing outside it; build/ is already in .gitignore.
ANALYSIS_ROOT = os.path.join(REPO, "build", "analysis")
DUMP_DIRS = {}

# --- the registry ---------------------------------------------------------
#
# `kind` is the only judgement in this file, and it is the one that decides
# whether a title belongs to this toolchain at all:
#
#   "ets"   Phar Lap TNT ETS, kernel statically linked into the game, based at
#           0x100000, DATA/.bss carried as a __pl_unpackrom record stream. The
#           VCThunder loader can host these.
#   "win32" an ordinary Windows executable that already runs on an OS. Nothing
#           here applies: no span to own, no ETS boundary, no record stream.
TITLES = {
    "hydro": {
        "name":    "Hydro Thunder",
        "kind":    "ets",
        "exe":     "HYDRO.EXE",
        "sha256":  "a421f5fd4ee0d90f57815975d4a47bb784f2b46d48085e42b422605a029f8e3f",
        "build":   "v01.01b, 1999-02-27",
        "assets":  ["HT.R2"],
        "io":      "Diego",     # Diego I/O board on COM1
        "glide":   2,
    },
    "offroad": {
        "name":    "Offroad Thunder",
        "kind":    "ets",
        "exe":     "offroad3.exe",
        "sha256":  "fa739d0f1a80c6e429ca73fdee04c3d6d6325bb95848cb701114f74bf2a5e9bf",
        "build":   "2000-02-25",
        "assets":  ["GD3.R2"],
        "io":      "MagicBus",  # Diego successor; _mb_io_comm_* in the binary
        "glide":   2,
    },
}

DEFAULT = "hydro"


class Title:
    def __init__(self, tid):
        if tid not in TITLES:
            raise SystemExit(f"unknown title {tid!r}; known: {', '.join(TITLES)}")
        self.id = tid
        self.meta = TITLES[tid]
        self.name = self.meta["name"]
        self.kind = self.meta["kind"]
        self.exe = self._locate()
        self._data = None
        self._syms = None
        self._sections = None
        # A CLONE HAS NO DUMP, and that is a normal state, not an error: the
        # tree builds and `make check` runs without one. So the layout is read
        # only when the file is actually there, and asking for a layout field
        # without it produces the message below rather than a traceback out of
        # a PE header parser. That distinction is a stranger's first impression
        # of this repository.
        self.available = os.path.isfile(self.exe)
        if self.kind == "ets" and self.available:
            self._layout()

    # Layout fields exist only once _layout() has run. Anything else reaching
    # for one has skipped the availability check.
    _LAYOUT_FIELDS = frozenset("""
        image_base code_file_off code_size code_va data_va data_size bss_va
        bss_size stack_va stack_size span_end entry_va entry_rva
    """.split())

    def __getattr__(self, name):
        if name in Title._LAYOUT_FIELDS:
            raise SystemExit(self.missing_message())
        raise AttributeError(name)

    def missing_message(self):
        return (
            f"no game dump for {self.id}: looked for:\n"
            f"    {self.exe}\n\n"
            f"{self.name} is supplied by you, from your own lawful copy. This\n"
            f"repository contains no game material. Get a data directory with:\n\n"
            f"    python3 tools/ingest-chd.py --title {self.id} --chd <your.chd> --out data\n\n"
            f"then point the build at it:\n\n"
            f"    VCT_DUMPS=$PWD/data make defs\n\n"
            f"See docs/building.md. A dump is required to BUILD, not just to\n"
            f"run: generated/span.h carries the address span the stub reserves\n"
            f"at link time. `make` and `make check` both stop here without one.")

    # --- where the game binary is -----------------------------------------
    def _locate(self):
        """The title's executable, from whichever source this machine has.

        First hit wins:

          1. VCT_DUMP_<TITLE>   an explicit path to the one executable. The
                                escape hatch for a layout nobody predicted.
          2. VCT_DUMPS          a directory of ingest output, laid out exactly
                                like the run directory's data/:
                                <VCT_DUMPS>/<title>/<exe>. One ingest run then
                                feeds both the build and the run directory, and
                                nobody has to learn a second layout.
          3. data/              this repository's own <title>/<exe>, where
                                ingest-chd.py writes by default.
          4. DUMP_DIRS          a per-title directory, when one is set.

        Nothing here decides whether the file is the RIGHT one: verify() does
        that against the registered SHA-256, and it is the only thing that may.
        A path that exists is not a dump.
        """
        env = os.environ.get("VCT_DUMP_" + self.id.upper())
        if env:
            return os.path.abspath(os.path.expanduser(env))

        dumps = os.environ.get("VCT_DUMPS")
        if dumps:
            return os.path.join(os.path.abspath(os.path.expanduser(dumps)),
                                self.id, self.meta["exe"])

        # The repository's own data/: where tools/ingest-chd.py writes by
        # default and where the run directory keeps the same layout. Checking
        # it means the ordinary path (ingest, then build) needs NO environment
        # variable at all; without it the tool has to end by telling the user
        # to set VCT_DUMPS, which is one more thing to get wrong.
        local = os.path.join(REPO, "data", self.id, self.meta["exe"])
        if os.path.isfile(local) or self.id not in DUMP_DIRS:
            return local
        return os.path.join(DUMP_DIRS[self.id], self.meta["exe"])

    # --- paths ------------------------------------------------------------
    @property
    def analysis_dir(self):
        """Where derived per-title data goes: the symbol cache above all."""
        return os.path.join(ANALYSIS_ROOT, self.id)

    @property
    def symbols_path(self):
        return os.path.join(self.analysis_dir, "p1", "json", "symbols.json")

    def out(self, *parts):
        """A per-title analysis path, created on demand."""
        p = os.path.join(self.analysis_dir, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    # --- content ----------------------------------------------------------
    def read(self):
        if self._data is None:
            if not self.available:
                raise SystemExit(self.missing_message())
            with open(self.exe, "rb") as f:
                self._data = f.read()
        return self._data

    def sha256(self):
        return hashlib.sha256(self.read()).hexdigest()

    def verify(self):
        """Loud, not fatal: an unexpected hash means the analysis below it is
        about a different file, which is worth saying before the numbers are."""
        want = self.meta.get("sha256")
        got = self.sha256()
        if want and want != got:
            print(f"WARNING: {self.id} sha256 {got}\n"
                  f"         registry expects {want}", file=sys.stderr)
            return False
        return True

    def symbols(self):
        if self._syms is None:
            if not os.path.exists(self.symbols_path):
                raise SystemExit(
                    f"no symbol cache for {self.id}; run:\n"
                    f"    python3 scripts/p1-symbols.py --title {self.id}")
            self._syms = json.load(open(self.symbols_path))
        return self._syms

    def va(self, name):
        """VA of a symbol, or None. Absent is a normal answer across titles:
        _net_link_PreWorkReceive exists in Hydro and not in Offroad."""
        v = self.symbols().get(name)
        return v["va"] if v and v["va"] else None

    def sym_floats(self, name):
        """How many 4-byte floats a symbol spans, taken from the distance to the
        NEXT symbol. An array's length is not written down anywhere else in a
        stripped-but-CodeView'd binary, and hardcoding one would be a size,
        which is per-title DATA. None when the symbol is absent or
        is the last one, because a guess there would be unbounded."""
        syms = self.symbols()
        v = syms.get(name)
        if not v or not v["va"]:
            return None
        here = v["va"]
        nxt = min((s["va"] for s in syms.values() if s["va"] > here),
                  default=None)
        if nxt is None or nxt <= here:
            return None
        return (nxt - here) // 4

    # --- layout, read out of the image ------------------------------------
    def _sects(self):
        if self._sections is None:
            d = self.read()
            pe = struct.unpack_from("<I", d, 0x3C)[0]
            nsect = struct.unpack_from("<H", d, pe + 6)[0]
            optsz = struct.unpack_from("<H", d, pe + 20)[0]
            self.image_base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
            self.entry_rva = struct.unpack_from("<I", d, pe + 24 + 16)[0]
            off = pe + 24 + optsz
            self._sections = {}
            for i in range(nsect):
                h = d[off + i * 40: off + (i + 1) * 40]
                nm = h[:8].rstrip(b"\0").decode("latin1")
                vsize, vaddr, rawsz, rawoff = struct.unpack_from("<IIII", h, 8)
                # VirtualAddress is an RVA; every consumer here wants a VA.
                # VirtualSize is the section's real length and SizeOfRawData is
                # it rounded up to file alignment, so prefer VirtualSize: using
                # the raw size puts CODE's end 11 bytes past the last real byte
                # and DATA's 12 past, which is enough to make a range check that
                # should fail succeed.
                self._sections[nm] = {
                    "va": self.image_base + vaddr,
                    "size": vsize or rawsz,
                    "file": rawoff,
                }
        return self._sections

    def _layout(self):
        s = self._sects()
        c, dat, bss, stk = s["CODE"], s["DATA"], s[".bss"], s[".stack"]
        self.code_va, self.code_size, self.code_file_off = c["va"], c["size"], c["file"]
        self.data_va, self.data_size = dat["va"], dat["size"]
        self.bss_va, self.bss_size = bss["va"], bss["size"]
        self.stack_va, self.stack_size = stk["va"], stk["size"]
        self.span_end = self.stack_va + self.stack_size
        self.entry_va = self.image_base + self.entry_rva
        self._stream_base = None

    @property
    def real_code_end_va(self):
        """Where machine code stops and the __pl_unpackrom record stream begins.

        Derived from the literal __pl_unpackrom loads into ebx (`BB imm32`), which
        is the function's own statement of where its stream starts, and it
        agrees with the value derived by hand for Hydro (0x1f7b58). Every linear
        disassembly must stop here; past it, CODE is data and decodes as garbage,
        so a raw opcode scan that runs on is almost all false positives.
        """
        if self._stream_base is None:
            va = self.va("__pl_unpackrom")
            if va is None:
                raise SystemExit(f"{self.id}: no __pl_unpackrom symbol")
            d, off = self.read(), va - self.image_base
            base = None
            for i in range(off, off + 0x80):
                if d[i] == 0xBB:                     # mov ebx, imm32
                    cand = struct.unpack_from("<I", d, i + 1)[0]
                    if self.code_va < cand < self.code_va + self.code_size:
                        base = cand
                        break
            if base is None:
                raise SystemExit(f"{self.id}: no stream base literal in __pl_unpackrom")
            self._stream_base = base
        return self._stream_base

    @property
    def code_start_off(self):
        return self.code_file_off

    @property
    def real_code_end_off(self):
        return self.real_code_end_va - self.image_base

    @property
    def code_end_off(self):
        return self.code_file_off + self.code_size

    @property
    def code_end_va(self):
        return self.code_va + self.code_size

    @property
    def data_end_va(self):
        return self.data_va + self.data_size

    @property
    def bss_end_va(self):
        return self.bss_va + self.bss_size

    @property
    def code_contents_end_off(self):
        """End of CODE's bytes in the file. .idata is laid out immediately after
        it, so its file offset is the exact end including LinkLoc's alignment
        pad; scans that used a hand-written 0x19CBE0 were using precisely this."""
        idata = self._sects().get(".idata")
        return idata["file"] if idata and idata["file"] else self.code_end_off

    def describe(self):
        if self.kind != "ets":
            return (f"{self.id:8s} {self.name:16s} kind={self.kind}  "
                    f"glide{self.meta['glide']}  io={self.meta['io']}  "
                    f"-- not an ETS title, VCThunder's loader does not apply")
        span_kb = (self.span_end - self.image_base) // 1024
        try:
            ends = f"{self.real_code_end_va:#08x}"
        except SystemExit:
            ends = "?(no symbols)"
        return (f"{self.id:8s} {self.name:16s} base {self.image_base:#08x}  "
                f"CODE {self.code_va:#08x}+{self.code_size:#x}  "
                f"code ends {ends}  "
                f"span end {self.span_end:#08x} ({span_kb} KB)  io={self.meta['io']}")


_cache = {}


def get(tid):
    if tid not in _cache:
        _cache[tid] = Title(tid)
    return _cache[tid]


def pick(argv=None):
    """Consume `--title <id>` from argv (default hydro) and return the Title.

    Removes the option in place so callers can keep parsing their own flags
    positionally, which is how every script here already works.
    """
    argv = sys.argv if argv is None else argv
    tid = DEFAULT
    if "--title" in argv:
        i = argv.index("--title")
        tid = argv[i + 1]
        del argv[i:i + 2]
    return get(tid)


def ets_titles():
    return [t for t in TITLES if TITLES[t]["kind"] == "ets"]


if __name__ == "__main__":
    argv = sys.argv[1:]
    # `make check` asks this before it tries to verify generated/ against the
    # dumps. Exit 0 only when every ETS title resolves to a file that is really
    # there: a clone without dumps must SKIP that step, not fail it.
    # The ETS title ids, one per line, for the Makefile to loop over. The
    # titles are DATA; nothing downstream should spell them out.
    if "--ets-ids" in argv:
        print("\n".join(ets_titles()))
        sys.exit(0)
    # Every file gen-gamedefs.py reads PER TITLE, for tools/stamp.py: the dump
    # itself and the symbol cache derived from it. Named here rather than in the
    # Makefile because both paths are resolved by the search this module owns,
    # and a stamp over the wrong path is a stamp that never goes stale.
    if "--stamp-inputs" in argv:
        for t in ets_titles():
            title = get(t)
            print(title.exe)
            print(title.symbols_path)
        sys.exit(0)
    if "--have-dumps" in argv:
        ok = True
        for t in ets_titles():
            title = get(t)
            print(f"{t:8s} {'found  ' if title.available else 'MISSING'} {title.exe}")
            ok &= title.available
        sys.exit(0 if ok else 1)
    ids = [argv[argv.index("--title") + 1]] if "--title" in argv else list(TITLES)
    for tid in ids:
        t = get(tid)
        ok = "  " if os.path.exists(t.exe) else "  [MISSING] "
        print(ok + t.describe())
        if os.path.exists(t.exe) and t.kind == "ets":
            t.verify()
            have = "yes" if os.path.exists(t.symbols_path) else "NO: run p1-symbols.py"
            print(f"           sha256 {t.sha256()}")
            print(f"           symbol cache: {have}")
