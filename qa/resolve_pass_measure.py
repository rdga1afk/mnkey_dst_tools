#!/usr/bin/env python3
"""
resolve_pass_measure.py -- block-sampled, DVFS-safe measurement of the
terrain ground-shading resolve pass's real GPU-ms contribution.

Supersedes tests/scenarios/resolve_split_ab.lua (editor --exec, 3s settle +
3s sampling per condition -- far too short) and the earlier interleaved
2-vs-4-draws comparisons in docs/RESOLVE_OPT.md, both of which predate the
2026-09-19/21 DVFS finding: Intel HD 520 ramps/decays gt_cur_freq_mhz within
a few seconds of a load change, so comparing conditions that switch faster
than that (or without a settle window per condition) means each side
partially measures the OTHER side's frequency state, not its own
steady-state cost.

Protocol mirrors render_profiles_tier_measure.py's already-validated block
shape (40x5s=200s/condition gave 994-999MHz avg, 0% time at the 300MHz
floor) but needs only ONE process launch: md.set_terrain_resolve_repeat(n)
is an in-process toggle (SceneRender::terrain_resolve_repeat_debug), so
there's no cross-process variance to worry about at all, unlike the tier
script's render_settings.json-and-relaunch design. Block A = repeat=1
(normal resolve), Block B = repeat=0 (resolve draw loop runs zero times).
Each block gets its own settle window before sampling starts, and logs
gt_cur_freq_mhz in parallel so the run is self-auditing: if the two blocks'
average frequency differ by more than --freq-mismatch-pct, the script
refuses to report the delta as trustworthy (exit code 2) instead of
printing a number that looks precise but isn't comparable.

Requires a MONKEY_DUST_EDITOR=OFF, MD_PERF_TEST_HOOKS=ON build (same as
render_profiles_tier_measure.py). md.set_terrain_resolve_repeat/
md.get_gpu_ms/md.granite_terrain_stats are all registered unconditionally
(lua_scenario_api.cpp/lua_scenario_api_terrain.cpp have zero #ifdef).

Usage:
    python3 tools/qa/resolve_pass_measure.py --zone worst
    python3 tools/qa/resolve_pass_measure.py --zone canyon --exe build_release/game/monkey_dust
"""
import argparse
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver  # noqa: E402
from qa_stats import percentile  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
FREQ_PATH = Path("/sys/class/drm/card1/gt_cur_freq_mhz")
OUT_DIR = REPO_ROOT / "docs" / "audit" / "raw"

CHUNK_SIZE = 460.8
ZONES = {
    "worst":  (24, 12),   # established P5 worst-zone horizon pose
    "canyon": (6, 42),
}
PITCH, DIST = 10.0, 22.0
SETTLE_S = 20.0          # let DVFS ramp/decay into the new condition before sampling
N_SAMPLES = 40
SAMPLE_INTERVAL_S = 5.0  # 40x5s = 200s/block, matches render_profiles_tier_measure.py
FREQ_SAMPLE_INTERVAL_S = 0.2
FLOOR_MHZ = 300


def freq_logger(stop_event, samples, log_path):
    with open(log_path, "w") as flog:
        while not stop_event.is_set():
            try:
                mhz = int(FREQ_PATH.read_text().strip())
            except Exception:
                mhz = -1
            ts = time.time()
            flog.write(f"{ts:.6f} {mhz}\n")
            flog.flush()
            if mhz >= 0:
                samples.append(mhz)
            time.sleep(FREQ_SAMPLE_INTERVAL_S)


def run_block(d, label, repeat_n, out_dir, tag):
    # Verify the independent variable actually changed before trusting
    # anything downstream (docs/AI_DEV_PROTOCOL.md B.1 precedent --
    # same check render_profiles_tier_measure.py does for RenderTier).
    d.send(f"md.set_terrain_resolve_repeat({repeat_n})")
    got, _err = d.get_number("md.granite_terrain_stats().resolve_repeat_debug")
    if got is None or int(got) != repeat_n:
        print(f"[resolve-measure] ERROR: resolve_repeat_debug={got} "
              f"(expected {repeat_n}) -- toggle did not take effect, "
              f"refusing to trust this block", file=sys.stderr)
        return None, None

    print(f"[resolve-measure] {label}: settling {SETTLE_S}s ...")
    time.sleep(SETTLE_S)

    freq_samples = []
    stop_event = threading.Event()
    freq_log_path = out_dir / f"resolve_measure_freq_{tag}.log"
    ft = threading.Thread(target=freq_logger, args=(stop_event, freq_samples, freq_log_path))
    ft.start()

    gpu_ms_vals = []
    print(f"[resolve-measure] {label}: sampling {N_SAMPLES}x{SAMPLE_INTERVAL_S:.0f}s "
          f"({N_SAMPLES * SAMPLE_INTERVAL_S:.0f}s total) ...")
    for i in range(N_SAMPLES):
        time.sleep(SAMPLE_INTERVAL_S)
        gpu_ms, _gerr = d.get_number("md.get_gpu_ms()")
        if gpu_ms is not None:
            gpu_ms_vals.append(gpu_ms)
        if (i + 1) % 10 == 0:
            print(f"[resolve-measure]   {label} {i+1}/{N_SAMPLES} (last gpu_ms={gpu_ms})")

    stop_event.set()
    ft.join(timeout=5)
    return gpu_ms_vals, freq_samples


def summarize_freq(label, freq_samples):
    if not freq_samples:
        print(f"[resolve-measure] {label}: no freq samples collected")
        return None
    at_floor = sum(1 for x in freq_samples if x <= FLOOR_MHZ)
    pct_floor = 100.0 * at_floor / len(freq_samples)
    avg = sum(freq_samples) / len(freq_samples)
    print(f"[resolve-measure] {label} gt_cur_freq_mhz N={len(freq_samples)} "
          f"min/avg/max={min(freq_samples)}/{avg:.1f}/{max(freq_samples)} MHz, "
          f"%@{FLOOR_MHZ}MHz-floor={pct_floor:.1f}%")
    return avg


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zone", required=True, choices=list(ZONES.keys()))
    ap.add_argument("--exe", default=str(REPO_ROOT / "build_release" / "game" / "monkey_dust"))
    ap.add_argument("--freq-mismatch-pct", type=float, default=5.0,
                     help="max allowed %% difference between block A/B avg "
                          "gt_cur_freq_mhz before the delta is flagged untrustworthy")
    args = ap.parse_args()

    exe_path = Path(args.exe)
    if not exe_path.exists():
        print(f"ERROR: {exe_path} missing", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    zx, zz = ZONES[args.zone]
    wx = (zx + 0.5) * CHUNK_SIZE
    wz = (zz + 0.5) * CHUNK_SIZE

    d = None
    try:
        d = Driver(exe=str(exe_path))
        print(f"[resolve-measure] launching {exe_path} ...")
        if not d.launch(wait_s=40):
            print("[resolve-measure] ERROR: driver failed to connect", file=sys.stderr)
            return 1
        print("[resolve-measure] connected")

        d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
        d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
        d.send(f"md.set_camera_orbit(0.0, {PITCH}, {DIST})")
        d.send("md.set_vsync(-1)")
        print(f"[resolve-measure] initial settle {SETTLE_S}s at zone{(zx, zz)} "
              f"(wx={wx:.1f}, wz={wz:.1f}) ...")
        time.sleep(SETTLE_S)

        d.send("md.set_gpu_sync_timing(true)")

        a_vals, a_freq = run_block(d, "A (repeat=1, normal)", 1, OUT_DIR, f"{args.zone}_A")
        b_vals, b_freq = run_block(d, "B (repeat=0, skip)",   0, OUT_DIR, f"{args.zone}_B")

        d.send("md.set_terrain_resolve_repeat(1)")
        d.send("md.set_gpu_sync_timing(false)")
        d.shutdown()
        d = None

        if not a_vals or not b_vals:
            print("[resolve-measure] ERROR: one or both blocks produced no "
                  "trustworthy samples -- see errors above", file=sys.stderr)
            return 1

        print(f"\n[resolve-measure] === resolve pass @ zone{(zx, zz)} ({args.zone}) ===")
        print(f"[resolve-measure] A (repeat=1) N={len(a_vals)} "
              f"p50/p95/p99 = {percentile(a_vals, 0.50):.3f} / "
              f"{percentile(a_vals, 0.95):.3f} / {percentile(a_vals, 0.99):.3f}")
        print(f"[resolve-measure] B (repeat=0) N={len(b_vals)} "
              f"p50/p95/p99 = {percentile(b_vals, 0.50):.3f} / "
              f"{percentile(b_vals, 0.95):.3f} / {percentile(b_vals, 0.99):.3f}")

        a_freq_avg = summarize_freq("A", a_freq)
        b_freq_avg = summarize_freq("B", b_freq)

        a_p50 = percentile(a_vals, 0.50)
        b_p50 = percentile(b_vals, 0.50)
        delta = a_p50 - b_p50
        frac = delta / a_p50 if a_p50 else float("nan")
        print(f"[resolve-measure] resolve pass delta (p50 A-B) = {delta:.3f}ms "
              f"({frac * 100:.1f}% of full-frame A)")

        if a_freq_avg and b_freq_avg:
            freq_diff_pct = abs(a_freq_avg - b_freq_avg) / max(a_freq_avg, b_freq_avg) * 100.0
            if freq_diff_pct > args.freq_mismatch_pct:
                print(f"[resolve-measure] WARNING: A/B avg gt_cur_freq_mhz differ by "
                      f"{freq_diff_pct:.1f}% (> {args.freq_mismatch_pct:.1f}% threshold) -- "
                      f"blocks ran at meaningfully different GPU clocks, the delta above "
                      f"is NOT a clean measurement of the resolve pass alone",
                      file=sys.stderr)
                return 2
            print(f"[resolve-measure] A/B avg freq within {freq_diff_pct:.1f}% -- "
                  f"delta considered trustworthy")
        return 0
    finally:
        if d is not None:
            try:
                d.shutdown()
            except Exception:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
