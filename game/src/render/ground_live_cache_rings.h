#pragma once
// Live-ground cache, hour 1 (docs/research/LIVE_GROUND_CACHE_DESIGN_2026-11-13.md §4): the CPU bookkeeping of the camera-centred world rings.
// Each ring is a 2048 x 2048 toroidal texture of world texels (texel size 0.25 / 1 / 4 m); texel (tx, tz) of the world lives at ((tx mod N), (tz mod N)).
// The ring keeps one rectangle of baked texels (world texel coordinates, half-open). Moving the camera moves the wanted rectangle; the part of the baked
// rectangle that leaves it is dropped, the new strips are queued for the bake, nearest ring first, within a per-frame texel budget.
// No GPU here: the bake / serving passes read Wanted / Baked / NextStrip. Fixed arrays only (hot path rule).
#include <cstdint>

namespace md {

struct CacheRect { int32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0; bool Empty() const { return x1 <= x0 || z1 <= z0; } int64_t Area() const { return Empty() ? 0 : (int64_t)(x1 - x0) * (z1 - z0); } };

class GroundCacheRings {
public:
    static constexpr int kRings = 3, kSize = 2048, kSnap = 16;   // the centre moves in steps of kSnap texels: a strip is at least kSnap texels deep
    static constexpr float kTexel[kRings] = { 0.25f, 1.f, 4.f };

    void Reset() { for (auto& r : ring_) r = Ring{}; }
    // Camera position in metres (world corner origin). Updates the wanted rectangles and trims the baked ones; true when any ring moved.
    bool Update(float cam_x, float cam_z);
    // The next strip to bake (world texel rect of ring *ring), at most max_texels; false when every ring is complete. Nearest ring first.
    bool NextStrip(int64_t max_texels, int* ring, CacheRect* strip, int32_t max_depth = 1 << 30) const;   // max_depth: the strip's thickness across its length
    // The bake of a strip returned by NextStrip finished: the baked rectangle grows by it (the strip is adjacent to it by construction).
    void MarkBaked(int ring, const CacheRect& strip);
    // A world rectangle in metres is fully served by ring r (inside its baked texels).
    bool Covers(int r, float x0, float z0, float x1, float z1) const;
    const CacheRect& Wanted(int r) const { return ring_[r].want; }
    const CacheRect& Baked(int r) const { return ring_[r].baked; }
    void Invalidate() { for (auto& r : ring_) r.baked = CacheRect{}; }   // wetness changed: everything is baked again, progressively

private:
    struct Ring { CacheRect want, baked; bool init = false; };
    Ring ring_[kRings];
};

}   // namespace md
