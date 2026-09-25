#!/usr/bin/env python3
"""
perf_phase4_world_scan.py -- PERF_MASTER_PROMPT.md FAZA 4 (consolidation):
re-runs Faza 1's world map scan against the CURRENT release build (with
the ground-shading Granite-branchless SIMD32 fix, task #570, already
landed) to confirm the win is global, not local to the 5 worst points
found in the original 2026-08-1x cycle.

Drives an ALREADY-RUNNING build_release/game/monkey_dust process live via
tools/qa/game_cmd_driver.Driver (tmp_/game_cmd/ file-based Lua command
channel, game/src/ui_driver/game_cmd_file.cpp), using ONLY the subset of
md.* functions that are registered UNCONDITIONALLY (not #ifdef
MONKEY_DUST_EDITOR-gated): teleport_camera, teleport_player,
set_camera_orbit, set_gpu_sync_timing, get_gpu_ms, log. This requires the
game binary built with -DMD_PERF_TEST_HOOKS=ON (enables GameCmdFile_Poll()
in a no-ImGui release build, game/src/main.cpp:122) IN ADDITION TO
-DMONKEY_DUST_EDITOR=OFF -- MD_PERF_TEST_HOOKS was OFF by default and had
to be explicitly enabled for this run (2026-08-29 discovery).

Documented deviations from PERF_MASTER_PROMPT.md's exact letter (R1-R10),
found necessary because the release build lacks editor-only APIs:
  R3 (settle protocol): release has NO md.get_camera_pose() (EditorCore-
      only, returns nil outside MONKEY_DUST_EDITOR) and md.get_player_pos()
      returns only {x,z,ok}, no y -- so the literal "|camera.y delta|
      <0.05m x6 polls" check cannot be implemented in release. Substituted
      a fixed SETTLE_S wait per point. Reduced from the master prompt's
      own ~10-15s empirical convergence window to 6s to keep total runtime
      for a 256-point x 2-pose confirmation run bounded to ~1h -- most
      points converge faster than the documented worst case; this is a
      real, disclosed risk (some slow-converging points may show inflated
      numbers), not a silent shortcut.
  R9 (sample hygiene): SAMPLES_PER_POINT=20, reduced from the original
      Phase 1's 60 (and below R9's own stated ">=40 for fast A/B/A blocks"
      floor) for the same total-runtime reason. This is a real, disclosed
      reduction in statistical confidence per point, acceptable for a
      "confirm the effect is still large and global, not gone/reversed"
      re-check, NOT sufficient rigor for hunting a NEW subtle regression.
  R7 (NPC in scene): NOT satisfied -- md.spawn() exists in the release API
      but this driver does not yet call it. The original Phase 1/South
      Hive investigation used an NPC-populated scene. This re-run is
      NPC-less. If Gate 4 numbers look better than the original NPC-
      inclusive baseline, THIS is a likely reason -- flagged honestly,
      not hidden. An NPC-inclusive re-run should follow before fully
      trusting a direct before/after comparison against the original
      Phase 1 numbers.

R1 (release-only): verified separately, 2026-08-29 -- 0 real `ImGui::` /
`ImGui_Impl*` symbols in build_release/game/monkey_dust (the raw
`nm | grep -ci imgui` count of 1 is a documented harmless false positive:
UiDriver_PreImGuiNewFrame's #else stub is an empty {} body whose NAME
merely contains the substring "ImGui").

USAGE:
  # 1. build once: cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release \
  #      -DUSE_SDL3=ON -DMONKEY_DUST_EDITOR=OFF -DMD_PERF_TEST_HOOKS=ON
  #      ninja -C build_release monkey_dust
  # 2. run this script (it launches+drives+shuts down the process itself):
  python3 tools/qa/perf_phase4_world_scan.py --out tools/qa/reports/phase4_world_scan.csv
  # 3. --tpg on|off (2026-09-13, TerrainProjectedGrid A/B): same 18-zone x
  #    2-pose sweep with md.set_terrain_projected_grid() forced to a fixed
  #    state for the whole run -- run once per state, diff the two CSVs.
  python3 tools/qa/perf_phase4_world_scan.py --tpg off --out tools/qa/reports/phase4_world_scan_tpg_off.csv
  python3 tools/qa/perf_phase4_world_scan.py --tpg on  --out tools/qa/reports/phase4_world_scan_tpg_on.csv

DRIFT (2026-09-13, terrain-next Stage 0): a same-config recheck sweep
run ~1.5h after this script's own earlier run (many rebuild+sweep
cycles in between, no idle gap) came back +28.9% slower on average --
real CPU RAPL PL1 thermal drift accumulated over the SESSION, not a
per-benchmark artifact (see feedback_pl1_throttle_28s_window memory).
A baseline point is now re-measured every --recheck-every points
(default 12, ~0 to disable) and logged with is_recheck=1 -- if that
recheck drifts >10% from its own first measurement, DO NOT trust
deltas against an out-of-window baseline; rerun the comparison config
in the same window instead.
"""

import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver, median  # noqa: E402
from qa_stats import percentile as _percentile  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = REPO_ROOT / "game" / "data" / "terrain_config.txt"
RELEASE_EXE = REPO_ROOT / "build_release" / "game" / "monkey_dust"

CHUNK_SIZE = 460.8
GRID_STEP = 4            # 64/4 = 16 -> 16x16 = 256 candidate points
SETTLE_S = 6.0             # R3 deviation -- see module doc comment
SAMPLES_PER_POINT = 20     # R9 deviation -- see module doc comment
SAMPLE_INTERVAL_S = 0.03   # small gap between get_gpu_ms() polls
POSES = [
    ("gameplay", 40.0, 22.0),   # (label, pitch_deg, orbit_dist_m)
    ("horizon",  10.0, 22.0),
]


def parse_valid_zones(path: Path):
    valid = set()
    if not path.exists():
        return valid
    gx = gz = None
    for line in path.read_text().splitlines():
        s = line.strip()
        m = re.match(r"^grid_x=(-?\d+)$", s)
        if m:
            gx = int(m.group(1))
            continue
        m = re.match(r"^grid_z=(-?\d+)$", s)
        if m:
            gz = int(m.group(1))
            if gx is not None:
                valid.add((gx, gz))
            gx = gz = None
    return valid


def percentile(xs, p):
    return _percentile(xs, p, method="nearest_round")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="tools/qa/reports/phase4_world_scan.csv")
    ap.add_argument("--exe", default=str(RELEASE_EXE))
    ap.add_argument("--limit", type=int, default=0, help="stop after N points (0=all), for a quick smoke test")
    ap.add_argument("--tpg", choices=["off", "on"], default="off",
                     help="md.set_terrain_projected_grid() state for the whole sweep "
                          "(TerrainProjectedGrid A/B, docs/TERRAIN_PROJECTED_GRID.md); "
                          "unconditionally registered, works in MONKEY_DUST_EDITOR=OFF builds")
    ap.add_argument("--recheck-every", type=int, default=12,
                     help="re-measure the FIRST point every N measurements as an "
                          "in-window drift sanity check (0=disable). See module "
                          "doc comment, DRIFT section -- a same-session +28.9% "
                          "thermal drift was caught this way on 2026-09-13.")
    args = ap.parse_args()

    valid_zones = parse_valid_zones(CONFIG_PATH)
    points = []
    for zx in range(0, 64, GRID_STEP):
        for zz in range(0, 64, GRID_STEP):
            if not valid_zones or (zx, zz) in valid_zones:
                points.append((zx, zz))
    if args.limit > 0:
        points = points[: args.limit]

    total = len(points) * len(POSES)
    est_min = total * (SETTLE_S + SAMPLES_PER_POINT * SAMPLE_INTERVAL_S) / 60.0
    print(f"[phase4] points={len(points)} poses={len(POSES)} total_measurements={total}")
    print(f"[phase4] SETTLE_S={SETTLE_S} SAMPLES_PER_POINT={SAMPLES_PER_POINT} -- rough estimate {est_min:.0f} min")

    out_path = REPO_ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_f = open(out_path, "w")
    out_f.write("zone_x,zone_z,world_x,world_z,pose,gpu_ms_median,gpu_ms_p95,gpu_ms_min,gpu_ms_max,n_samples,tpg_capped_corners,node_count,is_recheck\n")
    out_f.flush()

    d = Driver(exe=args.exe)
    print(f"[phase4] launching {args.exe} ...")
    if not d.launch(wait_s=40):
        print("[phase4] ERROR: driver failed to connect", file=sys.stderr)
        return 1
    print("[phase4] connected")

    ok, r = d.send("md.set_gpu_sync_timing(true)")
    if not ok:
        print(f"[phase4] ERROR: set_gpu_sync_timing failed: {r}", file=sys.stderr)
        d.shutdown()
        return 1

    tpg_on = (args.tpg == "on")
    ok, r = d.send(f"md.set_terrain_projected_grid({'true' if tpg_on else 'false'})")
    if not ok:
        print(f"[phase4] ERROR: set_terrain_projected_grid failed: {r}", file=sys.stderr)
        d.shutdown()
        return 1
    print(f"[phase4] terrain_projected_grid_ = {tpg_on}")

    def measure_point(zx, zz, wx, wz, label, pitch, dist):
        d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
        d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
        d.send(f"md.set_camera_orbit(0.0, {pitch}, {dist})")
        time.sleep(SETTLE_S)
        samples = []
        for _ in range(SAMPLES_PER_POINT):
            val, err = d.get_number("md.get_gpu_ms()")
            if val is not None and val > 0:
                samples.append(val)
            time.sleep(SAMPLE_INTERVAL_S)
        if samples:
            med = median(samples)
            p95 = percentile(samples, 0.95)
            lo, hi = min(samples), max(samples)
        else:
            med = p95 = lo = hi = 0.0
        capped, _ = d.get_number("md.granite_terrain_stats().tpg_capped_corners")
        capped_i = int(capped) if capped is not None else -1
        nc, _ = d.get_number("md.granite_terrain_stats().node_count")
        nc_i = int(nc) if nc is not None else -1
        return med, p95, lo, hi, len(samples), capped_i, nc_i

    def write_row(zx, zz, wx, wz, label, med, p95, lo, hi, n, capped_i, nc_i, is_recheck):
        out_f.write(f"{zx},{zz},{wx:.1f},{wz:.1f},{label},{med:.4f},{p95:.4f},{lo:.4f},{hi:.4f},"
                     f"{n},{capped_i},{nc_i},{1 if is_recheck else 0}\n")
        out_f.flush()

    idx = 0
    t0 = time.monotonic()
    ref = None          # (zx, zz, wx, wz, label, pitch, dist) -- first point measured
    ref_first_med = None
    measured_since_recheck = 0
    try:
        for (zx, zz) in points:
            wx = (zx + 0.5) * CHUNK_SIZE
            wz = (zz + 0.5) * CHUNK_SIZE
            for (label, pitch, dist) in POSES:
                idx += 1
                med, p95, lo, hi, n, capped_i, nc_i = measure_point(zx, zz, wx, wz, label, pitch, dist)
                write_row(zx, zz, wx, wz, label, med, p95, lo, hi, n, capped_i, nc_i, is_recheck=False)
                elapsed = time.monotonic() - t0
                eta_min = (elapsed / idx) * (total - idx) / 60.0 if idx else 0.0
                print(f"[{idx}/{total}] zone({zx},{zz}) {label}: median={med:.2f}ms n={n} "
                      f"capped={capped_i} node_count={nc_i}  (eta {eta_min:.0f}min)")

                if ref is None:
                    ref = (zx, zz, wx, wz, label, pitch, dist)
                    ref_first_med = med
                measured_since_recheck += 1

                if args.recheck_every > 0 and measured_since_recheck >= args.recheck_every:
                    measured_since_recheck = 0
                    rzx, rzz, rwx, rwz, rlabel, rpitch, rdist = ref
                    rmed, rp95, rlo, rhi, rn, rcapped_i, rnc_i = measure_point(
                        rzx, rzz, rwx, rwz, rlabel, rpitch, rdist)
                    write_row(rzx, rzz, rwx, rwz, rlabel, rmed, rp95, rlo, rhi, rn, rcapped_i, rnc_i, is_recheck=True)
                    drift_pct = (rmed - ref_first_med) / ref_first_med * 100.0 if ref_first_med else 0.0
                    flag = "  <-- DRIFT >10%, see module doc" if abs(drift_pct) > 10.0 else ""
                    print(f"[RECHECK] zone({rzx},{rzz}) {rlabel}: median={rmed:.2f}ms "
                          f"(first={ref_first_med:.2f}ms, drift={drift_pct:+.1f}%){flag}")
    finally:
        out_f.close()
        d.shutdown()

    print(f"[phase4] done -- wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
