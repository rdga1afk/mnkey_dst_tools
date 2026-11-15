#include <gtest/gtest.h>
#include <monkey_dust/world/grass_placement.h>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace md::grass;

namespace {
float FlatSlope(float, float, void*) { return 0.0f; }
float SteepEast(float x, float, void*) { return x > 50.0f ? 2000.0f : 0.0f; }
float Height(float x, float, void*) { return x; }

// Straight transcription of the exe's stride walk (GRASS_EXE_2026-11-15.md), kept apart from the engine type on purpose.
struct RefTable {
    std::vector<float> t; uint32_t idx = 0, stride = 1, ctr = 0;
    float next() {
        if (uint32_t(t.size() - stride) <= idx) {
            uint32_t c = ctr; ctr = c + 1;
            if (ctr == stride) { stride += 1; ctr = 0; }
            idx = ctr;
            if (stride > 0x17) stride = 1;
        }
        uint32_t r = idx; idx = r + stride;
        return t[r + stride];
    }
};
} // namespace

TEST(GrassPlacement, TableWalkStartsAtEntryOneAndStridesToTheEnd) {
    std::vector<float> st(60000);
    for (uint32_t i = 0; i < st.size(); ++i) st[i] = float(i);
    RandomTable rt; rt.Init(st.data(), uint32_t(st.size()));
    EXPECT_EQ(rt.Unit(), 1.0f);      // first draw = table[1]
    EXPECT_EQ(rt.Unit(), 2.0f);
    RefTable ref; ref.t = st;
    RandomTable b; b.Init(st.data(), uint32_t(st.size()));
    for (int i = 0; i < 300000; ++i) ASSERT_EQ(b.Unit(), ref.next()) << i;   // many wraps: stride 1 .. 23 and back
    EXPECT_LE(b.stride, 0x17u);
}

TEST(GrassPlacement, TableValueIsU32OverMax) {
    EXPECT_EQ(TableValue(0), 0.0f);
    EXPECT_EQ(TableValue(0xffffffffu), 1.0f);
    EXPECT_NEAR(TableValue(0x80000000u), 0.5f, 1e-6f);
}

TEST(GrassPlacement, BilinearSamplerMatchesTheExeFormula) {
    const uint32_t W = 5, H = 5;
    uint8_t px[W * H];
    for (uint32_t z = 0; z < H; ++z) for (uint32_t x = 0; x < W; ++x) px[z * W + x] = uint8_t(10 * x + 20 * z);
    const Rect b{0, 0, 30, 30};
    // x = 15, z = 15: xf = (5-2)*0.5 - 0.5 + 1 = 2.0 exactly -> weights 0, cell (2, 2) = 10*2 + 20*2 = 60
    EXPECT_NEAR(DensityBilinear(px, W, H, 15, 15, b), 60.0f / 255.0f, 1e-5f);
    // x = 20: xf = 3*(20/30) + 0.5 = 2.5 -> halfway between columns 2 and 3 (values 20 and 30 on row 2 -> 60 and 70)
    EXPECT_NEAR(DensityBilinear(px, W, H, 20, 15, b), 65.0f / 255.0f, 1e-5f);
    // outside the bounds and on the far edge give 0; inside, xf runs 0.5 .. W - 1.5, so the xi >= W - 1 reject never triggers there
    EXPECT_EQ(DensityBilinear(px, W, H, -1, 15, b), 0.0f);
    EXPECT_EQ(DensityBilinear(px, W, H, 30, 15, b), 0.0f);
    EXPECT_GT(DensityBilinear(px, W, H, 29.9f, 15, b), 0.0f);   // xf = 3 * 0.9967 + 0.5 = 3.49, xi 3 = W - 2 is valid
}

TEST(GrassPlacement, PopulateMatchesAnIndependentTranscription) {
    std::vector<float> st(60000);
    uint32_t s = 12345u;
    for (auto& v : st) { s = s * 1664525u + 1013904223u; v = TableValue(s); }
    const uint32_t W = 9, H = 9;
    std::vector<uint8_t> map(W * H);
    for (uint32_t i = 0; i < map.size(); ++i) map[i] = uint8_t((i * 37u) & 0xff);
    Layer l{}; l.density = 0.02f; l.max_slope = 1000.0f; l.map = map.data(); l.map_w = W; l.map_h = H; l.map_bounds = {0, 0, 100, 100};
    const Rect page{0, 0, 100, 100};
    const TerrainFns t{SteepEast, Height, nullptr};
    const uint32_t count = CandidateCount(l, page);
    ASSERT_EQ(count, 200u);

    RandomTable rt; rt.Init(st.data(), uint32_t(st.size()));
    std::vector<float> out(4 * count);
    const uint32_t n = PopulateBilinear(rt, l, t, page, count, out.data());

    RefTable ref; ref.t = st;
    std::vector<float> want;
    for (uint32_t i = 0; i < count; ++i) {
        float x = page.left + (page.right - page.left) * ref.next();
        float z = page.top + (page.bottom - page.top) * ref.next();
        if (!(SteepEast(x, z, nullptr) <= 1000.0f)) continue;                 // slope filter: no more draws
        float u = ref.next();
        if (DensityBilinear(map.data(), W, H, x, z, l.map_bounds) <= u) { ref.next(); ref.next(); continue; }
        want.push_back(x); want.push_back(z); want.push_back(ref.next());
        want.push_back((6.28318530717958647692f - 0.0f) * ref.next() + 0.0f);
    }
    ASSERT_EQ(size_t(n) * 4, want.size());
    for (size_t i = 0; i < want.size(); ++i) ASSERT_EQ(out[i], want[i]) << i;
    for (uint32_t i = 0; i < n; ++i) EXPECT_LE(out[4 * i], 50.0f);           // nothing placed on the steep half
}

TEST(GrassPlacement, HeightRangeRejectsOutsideAndConsumesTwoDraws) {
    std::vector<float> st(60000, 0.0f);
    for (uint32_t i = 0; i < st.size(); ++i) st[i] = float(i % 1000) / 1000.0f;
    std::vector<uint8_t> map(9 * 9, 255);
    Layer l{}; l.density = 0.01f; l.max_slope = 1000.0f; l.map = map.data(); l.map_w = 9; l.map_h = 9; l.map_bounds = {0, 0, 100, 100};
    l.min_y = 20.0f; l.max_y = 60.0f;
    RandomTable rt; rt.Init(st.data(), uint32_t(st.size()));
    std::vector<float> out(4 * 100);
    const uint32_t n = PopulateBilinear(rt, l, TerrainFns{FlatSlope, Height, nullptr}, {0, 0, 100, 100}, 100, out.data());
    ASSERT_GT(n, 0u);
    for (uint32_t i = 0; i < n; ++i) { EXPECT_GE(out[4 * i], 20.0f); EXPECT_LE(out[4 * i], 60.0f); }
}

TEST(GrassPlacement, AcceptanceRateFollowsTheDensityMap) {
    std::vector<float> st(60000);
    uint32_t s = 99u;
    for (auto& v : st) { s = s * 1664525u + 1013904223u; v = TableValue(s); }
    std::vector<uint8_t> map(129 * 129, 128);   // ~0.502 density everywhere
    Layer l{}; l.density = 0.05f; l.max_slope = 1000.0f; l.map = map.data(); l.map_w = 129; l.map_h = 129; l.map_bounds = {0, 0, 250, 250};
    RandomTable rt; rt.Init(st.data(), uint32_t(st.size()));
    uint64_t cand = 0, got = 0;
    std::vector<float> out(4 * 4000);
    for (int page = 0; page < 20; ++page) {
        const uint32_t c = CandidateCount(l, {0, 0, 250, 250});
        ASSERT_LE(c, 4000u);
        got += PopulateBilinear(rt, l, TerrainFns{FlatSlope, Height, nullptr}, {0, 0, 250, 250}, c, out.data());
        cand += c;
    }
    // Candidates near the map edge fall in the unsampled border (sampler returns 0), so the rate is slightly below p.
    const double p = 128.0 / 255.0, sigma = std::sqrt(double(cand) * p * (1.0 - p));
    EXPECT_LE(double(got), double(cand) * p + 3.0 * sigma);
    EXPECT_GE(double(got), double(cand) * p * 0.95 - 3.0 * sigma);
}
