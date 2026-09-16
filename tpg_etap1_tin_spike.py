#!/usr/bin/env python3
"""
tpg_etap1_tin_spike.py — TPG Етап 1 Stage-0 spike: adaptive Delaunay/TIN
terrain mesh vs current regular-grid baseline, on real Kenshi height data.

Context: docs/DAGOR_ANALYSIS_2026-09.md found Dagor Engine bakes terrain
geometry offline via adaptive (error-threshold + point-budget driven)
Delaunay triangulation instead of our runtime regular-grid VTF sampling
(terrain_quadtree.vert). This script is a small, isolated numerical spike
to check whether the same idea gives a real triangle-count win on OUR
real data before any engine integration (docs/DAGOR_IMPLEMENTATION_PROMPT.md,
Частина D, КРОК 9). Does NOT touch game/engine code.

Algorithm: adapted from the SAME class of technique Recast (already a
project dependency) uses for navmesh detail meshes
(engine/third_party/recastnavigation/Recast/Source/RecastMeshDetail.cpp:
672-892, buildPolyDetail) -- greedy point insertion: start from the 4
corners, repeatedly insert the raw-grid sample point with the largest
height error against the CURRENT Delaunay triangulation's interpolated
surface, until max error < threshold or point budget is hit. Not a
literal port -- reimplemented here in Python using scipy for the
triangulation itself (Recast's own triangulator is written for navmesh
polygons, not reusable standalone).

Data source: real Kenshi fullmap.tif (same file, same read convention as
tools/md_heightmap_import.py's extract_zone -- 16385x16385 uint16,
64 zones x 256px + 1 shared-border pixel, height_m = pixel / 128.0).

Baseline comparison: current TerrainQuadtree renders each zone as
kFlatLodDepth=3 -> 64 nodes x 17x17 grid (16x16 quads = 512 tris/node)
= 32,768 triangles/zone (engine/src/world/terrain_quadtree.cpp:26,
shaders/terrain_quadtree.vert:60). A regular 129x129 grid over the same
zone (its native heightmap sample resolution) gives 128*128*2 = 32,768
triangles too -- the same order of magnitude, used here as the
"baseline" mesh for a like-for-like RMSE comparison.

Usage:
    python3 tools/tpg_etap1_tin_spike.py [--zone ZX ZZ] [--max-error M]
                                          [--point-budget N]

Output (scratchpad, not committed): .obj files for both meshes + a
summary table (triangle counts, height-field RMSE against ground truth).
This is a NUMERICAL fidelity check (reconstruct height from each mesh at
every raw texel, compare to the real heightmap) -- NOT a screenshot/
live-render comparison, since this spike does not touch the renderer at
all (that is Етап 2, gated on this spike's GO verdict).
"""
import argparse
import os
import sys
import time

import numpy as np

FULLMAP = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data/newland/land/fullmap.tif"
ZONE_PX = 256        # pixels per zone in fullmap (+ 1 shared border)
SCALE = 128.0         # uint16 / SCALE = metres
CHUNK_SIZE_M = 460.8  # TS_CHUNK_SIZE_M -- world metres per zone side

OUT_DIR = "/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/021d5116-7c77-464a-bd69-2b8160c80074/scratchpad"


def load_zone_heights(zx: int, zz: int) -> np.ndarray:
    """Return (257,257) float32 heightmap in metres for zone (zx,zz), real Kenshi data."""
    import tifffile
    print(f"[spike] reading {FULLMAP} (zone {zx},{zz}) ...", flush=True)
    arr = tifffile.imread(FULLMAP)
    assert arr.shape == (16385, 16385), f"unexpected fullmap shape {arr.shape}"
    py0, px0 = zz * ZONE_PX, zx * ZONE_PX
    patch = arr[py0: py0 + ZONE_PX + 1, px0: px0 + ZONE_PX + 1].astype(np.float32) / SCALE
    print(f"[spike] zone patch {patch.shape}, height range "
          f"{patch.min():.1f}-{patch.max():.1f}m")
    return patch


def bilinear_sample(height: np.ndarray, x: float, y: float) -> float:
    """Sample height at fractional (col,row)=(x,y) in the source grid, bilinear."""
    h, w = height.shape
    x = min(max(x, 0.0), w - 1.0001)
    y = min(max(y, 0.0), h - 1.0001)
    x0, y0 = int(x), int(y)
    x1, y1 = x0 + 1, y0 + 1
    fx, fy = x - x0, y - y0
    h00, h10 = height[y0, x0], height[y0, x1]
    h01, h11 = height[y1, x0], height[y1, x1]
    return (h00 * (1 - fx) * (1 - fy) + h10 * fx * (1 - fy)
            + h01 * (1 - fx) * fy + h11 * fx * fy)


def build_baseline_mesh(height: np.ndarray, stride: int):
    """Regular grid mesh at native resolution downsampled by `stride`. Returns (pts_xy, z, tris)."""
    from scipy.spatial import Delaunay
    h, w = height.shape
    xs = np.arange(0, w, stride)
    ys = np.arange(0, h, stride)
    if xs[-1] != w - 1:
        xs = np.append(xs, w - 1)
    if ys[-1] != h - 1:
        ys = np.append(ys, h - 1)
    gx, gy = np.meshgrid(xs, ys)
    pts = np.column_stack([gx.ravel(), gy.ravel()]).astype(np.float64)
    z = height[gy.ravel(), gx.ravel()]
    tri = Delaunay(pts)
    return pts, z, tri.simplices


def build_tin_mesh(height: np.ndarray, max_error_m: float, point_budget: int, cand_step: int = 4):
    """Greedy error-driven adaptive triangulation -- Recast buildPolyDetail-style.

    Start from the 4 corners + midpoints of edges (boundary must be present
    so the whole zone is covered), then repeatedly: interpolate the current
    triangulation's height at every raw grid sample, find the sample with
    max |interpolated - real| error, insert it as a new point, re-triangulate.
    Stop when max error < max_error_m or point_budget is reached.
    """
    from scipy.spatial import Delaunay
    from scipy.interpolate import LinearNDInterpolator

    h, w = height.shape
    corners = np.array([[0, 0], [w - 1, 0], [0, h - 1], [w - 1, h - 1]], dtype=np.float64)
    pts = list(corners)

    # Coarse candidate grid to search for max-error insertion points (every
    # raw texel would be correct but slow in pure Python for a 257x257
    # patch across many iterations -- sample every 4th texel as candidates,
    # which is still a real error signal, not synthetic).
    cx, cy = np.meshgrid(np.arange(0, w, cand_step), np.arange(0, h, cand_step))
    cand = np.column_stack([cx.ravel(), cy.ravel()]).astype(np.float64)
    cand_z = height[cy.ravel(), cx.ravel()]

    t0 = time.time()
    while len(pts) < point_budget:
        P = np.array(pts)
        Z = np.array([height[int(round(p[1])), int(round(p[0]))] for p in pts])
        tri = Delaunay(P)
        interp = LinearNDInterpolator(tri, Z)
        est = interp(cand)
        est = np.nan_to_num(est, nan=0.0)
        err = np.abs(est - cand_z)
        worst = int(np.argmax(err))
        worst_err = err[worst]
        if worst_err <= max_error_m:
            print(f"[spike] TIN converged: max_error={worst_err:.3f}m <= {max_error_m}m, "
                  f"{len(pts)} points")
            break
        pts.append(cand[worst])
    else:
        print(f"[spike] TIN stopped at point_budget={point_budget}, "
              f"max_error still {worst_err:.3f}m")
    P = np.array(pts)
    Z = np.array([height[int(round(p[1])), int(round(p[0]))] for p in pts])
    tri = Delaunay(P)
    print(f"[spike] TIN build took {time.time()-t0:.1f}s, {len(pts)} points, "
          f"{len(tri.simplices)} triangles")
    return P, Z, tri.simplices


def compute_rmse(height: np.ndarray, pts: np.ndarray, z: np.ndarray, tris: np.ndarray) -> float:
    """RMSE of the mesh's interpolated height vs the real heightmap, at every raw texel."""
    from scipy.spatial import Delaunay
    from scipy.interpolate import LinearNDInterpolator
    tri = Delaunay(pts)
    interp = LinearNDInterpolator(tri, z)
    h, w = height.shape
    gx, gy = np.meshgrid(np.arange(w), np.arange(h))
    query = np.column_stack([gx.ravel(), gy.ravel()]).astype(np.float64)
    est = interp(query)
    valid = ~np.isnan(est)
    diff = est[valid] - height.ravel()[valid]
    return float(np.sqrt(np.mean(diff * diff))), float(valid.sum()) / valid.size


def write_obj(path: str, pts: np.ndarray, z: np.ndarray, tris: np.ndarray, px_to_m: float):
    """pts are (col,row) in texel space; convert to world-relative metres for inspection."""
    with open(path, "w") as f:
        for (x, y), zz in zip(pts, z):
            f.write(f"v {x*px_to_m:.3f} {zz:.3f} {y*px_to_m:.3f}\n")
        for a, b, c in tris:
            f.write(f"f {a+1} {b+1} {c+1}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zone", nargs=2, type=int, default=[28, 16],
                     help="zone grid x,z (default 28 16 -- mountain cluster used for TPG GATE1)")
    ap.add_argument("--max-error", type=float, default=1.0, help="metres")
    ap.add_argument("--point-budget", type=int, default=8000)
    ap.add_argument("--baseline-stride", type=int, default=2,
                     help="257px / stride ~= grid resolution; stride=2 -> 129x129, matches "
                          "zone's native heightmap sample resolution")
    ap.add_argument("--cand-step", type=int, default=4,
                     help="candidate-point sampling stride for TIN insertion search; "
                          "1 = every raw texel (slower, accuracy ceiling removed)")
    args = ap.parse_args()

    zx, zz = args.zone
    height = load_zone_heights(zx, zz)
    px_to_m = CHUNK_SIZE_M / (height.shape[0] - 1)

    print("\n=== baseline (regular grid, native-res-equivalent) ===")
    bp, bz, btri = build_baseline_mesh(height, stride=args.baseline_stride)
    b_rmse, b_cov = compute_rmse(height, bp, bz, btri)
    print(f"[spike] baseline: {len(btri)} triangles, RMSE={b_rmse:.4f}m, coverage={b_cov*100:.1f}%")

    print("\n=== TIN (adaptive Delaunay, error-driven) ===")
    tp, tz, ttri = build_tin_mesh(height, max_error_m=args.max_error, point_budget=args.point_budget,
                                   cand_step=args.cand_step)
    t_rmse, t_cov = compute_rmse(height, tp, tz, ttri)
    print(f"[spike] TIN:      {len(ttri)} triangles, RMSE={t_rmse:.4f}m, coverage={t_cov*100:.1f}%")

    os.makedirs(OUT_DIR, exist_ok=True)
    write_obj(os.path.join(OUT_DIR, f"tpg_etap1_baseline_{zx}_{zz}.obj"), bp, bz, btri, px_to_m)
    write_obj(os.path.join(OUT_DIR, f"tpg_etap1_tin_{zx}_{zz}.obj"), tp, tz, ttri, px_to_m)

    print("\n=== SUMMARY (zone %d,%d) ===" % (zx, zz))
    print(f"{'':12s} {'triangles':>10s} {'RMSE (m)':>10s}")
    print(f"{'baseline':12s} {len(btri):10d} {b_rmse:10.4f}")
    print(f"{'TIN':12s} {len(ttri):10d} {t_rmse:10.4f}")
    reduction = 100.0 * (1.0 - len(ttri) / len(btri))
    print(f"\ntriangle reduction: {reduction:.1f}%  "
          f"(RMSE {'better' if t_rmse < b_rmse else 'worse'} by {abs(t_rmse-b_rmse):.4f}m)")
    print(f".obj files written to {OUT_DIR}")


if __name__ == "__main__":
    main()
