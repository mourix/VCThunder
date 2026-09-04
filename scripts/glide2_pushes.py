# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""glide2_pushes.py -- argument sizes where call-site cleanup is not observable.

Both games have Glide 2.53 compiled in, so every `_gr*` is an ordinary function
inside the image and every call to it is a direct `call` with its arguments
pushed in front of it. Where the compiler emitted `add esp, N` after a call,
that immediate is the argument size and `gen-gamedefs.observed_argbytes` reads
it. Where it folded several calls' cleanup into one, or omitted it because the
block ends in a jump, this module supplies the answer instead, by counting the
pushes that belong to the call -- from the user's own dump, and from nothing
else.

VALIDATION IS THE POINT, AND IT IS NOT OPTIONAL. Scored against the cleanup
immediate over every entry where both are available (55 in Hydro, 49 in
Offroad), the summary statistics are NOT exact: every failure is a walk that
absorbed or lost a neighbouring call's pushes, and every one occurs where the
cleanup immediate already has the answer. Hence the rule this module
implements:

    the pushes decide ONLY where the histogram is UNANIMOUS.

Across both titles all 27 entries with no observable cleanup are unanimous, so
nothing is left to a statistic. A future title that is not gets `None` here and
is reported as unsized rather than guessed at.
"""

import x86len

# capstone IS THE ORACLE, BUT IT IS NOT A REQUIREMENT. x86len.py is a
# length-only decoder written against it and validated on both dumps; its
# docstring carries that record. It exists because there is no capstone for an
# MSYS2 MINGW32 shell -- no pacman package in mingw32 or clang32, so pip has to
# build the C library from source -- and `make defs` is not skippable. Where
# capstone is importable it is used, so the audited decoder is what runs
# wherever it can.
try:
    import capstone
except ImportError:                                          # pragma: no cover
    capstone = None

DECODER = "capstone" if capstone else "x86len"


class _Insn:
    """The two fields _pushes_before() reads, shaped like a capstone insn.

    x86len returns a class, not a name. Rendering it back into the mnemonic
    capstone would have printed keeps ONE walk-back rather than two: the code
    below is the audited one and never learns which decoder fed it.
    """
    __slots__ = ("mnemonic", "op_str")

    _AS = {x86len.PUSH: ("push", ""), x86len.BRANCH: ("jmp", ""),
           x86len.CALL: ("call", ""), x86len.RET: ("ret", ""),
           x86len.LEAVE: ("leave", ""), x86len.ADDESP: ("add", "esp, 0"),
           x86len.SUBESP: ("sub", "esp, 0"), x86len.OTHER: ("nop", "")}

    def __init__(self, kind, target):
        self.mnemonic, self.op_str = self._AS[kind]
        if kind == x86len.CALL and target is not None:
            self.op_str = hex(target)


def _sweep(t, md):
    """Linear disassembly of CODE, resynchronising past undecodable bytes.

    A decoder stops at the first byte it cannot decode, and a 1 MB CODE segment
    with jump tables and string data in it has thousands: one `md.disasm()` over
    Hydro covers 27 KB of 1,012 KB and finds no call site at all. Restarting a
    byte later is what makes this a sweep rather than a prefix. x86len needs the
    same treatment for the same reason, and skips a little more than capstone
    does, so the two resynchronise at slightly different places inside data.
    """
    data = t.read()
    lo, hi = t.code_start_off, t.real_code_end_off
    insns = []
    off = lo
    if md is None:
        while off < hi:
            r = x86len.decode(data, off, t.image_base)
            if r is None:
                off += 1
                continue
            insns.append(_Insn(r[1], r[2]))
            off += r[0]
        return insns
    while off < hi:
        got = 0
        for ins in md.disasm(data[off:hi], t.image_base + off):
            insns.append(ins)
            got += ins.size
        off += got + 1 if got else 1
    return insns


def _pushes_before(insns, i):
    """Bytes pushed for the call at index i, or None if unreadable.

    Walks back over the block, summing pushes, stopping at the previous call,
    any branch, or a stack cleanup. A `sub esp` means the compiler reserved the
    argument area and will `mov` into it, which push counting cannot describe.
    """
    total = 0
    j, seen = i - 1, 0
    while j >= 0 and seen < 64:
        ins = insns[j]
        m = ins.mnemonic
        if m in ("push", "pushf", "pushfd"):
            total += 4
        elif m.startswith("j") or m in ("call", "ret", "retn", "leave"):
            return total
        elif m == "add" and ins.op_str.startswith("esp,"):
            return total
        elif m == "sub" and ins.op_str.startswith("esp,"):
            return None
        j -= 1
        seen += 1
    return total


# The sweep is the expensive half (~3 s per title) and every caller wants the
# same instruction list, so it is built once per title.
_SWEEP_CACHE = {}


def push_argbytes(t, targets):
    """{name: argbytes} for names whose push histogram is unanimous.

    `targets` maps VA -> symbol name. Names with a split histogram, or with no
    observed call site, are simply absent from the result: this module never
    guesses, and gen-gamedefs reports an unsized entry rather than emitting one.
    """
    md = None
    if capstone:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
    if t.id not in _SWEEP_CACHE:
        _SWEEP_CACHE[t.id] = _sweep(t, md)
    insns = _SWEEP_CACHE[t.id]

    hist = {n: {} for n in targets.values()}
    for i, ins in enumerate(insns):
        if ins.mnemonic != "call" or not ins.op_str.startswith("0x"):
            continue
        n = targets.get(int(ins.op_str, 16))
        if n is None:
            continue
        b = _pushes_before(insns, i)
        if b is not None:
            hist[n][b] = hist[n].get(b, 0) + 1

    return ({n: next(iter(h)) for n, h in hist.items() if len(h) == 1},
            hist)
