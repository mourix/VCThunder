# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""x86len.py -- a length-only x86-32 decoder, so that capstone is optional.

WHY THIS EXISTS. `make defs` is the one step a person building this from their
own dump cannot skip, and until now it needed Python's `capstone`. That is a
native extension, and on the host this project most wants to be easy -- an
MSYS2 MINGW32 shell, which is how you build here without WSL -- there is no way
to install it: the mingw32 and clang32 repositories carry no capstone package
at all, not the binding and not the C library, so pacman cannot help and pip
falls back to building capstone from source. This module removes the
dependency. capstone is still used when it is importable, and is still the
oracle this was written against; see glide2_pushes.py.

WHAT IT DECODES. Only what the push sweep reads: an instruction's LENGTH, and
which of eight classes it falls into. It does not name registers, it does not
name the instruction, and it has no opinion about anything the walk-back in
glide2_pushes._pushes_before() does not test. Unknown encodings return None,
which the sweep handles exactly as it already handles capstone stopping:
resynchronise one byte later. A 1 MB CODE segment with jump tables and string
data in it does that thousands of times either way.

THE CLASS IS THE PRINTED MNEMONIC, NOT THE OPCODE. The walk-back matches on
capstone's mnemonic string, so this has to reproduce capstone's *naming*, which
is not the same thing as reproducing x86. Four cases cost a round each:
`f0 2b 20` is `lock sub` and so is not a `sub`; `f2 79` is `bnd jns` and so
does not start with `j`; `f3 c2` is `repz ret` and so is not a `ret`, while
`f3 7d` is a plain `jge` because F3 means nothing before a Jcc; and `9a` is
`lcall`, not `call`. An `add`/`sub` against esp also counts in every encoding,
not just the immediate forms -- capstone prints `sub esp, eax` for `2b e0` and
the walk-back reads that as a stack adjustment.

VALIDATION (2026-08-22, both titles, whole CODE segment).

  * Instruction-by-instruction against capstone 5.0.7 at every offset both
    decoders reached: Hydro 334,142 shared offsets, Offroad 360,642, and
    ZERO disagreements in either length or class.
  * capstone reaches ~0.1% more offsets than this does (it decodes SSE forms
    this skips), so the two resynchronise differently inside data. That shows
    up once: Hydro's `_grSstSelect` histogram is {4: 4} under capstone and
    {4: 3, 0: 1} here, one extra call site picked up out of a jump table.
    It splits the histogram, so this module returns nothing for that name --
    the conservative direction, and the direction the module is built to fail
    in. Its argument size is observable from the cleanup immediate anyway.
  * The only thing that actually has to hold, and does: `gen-gamedefs.py`
    emits BYTE-IDENTICAL profile sources for both titles with either decoder.
    Hydro 94b9d7fb40f5be1f..., Offroad e549d9aef55b5a79... (sha256 of the
    emitted source, both decoders).

Re-run all of it with `python3 scripts/x86len.py --verify`, which needs
capstone and is what `make check` runs when capstone is present.
"""

PUSH, BRANCH, CALL, RET, LEAVE, ADDESP, SUBESP, OTHER = (
    "push", "branch", "call", "ret", "leave", "addesp", "subesp", "other")

PREFIXES = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0xF0, 0xF2, 0xF3}

# operand encodings: n=none, M=modrm, Mb=modrm+imm8, Mz=modrm+immZ,
# Ib/Iw/Iz=immediate only, Jb/Jz=relative, Ob=moffs32
_ONE = {}


def _fill(spec, *ops):
    for o in ops:
        _ONE[o] = spec


# arithmetic r/m,r and r,r/m families 00..3F, minus the segment prefixes
for base in (0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38):
    _fill("M", base + 0, base + 1, base + 2, base + 3)
    _fill("Ib", base + 4)
    _fill("Iz", base + 5)
_fill("n", *range(0x40, 0x60))            # inc/dec r32, push/pop r32
_fill("n", 0x60, 0x61, 0x90, 0x98, 0x99, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
      0xC3, 0xCB, 0xCC, 0xCE, 0xCF, 0xD7, 0xF4, 0xF5, 0xF8, 0xF9, 0xFA,
      0xFB, 0xFC, 0xFD, 0x27, 0x2F, 0x37, 0x3F, 0xD6)
_fill("n", *range(0x6C, 0x70))            # ins/outs
_fill("n", *range(0xA4, 0xB0))            # movs/cmps/stos/lods/scas
_fill("Iz", 0x68)                         # push imm32
_fill("Ib", 0x6A)                         # push imm8
_fill("Mz", 0x69)                         # imul r,r/m,imm32
_fill("Mb", 0x6B)                         # imul r,r/m,imm8
_fill("Jb", *range(0x70, 0x80))           # jcc rel8
_fill("Mb", 0x80, 0x83, 0xC0, 0xC1, 0xC6)
_fill("Mz", 0x81, 0xC7)
_fill("M", 0x62, 0x63, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B,
      0x8C, 0x8D, 0x8E, 0x8F, 0xD0, 0xD1, 0xD2, 0xD3, 0xFE, 0xFF,
      0xF6, 0xF7)                          # F6/F7 imm handled specially below
_fill("n", *range(0x91, 0x98))            # xchg eax, r32
_fill("Iz", 0x9A)                          # far ptr16:32 -> handled specially
_fill("Ob", 0xA0, 0xA1, 0xA2, 0xA3)
_fill("Ib", 0xA8)
_fill("Iz", 0xA9)
_fill("Ib", *range(0xB0, 0xB8))            # mov r8, imm8
_fill("Iz", *range(0xB8, 0xC0))            # mov r32, imm32
_fill("Iw", 0xC2, 0xCA)
_fill("n", 0xC9)                           # leave
_fill("Ib", 0xCD)
_fill("Ib", 0xD4, 0xD5)
_fill("n", *range(0xD8, 0xE0))             # x87: modrm, handled specially
_fill("Jb", 0xE0, 0xE1, 0xE2, 0xE3, 0xEB)
_fill("Ib", 0xE4, 0xE5, 0xE6, 0xE7)
_fill("Jz", 0xE8, 0xE9)
_fill("n", 0xEC, 0xED, 0xEE, 0xEF)

_TWO_M = set(range(0x00, 0x04)) | {0x0D} | set(range(0x10, 0x30)) | \
    set(range(0x40, 0x50)) | set(range(0x50, 0x80)) | set(range(0x90, 0x98)) | \
    {0xA3, 0xAB, 0xAF, 0xB0, 0xB1, 0xB3, 0xB6, 0xB7, 0xBB, 0xBC, 0xBD,
     0xBE, 0xBF, 0xC0, 0xC1, 0xC3, 0xB2, 0xB4, 0xB5, 0x02, 0x03} | \
    set(range(0xD0, 0x100))
_TWO_MB = {0x70, 0x71, 0x72, 0x73, 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6,
           0x0F}
_TWO_JZ = set(range(0x80, 0x90))
_TWO_N = {0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x30, 0x31, 0x32, 0x33, 0x34,
          0x35, 0x37, 0xA0, 0xA1, 0xA2, 0xA8, 0xA9, 0xAA, 0x0E} | \
    set(range(0xC8, 0xD0))


def _modrm(data, at, asz16=False):
    """Length of modrm+sib+disp starting at `at`, or None past the end."""
    if at >= len(data):
        return None
    m = data[at]
    if asz16:                                  # 0x67: no SIB, disp16 forms
        mod, rm = m >> 6, m & 7
        n = 1
        if mod == 1:
            n += 1
        elif mod == 2:
            n += 2
        elif mod == 0 and rm == 6:
            n += 2
        return n
    mod, rm = m >> 6, m & 7
    n = 1
    if mod != 3 and rm == 4:
        if at + 1 >= len(data):
            return None
        n += 1
        sib = data[at + 1]
        if mod == 0 and (sib & 7) == 5:
            n += 4
    if mod == 1:
        n += 1
    elif mod == 2:
        n += 4
    elif mod == 0 and rm == 5:
        n += 4
    return n


def decode(data, off, image_base):
    """(size, kind, target) or None."""
    i, osz16, asz16, pfx, n = off, False, False, 0, len(data)
    while i < n and (data[i] in PREFIXES or data[i] == 0x66 or data[i] == 0x67):
        if data[i] == 0x66:
            osz16 = True
        elif data[i] == 0x67:
            asz16 = True
        elif data[i] in (0xF0, 0xF2, 0xF3):
            pfx = data[i]
        i += 1
        if i - off > 4:
            return None
    if i >= n:
        return None
    op = data[i]
    i += 1

    if op == 0x0F:
        if i >= n:
            return None
        op2 = data[i]
        i += 1
        if op2 in _TWO_JZ:
            i += 2 if osz16 else 4
            return (i - off, BRANCH, None) if i <= n else None
        if op2 in _TWO_N:
            return (i - off, PUSH if op2 in (0xA0, 0xA8) else OTHER, None)
        if op2 in _TWO_MB:
            ln = _modrm(data, i, asz16)
            if ln is None:
                return None
            i += ln + 1
            return (i - off, OTHER, None) if i <= n else None
        if op2 in _TWO_M:
            ln = _modrm(data, i, asz16)
            if ln is None:
                return None
            i += ln
            return (i - off, OTHER, None) if i <= n else None
        return None

    spec = _ONE.get(op)
    if spec is None:
        return None

    # ---- special cases the plain table cannot carry -----------------------
    if op in (0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF):   # x87
        ln = _modrm(data, i, asz16)
        if ln is None:
            return None
        i += ln
        return (i - off, OTHER, None) if i <= n else None
    if op in (0xF6, 0xF7):                       # group3: /0 and /1 take imm
        if i >= n:
            return None
        reg = (data[i] >> 3) & 7
        ln = _modrm(data, i, asz16)
        if ln is None:
            return None
        i += ln
        if reg in (0, 1):
            i += 1 if op == 0xF6 else (2 if osz16 else 4)
        return (i - off, OTHER, None) if i <= n else None
    if op == 0xFF:                               # group5: push/call/jmp live here
        if i >= n:
            return None
        reg = (data[i] >> 3) & 7
        ln = _modrm(data, i, asz16)
        if ln is None:
            return None
        i += ln
        if i > n:
            return None
        kind = {2: CALL, 3: CALL, 4: BRANCH, 5: BRANCH, 6: PUSH}.get(reg, OTHER)
        if pfx == 0xF0 or (pfx == 0xF2 and kind in (BRANCH, CALL)):
            kind = OTHER
        return (i - off, kind, None)
    if op == 0x9A:                               # call far ptr16:32
        i += 2 + (2 if osz16 else 4)
        return (i - off, OTHER, None) if i <= n else None
    if op == 0xEA:
        return None

    # ---- generic operand decoding ----------------------------------------
    modrm_at = i
    if spec == "M":
        ln = _modrm(data, i, asz16)
        if ln is None:
            return None
        i += ln
    elif spec == "Mb":
        ln = _modrm(data, i, asz16)
        if ln is None:
            return None
        i += ln + 1
    elif spec == "Mz":
        ln = _modrm(data, i, asz16)
        if ln is None:
            return None
        i += ln + (2 if osz16 else 4)
    elif spec == "Ib":
        i += 1
    elif spec == "Iw":
        i += 2
    elif spec == "Iz":
        i += 2 if osz16 else 4
    elif spec == "Ob":
        i += 2 if asz16 else 4
    elif spec == "Jb":
        i += 1
    elif spec == "Jz":
        i += 2 if osz16 else 4
    if i > n:
        return None

    size = i - off
    # ---- classify ---------------------------------------------------------
    # A prefix can rename the instruction, and the walk-back reads the NAME:
    # capstone prints `lock sub` for f0 2b, `bnd jns` for f2 79 and `repz ret`
    # for f3 c2, none of which are the class the bare opcode would give.
    def _pfx(kind):
        if pfx == 0xF0:
            return OTHER
        if pfx == 0xF2 and kind in (BRANCH, RET, CALL):
            return OTHER
        if pfx == 0xF3 and kind == RET:
            return OTHER
        return kind
    if op in (0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x68, 0x6A, 0x9C):
        return (size, _pfx(PUSH), None)
    if op == 0xC9:
        return (size, _pfx(LEAVE), None)
    if op in (0xC2, 0xC3):
        return (size, _pfx(RET), None)
    if op == 0xE8:
        # The displacement is as wide as the operand size, not always 4: a
        # 66-prefixed `call rel16` would otherwise be read four bytes back.
        w = 2 if osz16 else 4
        rel = int.from_bytes(data[off + size - w:off + size], "little", signed=True)
        return (size, _pfx(CALL), (image_base + off + size + rel) & 0xFFFFFFFF)
    if op in (0xE9, 0xEB, 0xE3) or 0x70 <= op <= 0x7F:
        return (size, _pfx(BRANCH), None)
    if op in (0x81, 0x83):
        m = data[modrm_at]
        if m in (0xC4, 0xEC):          # mod 3, rm 4 (esp): /0 = add, /5 = sub
            return (size, _pfx(ADDESP if m == 0xC4 else SUBESP), None)
    if op in (0x01, 0x29, 0x03, 0x2B):
        m = data[modrm_at]
        # 01/29 write r/m; 03/2B write reg. esp is 4 in whichever field is first.
        rm_first = op in (0x01, 0x29)
        first = (m & 7) if rm_first else ((m >> 3) & 7)
        if first == 4 and (not rm_first or (m >> 6) == 3):
            return (size, _pfx(ADDESP if op in (0x01, 0x03) else SUBESP), None)
    return (size, OTHER, None)


# --- self-check -----------------------------------------------------------
#
# Not a unit test with hand-written cases: the corpus is the two dumps, and the
# oracle is capstone. It reports and exits non-zero on any disagreement.
def _verify():
    import os
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import capstone
    from titles import ets_titles, get

    _MN = {"push": ("push", ""), BRANCH: ("jmp", ""), CALL: ("call", ""),
           RET: ("ret", ""), LEAVE: ("leave", ""), ADDESP: ("add", "esp, 0"),
           SUBESP: ("sub", "esp, 0"), OTHER: ("nop", "")}

    def classify(ins):
        m = ins.mnemonic
        if m in ("push", "pushf", "pushfd"):
            return PUSH
        if m == "call":
            return CALL
        if m in ("ret", "retn"):
            return RET
        if m == "leave":
            return LEAVE
        if m.startswith("j"):
            return BRANCH
        if m == "add" and ins.op_str.startswith("esp,"):
            return ADDESP
        if m == "sub" and ins.op_str.startswith("esp,"):
            return SUBESP
        return OTHER

    bad = 0
    for tid in ets_titles():
        t = get(tid)
        data, lo, hi = t.read(), t.code_start_off, t.real_code_end_off
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

        cs, off = {}, lo
        while off < hi:
            got = 0
            for ins in md.disasm(data[off:hi], t.image_base + off):
                k = classify(ins)
                tgt = (int(ins.op_str, 16)
                       if k == CALL and ins.op_str.startswith("0x") else None)
                cs[ins.address - t.image_base] = (ins.size, k, tgt)
                got += ins.size
            off += got + 1 if got else 1

        mine, off = {}, lo
        while off < hi:
            r = decode(data, off, t.image_base)
            if r is None:
                off += 1
                continue
            mine[off] = r
            off += r[0]

        shared = cs.keys() & mine.keys()
        diff = sorted(o for o in shared if cs[o] != mine[o])
        print(f"{tid}: capstone {len(cs)}, x86len {len(mine)}, "
              f"shared {len(shared)}, disagreements {len(diff)}")
        for o in diff[:10]:
            print(f"   {t.image_base + o:#x}  capstone {cs[o]}  x86len {mine[o]}"
                  f"  {data[o:o + 8].hex()}")
        bad += len(diff)
    return 1 if bad else 0


if __name__ == "__main__":
    import sys
    if "--verify" in sys.argv:
        raise SystemExit(_verify())
    raise SystemExit("x86len.py is a library; --verify cross-checks it against capstone")
