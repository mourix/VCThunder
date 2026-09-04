#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""chd.py -- read a MAME CHD v5 without MAME.

WHY THIS EXISTS
    `tools/ingest-chd.py` is the front door, and until now it opened only for
    someone who already had `chdman`. That is a 100 MB MAME download to run one
    command once, and it costs the user disk as well: chdman writes the WHOLE
    uncompressed disk before anything is read out of it -- 3.2 GB for Hydro and
    6.4 GB for Offroad -- and the ingest then copies ~1 GB of files out and
    deletes the rest.

    This reads the CHD directly, decoding only the hunks the files actually
    occupy. No download, no scratch image, no PATH, and nothing to install:
    everything here is the Python standard library, which the build already
    requires. It is the same move `scripts/x86len.py` made for capstone.

    `chdman` remains the fallback, not the front door: a CHD older than v5, or
    one with a parent, raises ChdUnsupported and ingest-chd.py falls back to it
    with the reason printed.

WHAT A v5 CHD IS, IN THE ORDER THIS FILE READS IT
    A 124-byte big-endian header names four compressors, the hunk size (4096
    here) and the unit size (512), and carries a SHA-1 of the uncompressed
    image. A compressed MAP then says, per hunk, which of those four codecs it
    used, where its payload is, how long it is, and a CRC-16 OF THE DECODED
    HUNK. That last field is what makes this file checkable: every hunk carries
    its own oracle, so a decoder bug cannot pass silently, and `--verify`
    decodes an entire image and reports both the per-hunk CRCs and the image
    SHA-1 against the header's.

    Measured against the user's own two CHDs, 2026-08-22: all 788,256 hunks of
    Hydro and all 1,574,370 of Offroad decode with a matching CRC-16, and both
    images' SHA-1 matches the header -- 30 s and 1m42s on sixteen cores. The
    codec histogram is
    identical to `chdman info -v`'s, category by category, which is how the map
    decoder was checked before any hunk was.

THE THREE THINGS THAT ARE EASY TO GET WRONG
    1. `mapbytes` in the map header counts the payload AFTER those 16 bytes.
       Reading the header and then starting the bitstream 16 bytes further in
       still decodes a plausible-looking tree, and then fails on a length.
    2. The map's CRC-16 seed is 0xFFFF, not 0. A zero seed produces a mismatch
       that looks like a map bug and is not one.
    3. A COMPRESSION_SELF entry's 48-bit field is a HUNK NUMBER, not a byte or
       unit offset. Scaling it by unitbytes gives a reader that decodes 9% of
       an image correctly and mis-assembles the rest -- and the 9% is the part
       that carries the CRCs, so a spot check of the codecs will not see it.

HOW IT WAS DERIVED, AND WHY THAT IS RECORDABLE
    From the container's own structure, black box. No MAME or libchdr source
    was read; `chdman info -v` was RUN and its output compared, which is the
    same standing this project gives 86Box. Every layer here carries its own
    oracle -- a per-hunk CRC-16 of the DECODED hunk, and the image's SHA-1 --
    so a wrong decoder cannot pass, and the three corrections listed above were
    each found as a failing checksum rather than avoided by transcription --
    which is the evidence, because a transcription does not make those
    mistakes. The project's sourcing record carries the validation run.

NO GAME MATERIAL IS IN THIS FILE. It is a container reader written from the
format's own structure, and it reads the user's own lawful dump on the user's
own disk.
"""

import collections
import hashlib
import lzma
import multiprocessing
import os
import struct
import sys
import zlib
from array import array

HEADER_LEN = 124
MAGIC = b"MComprHD"


class ChdError(Exception):
    """The file is a CHD and something in it is wrong."""


class ChdUnsupported(ChdError):
    """A CHD this reader will not open. The caller falls back to chdman."""


# ---- CRC-16/CCITT --------------------------------------------------------
#
# Two bytes at a time. A 16-bit register that consumes 16 bits per step depends
# only on `crc ^ word`, so one 65,536-entry table replaces two lookups and two
# shifts per pair -- 2,048 iterations for a hunk instead of 4,096 x 4 ops. That
# matters: this runs on every decoded hunk, and the byte-at-a-time version cost
# more than decompressing the hunk did.

def _crc16_tables():
    t8 = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        t8.append(c)
    t16 = []
    for x in range(65536):
        hi = t8[x >> 8]
        t16.append((((hi & 0xFF) << 8) ^ t8[(hi >> 8) ^ (x & 0xFF)]) & 0xFFFF)
    return t8, t16


_T8, _T16 = _crc16_tables()


def crc16(data, seed=0xFFFF):
    c = seed
    n = len(data)
    if n >= 2:
        for w in struct.unpack(f">{n // 2}H", data[:n & ~1]):
            c = _T16[c ^ w]
    if n & 1:
        c = ((c << 8) ^ _T8[(c >> 8) ^ data[-1]]) & 0xFFFF
    return c


# ---- bitstream -----------------------------------------------------------

class BitReader:
    """The container's bitstream: big-endian, MSB first, zeroes past the end.

    Reading past the end is not an error and must not be one here: the huffman
    trees are written without padding, so the last symbol of a hunk can
    legitimately need bits the encoder never wrote. Established the same way as
    everything else in this file -- a reader that faults there stops on the
    final symbol of hunks whose CRC-16 is otherwise reachable.
    """

    __slots__ = ("d", "p", "b", "n", "L")

    def __init__(self, d):
        self.d = d
        self.p = 0
        self.b = 0
        self.n = 0
        self.L = len(d)

    def read(self, k):
        if k == 0:
            return 0
        b, n = self.b, self.n
        while n < k:
            b = (b << 8) | (self.d[self.p] if self.p < self.L else 0)
            self.p += 1
            n += 8
        n -= k
        self.b = b & ((1 << n) - 1)
        self.n = n
        return (b >> n) & ((1 << k) - 1)

    def peek8(self):
        """The next 8 bits without consuming them, for a table-driven decode."""
        b, n = self.b, self.n
        while n < 8:
            b = (b << 8) | (self.d[self.p] if self.p < self.L else 0)
            self.p += 1
            n += 8
        self.b, self.n = b, n
        return (b >> (n - 8)) & 0xFF

    def skip(self, k):
        self.n -= k
        self.b &= (1 << self.n) - 1


# ---- huffman -------------------------------------------------------------

class Huffman:
    """The container's canonical huffman decoder.

    Its code assignment runs from the LONGEST length down, which is not the
    order most descriptions of canonical codes use; assigning shortest-first
    still builds a valid-looking tree and then fails on a length, which is how
    the direction was settled. The resulting codes are ordinary canonical
    prefix codes either way, which is why the 8-bit lookup table below is
    legitimate.
    """

    __slots__ = ("numcodes", "maxbits", "lengths", "prim", "slow")

    def __init__(self, numcodes, maxbits):
        self.numcodes = numcodes
        self.maxbits = maxbits
        self.lengths = [0] * numcodes
        self.prim = None
        self.slow = None

    def _assign(self):
        hist = [0] * 33
        for L in self.lengths:
            if L > self.maxbits:
                raise ChdError(f"huffman length {L} > maxbits {self.maxbits}")
            hist[L] += 1
        starts = [0] * 33
        cur = 0
        for L in range(32, 0, -1):
            nxt = (cur + hist[L]) >> 1
            if L != 1 and nxt * 2 != cur + hist[L]:
                raise ChdError("huffman code lengths are not a valid tree")
            starts[L] = cur
            cur = nxt
        # An 8-bit primary table decodes a symbol per lookup; longer codes fall
        # back to the bit-at-a-time walk. On the user's Offroad CHD this took
        # the huff hunks from 6.4 ms to 2.3 ms each.
        prim = [None] * 256
        slow = {}
        for sym, L in enumerate(self.lengths):
            if not L:
                continue
            code = starts[L]
            starts[L] += 1
            if L <= 8:
                base = code << (8 - L)
                entry = (sym, L)
                for i in range(base, base + (1 << (8 - L))):
                    prim[i] = entry
            else:
                slow[(L << 24) | code] = sym
        self.prim, self.slow = prim, slow

    def decode_one(self, bits):
        # Every code fits the primary table when maxbits <= 8, which covers
        # both trees that are decoded symbol by symbol in Python: the map's
        # 16-code tree, read once per hunk of the whole file, and the 24-code
        # tree the huff codec's own tree is written with.
        if self.maxbits <= 8:
            e = self.prim[bits.peek8()]
            if e is None:
                raise ChdError("undecodable huffman code")
            bits.skip(e[1])
            return e[0]
        code = 0
        for L in range(1, self.maxbits + 1):
            code = (code << 1) | bits.read(1)
            if L <= 8:
                e = self.prim[code << (8 - L)]
                if e is not None and e[1] == L:
                    return e[0]
            else:
                s = self.slow.get((L << 24) | code)
                if s is not None:
                    return s
        raise ChdError("undecodable huffman code")

    def import_tree_rle(self, bits):
        numbits = 5 if self.maxbits >= 16 else (4 if self.maxbits >= 8 else 3)
        cur = 0
        while cur < self.numcodes:
            nb = bits.read(numbits)
            if nb != 1:
                self.lengths[cur] = nb
                cur += 1
            elif (nb := bits.read(numbits)) == 1:    # a doubled 1 is one 1
                self.lengths[cur] = 1
                cur += 1
            else:
                rep = bits.read(numbits) + 3
                while rep and cur < self.numcodes:
                    self.lengths[cur] = nb
                    cur += 1
                    rep -= 1
        self._assign()

    def import_tree_huffman(self, bits):
        """The tree used by the `huff` codec: code lengths, RLE'd, and then
        themselves huffman-coded by a small 24-code tree."""
        small = Huffman(24, 6)
        small.lengths[0] = bits.read(3)
        start = bits.read(3) + 1
        count = 0
        for i in range(1, 24):
            if i < start or count == 7:
                small.lengths[i] = 0
            else:
                count = bits.read(3)
                small.lengths[i] = 0 if count == 7 else count
        small._assign()

        temp = self.numcodes - 9
        rlefullbits = 0
        while temp:
            temp >>= 1
            rlefullbits += 1

        last = 0
        cur = 0
        while cur < self.numcodes:
            v = small.decode_one(bits)
            if v != 0:
                last = v - 1
                self.lengths[cur] = last
                cur += 1
            else:
                n = bits.read(3) + 2
                if n == 9:
                    n += bits.read(rlefullbits)
                while n and cur < self.numcodes:
                    self.lengths[cur] = last
                    cur += 1
                    n -= 1
        self._assign()


def huff_decompress(raw, outlen):
    """The `huff` codec: a per-hunk tree, then one symbol per output byte."""
    bits = BitReader(raw)
    tree = Huffman(256, 16)
    tree.import_tree_huffman(bits)
    prim, slow = tree.prim, tree.slow
    out = bytearray(outlen)
    d, L = raw, len(raw)
    buf, n, p = bits.b, bits.n, bits.p
    for i in range(outlen):
        while n < 16:
            buf = (buf << 8) | (d[p] if p < L else 0)
            p += 1
            n += 8
        e = prim[(buf >> (n - 8)) & 0xFF]
        if e is not None:
            sym, cl = e
            n -= cl
        else:
            cl = 8
            while True:
                cl += 1
                if cl > 16:
                    raise ChdError("undecodable huffman code in hunk")
                sym = slow.get((cl << 24) | ((buf >> (n - cl)) & ((1 << cl) - 1)))
                if sym is not None:
                    n -= cl
                    break
        buf &= (1 << n) - 1
        out[i] = sym
    return bytes(out)


# ---- FLAC ----------------------------------------------------------------
#
# CHD's `flac` codec is a bare stream of FLAC frames: one leading byte, 'L' or
# 'B', says whether the 16-bit samples are little- or big-endian, and the rest
# has no STREAMINFO and no metadata blocks at all -- a decoder handing these to
# a general FLAC library has to synthesise a header for it first. So this needs
# the frame layer of the published FLAC format only, and it is used on a
# DISK IMAGE: the "samples" are the user's file data reinterpreted as two
# channels of 16-bit audio, which is why it wins on some hunks and is exactly as
# lossless as the others.

_BLOCKSIZE = {1: 192, 2: 576, 3: 1152, 4: 2304, 5: 4608,
              8: 256, 9: 512, 10: 1024, 11: 2048, 12: 4096, 13: 8192,
              14: 16384, 15: 32768}
_SAMPLESIZE = {1: 8, 2: 12, 4: 16, 5: 20, 6: 24, 7: 32}


class _FlacBits(BitReader):
    """BitReader plus the two operations only FLAC needs."""

    __slots__ = ()

    def signed(self, k):
        v = self.read(k)
        return v - (1 << k) if v & (1 << (k - 1)) else v

    def unary(self):
        """Count zero bits up to the next 1. This is the rice code's quotient
        and it is read once per residual, so it is worth not doing bit by bit:
        bit_length() finds the 1 inside whatever is already buffered."""
        c = 0
        while True:
            if self.n == 0:
                self.b = self.d[self.p] if self.p < self.L else 0
                self.p += 1
                self.n = 8
            if self.b:
                k = self.b.bit_length()
                c += self.n - k
                self.n = k - 1
                self.b &= (1 << (k - 1)) - 1
                return c
            c += self.n
            self.b = 0
            self.n = 0

    def align(self):
        self.b = 0
        self.n = 0


def _residual(br, out, blocksize, order):
    method = br.read(2)
    if method > 1:
        raise ChdError("reserved FLAC residual coding method")
    pbits, escape = (4, 15) if method == 0 else (5, 31)
    porder = br.read(4)
    nparts = 1 << porder
    if blocksize % nparts:
        raise ChdError("FLAC partition order does not divide the block")
    i = order
    for part in range(nparts):
        count = (blocksize >> porder) - (order if part == 0 else 0)
        param = br.read(pbits)
        if param == escape:
            nbits = br.read(5)
            for _ in range(count):
                out[i] = br.signed(nbits) if nbits else 0
                i += 1
        elif param:
            unary, read = br.unary, br.read
            for _ in range(count):
                v = (unary() << param) | read(param)
                out[i] = (v >> 1) ^ -(v & 1)
                i += 1
        else:
            unary = br.unary
            for _ in range(count):
                v = unary()
                out[i] = (v >> 1) ^ -(v & 1)
                i += 1


def _subframe(br, blocksize, bps):
    if br.read(1):
        raise ChdError("FLAC subframe padding bit set")
    stype = br.read(6)
    wasted = 0
    if br.read(1):
        wasted = br.unary() + 1
        bps -= wasted
    if stype == 0:                                    # constant
        out = [br.signed(bps)] * blocksize
    elif stype == 1:                                  # verbatim
        out = [br.signed(bps) for _ in range(blocksize)]
    elif 8 <= stype <= 12:                            # fixed, order 0..4
        order = stype - 8
        out = [0] * blocksize
        for i in range(order):
            out[i] = br.signed(bps)
        _residual(br, out, blocksize, order)
        if order == 1:
            for i in range(1, blocksize):
                out[i] += out[i - 1]
        elif order == 2:
            for i in range(2, blocksize):
                out[i] += 2 * out[i - 1] - out[i - 2]
        elif order == 3:
            for i in range(3, blocksize):
                out[i] += 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]
        elif order == 4:
            for i in range(4, blocksize):
                out[i] += (4 * out[i - 1] - 6 * out[i - 2]
                           + 4 * out[i - 3] - out[i - 4])
    elif stype >= 32:                                 # LPC, order 1..32
        order = stype - 31
        out = [0] * blocksize
        for i in range(order):
            out[i] = br.signed(bps)
        prec = br.read(4) + 1
        if prec == 16:
            raise ChdError("invalid FLAC LPC precision")
        shift = br.signed(5)
        if shift < 0:
            raise ChdError("negative FLAC LPC shift")
        coef = [br.signed(prec) for _ in range(order)]
        _residual(br, out, blocksize, order)
        rc = coef[::-1]
        for i in range(order, blocksize):
            s = 0
            w = out[i - order:i]
            for j in range(order):
                s += rc[j] * w[j]
            out[i] += s >> shift
    else:
        raise ChdError(f"reserved FLAC subframe type {stype}")
    if wasted:
        out = [v << wasted for v in out]
    return out


def flac_decompress(raw, outlen):
    """One CHD flac hunk -> its bytes. Two channels of 16-bit samples."""
    tag = raw[0:1]
    if tag not in (b"L", b"B"):
        raise ChdError(f"FLAC hunk has no endian tag (got {raw[0:1]!r})")
    nsamples = outlen // 4
    br = _FlacBits(raw[1:])
    left, right = array("i"), array("i")
    done = 0
    while done < nsamples:
        if br.read(14) != 0x3FFE:
            raise ChdError("lost FLAC frame sync")
        br.read(2)                                    # reserved, blocking mode
        bs_code, sr_code = br.read(4), br.read(4)
        ch_asgn, ss_code = br.read(4), br.read(3)
        br.read(1)                                    # reserved
        b0 = br.read(8)                               # UTF-8 coded frame number
        if b0 >= 0xC0:
            m = 0x20
            while b0 & m:
                br.read(8)
                m >>= 1
        if bs_code == 6:
            blocksize = br.read(8) + 1
        elif bs_code == 7:
            blocksize = br.read(16) + 1
        else:
            blocksize = _BLOCKSIZE.get(bs_code) or nsamples
        if sr_code == 12:
            br.read(8)
        elif sr_code in (13, 14):
            br.read(16)
        bps = _SAMPLESIZE.get(ss_code, 16)
        br.read(8)                                    # header CRC-8
        if (ch_asgn + 1 if ch_asgn < 8 else 2) != 2:
            raise ChdError("CHD flac hunk is not two channels")
        a = _subframe(br, blocksize, bps + (1 if ch_asgn == 9 else 0))
        b = _subframe(br, blocksize, bps + (1 if ch_asgn in (8, 10) else 0))
        br.align()
        br.read(16)                                   # frame CRC-16
        if ch_asgn == 8:                              # left / side
            b = [a[i] - b[i] for i in range(blocksize)]
        elif ch_asgn == 9:                            # right / side
            a = [a[i] + b[i] for i in range(blocksize)]
        elif ch_asgn == 10:                           # mid / side
            la, lb = [0] * blocksize, [0] * blocksize
            for i in range(blocksize):
                mid = (a[i] << 1) | (b[i] & 1)
                la[i] = (mid + b[i]) >> 1
                lb[i] = (mid - b[i]) >> 1
            a, b = la, lb
        left.extend(a)
        right.extend(b)
        done += blocksize
    inter = array("h", bytes(outlen))
    inter[0::2] = array("h", left)
    inter[1::2] = array("h", right)
    if tag == b"B":
        inter.byteswap()
    return inter.tobytes()


# ---- the container -------------------------------------------------------

# Map entry types, as they appear in the decompressed map.
_TYPE_0, _TYPE_1, _TYPE_2, _TYPE_3 = 0, 1, 2, 3
_NONE, _SELF, _PARENT = 4, 5, 6
# ...and the pseudo-types that only ever appear in the compressed stream.
_RLE_SMALL, _RLE_LARGE = 7, 8
_SELF_0, _SELF_1 = 9, 10
_PARENT_SELF, _PARENT_0, _PARENT_1 = 11, 12, 13

_KIND = {_NONE: "Uncompressed", _SELF: "Copy from self", _PARENT: "From parent"}


# ---- decoding across processes -------------------------------------------
#
# The four codecs are the whole cost of a read: 86% of an ingest is CPU inside
# huff_decompress and flac_decompress, and 14% is reading the payloads. Hunks
# are independent, so this is embarrassingly parallel -- which is what makes a
# pure-Python reader practical rather than merely correct. On a 16-thread part
# it turned Offroad's ingest from 5m20s into 45 s, and Hydro's from 70 s into
# 11 s, which is level with chdman on the same machine.
#
# Each worker opens the file itself, so the I/O parallelises too. Under `fork`
# it also INHERITS the parent's decoded map rather than decoding its own: that
# map costs two seconds and 30 MB for Offroad, and paying it once per worker
# would eat the win on the smaller title. Under `spawn` (Windows) there is no
# inheriting, so a worker builds its own; correctness is identical either way.

_WORKER = None


def _worker_init(path, inherited):
    global _WORKER
    if inherited is not None:
        # Its file handle came through the fork; never share one, because two
        # processes seeking the same descriptor read each other's bytes.
        inherited.f = open(path, "rb")
        inherited._cache = {}
        inherited._cache_max = 512
        inherited._pool = None
        _WORKER = inherited
    else:
        _WORKER = Chd(path, cache_hunks=512, jobs=1)


def _worker_batch(hunks):
    return [_WORKER.hunk(h) for h in hunks]


def default_jobs():
    """One worker per hardware thread, capped. A one-shot front-door step is
    exactly the case where taking the whole machine for half a minute is the
    polite thing to do."""
    return max(1, min(os.cpu_count() or 1, 32))


class Chd:
    """A CHD v5, open for reading, hunk by hunk."""

    def __init__(self, path, cache_hunks=4096, jobs=None):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(HEADER_LEN)
        if len(hdr) < HEADER_LEN or hdr[:8] != MAGIC:
            raise ChdUnsupported(f"{path}: not a CHD file")
        hlen, version = struct.unpack_from(">II", hdr, 8)
        if version != 5:
            raise ChdUnsupported(
                f"{path}: CHD version {version}; this reader handles v5 only")
        self.compressors = [hdr[16 + i * 4:20 + i * 4] for i in range(4)]
        (self.logicalbytes, self.mapoffset,
         self.metaoffset) = struct.unpack_from(">QQQ", hdr, 32)
        self.hunkbytes, self.unitbytes = struct.unpack_from(">II", hdr, 56)
        self.rawsha1 = hdr[64:84]
        self.sha1 = hdr[84:104]
        self.parentsha1 = hdr[104:124]
        if self.parentsha1 != b"\0" * 20:
            raise ChdUnsupported(
                f"{path}: this CHD has a parent; only whole CHDs are read here")
        if not self.hunkbytes or self.hunkbytes % 4:
            raise ChdError(f"{path}: implausible hunk size {self.hunkbytes}")
        self.hunkcount = (self.logicalbytes + self.hunkbytes - 1) // self.hunkbytes
        self.compressed = self.compressors[0] != b"\0\0\0\0"
        for c in self.compressors:
            if c not in (b"\0\0\0\0", b"lzma", b"zlib", b"huff", b"flac"):
                raise ChdUnsupported(
                    f"{path}: codec {c.decode('latin1')!r} is not implemented here")
        self._read_map()
        # The map carries a CRC-16 of itself, and it is checked before the map
        # is trusted: a map read wrongly still yields plausible
        # offsets, and the failure would then look like a codec bug in whatever
        # hunk happened to be asked for first. Two seconds on 1.6 million hunks.
        got, want = self.map_crc()
        if want is not None and got != want:
            raise ChdError(f"{path}: the hunk map does not match its own CRC-16 "
                           f"({got:#06x} computed, {want:#06x} stored)")
        # LZMA in a CHD has no header: the properties are the ones chdman's
        # encoder derives from the hunk size (level 9 normalised against
        # reduceSize == hunkbytes), which for a 4 KiB hunk is a 4 KiB dictionary.
        self._lzma = [{"id": lzma.FILTER_LZMA1, "lc": 3, "lp": 0, "pb": 2,
                       "dict_size": max(4096, self.hunkbytes)}]
        self._cache = {}
        self._cache_max = cache_hunks
        self.hunks_decoded = 0
        self.jobs = default_jobs() if jobs is None else max(1, int(jobs))
        self._pool = None

    # -- map --------------------------------------------------------------

    def _read_map(self):
        n = self.hunkcount
        self.comps = bytearray(n)
        self.offsets = array("Q", bytes(8 * n))
        self.lengths = array("I", bytes(4 * n))
        self.crcs = array("H", bytes(2 * n))
        if not self.compressed:
            self.f.seek(self.mapoffset)
            raw = self.f.read(4 * n)
            for h in range(n):
                off = int.from_bytes(raw[h * 4:h * 4 + 4], "big")
                self.comps[h] = _NONE if off else _SELF
                self.offsets[h] = off * self.hunkbytes
                self.lengths[h] = self.hunkbytes
            self.mapcrc = None
            return

        self.f.seek(self.mapoffset)
        mh = self.f.read(16)
        mapbytes, = struct.unpack_from(">I", mh, 0)
        firstoffs = int.from_bytes(mh[4:10], "big")
        self.mapcrc, = struct.unpack_from(">H", mh, 10)
        lengthbits, selfbits, parentbits = mh[12], mh[13], mh[14]
        bits = BitReader(self.f.read(mapbytes))     # NOT mapoffset+16+16

        tree = Huffman(16, 8)
        tree.import_tree_rle(bits)
        comps = self.comps
        lastcomp = 0
        rep = 0
        for h in range(n):
            if rep > 0:
                comps[h] = lastcomp
                rep -= 1
                continue
            v = tree.decode_one(bits)
            if v == _RLE_SMALL:
                comps[h] = lastcomp
                rep = 2 + tree.decode_one(bits)
            elif v == _RLE_LARGE:
                comps[h] = lastcomp
                rep = 2 + 16 + (tree.decode_one(bits) << 4) + tree.decode_one(bits)
            else:
                lastcomp = comps[h] = v

        offsets, lengths, crcs = self.offsets, self.lengths, self.crcs
        cur = firstoffs
        lastself = lastparent = 0
        for h in range(n):
            c = comps[h]
            offset, length, crc = cur, 0, 0
            if c <= _TYPE_3:
                length = bits.read(lengthbits)
                cur += length
                crc = bits.read(16)
            elif c == _NONE:
                length = self.hunkbytes
                cur += length
                crc = bits.read(16)
            elif c == _SELF:
                offset = lastself = bits.read(selfbits)
            elif c == _PARENT:
                offset = lastparent = bits.read(parentbits)
            elif c in (_SELF_0, _SELF_1):
                if c == _SELF_1:
                    lastself += 1
                c = _SELF
                offset = lastself
            elif c == _PARENT_SELF:
                c = _PARENT
                lastparent = offset = (h * self.hunkbytes) // self.unitbytes
            elif c in (_PARENT_0, _PARENT_1):
                if c == _PARENT_1:
                    lastparent += self.hunkbytes // self.unitbytes
                c = _PARENT
                offset = lastparent
            else:
                raise ChdError(f"unknown map entry type {c} at hunk {h}")
            comps[h] = c
            offsets[h] = offset
            lengths[h] = length
            crcs[h] = crc

    def map_crc(self):
        """Re-assemble the 12-byte-per-hunk raw map and CRC it against the
        value the map header carries. Returns (computed, stored)."""
        if self.mapcrc is None:
            return None, None
        raw = bytearray(12 * self.hunkcount)
        for h in range(self.hunkcount):
            o = h * 12
            raw[o] = self.comps[h]
            raw[o + 1:o + 4] = int(self.lengths[h]).to_bytes(3, "big")
            raw[o + 4:o + 10] = int(self.offsets[h]).to_bytes(6, "big")
            raw[o + 10:o + 12] = int(self.crcs[h]).to_bytes(2, "big")
        return crc16(bytes(raw)), self.mapcrc

    # -- hunks -------------------------------------------------------------

    def hunk(self, h):
        """One hunk's bytes, decoded and CRC-checked."""
        if h < 0 or h >= self.hunkcount:
            raise ChdError(f"hunk {h} out of range")
        got = self._cache.get(h)
        if got is not None:
            self._touch(h)
            return got

        # COMPRESSION_SELF's field is a HUNK NUMBER. Followed with a loop
        # rather than recursion: a chain of them is legal.
        start = h
        seen = 0
        while self.comps[h] == _SELF:
            if not self.compressed and self.offsets[h] == 0:
                return bytes(self.hunkbytes)         # never written; reads zero
            h = self.offsets[h]
            seen += 1
            if seen > 64 or h >= self.hunkcount:
                raise ChdError(f"self-reference chain from hunk {start} is bad")
            got = self._cache.get(h)
            if got is not None:
                self._touch(h)
                self._store(start, got)
                return got

        c = self.comps[h]
        if c == _PARENT:
            raise ChdUnsupported("this CHD needs its parent CHD")
        self.f.seek(self.offsets[h])
        raw = self.f.read(self.lengths[h])
        if len(raw) != self.lengths[h]:
            raise ChdError(f"hunk {h}: file ends inside the payload")
        if c == _NONE:
            data = raw
        else:
            codec = self.compressors[c]
            if codec == b"zlib":
                data = zlib.decompress(raw, -15, self.hunkbytes)
            elif codec == b"lzma":
                d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=self._lzma)
                data = d.decompress(raw, max_length=self.hunkbytes)
            elif codec == b"huff":
                data = huff_decompress(raw, self.hunkbytes)
            elif codec == b"flac":
                data = flac_decompress(raw, self.hunkbytes)
            else:
                raise ChdUnsupported(f"codec {codec.decode('latin1')!r}")
        if len(data) != self.hunkbytes:
            raise ChdError(f"hunk {h}: decoded {len(data)} of {self.hunkbytes} bytes")
        # An uncompressed CHD's map carries no CRCs at all, and checking a
        # stored zero against a real one fails every hunk of a perfectly good
        # file.
        if self.compressed and crc16(data) != self.crcs[h]:
            raise ChdError(f"hunk {h}: CRC-16 mismatch; the CHD or this reader "
                           f"is wrong, and either way the data is not usable")
        self.hunks_decoded += 1
        self._store(h, data)
        if start != h:
            self._store(start, data)
        return data

    def _touch(self, h):
        """Move a hunk to the young end. THE CACHE MUST BE AN LRU AND NOT A
        QUEUE. A disk image is mostly one hunk of zeroes with a million "copy
        from self" entries pointing at it, and each of those caches the alias as
        well -- so under a plain insertion-order queue the single most valuable
        entry in the cache is evicted by the very references that want it, and
        then decoded again, and again. Measured on Offroad, where 1,117,652 of
        the 1,198,835 self-references point at one hunk: this line is the
        difference between a whole-image verify in minutes and one in hours."""
        cache = self._cache
        cache[h] = cache.pop(h)

    def _store(self, h, data):
        cache = self._cache
        if h in cache:
            del cache[h]
        cache[h] = data
        if len(cache) > self._cache_max:
            # Insertion-ordered, oldest first: drop the oldest eighth in one
            # pass rather than one entry per insert.
            for k in list(cache)[:self._cache_max // 8]:
                del cache[k]

    # -- decoding in parallel ----------------------------------------------

    def _get_pool(self):
        """The worker pool, made on first use and never on a serial run."""
        if self.jobs < 2:
            return None
        if self._pool is None:
            try:
                ctx = multiprocessing.get_context("fork")
                inherited = self
            except ValueError:                    # no fork: Windows
                ctx = multiprocessing.get_context("spawn")
                inherited = None
            try:
                self._pool = ctx.Pool(self.jobs, initializer=_worker_init,
                                      initargs=(self.path, inherited))
            except (OSError, ValueError, ImportError) as exc:
                # A sandbox with no /dev/shm, or a Python built without it.
                # Falling back is right; failing the ingest over it is not.
                print(f"chd: no worker pool ({exc}); decoding on one core")
                self.jobs = 1
                return None
        return self._pool

    def decode_stream(self, hunks, batch=16, ahead=None):
        """Yield each hunk's bytes, IN ORDER, decoded across processes.

        Ordered because both callers need it: a SHA-1 over the image, and a
        file being written out of it. The look-ahead is bounded, so a 6 GB
        image streams through a few megabytes of memory rather than being
        assembled in it.
        """
        pool = self._get_pool()
        if pool is None:
            for h in hunks:
                yield self.hunk(h)
            return
        ahead = ahead or 4 * self.jobs
        pending = collections.deque()
        cur = []
        for h in hunks:
            cur.append(h)
            if len(cur) >= batch:
                pending.append(pool.apply_async(_worker_batch, (cur,)))
                cur = []
                if len(pending) >= ahead:
                    for data in pending.popleft().get():
                        self.hunks_decoded += 1
                        yield data
        if cur:
            pending.append(pool.apply_async(_worker_batch, (cur,)))
        while pending:
            for data in pending.popleft().get():
                self.hunks_decoded += 1
                yield data

    def prefetch(self, hunks):
        """Decode these hunks in parallel and leave them in the cache, so the
        ordinary read path finds them already there."""
        todo = [h for h in hunks if h not in self._cache]
        if len(todo) < 2 or self.jobs < 2:
            return
        for h, data in zip(todo, self.decode_stream(todo)):
            self._store(h, data)

    # -- metadata ----------------------------------------------------------

    def metadata(self):
        """Every metadata entry as (tag, index, bytes). The disk geometry lives
        here, in a 'GDDD' entry, and it is the only reason to look."""
        out = []
        offs = self.metaoffset
        counts = {}
        seen = set()
        while offs and offs not in seen:          # a bad chain must not loop
            seen.add(offs)
            self.f.seek(offs)
            head = self.f.read(16)
            if len(head) < 16:
                break
            tag = head[0:4]
            length = int.from_bytes(head[5:8], "big")
            nxt = int.from_bytes(head[8:16], "big")
            data = self.f.read(length)
            idx = counts.get(tag, 0)
            counts[tag] = idx + 1
            out.append((tag.decode("latin1"), idx, data))
            offs = nxt
        return out

    def close(self):
        if self._pool is not None:
            self._pool.terminate()
            self._pool.join()
            self._pool = None
        self.f.close()
        self._cache.clear()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class ChdImage:
    """The CHD as a read-only file object: seek(), read(), tell().

    This is the whole point of the module. `ingest-chd.py`'s partition reader
    and FAT reader take a file object and do nothing but seek and read, so
    handing them one of these makes the CHD itself the input and deletes the
    multi-gigabyte scratch image from the process entirely.
    """

    def __init__(self, path_or_chd, window=2048, **kw):
        self.chd = path_or_chd if isinstance(path_or_chd, Chd) \
            else Chd(path_or_chd, **kw)
        self.pos = 0
        self.size = self.chd.logicalbytes
        self._plan = None
        self._at = 0
        self._window = window

    def plan_reads(self, ranges):
        """Say which byte ranges are about to be read, in the order they will
        be read in.

        A file's cluster chain is known before a byte of it is copied, so the
        reader can hand that list over and have the hunks decoded in parallel,
        a window at a time, instead of one at a time on demand. Without this
        the pool is useless here: a sequential `read()` can only ever ask for
        the next hunk, which is one core's worth of work.
        """
        hb = self.chd.hunkbytes
        plan = []
        last = -1
        for start, length in ranges:
            if length <= 0:
                continue
            for h in range(start // hb, (start + length - 1) // hb + 1):
                if h != last:
                    plan.append(h)
                    last = h
        self._plan, self._at = plan, 0

    def _prefetch_at(self, h):
        """If h is where the plan says we are, decode the next window."""
        plan = self._plan
        if not plan or self.chd.jobs < 2 or h in self.chd._cache:
            return
        i = self._at
        while i < len(plan) and plan[i] != h and i < self._at + 4:
            i += 1                                # tolerate a hunk or two of slack
        if i >= len(plan) or plan[i] != h:
            return                                # not the planned order: on demand
        window = plan[i:i + self._window]
        self._at = i + len(window)
        self.chd.prefetch(window)

    def seek(self, offset, whence=0):
        base = {0: 0, 1: self.pos, 2: self.size}[whence]
        self.pos = max(0, base + offset)
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        if not n:
            return b""
        hb = self.chd.hunkbytes
        out = bytearray()
        pos = self.pos
        while n:
            h, off = divmod(pos, hb)
            self._prefetch_at(h)
            take = min(n, hb - off)
            out += self.chd.hunk(h)[off:off + take]
            pos += take
            n -= take
        self.pos = pos
        return bytes(out)

    def close(self):
        self.chd.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


# ---- cli -----------------------------------------------------------------

def info(path):
    with Chd(path) as c:
        comp = ", ".join(x.decode("latin1") if x != b"\0\0\0\0" else "none"
                         for x in c.compressors)
        print(f"{path}")
        print(f"  version      5")
        print(f"  logical size {c.logicalbytes:,} bytes")
        print(f"  hunk / unit  {c.hunkbytes} / {c.unitbytes}")
        print(f"  hunks        {c.hunkcount:,}")
        print(f"  compressors  {comp}")
        print(f"  data SHA-1   {c.rawsha1.hex()}")
        got, want = c.map_crc()
        if want is not None:
            print(f"  map CRC-16   {got:#06x} computed, {want:#06x} stored: "
                  f"{'MATCH' if got == want else 'MISMATCH'}")
        for tag, idx, data in c.metadata():
            text = data.rstrip(b"\0").decode("latin1", "replace")
            print(f"  metadata     {tag}[{idx}] {text}")
        hist = {}
        for h in range(c.hunkcount):
            hist[c.comps[h]] = hist.get(c.comps[h], 0) + 1
        print(f"\n      Hunks  Percent  Name")
        print(f"  ---------  -------  ----------------")
        for k in sorted(hist):
            name = _KIND.get(k) or c.compressors[k].decode("latin1")
            print(f"  {hist[k]:>9,}  {100.0 * hist[k] / c.hunkcount:6.1f}%  {name}")
    return 0


def verify(path, jobs=None):
    """Decode EVERY hunk and check the two things the file says about itself:
    each hunk's CRC-16 (inside Chd.hunk) and the SHA-1 of the whole image.

    This is the cross-check that needs no MAME. It is also slow -- it decodes
    the entire disk rather than the ~1 GB of files an ingest touches -- and it
    is the reason to trust the fast path that does not."""
    # A whole-image pass walks every "copy from self" reference there is, and
    # the ones a small cache drops have to be decoded again. 64 MB of hunks is
    # nothing next to decoding the disk twice.
    with Chd(path, cache_hunks=16384, jobs=jobs) as c:
        got, want = c.map_crc()
        if want is not None and got != want:
            print(f"FAILED: map CRC-16 {got:#06x} != stored {want:#06x}")
            return 1
        print(f"map CRC-16 OK" if want is not None else "uncompressed map")
        h = hashlib.sha1()
        left = c.logicalbytes
        step = max(1, c.hunkcount // 40)
        print(f"  decoding on {c.jobs} core{'' if c.jobs == 1 else 's'}")
        for n, data in enumerate(c.decode_stream(range(c.hunkcount))):
            if len(data) > left:
                data = data[:left]
            h.update(data)
            left -= len(data)
            if n % step == 0:
                pct = 100.0 * n / c.hunkcount
                print(f"\r  {pct:5.1f}%  hunk {n:,}/{c.hunkcount:,}",
                      end="", flush=True)
        print(f"\r  100.0%  {c.hunkcount:,} hunks decoded" + " " * 20)
        print("  every hunk matched its own CRC-16" if want is not None else
              "  an uncompressed CHD carries no per-hunk CRCs: the SHA-1 is the check")
        digest = h.digest()
        print(f"  SHA-1 {digest.hex()}")
        print(f"  header {c.rawsha1.hex()}")
        if digest != c.rawsha1:
            print("FAILED: the image this reader produces is not the image the "
                  "CHD says it holds.")
            return 1
        print("OK: byte-for-byte the image chdman would have written.")
    return 0


def check(path, per_codec=100, jobs=1):
    """The bounded version of --verify, for `make check`.

    A full verify decodes the whole disk and takes minutes; this checks the map
    against its CRC-16 and then a spread of hunks of EVERY codec the file
    actually uses against theirs. It is seconds, it exercises each decoder on
    real data, and unlike a fixed sample it cannot silently stop covering a
    codec: what it checks is derived from what the file contains.
    """
    with Chd(path, jobs=jobs) as c:
        print(f"{os.path.basename(path)}:")
        got, want = c.map_crc()
        if want is None:
            print("  uncompressed map, no CRCs to check")
        else:
            print(f"  map CRC-16 {got:#06x} == {want:#06x}")
        buckets = {}
        for h in range(c.hunkcount):
            buckets.setdefault(c.comps[h], []).append(h)
        bad = 0
        for k in sorted(buckets):
            hs = buckets[k]
            name = _KIND.get(k) or c.compressors[k].decode("latin1")
            sample = hs[::max(1, len(hs) // per_codec)][:per_codec]
            ok = 0
            for h in sample:
                try:
                    c.hunk(h)                        # CRC is checked inside
                    ok += 1
                except ChdError as exc:
                    if ok == len(sample) - 1 or bad == 0:
                        print(f"    {name}: hunk {h}: {exc}")
                    bad += 1
            print(f"  {name:<14} {ok:>4}/{len(sample):<4} sampled hunks of "
                  f"{len(hs):,} match their CRC-16")
    return 1 if bad else 0


def selftest():
    """Check what can be checked without a CHD: the CRC-16, and the container
    plumbing round-tripped through a file this builds itself.

    This is deliberately modest about what it covers. It exercises the header,
    the uncompressed map, hunk assembly, self-references and ChdImage's seek and
    read; it does NOT exercise the compressed map or any of the four codecs,
    because writing a CHD would mean writing an encoder for each. Those are
    checked by `--verify` against a real CHD, which is where a decoder bug will
    actually show, and every hunk in one carries its own CRC-16 for the purpose.
    """
    ok = True

    # CRC-16/CCITT, the standard check value.
    got = crc16(b"123456789")
    print(f"  crc16('123456789') = {got:#06x}, want 0x29b1: "
          f"{'OK' if got == 0x29B1 else 'FAILED'}")
    ok &= got == 0x29B1

    # A whole uncompressed CHD, built here, read back through ChdImage. The
    # last two hunks are never written, which is how a CHD stores a hole: the
    # map entry is 0 and the reader must produce zeroes for it.
    import tempfile
    hb, n = 4096, 8
    data = b"".join(bytes([i + 1]) * hb for i in range(n - 2)) + bytes(2 * hb)
    hdr = bytearray(hb)
    hdr[0:8] = MAGIC
    struct.pack_into(">II", hdr, 8, HEADER_LEN, 5)
    struct.pack_into(">QQQ", hdr, 32, len(data), hb + (n - 2) * hb, 0)
    struct.pack_into(">II", hdr, 56, hb, 512)
    hdr[64:84] = hashlib.sha1(data).digest()
    table = b"".join(struct.pack(">I", 0 if i >= n - 2 else 1 + i)
                     for i in range(n))
    with tempfile.NamedTemporaryFile(suffix=".chd", delete=False) as tf:
        tf.write(bytes(hdr) + data[:(n - 2) * hb] + table)
        path = tf.name
    try:
        with ChdImage(path) as img:
            same = img.read() == data
            print(f"  8-hunk uncompressed CHD reads back identical: "
                  f"{'OK' if same else 'FAILED'}")
            ok &= same
            img.seek(hb * 3 + 17)
            spans = img.read(hb * 2)                 # across a hunk boundary
            want = data[hb * 3 + 17:hb * 5 + 17]
            print(f"  a read across a hunk boundary is right: "
                  f"{'OK' if spans == want else 'FAILED'}")
            ok &= spans == want
            img.seek(-hb, 2)
            hole = img.read()
            print(f"  an unwritten hunk reads as zeroes: "
                  f"{'OK' if hole == bytes(hb) else 'FAILED'}")
            ok &= hole == bytes(hb)
    finally:
        os.unlink(path)

    # A file that is not a CHD, and a version this reader does not open, must
    # both be refused by name rather than misread.
    for blob, what in ((b"not a chd at all" + bytes(200), "a non-CHD file"),
                       (bytes(MAGIC) + struct.pack(">II", 124, 4) + bytes(112),
                        "a v4 CHD")):
        with tempfile.NamedTemporaryFile(suffix=".chd", delete=False) as tf:
            tf.write(blob)
            path = tf.name
        try:
            Chd(path)
            print(f"  {what} was opened: FAILED")
            ok = False
        except ChdUnsupported as exc:
            print(f"  {what} is declined by name: OK ({exc.args[0].split(': ')[-1]})")
        finally:
            os.unlink(path)

    print("selftest: " + ("all OK" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(
        description="Read a MAME CHD v5 without MAME.",
        epilog="With no option, prints what `chdman info -v` prints.")
    ap.add_argument("chd", nargs="*")
    ap.add_argument("--jobs", type=int, metavar="N",
                    help="decode on N cores (default: all of them for --verify, "
                         "one for --check, which is a sample and not a sweep)")
    ap.add_argument("--check", action="store_true",
                    help="the bounded check `make check` runs: the map, and a "
                         "spread of hunks of every codec the file uses")
    ap.add_argument("--selftest", action="store_true",
                    help="check the container plumbing without needing a CHD")
    ap.add_argument("--verify", action="store_true",
                    help="decode every hunk and check its CRC-16, then check "
                         "the whole image against the SHA-1 in the header")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.chd:
        ap.error("name a CHD file, or pass --selftest")
    rc = 0
    for p in a.chd:
        try:
            rc |= (verify(p, a.jobs) if a.verify else
                   (check(p, jobs=a.jobs or 1) if a.check else info(p)))
        except ChdError as exc:
            print(f"{p}: {exc}")
            rc |= 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
