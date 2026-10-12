#!/usr/bin/env python3
"""Platform image and core icon for the Pocket menu (DECISIONS D42 style, D54).

    python tools/ui/platform_art.py      -> pkg/platforms/_images/ambient.bin, pkg/icon.bin,
                                            docs/ui/platform_art.png (3x preview of both)

Format (Analogue's core template + the openFPGA library converter): platform image 521x165,
icon 36x36; one 16-bit little-endian word per pixel, value = 255 - brightness (255 = black),
row-major from the upright picture rotated 90 degrees counter-clockwise.

Style: a single-colour phosphor display (thin double frames, corner crosshairs, blocky
wide-tracked caps from tools/ui/mockup.py, dotted rules, wireframe globe). Original art only.
"""
import math
import os
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mockup import cross, dotted_h, frame, text, text_w  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FULL, MID, DIM = 255, 150, 80                     # brightness levels (greyscale on the Pocket)


def globe(d, cx, cy, r, turn=0.0, meridians=7, parallels=(-50, -25, 0, 25, 50), step=4):
    """Wireframe sphere: near-side meridians (turned by `turn` radians) and parallels."""
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=FULL)
    for k in range(meridians):
        lon = -math.pi / 2 + (k + 0.5 + turn) * math.pi / meridians
        sx = r * math.sin(lon)
        pts = [(cx + sx * math.cos(math.radians(lat)), cy - r * math.sin(math.radians(lat)))
               for lat in range(-90, 91, step)]
        d.line(pts, fill=MID)
    for lat in parallels:
        y = cy - r * math.sin(math.radians(lat))
        rr = r * math.cos(math.radians(lat))
        d.line([cx - rr, y, cx + rr, y], fill=MID if lat else FULL)


def platform_image():
    W, H = 521, 165
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    frame(d, 0, 0, W - 1, H - 1, FULL)
    for (x, y) in ((16, 16), (W - 17, 16), (16, H - 17), (W - 17, H - 17)):
        cross(d, x, y, MID, r=7)
    # left: wireframe globe with irregular terrain contours on its face
    cx, cy, r = 92, 82, 58
    globe(d, cx, cy, r, turn=0.3)
    for k in range(3):
        rr = 8 + k * 6
        pts = []
        for i in range(41):
            a = 2 * math.pi * i / 40
            w = rr * (1 + 0.22 * math.sin(3 * a + k) + 0.12 * math.cos(5 * a + 2 * k))
            pts.append((cx + 14 + 1.5 * w * math.cos(a), cy + 16 + 0.8 * w * math.sin(a)))
        d.line(pts, fill=FULL)
    d.line([cx - r - 10, cy, cx - r - 4, cy], fill=FULL)
    d.line([cx + r + 4, cy, cx + r + 10, cy], fill=FULL)
    d.line([cx, cy - r - 10, cx, cy - r - 4], fill=FULL)
    d.line([cx, cy + r + 4, cx, cy + r + 10], fill=FULL)
    # right: title block
    x0 = 178
    d.line([x0 - 8, 10, x0 - 8, H - 11], fill=MID)
    text(d, x0, 22, "AMBIENT", FULL, scale=5, spacing=2)
    dotted_h(d, x0, W - 14, 72, MID, step=4, size=2)
    text(d, x0, 80, "GENERATIVE SOUND SYSTEM", FULL, scale=1, spacing=3)
    # a slow waveform trace
    pts = []
    for i in range(W - 14 - x0):
        t = i / 40.0
        pts.append((x0 + i, 106 + 9 * math.sin(t * 2.0) * math.sin(t * 0.35 + 1.0) + 2 * math.sin(t * 5.1)))
    d.line(pts, fill=MID)
    # four boxed mode readouts
    bx, bw = x0, 72
    for i, name in enumerate(("DRIFT", "TIDES", "HUSH", "MOTHER")):
        x = bx + i * (bw + 8)
        d.rectangle([x, 126, x + bw, 144], outline=FULL)
        text(d, x + (bw - text_w(name, 1, 1)) // 2 + 1, 131, name, FULL, scale=1, spacing=1)
    return im


def icon():
    im = Image.new("L", (36, 36), 0)
    d = ImageDraw.Draw(im)
    frame(d, 0, 0, 35, 35, FULL)
    globe(d, 17.5, 17.5, 11, turn=0.3, meridians=4, parallels=(-35, 0, 35), step=6)
    for (x, y) in ((6, 6), (29, 6), (6, 29), (29, 29)):
        d.point((x, y), fill=FULL)
    return im


def to_bin(im):
    """Upright greyscale picture -> Pocket .bin (rotated 90 degrees CCW, 255 - brightness)."""
    rot = im.rotate(90, expand=True)
    return struct.pack("<%dH" % (rot.width * rot.height), *(255 - v for v in rot.tobytes()))


def main():
    plat, ico = platform_image(), icon()
    files = [(plat, "pkg/platforms/_images/ambient.bin", 521 * 165 * 2), (ico, "pkg/icon.bin", 36 * 36 * 2)]
    for im, path, size in files:
        b = to_bin(im)
        assert len(b) == size, (path, len(b))
        with open(os.path.join(ROOT, path), "wb") as f:
            f.write(b)
        print(f"{path}: {len(b)} bytes")
    # preview: both images at 3x, phosphor green on black
    s = 3
    pv = Image.new("RGB", ((521 + 36 + 24) * s, (165 + 16) * s), (0, 0, 0))
    for im, x in ((plat, 8), (ico, 521 + 16)):
        g = im.resize((im.width * s, im.height * s), Image.NEAREST)
        col = Image.merge("RGB", (g.point(lambda v: v * 70 // 255), g, g.point(lambda v: v * 130 // 255)))
        pv.paste(col, (x * s, 8 * s))
    out = os.path.join(ROOT, "docs", "ui", "platform_art.png")
    pv.save(out)
    print(f"docs/ui/platform_art.png: {pv.width}x{pv.height}")


if __name__ == "__main__":
    main()
