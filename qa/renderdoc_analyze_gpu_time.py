#!/usr/bin/env python3
"""RENDER-AUDIT-3/4 (docs/RENDER_AUDIT_2026.md): open a .rdc capture
headlessly (renderdoc Python API, no GUI) and fetch the real per-action
GPU Duration timestamp-query counter, summed by top-level debug group
(GpuPushDebugGroup label). Reports which pass dominates real GPU time.

Usage: python3 renderdoc_analyze_gpu_time.py path/to/capture.rdc

Caveat (verified 2026-09-20): these numbers come from RenderDoc's own
replay of the capture, not the live game loop -- replay adds overhead
similar to how md.set_gpu_sync_timing's serialization was found to
distort live measurements (§2.2 of the audit doc). Relative proportions
between debug groups are more trustworthy than absolute ms totals.
Also: debug-group labels can go stale relative to what code actually
runs inside them after a refactor (RENDER-AUDIT-4 found exactly this --
a "Cull" marker whose real GPU cost was almost entirely a hoisted
terrain G-buffer draw, not culling) -- don't trust a marker name alone,
cross-check against the actual vkCmd*/per-eventId calls if a number
looks surprising.
"""
import sys
import renderdoc as rd

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} path/to/capture.rdc")
        sys.exit(1)
    path = sys.argv[1]
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, "", None)
    if not result.OK():
        print(f"ERROR opening capture: {result.Message()}")
        sys.exit(1)

    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if not result.OK():
        print(f"ERROR opening replay controller: {result.Message()}")
        sys.exit(1)

    counters = controller.EnumerateCounters()
    gpu_dur_counter = None
    gpu_dur_desc = None
    for c in counters:
        desc = controller.DescribeCounter(c)
        if "GPU" in desc.name and "Duration" in desc.name:
            gpu_dur_counter = c
            gpu_dur_desc = desc
            print(f"Using counter: {desc.name} ({desc.description})")
            print(f"  unit={desc.unit} resultType={desc.resultType} resultByteWidth={desc.resultByteWidth}")
            break
    if gpu_dur_counter is None:
        print("Available counters:")
        for c in counters:
            desc = controller.DescribeCounter(c)
            print(f"  {desc.name}")
        sys.exit(1)

    results = controller.FetchCounters([gpu_dur_counter])
    print(f"Fetched {len(results)} counter results")
    if results:
        r0 = results[0]
        print(f"Sample raw value: d={r0.value.d} f={r0.value.f} u32={r0.value.u32} u64={r0.value.u64}")

    # Pick the correct union field based on resultType (CompType).
    is_float = gpu_dur_desc.resultType == rd.CompType.Float
    wide = gpu_dur_desc.resultByteWidth == 8

    def extract(v):
        if is_float:
            return v.d if wide else v.f
        else:
            return v.u64 if wide else v.u32

    # Build eid -> gpu_duration_seconds map
    eid_to_s = {}
    for r in results:
        eid_to_s[r.eventId] = extract(r.value)

    # Walk the action (draw/dispatch) tree, group by top-level debug marker.
    group_totals = {}
    group_counts = {}
    root_actions = controller.GetRootActions()

    def walk(actions, current_group):
        for a in actions:
            grp = current_group
            is_marker = bool(a.flags & rd.ActionFlags.PushMarker)
            if is_marker:
                grp = a.customName if a.customName else a.GetName(controller.GetStructuredFile())
            if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Copy | rd.ActionFlags.Clear):
                secs = eid_to_s.get(a.eventId, None)
                key = grp if grp else "(ungrouped)"
                if secs is not None:
                    group_totals[key] = group_totals.get(key, 0.0) + secs
                    group_counts[key] = group_counts.get(key, 0) + 1
            if a.children:
                walk(a.children, grp)

    walk(root_actions, "(root)")

    print("\n=== Per-debug-group GPU time (real timestamp-query counter) ===")
    total_s = sum(group_totals.values())
    for k, v in sorted(group_totals.items(), key=lambda kv: -kv[1]):
        pct = (v / total_s * 100.0) if total_s > 0 else 0.0
        print(f"  {k:45s} {v*1000.0:8.4f} ms  ({pct:5.1f}%)  n={group_counts[k]}")
    print(f"\nTOTAL (summed groups): {total_s*1000.0:.4f} ms")

    controller.Shutdown()
    cap.Shutdown()

if __name__ == "__main__":
    main()
