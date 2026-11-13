#include "ground_live_cache_rings.h"
#include <algorithm>
#include <cmath>

namespace md {

static CacheRect Intersect(const CacheRect& a, const CacheRect& b) {
    CacheRect r{ std::max(a.x0, b.x0), std::max(a.z0, b.z0), std::min(a.x1, b.x1), std::min(a.z1, b.z1) };
    return r.Empty() ? CacheRect{} : r;
}

static int32_t FloorDiv(float m, float t) { return (int32_t)std::floor(m / t); }

bool GroundCacheRings::Update(float cam_x, float cam_z) {
    bool moved = false;
    for (int r = 0; r < kRings; ++r) {
        Ring& g = ring_[r];
        const int32_t cx = FloorDiv(cam_x, kTexel[r]), cz = FloorDiv(cam_z, kTexel[r]);
        // snapped centre: whole kSnap steps, so a strip is never thinner than kSnap texels
        const int32_t sx = (int32_t)std::floor((double)cx / kSnap) * kSnap, sz = (int32_t)std::floor((double)cz / kSnap) * kSnap;
        const CacheRect w{ sx - kSize / 2, sz - kSize / 2, sx + kSize / 2, sz + kSize / 2 };
        if (!g.init || w.x0 != g.want.x0 || w.z0 != g.want.z0) {
            moved = true;
            g.want = w; g.init = true;
            g.baked = Intersect(g.baked, w);   // what left the ring is dropped (its toroidal slots are reused by the new strips)
        }
    }
    return moved;
}

bool GroundCacheRings::NextStrip(int64_t max_texels, int* ring, CacheRect* strip) const {
    if (max_texels < 1) return false;
    for (int r = 0; r < kRings; ++r) {
        const Ring& g = ring_[r];
        if (!g.init) continue;
        const CacheRect& w = g.want; const CacheRect& b = g.baked;
        if (b.Empty()) {   // first rows: the full width, centred on the camera row
            const int32_t width = w.x1 - w.x0;
            const int32_t d = (int32_t)std::max<int64_t>(1, std::min<int64_t>(w.z1 - w.z0, max_texels / width));
            const int32_t cz = (w.z0 + w.z1) / 2;
            *ring = r; *strip = CacheRect{ w.x0, cz - d / 2, w.x1, cz - d / 2 + d };
            return true;
        }
        // the four missing sides, thickest first; a z strip spans the baked x range, an x strip the baked z range, so baked stays one rectangle
        const int32_t miss[4] = { b.z0 - w.z0, w.z1 - b.z1, b.x0 - w.x0, w.x1 - b.x1 };
        int side = -1, best = 0;
        for (int s = 0; s < 4; ++s) if (miss[s] > best) { best = miss[s]; side = s; }
        if (side < 0) continue;   // complete
        const int32_t len = side < 2 ? (b.x1 - b.x0) : (b.z1 - b.z0);
        const int32_t d = (int32_t)std::max<int64_t>(1, std::min<int64_t>(best, max_texels / std::max(len, 1)));
        CacheRect s;
        if (side == 0) s = CacheRect{ b.x0, b.z0 - d, b.x1, b.z0 };
        else if (side == 1) s = CacheRect{ b.x0, b.z1, b.x1, b.z1 + d };
        else if (side == 2) s = CacheRect{ b.x0 - d, b.z0, b.x0, b.z1 };
        else s = CacheRect{ b.x1, b.z0, b.x1 + d, b.z1 };
        *ring = r; *strip = s;
        return true;
    }
    return false;
}

void GroundCacheRings::MarkBaked(int r, const CacheRect& s) {
    if (r < 0 || r >= kRings) return;
    Ring& g = ring_[r];
    CacheRect& b = g.baked;
    const CacheRect si = Intersect(s, g.want);   // the camera may have moved while the strip was baked: only the part still wanted counts
    if (si.Empty()) return;
    if (b.Empty()) { b = si; return; }
    // grow only when the strip is a full side of the baked rectangle (it is, unless the camera moved in between: then the strip is dropped and baked again later)
    if (si.x0 == b.x0 && si.x1 == b.x1 && si.z1 == b.z0) b.z0 = si.z0;
    else if (si.x0 == b.x0 && si.x1 == b.x1 && si.z0 == b.z1) b.z1 = si.z1;
    else if (si.z0 == b.z0 && si.z1 == b.z1 && si.x1 == b.x0) b.x0 = si.x0;
    else if (si.z0 == b.z0 && si.z1 == b.z1 && si.x0 == b.x1) b.x1 = si.x1;
}

bool GroundCacheRings::Covers(int r, float x0, float z0, float x1, float z1) const {
    if (r < 0 || r >= kRings) return false;
    const CacheRect& b = ring_[r].baked;
    if (b.Empty()) return false;
    const float t = kTexel[r];
    return FloorDiv(x0, t) >= b.x0 && FloorDiv(z0, t) >= b.z0 && (int32_t)std::ceil(x1 / t) <= b.x1 && (int32_t)std::ceil(z1 / t) <= b.z1;
}

}   // namespace md
