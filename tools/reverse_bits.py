#!/usr/bin/env python3
"""Bit-reverse every byte of a Quartus .rbf into the .rbf_r format the Analogue Pocket loads.

    python tools/reverse_bits.py src/fpga/output_files/ap_core.rbf bitstream.rbf_r
"""
import sys

TABLE = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
with open(dst, "wb") as f:
    f.write(data.translate(TABLE))
print(f"{src} -> {dst} ({len(data)} bytes)")
