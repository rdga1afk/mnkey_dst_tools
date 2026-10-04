#!/usr/bin/env python3
"""
md_downscale_terrain_tex.py -- 2048^2 terrain ground-layer DDS -> 1024^2 by DROPPING mip 0.

The DDS files already carry a pre-filtered mip chain, so mip 1 is the 1024^2 image: no recompression,
no quality loss beyond the resolution itself (header width/height/linear size/mip count are rewritten,
block data is copied verbatim). Source: tmp_/kenshi_re/terrain_textures (permanent RE data, untouched).
Output: game/data/textures/terrain_1024/ (gitignored, regenerate-don't-commit like md_ground_baked.dds).

Why: 227 layer files = 981 MB at 2048^2, 245 MB at 1024^2 (4x less VRAM/disk/load time on the iGPU, which
shares system RAM). With the x10 ground UV scale (terrain_units10) 1024 still gives ~0.7 cm/texel at a 7 m tile.
No measurable GPU-ms change (cross-process ABBA noise > effect, see docs/TERRAIN_SURFACE_PLAN_2026-10-02.md).

Usage: python3 tools/md_downscale_terrain_tex.py      # reads the layer list from game/data/biome_table.txt
"""
import os
import struct
import sys

SRC_DIR = "tmp_/kenshi_re/terrain_textures"
DST_DIR = "game/data/textures/terrain_1024"
TABLE = "game/data/biome_table.txt"


def main() -> int:
    names = set()
    for line in open(TABLE, encoding="utf-8"):
        if line.startswith("tex "):
            for p in line.rstrip("\n").split("|")[1:3]:
                names.add(os.path.basename(p.strip()))
    os.makedirs(DST_DIR, exist_ok=True)
    tin = tout = 0
    for n in sorted(names):
        b = open(os.path.join(SRC_DIR, n), "rb").read()
        h, w, lin, _, m = struct.unpack("<5I", b[12:32])
        cc = b[84:88]
        if w != 2048 or h != 2048 or cc not in (b"DXT1", b"DXT5"):
            print(f"SKIP {n}: {w}x{h} {cc!r} (expected 2048x2048 DXT1/DXT5)", file=sys.stderr)
            return 1
        mip0 = (w // 4) * (h // 4) * (8 if cc == b"DXT1" else 16)
        if lin != mip0:
            print(f"SKIP {n}: unexpected linear size {lin} != {mip0}", file=sys.stderr)
            return 1
        hdr = bytearray(b[:128])
        struct.pack_into("<I", hdr, 12, 1024)
        struct.pack_into("<I", hdr, 16, 1024)
        struct.pack_into("<I", hdr, 20, mip0 // 4)
        struct.pack_into("<I", hdr, 28, m - 1)
        out = bytes(hdr) + b[128 + mip0:]
        open(os.path.join(DST_DIR, n), "wb").write(out)
        tin += len(b)
        tout += len(out)
    print(f"{len(names)} files: {tin / 1e6:.1f} MB -> {tout / 1e6:.1f} MB in {DST_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
