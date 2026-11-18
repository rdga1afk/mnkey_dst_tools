// DE-316: the persistent terrain LOD tree against an oracle written from the plugin's rules (plugin_terrain_functions.md): split where value > threshold, the neighbour limit
// by RestrictLodNodes, release after 500 frames. CPU only, synthetic node tables.
#include <gtest/gtest.h>
#include <monkey_dust/world/terrain_lod_state.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <vector>

using namespace md;

namespace {
constexpr float kExtent = 29491.2f;

void Info(const void*, int L, int i, int j, float& err, float& ymin, float& ymax) {
    const float size = kExtent / (float)(1 << L);
    uint32_t h = (uint32_t)L * 73856093u ^ (uint32_t)i * 19349663u ^ (uint32_t)j * 83492791u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    err = size * 0.004f + size * 0.004f * (float)(h % 100) / 100.f;   // a rough surface: the error grows with the node, not monotonically
    ymin = 0.f; ymax = 40.f;
}
TerrainLodSource Src() { TerrainLodSource s; s.extent = kExtent; s.leaf_level = 10; s.cells = 16; s.info = Info; return s; }

const float kAll[16] = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // four planes that accept everything

TerrainLodParams Params() { TerrainLodParams p; p.min_level = 5; p.max_level = 10; return p; }

float Dist(const float cam[3], float x0, float z0, float size) {
    const float cy = cam[1] + (20.f - cam[1]) * 0.4f;
    const float dx = std::max(std::max(x0 - cam[0], 0.f), cam[0] - (x0 + size)), dz = std::max(std::max(z0 - cam[2], 0.f), cam[2] - (z0 + size));
    const float dy = std::max(std::max(0.f - cy, 0.f), cy - 40.f);
    return std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1.f);
}
float Thr(const TerrainLodParams& p, float d) { const float t = std::min(std::max((d - p.near_dist) / (p.far_dist - p.near_dist), 0.f), 1.f); return p.near_px + (p.far_px - p.near_px) * t; }

// the oracle: top-down, a node is a leaf where its value is at or below the threshold
void Ref(const float cam[3], const TerrainLodParams& p, int L, int i, int j, std::vector<TerrainLodNode>& out) {
    float err, a, b; Info(nullptr, L, i, j, err, a, b);
    const float size = kExtent / (float)(1 << L);
    const float d = Dist(cam, i * size, j * size, size);
    if (d > p.max_render_distance) return;
    const float e = p.viewport_px * err / d;
    if (L >= p.max_level || e <= Thr(p, d)) { TerrainLodNode n{}; n.origin_x = i * size; n.origin_z = j * size; n.size = size; n.level = L; n.y_min = 0; n.y_max = 40; n.morph = 1.f; out.push_back(n); return; }
    for (int q = 0; q < 4; ++q) Ref(cam, p, L + 1, i * 2 + (q & 1), j * 2 + (q >> 1), out);
}
using Key = std::tuple<int, long, long>;
Key K(const TerrainLodNode& n) { return { n.level, std::lround(n.origin_x), std::lround(n.origin_z) }; }
std::set<Key> KeySet(const std::vector<TerrainLodNode>& v, int n) { std::set<Key> s; for (int i = 0; i < n; ++i) s.insert(K(v[(size_t)i])); return s; }
size_t Diff(const std::set<Key>& a, const std::set<Key>& b) { size_t d = 0; for (const Key& k : a) d += !b.count(k); for (const Key& k : b) d += !a.count(k); return d; }

std::set<Key> RefSet(const float cam[3], const TerrainLodParams& p) {
    std::vector<TerrainLodNode> v; const int n0 = 1 << p.min_level;
    for (int j = 0; j < n0; ++j) for (int i = 0; i < n0; ++i) Ref(cam, p, p.min_level, i, j, v);
    const int cnt = (int)v.size(); v.resize((size_t)cnt * 4 + 64);
    const int n = RestrictLodNodes(v.data(), cnt, (int)v.size(), 2);
    return KeySet(v, n);
}
std::set<Key> StateSet(TerrainLodState& st, const float cam[3], const TerrainLodParams& p) {
    static std::vector<TerrainLodNode> buf(TerrainLodTree::kMaxNodes);
    const TerrainLodSource src = Src();
    int n = st.Select(src, cam, kAll, p, buf.data(), (int)buf.size());
    n = RestrictLodNodes(buf.data(), n, (int)buf.size(), 2);
    return KeySet(buf, n);
}
}  // namespace

// cold start per pose: exact; carried state (the tree keeps the earlier pose): differences only far from the camera
static void StoppedCamera(bool fresh) {
    static TerrainLodState st;
    const TerrainLodParams p = Params();
    const float poses[3][3] = { { 14000.f, 90.f, 14800.f }, { 9647.7f, 2.f, 15038.8f }, { 20000.f, 600.f, 8000.f } };
    for (const auto& c : poses) {
        if (fresh) st.Reset();
        std::set<Key> got;
        for (int f = 0; f < 4; ++f) got = StateSet(st, c, p);
        const std::set<Key> want = RefSet(c, p);
        if (fresh) EXPECT_EQ(Diff(got, want), 0u) << "pose " << c[0] << "," << c[2] << ": " << got.size() << " vs " << want.size();
        else {   // the tree carries the state of the earlier pose: the neighbour rule keeps finer nodes only at the edge of the range (stale neighbours that are not visited now)
            float worst_near = 1e9f;
            for (const Key* k : [&]{ std::vector<const Key*> v; for (const Key& a : got) if (!want.count(a)) v.push_back(&a); for (const Key& a : want) if (!got.count(a)) v.push_back(&a); return v; }()) {
                const float size = kExtent / (float)(1 << std::get<0>(*k));
                worst_near = std::min(worst_near, Dist(c, (float)std::get<1>(*k), (float)std::get<2>(*k), size));
            }
            EXPECT_GE(worst_near, 0.5f * p.max_render_distance) << "a node nearer than half the range differs from the stateless selection";
        }
    }
}

TEST(LodState, StoppedCameraColdStartEqualsStateless) { StoppedCamera(true); }
TEST(LodState, StoppedCameraCarriedStateDiffersOnlyFarAway) { StoppedCamera(false); }

TEST(LodState, MovingCameraChangesNoMoreThanStateless) {
    static TerrainLodState st;
    const TerrainLodParams p = Params();
    float cam[3] = { 12000.f, 80.f, 15000.f };
    std::set<Key> prev_s, prev_r; size_t sum_s = 0, sum_r = 0, worst_s = 0, worst_r = 0;
    for (int f = 0; f < 400; ++f) {
        cam[0] += 0.8333f; cam[2] -= 0.4f;   // 50 m/s at 60 fps
        const std::set<Key> s = StateSet(st, cam, p), r = RefSet(cam, p);
        if (f > 8) { const size_t ds = Diff(s, prev_s), dr = Diff(r, prev_r); sum_s += ds; sum_r += dr; worst_s = std::max(worst_s, ds); worst_r = std::max(worst_r, dr); }
        prev_s = s; prev_r = r;
    }
    std::printf("changed nodes over 390 frames: stateful %zu (worst %zu), stateless %zu (worst %zu)\n", sum_s, worst_s, sum_r, worst_r);
    EXPECT_LE(sum_s, sum_r);
    EXPECT_LE(worst_s, worst_r);
}

TEST(LodState, ChildrenAreReleasedAfter500UnusedFrames) {
    static TerrainLodState st;
    TerrainLodParams p = Params(); p.max_render_distance = 3000.f;
    float cam[3] = { 14000.f, 90.f, 14800.f };
    StateSet(st, cam, p);
    const int live = st.LiveBlocks();
    ASSERT_GT(live, 20);
    cam[0] = 2000.f; cam[2] = 2000.f;   // far away: the old region is out of range, never visited again
    for (int f = 0; f < 400; ++f) StateSet(st, cam, p);
    EXPECT_GT(st.LiveBlocks(), live / 2) << "released before 500 frames";
    for (int f = 0; f < 800; ++f) StateSet(st, cam, p);
    const std::set<Key> here = StateSet(st, cam, p);
    EXPECT_LT(st.LiveBlocks(), (int)here.size()) << "blocks of the old region are still held after 1200 frames";
}
