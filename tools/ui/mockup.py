#!/usr/bin/env python3
"""Render UI mockups at the real 320x288 resolution (Phase 5 design, spec 2.4).

    python tools/ui/mockup.py            -> docs/ui/mock_<mode>.png (+ 4x previews)

Style: late-70s/80s ship-computer CRTs (single-colour phosphor, square blocky caps,
dotted rules, thin double frames, corner crosshairs, a right-hand readout column,
wireframe graphics). Only things the planned hardware can draw are used: a 4-colour
palette per mode, 1-px lines, and an 8x8 square font drawn at 1x, 2x or 3x.
"""
import math
import os
import random

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W, H = 320, 288

# ---------------------------------------------------------------- square font (5x7 in 8x8)
F = {
    "A": ["#####", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],   # notched: not an 8
    "C": ["#####", "#....", "#....", "#....", "#....", "#....", "#####"],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": ["#####", "#....", "#....", "#.###", "#...#", "#...#", "#####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    "J": ["....#", "....#", "....#", "....#", "#...#", "#...#", "#####"],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "O": ["#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#####"],
    "P": ["#####", "#...#", "#...#", "#####", "#....", "#....", "#...."],
    "Q": ["#####", "#...#", "#...#", "#...#", "#.#.#", "#..#.", "####."],
    "R": ["#####", "#...#", "#...#", "#####", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],   # open: not a 5
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", "#####"],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "0": ["#####", "#...#", "#..##", "#.#.#", "##..#", "#...#", "#####"],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": ["#####", "....#", "....#", "#####", "#....", "#....", "#####"],
    "3": ["#####", "....#", "....#", ".####", "....#", "....#", "#####"],
    "4": ["#...#", "#...#", "#...#", "#####", "....#", "....#", "....#"],
    "5": ["#####", "#....", "#....", "#####", "....#", "....#", "#####"],
    "6": ["#####", "#....", "#....", "#####", "#...#", "#...#", "#####"],
    "7": ["#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."],
    "8": ["#####", "#...#", "#...#", "#####", "#...#", "#...#", "#####"],
    "9": ["#####", "#...#", "#...#", "#####", "....#", "....#", "#####"],
    ":": ["", "..#..", "", "", "", "..#..", ""],
    ".": ["", "", "", "", "", "", "..#.."],
    "/": ["....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."],
    "-": ["", "", "", "#####", "", "", ""],
    "+": ["", "..#..", "..#..", "#####", "..#..", "..#..", ""],
    "#": [".#.#.", "#####", ".#.#.", ".#.#.", "#####", ".#.#.", ""],
    "%": ["##..#", "##.#.", "...#.", "..#..", ".#...", ".#.##", "#..##"],
    "=": ["", "", "#####", "", "#####", "", ""],
    "|": ["..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    " ": [""] * 7,
}


def text(d, x, y, s, col, scale=1, spacing=1):
    """Draw text with the 8x8 cell font; `spacing` extra cells of air (wide tracking)."""
    for ch in s.upper():
        g = F.get(ch, F[" "])
        for r, row in enumerate(g):
            for c, px in enumerate(row):
                if px == "#":
                    d.rectangle([x + (c + 1) * scale, y + (r + 1) * scale,
                                 x + (c + 2) * scale - 1, y + (r + 2) * scale - 1], fill=col)
        x += (6 + spacing) * scale
    return x


def text_w(s, scale=1, spacing=1):
    return len(s) * (6 + spacing) * scale


def dotted_h(d, x0, x1, y, col, step=4, size=2):
    for x in range(x0, x1, step):
        d.rectangle([x, y, x + size - 1, y + size - 1], fill=col)


def frame(d, x0, y0, x1, y1, col, double=True):
    d.rectangle([x0, y0, x1, y1], outline=col)
    if double:
        d.rectangle([x0 + 2, y0 + 2, x1 - 2, y1 - 2], outline=col)


def cross(d, x, y, col, r=9):
    d.line([x - r, y - 1, x + r, y - 1], fill=col)
    d.line([x - r, y + 1, x + r, y + 1], fill=col)
    d.line([x - 1, y - r, x - 1, y + r], fill=col)
    d.line([x + 1, y - r, x + 1, y + r], fill=col)


def boxed(d, x, y, label, value, pal, w=80):
    text(d, x, y, label, pal[2])
    d.rectangle([x, y + 10, x + w, y + 22], outline=pal[1])
    text(d, x + 3, y + 12, value, pal[2])


def readouts(d, pal, mode_no, elapsed, extra):
    x = 232
    frame(d, x - 4, 6, 315, 283, pal[1], double=False)
    y = 12
    for label, value in [("ELAPSED", elapsed), ("DENSITY", "3"), ("BRIGHT", "3"), ("SEED", "0123"), *extra,
                         ("SYSTEM", f"# {mode_no}")]:
        boxed(d, x, y, label, value, pal)
        y += 30
    text(d, x, 260, "SLEEP", pal[2])
    d.rectangle([x, 270, x + 80, 280], outline=pal[1])
    text(d, x + 3, 271, "OFF", pal[2])


def base(pal, title, mode_no, elapsed, extra, crosses=((30, 60), (200, 60), (30, 258), (200, 258))):
    im = Image.new("RGB", (W, H), pal[0])
    d = ImageDraw.Draw(im)
    frame(d, 2, 2, 317, 285, pal[1])
    frame(d, 6, 6, 224, 32, pal[1], double=False)
    text(d, 12, 11, title, pal[2], scale=2, spacing=2)
    readouts(d, pal, mode_no, elapsed, extra)
    frame(d, 6, 36, 224, 283, pal[1], double=False)
    for (cx, cy) in crosses:
        cross(d, cx, cy, pal[1])
    return im, d


# ------------------------------------------------------------------- mode screens

def drift():
    """Cold, sparse: a slowly turning wireframe globe with contour lines (deorbital descent)."""
    pal = [(2, 4, 10), (40, 110, 255), (120, 230, 200), (230, 80, 50)]
    im, d = base(pal, "DRIFT", 1, "00:42:17", [("TENSION", "0.62")],
                 crosses=((30, 60), (200, 60), (30, 244), (200, 244)))
    cx, cy, r = 115, 158, 84
    for lon in range(-60, 61, 20):                      # meridians (only the near side)
        pts = [(cx + r * math.cos(math.radians(lat)) * math.sin(math.radians(lon + 20)),
                cy - r * math.sin(math.radians(lat))) for lat in range(-80, 81, 4)]
        d.line(pts, fill=pal[2])
    for lat in range(-60, 61, 20):
        y = cy - r * math.sin(math.radians(lat))
        rr = r * math.cos(math.radians(lat))
        d.line([cx - rr * 0.95, y, cx + rr * 0.95, y], fill=pal[2])
    random.seed(4)
    for k in range(5):                                   # contour rings (red, like terrain)
        pts, a0 = [], random.uniform(0, 6)
        for t in range(0, 361, 12):
            rad = 14 + k * 7 + random.uniform(-3, 3)
            pts.append((cx - 22 + rad * 0.8 * math.cos(math.radians(t)), cy - 32 + rad * 0.55 * math.sin(math.radians(t))))
        d.line(pts + pts[:1], fill=pal[3])
    text(d, 14, 262, "STRINGS 3   HORN --   DRONE G1", pal[2])
    return im


def tides():
    """Blade Runner: target rings on a grid; rings bloom on each note; timestamp."""
    pal = [(2, 8, 14), (40, 90, 200), (240, 200, 90), (230, 240, 255)]
    im, d = base(pal, "TIDES", 2, "16:48:53", [("CHORD", "IV MAJ7")])
    for x in range(14, 222, 12):
        d.line([x, 40, x, 280], fill=pal[1])
    for y in range(40, 281, 12):
        d.line([10, y, 221, y], fill=pal[1])
    cx, cy = 110, 160
    for r in (70, 56, 30):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=pal[2])
    for a in range(0, 360, 45):                          # spokes / segments
        ax, ay = math.cos(math.radians(a)), math.sin(math.radians(a))
        d.line([cx + 30 * ax, cy + 30 * ay, cx + 56 * ax, cy + 56 * ay], fill=pal[2])
    d.rectangle([cx - 8, cy - 40, cx + 8, cy + 40], outline=pal[2])
    d.rectangle([cx - 40, cy - 8, cx + 40, cy + 8], outline=pal[2])
    d.rectangle([172, 110, 200, 210], outline=pal[2])  # side gauge
    for y in range(116, 206, 8):
        d.line([168, y, 172, y], fill=pal[2])
    text(d, 118, 44, "16:48:53:20", pal[3])
    return im


def hush_screen(alert):
    """Lo-fi, after the two ENVIRON CTR stills: one layout, two states.
    Groove = blue working screen with dotted data rows; breakdown = orange alert with one big word."""
    if alert:
        pal = [(178, 52, 24), (240, 200, 180), (250, 240, 230), (255, 170, 120)]
    else:
        pal = [(40, 72, 160), (120, 200, 210), (150, 240, 220), (200, 255, 240)]
    im = Image.new("RGB", (W, H), pal[0])
    d = ImageDraw.Draw(im)
    text(d, 16, 14, "HUSH", pal[2], scale=3, spacing=1)
    text(d, 196, 22, "LOFI CTR", pal[2], scale=1, spacing=2)
    dotted_h(d, 14, 306, 46, pal[2])
    if alert:
        text(d, 40, 92, "BREAK", pal[2], scale=5, spacing=1)
    else:
        for i, y in enumerate(range(60, 160, 10)):          # dotted data rows, like the blue still
            if y in (100, 110):
                continue                                   # gap row for the readout line
            x0 = 40 + (i % 3) * 8
            dotted_h(d, x0, 270 - (i % 2) * 16, y, pal[1], step=5, size=3)
            d.line([24, y + 1, 27, y + 1], fill=pal[1])
            d.line([292, y + 1, 295, y + 1], fill=pal[1])
        text(d, 64, 101, "DMIN7 | G7 | CMAJ7", pal[3])
    dotted_h(d, 14, 306, 172, pal[2])
    for i in range(4):                                      # beat lights, step 2 lit
        x = 40 + i * 64
        d.rectangle([x, 186, x + 40, 198], outline=pal[2], fill=pal[2] if i == 1 else None)
    text(d, 16, 222, "078", pal[2], scale=4, spacing=1)
    text(d, 120, 236, "BPM", pal[2], scale=2)
    text(d, 200, 222, "DR 5", pal[2], scale=3, spacing=1)
    text(d, 16, 268, "BAR 24556", pal[3])
    return im


def hush():
    return hush_screen(False)


def hush_break():
    return hush_screen(True)


def mother():
    """The ship computer: green phosphor teletype, framed, status column, block cursor."""
    pal = [(0, 6, 2), (30, 140, 70), (70, 255, 130), (200, 255, 210)]
    im, d = base(pal, "MOTHER", 4, "03:11:52", [("ACTIVITY", "0.48")], crosses=((30, 258), (200, 258)))
    lines = ["INTERFACE READY", "", "ENVIRON CTR", "  COOLANT FLOW   NOMINAL", "  RELAY BANK 4   CYCLING",
             "  DRIVE 2        SEEK", "", "THERMAL  -12.4", "HULL     STABLE", "", "AWAITING INQUIRY"]
    y = 46
    for s in lines:
        text(d, 14, y, s, pal[2] if s != "INTERFACE READY" else pal[3])
        y += 12
    d.rectangle([14 + text_w("AWAITING INQUIRY") + 2, y - 11, 14 + text_w("AWAITING INQUIRY") + 8, y - 3], fill=pal[3])
    # motion-tracker sweep (only during the rare alert)
    cx, cy = 116, 236
    for r in (14, 28, 40):
        d.arc([cx - r, cy - r, cx + r, cy + r], 200, 340, fill=pal[1])
    d.line([cx, cy, cx + 30, cy - 26], fill=pal[2])
    d.rectangle([cx + 18, cy - 30, cx + 21, cy - 27], fill=pal[3])
    return im


def main():
    out = os.path.join(ROOT, "docs", "ui")
    os.makedirs(out, exist_ok=True)
    for name, fn in [("drift", drift), ("tides", tides), ("hush", hush), ("hush_break", hush_break),
                     ("mother", mother)]:
        im = fn()
        im.save(os.path.join(out, f"mock_{name}.png"))
        im.resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(out, f"mock_{name}_3x.png"))
    names = ["drift", "tides", "mother", "hush", "hush_break"]
    sheet = Image.new("RGB", (W * 3 + 16, H * 2 + 8), (20, 20, 20))
    for i, name in enumerate(names):
        sheet.paste(Image.open(os.path.join(out, f"mock_{name}.png")), ((i % 3) * (W + 8), (i // 3) * (H + 8)))
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(os.path.join(out, "mock_sheet.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()
