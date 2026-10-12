#!/usr/bin/env python3
"""Generate the text-layer data for the FPGA:

    python tools/gen_font.py

  src/fpga/core/ui/font8.hex        64 glyphs (ASCII 32..95) x 8 rows, bit 7 = leftmost.
                                    The square 5x7 font of the UI mockups (tools/ui/mockup.py).
  src/fpga/core/ui/screen_test.hex  initial character RAM of the Phase 3 test screen:
                                    1024 entries {colour[1:0], code[5:0]} at {row[4:0], col[4:0]},
                                    20 x 18 cells of 16 x 16 pixels.
"""
import importlib.util
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("mockup", os.path.join(ROOT, "tools", "ui", "mockup.py"))
mockup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mockup)
F = mockup.F

VERSION = json.load(open(os.path.join(ROOT, "pkg", "core.json")))["core"]["metadata"]["version"]
DIM, AMBER, GREEN, RED = 0, 1, 2, 3

# (row, col, colour, text); dynamic fields are drawn over by the test top level
SCREEN = [
    (1, 3, AMBER, "AMBIENT POCKET"),
    (2, 1, DIM, "ENGINE TEST V" + VERSION),
    (4, 1, AMBER, "SRAM WAIT TEST"),
    (5, 0, DIM, "6:-- 5:-- 4:-- 3:--"),
    (6, 0, DIM, "---- ---- ---- ----"),
    (8, 1, AMBER, "PLAYING"),
    (9, 1, GREEN, "------"),
    (9, 13, DIM, "00:00"),
    (11, 1, DIM, "MAX CYCLES"),
    (11, 13, AMBER, "---"),
    (12, 1, DIM, "OVERRUNS"),
    (12, 13, AMBER, "--"),
    (14, 1, DIM, "A  NEXT EXCERPT"),
    (15, 1, DIM, "B  RESTART"),
]


def code(ch):
    c = ord(ch.upper())
    return c - 32 if 32 <= c < 96 else 0


def glyph_rows(ch):
    g = F.get(ch)
    rows = [0] * 8
    if g:
        for r in range(7):
            for i, px in enumerate(g[r]):
                if px == "#":
                    rows[r + 1] |= 1 << (6 - i)          # 1 px left margin, 1 row top margin
    return rows


def write(path, lines):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", path)


def main():
    font = []
    for c in range(64):
        font += [f"{b:02x}" for b in glyph_rows(chr(c + 32))]
    write("src/fpga/core/ui/font8.hex", font)
    cram = [0] * 1024
    for row, col, colour, text in SCREEN:
        for i, ch in enumerate(text):
            cram[row * 32 + col + i] = (colour << 6) | code(ch)
    write("src/fpga/core/ui/screen_test.hex", [f"{v:02x}" for v in cram])


if __name__ == "__main__":
    main()
