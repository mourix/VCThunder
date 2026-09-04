#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""make-icon.py -- draw res/vcthunder.ico: a blue lightning bolt on a black disc.

The icon is a generated artefact, not a hand-drawn binary checked in and never
explained again: the shape lives here as coordinates, so "make the bolt thicker"
is an edit rather than a redraw in a paint program nobody has.

WHY IT IS DRAWN PER SIZE INSTEAD OF DOWNSAMPLED FROM ONE. An .ico carries
several bitmaps and Windows picks one: 16 px in the window caption, 32 px for
Alt-Tab and the taskbar, larger in Explorer. Letting the ICO writer resample a
single 256 px image gives a 16 px entry whose bolt is a grey smear, because a
stroke two pixels wide at that size cannot survive an area filter. Each size is
therefore rendered at 8x and reduced to itself, which keeps the antialiasing
proportional to the pixel it lands in.

Run:  python3 tools/make-icon.py            (writes res/vcthunder.ico, prints sha256)
"""
import hashlib
import os
import sys

from PIL import Image, ImageDraw

SS = 8                                      # supersample factor per size
SIZES = [16, 24, 32, 48, 64, 128, 256]

DISC = (0x00, 0x00, 0x00, 0xFF)             # the black circle
RIM = (0x1C, 0x2A, 0x3A, 0xFF)              # a hair of edge, so a black disc on
                                            # a black taskbar is still a disc
BOLT_TOP = (0x7A, 0xD1, 0xFF, 0xFF)         # lit top
BOLT_BOTTOM = (0x0E, 0x6F, 0xE0, 0xFF)      # deeper blue at the tip

# The classic bolt, in a 0..100 box with y running down, then pulled in towards
# the centre so its two extreme points clear the disc's edge rather than touch it.
BOLT = [(62, 4), (28, 56), (47, 56), (38, 96), (74, 40), (53, 40)]
BOLT_INSET = 0.80


def bolt_points(px):
    """The bolt polygon in pixels for a px-wide canvas."""
    return [((50.0 + (x - 50.0) * BOLT_INSET) * px / 100.0,
             (50.0 + (y - 50.0) * BOLT_INSET) * px / 100.0) for x, y in BOLT]


def vertical_gradient(px, top, bottom):
    grad = Image.new("RGBA", (1, px))
    for y in range(px):
        t = y / float(px - 1)
        grad.putpixel((0, y), tuple(int(round(a + (b - a) * t))
                                    for a, b in zip(top, bottom)))
    return grad.resize((px, px), Image.NEAREST)


def render(size):
    px = size * SS
    img = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Full-bleed disc with a half-pixel of breathing room, so the antialiased
    # edge is not clipped by the bitmap boundary.
    pad = max(1, int(round(px * 0.012)))
    draw.ellipse((pad, pad, px - pad - 1, px - pad - 1), fill=DISC, outline=RIM,
                 width=max(1, int(round(px * 0.02))))

    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).polygon(bolt_points(px), fill=255)
    img.paste(vertical_gradient(px, BOLT_TOP, BOLT_BOTTOM), (0, 0), mask)

    return img.resize((size, size), Image.LANCZOS)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "res", "vcthunder.ico")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    # LARGEST FIRST, AND THE REST APPENDED. Pillow's ICO writer skips any
    # requested size larger than the base image, so handing it the 16 px frame
    # first silently produces a one-entry icon, which is exactly what it did.
    frames = [render(s) for s in sorted(SIZES, reverse=True)]
    frames[0].save(out, format="ICO", sizes=[(s, s) for s in SIZES],
                   append_images=frames[1:])

    with open(out, "rb") as fp:
        blob = fp.read()
    got = sorted(Image.open(out).info.get("sizes", []))
    print("%s: %d bytes, sizes %s" % (os.path.relpath(out, root), len(blob),
                                      ", ".join("%dx%d" % s for s in got)))
    print("sha256 %s" % hashlib.sha256(blob).hexdigest())
    if got != sorted((s, s) for s in SIZES):
        print("WARNING: the written .ico does not carry every size asked for",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
