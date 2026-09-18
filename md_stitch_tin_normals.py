#!/usr/bin/env python3
"""
md_stitch_tin_normals.py -- TIN zone-boundary normal-seam fix.

Root cause (game/src/render/scene_render.h's tin_debug_zone_enabled_ doc
comment, 2026-09-17): md_bake_tin_terrain.py's fix_winding_and_normals()
averages each vertex's normal from ONLY that zone's own adjacent
triangles. Boundary vertices are geometrically shared with the
neighboring zone -- md_bake_tin_terrain.py's boundary_spacing seeding
(tin_etap1_spike.py's boundary_seed_points) forces every zone to include
the SAME world-space boundary positions its neighbors do (same source
Kenshi fullmap.tif pixel, same px_to_m constant) -- but each zone's
independent bake computes a DIFFERENT normal for that same physical
point, using only its own side's triangles. Invisible on gentle terrain,
a visible lighting seam on steep terrain (canyon walls, e.g. the
(21,34)/(22,34) boundary) since dot(N,sun) diverges more per degree of
surface tilt.

Fix: a post-bake GLOBAL pass across the whole baked set (can't be done
per-zone in a worker process -- needs both sides of every boundary
present). For every baked zone, find its boundary vertices (zone-local
x/z within BOUNDARY_EPS_M of 0 or CHUNK_SIZE_M). Group ALL zones'
boundary vertices by rounded WORLD (x,z) key -- boundary_spacing seeding
means true edge-adjacent AND corner (up to 4-zone) vertices land on
IDENTICAL world coordinates across zones by construction, so exact-match
grouping is correct, not an approximation needing interpolation/nearest-
neighbor search. For every key shared by >1 zone, sum the (already
unit-length) per-zone normals and renormalize -- the same area-weighted-
sum technique fix_winding_and_normals already uses, just extended across
the zone boundary instead of stopping at it. Rewrites normals in-place;
positions/indices/topology are untouched.

Idempotent: running twice is a no-op (shared normals already agree after
the first run, so a second pass reproduces the same average).

Usage:
    python3 tools/md_stitch_tin_normals.py [--dir game/data/terrain_tin_baked] [--dry-run]
"""
import argparse
import glob
import os
import struct
import sys

import numpy as np

MAGIC = b"MDTN"
CHUNK_SIZE_M = 460.8
BOUNDARY_EPS_M = 0.01   # zone-local distance from 0/CHUNK_SIZE_M to count as "on the edge"
KEY_DECIMALS = 3        # world-coord rounding for cross-zone vertex matching (mm precision)


def read_zone(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        assert magic == MAGIC, f"{path}: bad magic {magic!r}"
        version, zx, zz, vertex_count, index_count = struct.unpack("<IiiII", f.read(20))
        verts = np.frombuffer(f.read(vertex_count * 24), dtype=np.float32).reshape(vertex_count, 6)
        indices = np.frombuffer(f.read(index_count * 4), dtype=np.uint32)
    return {"path": path, "version": version, "zx": zx, "zz": zz,
            "positions": verts[:, 0:3].copy(), "normals": verts[:, 3:6].copy(),
            "indices": indices}


def write_zone(zone):
    verts = np.concatenate([zone["positions"], zone["normals"]], axis=1).astype(np.float32)
    with open(zone["path"], "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IiiII", zone["version"], zone["zx"], zone["zz"],
                             len(zone["positions"]), len(zone["indices"])))
        f.write(verts.tobytes())
        f.write(zone["indices"].astype(np.uint32).tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="game/data/terrain_tin_baked")
    ap.add_argument("--dry-run", action="store_true", help="report stitched-vertex count, don't write")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.dir, "zone_*_*.bin")))
    if not paths:
        print(f"[stitch] no .bin files in {args.dir}", file=sys.stderr)
        return 1
    print(f"[stitch] loading {len(paths)} zones ...")
    zones = [read_zone(p) for p in paths]

    groups = {}  # key -> list of (zone_index, vertex_index)
    for zi, zone in enumerate(zones):
        # float64 throughout -- numpy's weak-scalar promotion (NEP 50) keeps
        # a Python-float + float32-array addition IN float32 unless the
        # array is explicitly upcast first, and float32 only has ~3.5mm of
        # absolute precision at this world-coordinate magnitude (~13-30km).
        # That silently produced two DIFFERENT rounded keys for the SAME
        # physical boundary vertex depending on which zone computed it
        # (e.g. 27*460.8+460.8 -> 12902.399 vs 28*460.8+0 -> 12902.4),
        # leaving that pair unstitched. Found live via the (27,25)/(28,25)
        # validation pair: angle diff was unchanged before/after the first
        # (buggy) run of this script.
        ox = np.float64(zone["zx"]) * CHUNK_SIZE_M
        oz = np.float64(zone["zz"]) * CHUNK_SIZE_M
        pos = zone["positions"].astype(np.float64)
        on_edge = (
            (pos[:, 0] < BOUNDARY_EPS_M) | (pos[:, 0] > CHUNK_SIZE_M - BOUNDARY_EPS_M) |
            (pos[:, 2] < BOUNDARY_EPS_M) | (pos[:, 2] > CHUNK_SIZE_M - BOUNDARY_EPS_M)
        )
        for vi in np.nonzero(on_edge)[0]:
            wx = ox + pos[vi, 0]
            wz = oz + pos[vi, 2]
            key = (round(float(wx), KEY_DECIMALS), round(float(wz), KEY_DECIMALS))
            groups.setdefault(key, []).append((zi, int(vi)))

    shared = {k: v for k, v in groups.items() if len(v) > 1}
    solo = sum(1 for v in groups.values() if len(v) == 1)
    occupancy = {}
    for occ in shared.values():
        occupancy[len(occ)] = occupancy.get(len(occ), 0) + 1
    print(f"[stitch] {len(groups)} distinct boundary positions, "
          f"{len(shared)} shared across zones, {solo} unmatched (world edge / void neighbor)")
    print(f"[stitch] shared-vertex occupancy histogram (2=edge, 4=corner): {occupancy}")

    touched_zones = set()
    for occ in shared.values():
        acc = np.zeros(3, dtype=np.float64)
        for zi, vi in occ:
            acc += zones[zi]["normals"][vi].astype(np.float64)
        norm = np.linalg.norm(acc)
        if norm < 1e-9:
            continue
        acc /= norm
        for zi, vi in occ:
            zones[zi]["normals"][vi] = acc.astype(np.float32)
            touched_zones.add(zi)

    print(f"[stitch] {len(touched_zones)}/{len(zones)} zone files need rewriting")
    if args.dry_run:
        print("[stitch] --dry-run, not writing")
        return 0

    for zi in sorted(touched_zones):
        write_zone(zones[zi])
    print(f"[stitch] wrote {len(touched_zones)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
