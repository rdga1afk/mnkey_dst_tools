#!/usr/bin/env python3
"""
md_bake_tin_terrain.py — TIN Etap 2, Stage 1: production bake tool.

Extends tin_etap1_spike.py's validated algorithm (GO verdict,
docs/TIN_ETAP1_SPIKE_RESULT.md, 6 zones swept) from a numerical-only spike
(.obj output, offline RMSE check) into an engine-loadable binary vertex/
index buffer for ONE zone -- Stage 1 of docs/TIN_ETAP2_PLAN.md ("vertical
slice: 1 zone, end-to-end"). Reuses build_tin_mesh() unchanged (import,
not a copy -- see that module's own doc comment, it stays in the repo as
the research artifact; this is the NEW production consumer of it).

Adds two things the spike never needed:
  1. Per-vertex normals (averaged adjacent-triangle face normals) -- the
     spike only checked height RMSE, never rendered anything.
  2. Binary output (TerrainTinMesh's load format, see
     engine/include/monkey_dust/render/terrain_tin_mesh.h) instead of
     .obj -- directly uploadable to a GpuStaticBuffer, matching
     PropMesh's real vertex-buffer convention (pos+normal interleaved,
     stride 24 bytes), NOT terrain_quadtree.vert's procedural VTF path.

Output convention: presence-gated, like КРОК3's game/data/
terrain_detail_baked/ (gitignored, "regenerate-don't-commit", no MD5/
hash cache -- docs/TIN_ETAP2_PLAN.md's own scope-reduction decision).
Vertex positions are ZONE-LOCAL metres (0..CHUNK_SIZE_M on X/Z, absolute
metres on Y) -- terrain_tin.vert adds the zone's world origin, same
convention TerrainQuadtreeRenderer::DrawNode's origin_x/origin_z already
uses for its own per-draw UBO.

Usage:
    python3 tools/md_bake_tin_terrain.py --zone 28 16
    python3 tools/md_bake_tin_terrain.py --zone 28 16 --max-error 0.5 \\
        --point-budget 6000 --out-dir game/data/terrain_tin_baked
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tin_etap1_spike import load_zone_heights, build_tin_mesh, CHUNK_SIZE_M  # noqa: E402

MAGIC = b"MDTN"  # monkey_dust TeraiN
VERSION = 1


def fix_winding_and_normals(pts_texel: np.ndarray, z: np.ndarray, tris: np.ndarray,
                             px_to_m: float):
    """Area-weighted average of adjacent face normals -- standard technique,
    NOT the vertex shader's VTF-gradient approach (terrain_quadtree.vert has
    no equivalent need: TIN vertices are irregular, baked once, not sampled
    at draw time).

    scipy.spatial.Delaunay.simplices does NOT guarantee consistent winding
    per triangle (only valid triangulation topology) -- roughly half the
    triangles come out clockwise-from-above, half counter-clockwise. Stage
    1's first live render (docs/TIN_ETAP2_PLAN.md) found this: an earlier
    version of this function detected face_n.y<0 and NEGATED THE WHOLE
    vector to force y>=0 -- but negating x/z along with y silently flips
    the SLOPE DIRECTION the normal represents on backwards-wound triangles,
    not just its up/down sense. Result: correct magnitude, wrong direction,
    on ~half the mesh -- dot(N,sun) go negative/near-zero in a checkerboard
    pattern, clamped to black by the shading pass -- looked exactly like
    broken/floating geometry even though the underlying mesh topology was
    fine (verified via a geometric sanity check: zero degenerate or
    near-vertical triangles in the baked output).

    Fix: when face_n.y<0, swap two INDICES (fixes winding, an integer
    reorder) instead of negating the FLOAT normal vector -- this makes the
    recomputed cross product correct on both axes, and also gives the
    output index buffer consistent winding (matters if backface culling
    is ever enabled for this pipeline, unlike today's cull_back=false)."""
    n = len(pts_texel)
    positions = np.zeros((n, 3), dtype=np.float64)
    positions[:, 0] = pts_texel[:, 0] * px_to_m
    positions[:, 2] = pts_texel[:, 1] * px_to_m
    positions[:, 1] = z

    fixed_tris = tris.copy()
    normals = np.zeros((n, 3), dtype=np.float64)
    for i, (a, b, c) in enumerate(tris):
        pa, pb, pc = positions[a], positions[b], positions[c]
        face_n = np.cross(pb - pa, pc - pa)
        if face_n[1] < 0:
            b, c = c, b  # swap winding -- fixes the normal direction too
            fixed_tris[i, 1], fixed_tris[i, 2] = c, b
            pb, pc = pc, pb
            face_n = np.cross(pb - pa, pc - pa)
        normals[a] += face_n
        normals[b] += face_n
        normals[c] += face_n
    tris = fixed_tris

    lens = np.linalg.norm(normals, axis=1, keepdims=True)
    lens[lens < 1e-9] = 1.0
    normals = normals / lens
    return positions.astype(np.float32), normals.astype(np.float32), tris


def write_binary(path: str, zx: int, zz: int, positions: np.ndarray,
                  normals: np.ndarray, tris: np.ndarray):
    vertex_count = len(positions)
    index_count = len(tris) * 3
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IiiII", VERSION, zx, zz, vertex_count, index_count))
        for i in range(vertex_count):
            f.write(struct.pack("<6f", *positions[i], *normals[i]))
        idx = tris.astype(np.uint32).ravel()
        f.write(idx.tobytes())
    print(f"[bake] wrote {path}: {vertex_count} verts, {index_count} indices "
          f"({os.path.getsize(path)} bytes)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zone", nargs=2, type=int, required=True, help="zone grid x,z")
    ap.add_argument("--max-error", type=float, default=0.5, help="metres (validated value)")
    ap.add_argument("--point-budget", type=int, default=6000)
    ap.add_argument("--cand-step", type=int, default=4)
    ap.add_argument("--batch", type=int, default=60)
    ap.add_argument("--out-dir", default="game/data/terrain_tin_baked")
    args = ap.parse_args()

    zx, zz = args.zone
    height = load_zone_heights(zx, zz)
    px_to_m = CHUNK_SIZE_M / (height.shape[0] - 1)

    pts, z, tris = build_tin_mesh(height, max_error_m=args.max_error,
                                   point_budget=args.point_budget,
                                   cand_step=args.cand_step, batch=args.batch)
    positions, normals, tris = fix_winding_and_normals(pts, z, tris, px_to_m)

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, f"zone_{zx}_{zz}.bin")
    write_binary(out_path, zx, zz, positions, normals, tris)


if __name__ == "__main__":
    main()
