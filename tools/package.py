#!/usr/bin/env python3
"""Build the SD-card release layout and zip.

    python tools/package.py            # uses src/fpga/output_files/ap_core.rbf

dist/sdcard/
  Cores/beingFar.AmbientPocket/  bitstream.rbf_r, core.json, audio.json, data.json, input.json,
                                 interact.json, video.json, variants.json, icon.bin
  Platforms/ambient.json
  Platforms/_images/ambient.bin
dist/AmbientPocket-<version>.zip
"""
import json
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUTHOR, CORE, PLATFORM = "beingFar", "AmbientPocket", "ambient"


def main():
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
        src = os.path.join(pkg, f"{name}.json")
        json.load(open(src, encoding="utf-8"))                     # fail early on bad JSON
        shutil.copy(src, core_dir)
    shutil.copy(os.path.join(pkg, "icon.bin"), core_dir)
    shutil.copy(os.path.join(pkg, "info.txt"), core_dir)
    shutil.copy(os.path.join(pkg, "platforms", f"{PLATFORM}.json"), os.path.join(out, "Platforms"))
    shutil.copy(os.path.join(pkg, "platforms", "_images", f"{PLATFORM}.bin"),
                os.path.join(out, "Platforms", "_images"))
    version = json.load(open(os.path.join(pkg, "core.json")))["core"]["metadata"]["version"]
    zpath = os.path.join(ROOT, "dist", f"{CORE}-{version}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for d, _, files in os.walk(out):
            for fn in files:
                full = os.path.join(d, fn)
                z.write(full, os.path.relpath(full, out))
    print("SD layout:", out)
    print("zip:", zpath)


if __name__ == "__main__":
    main()
