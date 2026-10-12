#!/usr/bin/env python3
"""Build the SD-card layout and zip.

    python tools/package.py            # release: firmware baked into the bitstream
    python tools/package.py --dev      # dev: firmware loaded from the SD card (D44, D50)

Both use src/fpga/output_files/ap_core.rbf; the bitstream is the same. A dev build declares the
firmware data slot (pkg/data_dev.json) and ships Assets/ambient/common/composer.bin, made from
src/fpga/core/cpu/composer.hex. To try new music on a dev build, replace that one file.
Both declare the save slot (ambient.sav, D58); the APF creates it on Quit / power-off in
Saves/ambient/beingFar.AmbientPocket/.

dist/sdcard/
  Cores/beingFar.AmbientPocket/  bitstream.rbf_r, core.json, audio.json, data.json, input.json,
                                 interact.json, video.json, variants.json, icon.bin
  Platforms/ambient.json
  Platforms/_images/ambient.bin
  Assets/ambient/common/composer.bin       (--dev only)
dist/AmbientPocket-<version>[-dev].zip
"""
import json
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUTHOR, CORE, PLATFORM = "beingFar", "AmbientPocket", "ambient"


def write_firmware_bin(hex_path, out_path):
    """composer.hex (one little-endian word per line) -> the raw image, trailing zeros dropped."""
    data = bytearray()
    for line in open(hex_path):
        if line.strip():
            data += int(line, 16).to_bytes(4, "little")
    data = bytes(data).rstrip(b"\0")
    data += b"\0" * (-len(data) % 4)
    with open(out_path, "wb") as f:
        f.write(data)
    return len(data)


def main():
    dev = "--dev" in sys.argv[1:]
    rbf = os.path.join(ROOT, "src", "fpga", "output_files", "ap_core.rbf")
    if not os.path.exists(rbf):
        sys.exit("no bitstream: build with Quartus first (make fpga)")
    pkg = os.path.join(ROOT, "pkg")
    out = os.path.join(ROOT, "dist", "sdcard")
    shutil.rmtree(out, ignore_errors=True)
    core_dir = os.path.join(out, "Cores", f"{AUTHOR}.{CORE}")
    os.makedirs(core_dir)
    os.makedirs(os.path.join(out, "Platforms", "_images"))
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "reverse_bits.py"), rbf,
                           os.path.join(core_dir, "bitstream.rbf_r")])
    for name in ["core", "audio", "data", "input", "interact", "video", "variants"]:
        src = os.path.join(pkg, "data_dev.json" if dev and name == "data" else f"{name}.json")
        json.load(open(src, encoding="utf-8"))                     # fail early on bad JSON
        shutil.copy(src, os.path.join(core_dir, f"{name}.json"))
    shutil.copy(os.path.join(pkg, "icon.bin"), core_dir)
    shutil.copy(os.path.join(pkg, "info.txt"), core_dir)
    shutil.copy(os.path.join(pkg, "platforms", f"{PLATFORM}.json"), os.path.join(out, "Platforms"))
    shutil.copy(os.path.join(pkg, "platforms", "_images", f"{PLATFORM}.bin"),
                os.path.join(out, "Platforms", "_images"))
    if dev:
        assets = os.path.join(out, "Assets", PLATFORM, "common")
        os.makedirs(assets)
        n = write_firmware_bin(os.path.join(ROOT, "src", "fpga", "core", "cpu", "composer.hex"),
                               os.path.join(assets, "composer.bin"))
        print(f"dev build: composer.bin ({n} bytes) in {os.path.relpath(assets, out)}")
    version = json.load(open(os.path.join(pkg, "core.json")))["core"]["metadata"]["version"]
    zpath = os.path.join(ROOT, "dist", f"{CORE}-{version}{'-dev' if dev else ''}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for d, _, files in os.walk(out):
            for fn in files:
                full = os.path.join(d, fn)
                z.write(full, os.path.relpath(full, out))
    print("SD layout:", out)
    print("zip:", zpath)


if __name__ == "__main__":
    main()
