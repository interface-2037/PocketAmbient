#!/usr/bin/env python3
"""Composer firmware image -> BRAM init file for the 64 KB CPU RAM.

    python tools/fw_hex.py build/fw/composer.bin src/fpga/core/cpu/composer.hex

One 32-bit little-endian word per line ($readmemh), 16,384 lines, zero-padded. Prints the
image size and the RAM left for .bss and the stack (see build/fw/composer.map).
"""
import sys

WORDS = 16384


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    if len(data) > WORDS * 4:
        sys.exit(f"firmware image is {len(data)} bytes, larger than the {WORDS * 4}-byte CPU RAM")
    data += b"\0" * (WORDS * 4 - len(data))
    with open(dst, "w", newline="\n") as f:
        for i in range(WORDS):
            f.write("%08x\n" % int.from_bytes(data[4 * i:4 * i + 4], "little"))
    print(f"{dst}: image {len(data.rstrip(bytes(1)))} bytes of {WORDS * 4}")


if __name__ == "__main__":
    main()
