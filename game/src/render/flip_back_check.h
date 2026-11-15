#pragma once
// Debug invariant (DE-273, docs/research/DEBUG_INVARIANTS_DESIGN_2026-11-14.md item 3): no flip-back state. A value (a tier, a LOD level, a slot) that returns to an
// earlier value within kHist frames after it changed, while the view stood still (camera moved < 0.5 m and turned < 0.05 deg since that frame), is a violation: A -> B -> A.
// CPU only, fixed arrays, no allocation. BeginFrame once per frame, then Push per key.
#include <cmath>
#include <cstdint>

namespace md {
class FlipBackCheck {
public:
    static constexpr int kKeys = 1024, kHist = 8;
    static constexpr double kMoveM = 0.5, kTurnDeg = 0.05;
    // cam: absolute position in metres; dir: view direction (any length > 0)
    void BeginFrame(const double cam[3], const double dir[3]) {
        ++t_;
        const int s = int(t_ % kHist);
        for (int i = 0; i < 3; ++i) cam_[s][i] = cam[i];
        const double l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int i = 0; i < 3; ++i) dir_[s][i] = l > 0 ? dir[i] / l : 0.0;
    }
    // True when this value completes an A -> B -> A within the history with the view still. Counts it.
    bool Push(int key, int value) {
        Slot& k = keys_[key & (kKeys - 1)];
        const int s = int(t_ % kHist);
        bool bad = false;
        if (k.last_t + 1 == t_ || k.last_t == t_) {   // the chain must be consecutive frames
        } else k.n = 0;
        k.val[s] = value; k.stamp[s] = t_;
        // look back k frames: same value as now, a different one in between, and the view still between that frame and now
        int diff_seen = 0;
        for (int back = 1; back < kHist && back <= k.n; ++back) {
            const uint64_t tb = t_ - back; const int sb = int(tb % kHist);
            if (k.stamp[sb] != tb) break;
            if (k.val[sb] != value) { diff_seen = 1; continue; }
            if (diff_seen && Still(sb, s)) { bad = true; break; }
        }
        if (k.last_t != t_) ++k.n;
        k.last_t = t_;
        if (bad) ++violations_;
        return bad;
    }
    uint64_t Violations() const { return violations_; }
private:
    struct Slot { int val[kHist] = {}; uint64_t stamp[kHist] = {}; uint64_t last_t = ~0ull - 2; int n = 0; };
    bool Still(int a, int b) const {
        double d2 = 0, dot = 0;
        for (int i = 0; i < 3; ++i) { const double d = cam_[a][i] - cam_[b][i]; d2 += d * d; dot += dir_[a][i] * dir_[b][i]; }
        if (d2 > kMoveM * kMoveM) return false;
        if (dot > 1.0) dot = 1.0;
        return std::acos(dot) * 57.29577951308232 <= kTurnDeg;
    }
    Slot keys_[kKeys]; double cam_[kHist][3] = {}, dir_[kHist][3] = {}; uint64_t t_ = 0, violations_ = 0;
};
} // namespace md
