#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""make-conformance-capture.py -- a synthetic .vglc stream for the rasteriser.

WHY THIS EXISTS
---------------
The renderer's strongest instrument ("replay a capture and prove the change
moved no pixel") cannot run inside `make check` on a real capture: every
capture of a title is game material, and `tools/scan-contraband.py` correctly
refuses to let any into this repository.

This generator has no game in it. The scene below is authored here, from the
published Glide semantics and this project's own measurements, so the capture
it writes is project-authored text expanded into a binary at build time. That
makes the whole loop (generate, replay, compare hashes) something a check
target can run on a clone with no dumps.

WHAT IT DOES *NOT* REPLACE
--------------------------
An 86Box oracle diff. This says "the renderer draws what it drew before"; it
cannot say "the renderer draws what a Voodoo2 drew". Those are different
questions and only the second one needs a comparator. Nor is it a substitute
for the game captures: a synthetic scene covers the paths someone thought of,
and Offroad's 408 palette downloads in 60 frames are still the thing that found
a texture-cache bug 1,600 identical Hydro frames called perfect.

The golden hashes are per-toolchain: they are this renderer's own output, and a
different compiler may legitimately differ in the last bit.
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

VGLC_MAGIC = 0x434C4756
VGLC_VERSION = 1
VGLC_CONTROL = 0xFFFF
VGLC_FRAME, VGLC_LFB, VGLC_END = 1, 2, 3
VGLC_TAG_TEXDATA = 0x100
VGLC_TAG_LFB = 0x101

PAYLOADS = [
    "PL_NONE", "PL_V2", "PL_V3", "PL_VLIST", "PL_FOG", "PL_TEXDOWN",
    "PL_TEXINFO3", "PL_TEXINFO1", "PL_TEXPART", "PL_TEXTABLE", "PL_STATE_IN",
    "PL_STATE_OUT", "PL_HWCFG", "PL_CALLBACK", "PL_LFBINFO", "PL_LFBUNLOCK",
    "PL_WINOPEN", "PL_SWAP", "PL_VIDTIMING",
]


def read_entries() -> list[tuple[str, int, int]]:
    """The entry table, from the one place it is written down."""
    text = (ROOT / "renderer" / "api" / "entries.def").read_text(encoding="utf-8")
    out = []
    for name, argbytes, payload, _state in re.findall(
        r"^VGL_ENTRY\((\w+),\s*(\d+),\s*(\w+),\s*(\w+)\)", text, re.M
    ):
        out.append((name, int(argbytes), PAYLOADS.index(payload)))
    if not out:
        raise SystemExit("entries.def parsed to nothing")
    return out


# ---------------------------------------------------------------- the writer:

class Capture:
    def __init__(self, title: str):
        self.entries = read_entries()
        self.index = {n: i for i, (n, _, _) in enumerate(self.entries)}
        self.argbytes = {n: a for n, a, _ in self.entries}
        self.title = title
        self.records = bytearray()
        self.n_records = 0
        self.frames = 0

    @staticmethod
    def _pad(buf: bytearray) -> None:
        buf.extend(b"\0" * (-len(buf) % 4))

    def call(self, name: str, *args, blobs: list[tuple[int, bytes]] | None = None):
        """One recorded call. `args` are the stack words, already ints."""
        idx = self.index[name]
        words = self.argbytes[name] // 4
        if len(args) != words:
            raise SystemExit(f"{name} takes {words} words, got {len(args)}")
        blobs = blobs or []
        self.records += struct.pack("<HH", idx, len(blobs))
        for a in args:
            self.records += struct.pack("<I", a & 0xFFFFFFFF)
        for tag, data in blobs:
            self.records += struct.pack("<II", tag, len(data))
            self.records += data
            self._pad(self.records)
        self.n_records += 1

    def control(self, kind: int, *rest, blobs=None):
        blobs = blobs or []
        args = (kind,) + rest
        args = args + (0,) * (4 - len(args))
        self.records += struct.pack("<HH", VGLC_CONTROL, len(blobs))
        for a in args:
            self.records += struct.pack("<I", a & 0xFFFFFFFF)
        for tag, data in blobs:
            self.records += struct.pack("<II", tag, len(data))
            self.records += data
            self._pad(self.records)
        self.n_records += 1

    def frame(self):
        self.control(VGLC_FRAME, self.frames)
        self.frames += 1

    def write(self, path: Path):
        self.control(VGLC_END, self.n_records + 1, self.frames)
        table = bytearray()
        for name, argbytes, payload in self.entries:
            table += struct.pack("<HBB", argbytes, payload, len(name))
            table += name.encode("ascii")
            table.extend(b"\0" * (-(4 + len(name)) % 4))
        header_bytes = 128 + len(table)      # struct "<8I32s64s" is 128
        header = struct.pack(
            "<8I32s64s",
            VGLC_MAGIC, VGLC_VERSION, header_bytes, len(self.entries),
            60, 0, 0, 0,
            self.title.encode("ascii")[:31],
            b"synthetic (tools/make-conformance-capture.py)",
        )
        assert len(header) + len(table) == header_bytes, (len(header), header_bytes)
        path.write_bytes(bytes(header) + bytes(table) + bytes(self.records))
        return header_bytes


def f(x: float) -> int:
    """A float as the stack word that carries it."""
    return struct.unpack("<I", struct.pack("<f", x))[0]


# ------------------------------------------------------------------ the scene:

W, H = 640, 400
RES_640x400 = 6                       # the enum, per GLIDE_RESOLUTION
# One palette index per mip level, so a level shows as a flat colour.
MIP_COLOURS = (0x10, 0x40, 0x70, 0xA0, 0xD0, 0xF0)


def vertex(x, y, r, g, b, a, oow, s0=0.0, t0=0.0, s1=0.0, t1=0.0) -> bytes:
    """One GrVertex: x y z r g b ooz a oow (sow tow oow)x2, 15 floats."""
    return struct.pack(
        "<15f", x, y, 0.0, r, g, b, 0.0, a, oow,
        s0 * oow, t0 * oow, oow, s1 * oow, t1 * oow, oow,
    )


def tri(cap, va, vb, vc):
    cap.call("grDrawTriangle", 0, 0, 0,
             blobs=[(0, va), (1, vb), (2, vc)])


def tex_info(small_lod, large_lod, aspect, fmt) -> bytes:
    """GrTexInfo: smallLod, largeLod, aspectRatio, format, data."""
    return struct.pack("<5I", small_lod, large_lod, aspect, fmt, 0)


def checker(size: int, a: int, b: int, step: int = 4) -> bytes:
    """A deterministic 8-bit pattern: authored, not sampled from anything."""
    out = bytearray()
    for y in range(size):
        for x in range(size):
            out.append(a if ((x // step) + (y // step)) & 1 else b)
    return bytes(out)


def ramp565(size: int) -> bytes:
    out = bytearray()
    for y in range(size):
        for x in range(size):
            r, g, bl = x * 31 // (size - 1), y * 63 // (size - 1), 15
            out += struct.pack("<H", (r << 11) | (g << 5) | bl)
    return bytes(out)


def mip_chain(large_lod: int, small_lod: int, colours) -> bytes:
    """Every level from `large_lod` down to `small_lod`, concatenated.

    ONE buffer holding BOTH parities, which is what both games send: a split
    chain is downloaded twice from the same pointer, once masked EVEN and once
    ODD, and vgl_tex_download advances across the masked levels rather than
    skipping them in the source. Each level is a flat colour index so that
    which level got sampled is legible in the output.
    """
    out = bytearray()
    for lod in range(large_lod, small_lod + 1):
        side = 256 >> lod
        out += bytes([colours[lod % len(colours)]]) * (side * side)
    return bytes(out)


def quad(cap, v0, v1, v2, v3):
    tri(cap, v0, v1, v2)
    tri(cap, v0, v2, v3)


def build(cap: Capture):
    # --- the session -------------------------------------------------------
    cap.call("grGlideInit")
    cap.call("grSstSelect", 0)
    cap.call("grSstWinOpen", 0, RES_640x400, 0, 1, 0, 2, 1)

    # A 256-entry palette and two chains, downloaded once and re-sourced.
    pal = b"".join(struct.pack("<I", 0xFF000000 | (i << 16) | ((255 - i) << 8) | i)
                   for i in range(256))
    # LOD 8 is 1x1 and LOD 0 is 256x256; the aspect enum 3 is 1:1.
    info_p8 = tex_info(3, 3, 3, 5)                      # 32x32, GR_TEXFMT_P_8
    info_565 = tex_info(4, 4, 3, 10)                    # 16x16, RGB_565
    # A full chain, 32x32 down to 1x1, one flat index per level so which level
    # answered is legible in the picture rather than inferred from a histogram.
    info_chain = tex_info(8, 3, 3, 5)                   # smallLod 8, largeLod 3

    # --- frame 0: clears, gouraud, blend modes -----------------------------
    cap.frame()
    cap.call("grBufferClear", 0x00203040, 0xFF, 0xFFFF)
    cap.call("grDepthBufferMode", 0)
    cap.call("grDepthMask", 0)
    cap.call("grColorCombine", 1, 0, 0, 0, 0)           # LOCAL, iterated
    cap.call("grAlphaCombine", 1, 0, 0, 0, 0)
    cap.call("grCullMode", 0)
    for i, (src, dst) in enumerate(((4, 0), (1, 5), (4, 4), (8, 0))):
        cap.call("grAlphaBlendFunction", src, dst, 4, 0)
        x = 20.0 + i * 150.0
        tri(cap,
            vertex(x, 30.0, 255, 40, 40, 200, 1.0),
            vertex(x + 130.0, 45.0, 40, 255, 40, 200, 1.0),
            vertex(x + 20.0, 170.0, 40, 40, 255, 200, 1.0))
    cap.call("grBufferSwap", 1)

    # --- frame 1: one textured unit, both filters, wrap and clamp ----------
    cap.frame()
    cap.call("grBufferClear", 0x00101010, 0xFF, 0xFFFF)
    cap.call("grAlphaBlendFunction", 4, 0, 4, 0)
    cap.call("grTexDownloadTable", 0, 2, 0, blobs=[(2, pal)])   # GR_TEXTABLE_PALETTE
    cap.call("grTexDownloadMipMap", 0, 0, 0xFFFFFFFF, 0,
             blobs=[(3, info_p8), (VGLC_TAG_TEXDATA, checker(32, 0x20, 0xD0))])
    cap.call("grTexSource", 0, 0, 0xFFFFFFFF, 0, blobs=[(3, info_p8)])
    cap.call("grTexCombine", 0, 1, 0, 1, 0, 0, 0)       # DECAL
    cap.call("grTexMipMapMode", 0, 0, 0)                # no mipmapping here
    # factor 8 is GR_COMBINE_FACTOR_ONE (the ONE_MINUS bit over ZERO). With
    # factor 0 the combine is `0 * texture`, which is black, and a hash test
    # would have adopted a black screen as golden without complaint.
    cap.call("grColorCombine", 3, 8, 0, 1, 0)           # ONE * texture
    for i, (filt, clamp) in enumerate(((0, 0), (1, 0), (1, 1))):
        cap.call("grTexFilterMode", 0, filt, filt)
        cap.call("grTexClampMode", 0, clamp, clamp)
        x = 20.0 + i * 200.0
        # Wide enough in texel space that the 4-texel checker actually reads,
        # and outside 0..1 so wrap and clamp visibly disagree. Unequal w so the
        # perspective divide is exercised rather than skipped.
        quad(cap,
             vertex(x, 200.0, 255, 255, 255, 255, 1.00, -2.0, -2.0),
             vertex(x + 180.0, 200.0, 255, 255, 255, 255, 0.60, 6.0, -2.0),
             vertex(x + 180.0, 380.0, 255, 255, 255, 255, 0.60, 6.0, 6.0),
             vertex(x, 380.0, 255, 255, 255, 255, 1.00, -2.0, 6.0))
    cap.call("grBufferSwap", 1)

    # --- frame 2: MINIFICATION, so the mip chain is actually selected from;
    #
    # Everything above is magnified, which never leaves LOD 0 and so never
    # exercises select_level, the lvl_le/lvl_gt tables or the LOD histogram at
    # all. A receding quad with the texture repeating many times across it
    # sweeps the whole chain in one primitive.
    cap.frame()
    cap.call("grBufferClear", 0x00000020, 0xFF, 0xFFFF)
    cap.call("grTexDownloadMipMap", 0, 0, 0xFFFFFFFF, 0,
             blobs=[(3, info_chain),
                    (VGLC_TAG_TEXDATA, mip_chain(3, 8, MIP_COLOURS))])
    cap.call("grTexSource", 0, 0, 0xFFFFFFFF, 0, blobs=[(3, info_chain)])
    cap.call("grTexFilterMode", 0, 1, 1)
    cap.call("grTexClampMode", 0, 0, 0)
    cap.call("grTexMipMapMode", 0, 1, 0)                # GR_MIPMAP_NEAREST
    quad(cap,
         vertex(40.0, 380.0, 255, 255, 255, 255, 1.000, 0.0, 0.0),
         vertex(600.0, 380.0, 255, 255, 255, 255, 1.000, 4096.0, 0.0),
         vertex(400.0, 40.0, 255, 255, 255, 255, 0.025, 4096.0, 16384.0),
         vertex(240.0, 40.0, 255, 255, 255, 255, 0.025, 0.0, 16384.0))
    cap.call("grBufferSwap", 1)

    # --- frame 3: the SPLIT CHAIN across two units -------------------------
    #
    # The path this project has spent the most on. One buffer holding every
    # level is downloaded twice, masked EVEN to TMU0 and ODD to TMU1, and
    # TMU0's combine cross-fades between them on LOD_FRACTION, which is
    # two-TMU trilinear filtering, NOT detail texturing: the two look alike at
    # the call site and the misreading is expensive.
    cap.frame()
    cap.call("grBufferClear", 0x00000000, 0xFF, 0xFFFF)
    chain = mip_chain(3, 8, MIP_COLOURS)
    cap.call("grTexDownloadTable", 1, 2, 0, blobs=[(2, pal)])
    cap.call("grTexDownloadMipMap", 0, 0, 1, 0,          # EVEN levels
             blobs=[(3, info_chain), (VGLC_TAG_TEXDATA, chain)])
    cap.call("grTexDownloadMipMap", 1, 0, 2, 0,          # ODD levels
             blobs=[(3, info_chain), (VGLC_TAG_TEXDATA, chain)])
    cap.call("grTexSource", 0, 0, 1, 0, blobs=[(3, info_chain)])
    cap.call("grTexSource", 1, 0, 2, 0, blobs=[(3, info_chain)])
    cap.call("grTexFilterMode", 1, 1, 1)
    cap.call("grTexMipMapMode", 0, 1, 1)                # mode NEAREST, lodBlend
    cap.call("grTexMipMapMode", 1, 1, 1)
    cap.call("grTexCombine", 1, 1, 0, 1, 0, 0, 0)       # TMU1: its own texel
    # TMU0: LOD_FRACTION * (other - local) + local; the cross-fade itself.
    cap.call("grTexCombine", 0, 7, 5, 7, 5, 0, 0)
    quad(cap,
         vertex(40.0, 380.0, 255, 255, 255, 255, 1.000, 0.0, 0.0, 0.0, 0.0),
         vertex(600.0, 380.0, 255, 255, 255, 255, 1.000, 4096.0, 0.0, 4096.0, 0.0),
         vertex(400.0, 40.0, 255, 255, 255, 255, 0.025,
                4096.0, 16384.0, 4096.0, 16384.0),
         vertex(240.0, 40.0, 255, 255, 255, 255, 0.025,
                0.0, 16384.0, 0.0, 16384.0))
    cap.call("grTexMipMapMode", 0, 0, 0)
    cap.call("grTexMipMapMode", 1, 0, 0)
    cap.call("grBufferSwap", 1)

    # --- frame 4: two units, chroma key, alpha test, fog -------------------
    cap.frame()
    cap.call("grBufferClear", 0x00000000, 0xFF, 0xFFFF)
    cap.call("grTexDownloadMipMap", 1, 0, 0xFFFFFFFF, 0,
             blobs=[(3, info_565), (VGLC_TAG_TEXDATA, ramp565(16))])
    cap.call("grTexSource", 1, 0, 0xFFFFFFFF, 0, blobs=[(3, info_565)])
    cap.call("grTexFilterMode", 1, 1, 1)
    cap.call("grTexCombine", 1, 1, 0, 1, 0, 0, 0)       # TMU1: its own texel
    cap.call("grTexCombine", 0, 4, 1, 4, 1, 0, 0)       # TMU0: modulate other
    cap.call("grColorCombine", 3, 8, 0, 1, 0)
    cap.call("grChromakeyValue", 0x00202020)
    cap.call("grChromakeyMode", 1)
    cap.call("grAlphaTestReferenceValue", 0x40)
    cap.call("grAlphaTestFunction", 6)                  # GEQUAL
    fog = bytes(min(255, i * 4) for i in range(64))
    cap.call("grFogTable", 0, blobs=[(0, fog)])
    cap.call("grFogColorValue", 0x00806040)
    cap.call("grFogMode", 2)
    tri(cap,
        vertex(60.0, 40.0, 255, 255, 255, 255, 0.90, 0.0, 0.0, 0.0, 0.0),
        vertex(580.0, 60.0, 255, 255, 255, 255, 0.12, 3.0, 0.0, 1.0, 0.0),
        vertex(300.0, 360.0, 255, 255, 255, 255, 0.40, 1.5, 3.0, 0.5, 1.0))
    cap.call("grFogMode", 0)
    cap.call("grChromakeyMode", 0)
    cap.call("grAlphaTestFunction", 7)                  # ALWAYS
    cap.call("grBufferSwap", 1)

    # --- frame 5: the depth buffer, and a vertex list ---------------------
    cap.frame()
    cap.call("grBufferClear", 0x00303030, 0xFF, 0xFFFF)
    cap.call("grDepthBufferMode", 2)                    # W buffer
    cap.call("grDepthBufferFunction", 3)                # LEQUAL
    cap.call("grDepthMask", 1)
    cap.call("grColorCombine", 1, 0, 0, 0, 0)
    # Two overlapping triangles at different depths, near one drawn second.
    tri(cap,
        vertex(100.0, 100.0, 220, 60, 60, 255, 0.30),
        vertex(400.0, 120.0, 220, 60, 60, 255, 0.30),
        vertex(180.0, 340.0, 220, 60, 60, 255, 0.30))
    tri(cap,
        vertex(200.0, 60.0, 60, 220, 60, 255, 0.80),
        vertex(520.0, 140.0, 60, 220, 60, 255, 0.80),
        vertex(260.0, 300.0, 60, 220, 60, 255, 0.80))
    verts = b"".join((
        vertex(420.0, 200.0, 60, 60, 220, 255, 0.50),
        vertex(600.0, 230.0, 60, 60, 220, 255, 0.50),
        vertex(560.0, 380.0, 60, 60, 220, 255, 0.50),
        vertex(430.0, 350.0, 60, 60, 220, 255, 0.50),
    ))
    cap.call("grDrawPolygonVertexList", 4, 0, blobs=[(1, verts)])
    cap.call("grDepthBufferMode", 0)
    cap.call("grDepthMask", 0)
    cap.call("grBufferSwap", 1)

    # --- frame 6: an LFB page, which is a different mechanism entirely -----
    #
    # A frame with a full-surface LFB write is reproduced by memcpy whatever
    # the rasteriser did, so this frame proves the LFB path and
    # NOTHING about the rasteriser. It is here to keep that path covered and it
    # is counted separately in the report.
    cap.frame()
    region = bytearray()
    for y in range(64):
        for x in range(W):
            region += struct.pack("<H", (((x >> 3) & 31) << 11) |
                                       (((y >> 1) & 63) << 5) | (x & 31))
    # arg5 is the caller's GrLfbInfo_t. The replay substitutes a shadow buffer
    # keyed on this value, so it has to be a plausible non-null address from
    # the captured process rather than 0: a null one is a resource failure.
    cap.call("grLfbLock", 1, 1, 0, 0, 0, 0x00A00000)
    cap.control(VGLC_LFB, 1, W * 2, 0, blobs=[(VGLC_TAG_LFB, bytes(region))])
    cap.call("grLfbUnlock", 1, 1)
    cap.call("grBufferSwap", 1)

    cap.call("grGlideShutdown")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    args = ap.parse_args()

    cap = Capture("synthetic")
    build(cap)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    n = cap.write(args.out)
    print(f"wrote {args.out}: {cap.frames} frames, {cap.n_records} records, "
          f"{n}-byte header, {args.out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
