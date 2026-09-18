#!/usr/bin/env python3
"""
md_bake_tin_terrain_batch.py — TIN Etap 2 Stage 3 infrastructure: bake
many zones in one run, parallelized across worker processes.

Each zone bake (tin_etap1_spike.py's build_tin_mesh + md_bake_tin_terrain
.py's fix_winding_and_normals) is CPU-bound, independent of every other
zone (own fullmap.tif read, own scipy.spatial.Delaunay) -- embarrassingly
parallel, no shared state, no reason to run serially. Measured single-zone
cost: ~1.3-2.5s depending on terrain complexity (docs/TIN_ETAP2_PLAN.md) --
serial bake of the full 4096-zone (64x64) world would be ~1.5-2h; this
tool exists specifically to cut that down via multiprocessing.

--workers defaults to 2, matching this project's own standing hardware
constraint (i5-6200U, 2 physical cores -- see feedback_minimize_rebuild_
wait_time memory: "cap -j to 2 physical cores"). Raise it explicitly on
better hardware; this is a one-time asset-bake, not a build, so heavier
parallelism here is more acceptable than during ninja builds, but the
default stays conservative for this machine.

Usage:
    # Rectangular range (inclusive), matches the coordinate convention
    # md_bake_tin_terrain.py's own --zone uses:
    python3 tools/md_bake_tin_terrain_batch.py --range 27 25 30 27

    # Radius around a center zone -- previews the eventual streaming-
    # window shape (SceneRender::TNKN=9 is the game's own 9x9 analogue):
    python3 tools/md_bake_tin_terrain_batch.py --center 27 25 --radius 2

    python3 tools/md_bake_tin_terrain_batch.py --center 27 25 --radius 2 \\
        --workers 2 --max-error 0.5 --point-budget 6000

    # Full-world bake (all 64x64 zones, skipping ~900 flat ocean/void ones
    # and anything already baked) -- real scale, ~1.5-2h at 2 workers:
    python3 tools/md_bake_tin_terrain_batch.py --range 0 0 63 63 \\
        --skip-void --skip-existing --workers 2
"""
import argparse
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from md_bake_tin_terrain import bake_zone  # noqa: E402
from tin_etap1_spike import FULLMAP, ZONE_PX  # noqa: E402
from md_stitch_tin_normals import main as stitch_main  # noqa: E402


def _find_void_zones(zones, threshold):
    """Full-world bake finding (2026-09-17): the real Kenshi fullmap.tif
    covers all 64x64 zones at full resolution (confirmed -- 16385x16385 =
    64*256+1, no smaller embedded sub-region), but ~900/4096 of those
    zones are flat raw-uint16 0 (ocean/void outside the actual landmass,
    not a data-source artifact). Baking them wastes real CPU time (each
    still costs ~1.5-2.5s of triangulation/IO for a mesh that ends up
    trivially flat) for zero visual gain over the quadtree fallback, which
    renders the same flat height. threshold is in RAW uint16 units (same
    units as the source pixels, before the HEIGHT_MAX_M scale) -- default
    50 was picked by inspecting the real data (every genuine zone found so
    far has a range in the hundreds-to-thousands; every void zone found so
    far is exactly 0)."""
    import numpy as np
    import tifffile
    print(f"[batch] scanning {FULLMAP} for void zones (threshold={threshold} raw units)...")
    arr = tifffile.imread(FULLMAP)
    void = set()
    for zx, zz in zones:
        y0, x0 = zz * ZONE_PX, zx * ZONE_PX
        patch = arr[y0:y0 + ZONE_PX + 1, x0:x0 + ZONE_PX + 1]
        if int(patch.max()) - int(patch.min()) <= threshold:
            void.add((zx, zz))
    return void


def _bake_one(args):
    """Top-level (picklable) worker entry point -- ProcessPoolExecutor needs
    a plain function, not a closure, to pickle the call across processes."""
    zx, zz, kwargs = args
    t0 = time.time()
    try:
        path = bake_zone(zx, zz, **kwargs)
        return (zx, zz, True, path, time.time() - t0, None)
    except Exception as e:  # noqa: BLE001 -- report per-zone, don't crash the pool
        return (zx, zz, False, None, time.time() - t0, str(e))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--range", nargs=4, type=int, metavar=("ZX0", "ZZ0", "ZX1", "ZZ1"),
                     help="inclusive zone rectangle")
    ap.add_argument("--center", nargs=2, type=int, metavar=("ZX", "ZZ"),
                     help="bake a (2*radius+1)^2 window around this zone")
    ap.add_argument("--radius", type=int, default=1,
                     help="used with --center (default 1 -> 3x3 window)")
    ap.add_argument("--workers", type=int, default=2,
                     help="parallel worker processes (default 2, see module doc)")
    ap.add_argument("--max-error", type=float, default=0.5)
    ap.add_argument("--point-budget", type=int, default=6000)
    ap.add_argument("--cand-step", type=int, default=4)
    ap.add_argument("--batch", type=int, default=60)
    ap.add_argument("--out-dir", default="game/data/terrain_tin_baked")
    ap.add_argument("--no-boundary-stitch", action="store_true")
    ap.add_argument("--skip-existing", action="store_true",
                     help="skip zones whose .bin already exists in --out-dir")
    ap.add_argument("--skip-void", action="store_true",
                     help="skip zones that are flat (ocean/void outside the real landmass) "
                          "in the source fullmap.tif -- see _find_void_zones' doc comment")
    ap.add_argument("--void-threshold", type=int, default=50,
                     help="raw uint16 max-min range at/below which a zone counts as void "
                          "(default 50, see _find_void_zones)")
    ap.add_argument("--no-stitch-normals", action="store_true",
                     help="skip the post-bake cross-zone normal-seam fix (md_stitch_tin_normals.py) "
                          "-- default is to always run it after a successful bake, since a freshly "
                          "baked zone's boundary normals disagree with its already-baked neighbors "
                          "until stitched (game/src/render/scene_render.h's tin_debug_zone_enabled_ "
                          "doc comment)")
    args = ap.parse_args()

    if bool(args.range) == bool(args.center):
        print("ERROR: pass exactly one of --range or --center", file=sys.stderr)
        return 1

    if args.range:
        zx0, zz0, zx1, zz1 = args.range
        zones = [(zx, zz) for zx in range(min(zx0, zx1), max(zx0, zx1) + 1)
                           for zz in range(min(zz0, zz1), max(zz0, zz1) + 1)]
    else:
        cx, cz = args.center
        r = args.radius
        zones = [(zx, zz) for zx in range(cx - r, cx + r + 1)
                           for zz in range(cz - r, cz + r + 1)]

    if args.skip_existing:
        before = len(zones)
        zones = [(zx, zz) for zx, zz in zones
                 if not os.path.exists(os.path.join(args.out_dir, f"zone_{zx}_{zz}.bin"))]
        skipped = before - len(zones)
        if skipped:
            print(f"[batch] skipping {skipped} already-baked zone(s)")

    if args.skip_void:
        before = len(zones)
        void = _find_void_zones(zones, args.void_threshold)
        zones = [z for z in zones if z not in void]
        print(f"[batch] skipping {before - len(zones)} void zone(s) "
              f"(flat in source data, threshold={args.void_threshold})")

    if not zones:
        print("[batch] nothing to bake")
        return 0

    kwargs = dict(max_error=args.max_error, point_budget=args.point_budget,
                  cand_step=args.cand_step, batch=args.batch, out_dir=args.out_dir,
                  no_boundary_stitch=args.no_boundary_stitch)

    print(f"[batch] baking {len(zones)} zone(s) with {args.workers} worker(s): "
          f"{zones if len(zones) <= 12 else str(zones[:12])[:-1] + ', ...]'}")
    t0 = time.time()
    ok_count = 0
    fail_count = 0
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(_bake_one, (zx, zz, kwargs)) for zx, zz in zones]
        for i, fut in enumerate(as_completed(futures), 1):
            zx, zz, ok, path, dt, err = fut.result()
            if ok:
                ok_count += 1
                print(f"[batch] ({i}/{len(zones)}) zone({zx},{zz}) OK in {dt:.1f}s -> {path}")
            else:
                fail_count += 1
                print(f"[batch] ({i}/{len(zones)}) zone({zx},{zz}) FAILED in {dt:.1f}s: {err}",
                      file=sys.stderr)

    elapsed = time.time() - t0
    print(f"\n[batch] done: {ok_count} OK, {fail_count} failed, {elapsed:.1f}s total "
          f"({elapsed/len(zones):.1f}s/zone average, {args.workers} workers)")

    if ok_count and not args.no_stitch_normals:
        print("\n[batch] stitching cross-zone boundary normals "
              "(tools/md_stitch_tin_normals.py) ...")
        stitch_argv, sys.argv = sys.argv, ["md_stitch_tin_normals.py", "--dir", args.out_dir]
        try:
            stitch_main()
        finally:
            sys.argv = stitch_argv

    return 1 if fail_count else 0


if __name__ == "__main__":
    sys.exit(main())
