#!/usr/bin/env python3
"""
qa_stats.py — shared percentile() for tools/qa/*.py perf scripts.

Two genuinely different algorithms were independently written across 4
scripts (surgical-simplicity audit, docs/SURGICAL_SIMPLICITY_AUDIT_2026-09.md
§2): perf_phase4_world_scan.py/perf_ab_jobsystem_workers.py rounded to the
nearest sample index (no interpolation, empty input -> 0.0);
render_profiles_tier_measure.py/p5_reproducibility_recheck.py linearly
interpolate between the two nearest ranks (empty input -> nan). All 4 call
sites are diagnostic report/CSV output only, never a pass/fail gate, so this
consolidation keeps BOTH algorithms behind a method= switch rather than
picking a winner -- each caller passes its own original method, so no
existing report's numbers change.
"""


def percentile(vals, p, method="linear"):
    if method == "nearest_round":
        if not vals:
            return 0.0
        s = sorted(vals)
        n = len(s)
        idx = max(0, min(n - 1, int(round(p * (n - 1)))))
        return s[idx]

    if not vals:
        return float("nan")
    s = sorted(vals)
    k = (len(s) - 1) * p
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    if f == c:
        return s[f]
    return s[f] + (s[c] - s[f]) * (k - f)
