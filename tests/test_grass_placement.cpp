#include <gtest/gtest.h>
#include <monkey_dust/world/grass_placement.h>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// Orig: loadPage's count (exe 140a3b6b0) = (uint)(layer density * 1.0 * width * height), and the layer builder (1406d0340) sets the layer density to
// record density * 0.005 * the "grass density" option (settings loader 1403e84f0: 1.0 when the key is absent). Hand-computed expectations, truncation included.
TEST(GrassPlacement, CandidateCountIsRecordTimes0p005TimesOptionTimesPageArea) {
    const float page_w = 2304.f;   // half of a 4608-unit zone
    struct Row { float record, option, w, h; uint32_t want; };
    const Row rows[] = {
        { 1.0f, 1.0f, page_w, page_w, 26542u },    // 0.005 * 2304 * 2304 = 26542.08 -> 26542
        { 2.0f, 1.0f, page_w, page_w, 53084u },    // 53084.16
        { 1.0f, 2.0f, page_w, page_w, 53084u },    // the option multiplies exactly like the record
        { 1.0f, 0.5f, page_w, page_w, 13271u },    // 13271.04
        { 0.5f, 1.0f, 100.f, 200.f, 50u },         // 0.0025 * 20000 = 50
        { 1.0f, 1.0f, 10.f, 10.f, 0u },            // 0.5 -> truncates to 0
    };
    for (const Row& r : rows) {
        Layer l{}; l.density = r.record * 0.005f * r.option;
        const Rect page{ 0.f, 0.f, r.w, r.h };
        EXPECT_NEAR(double(CandidateCount(l, page)), double(r.want), 1.0) << r.record << " " << r.option;   // float rounding of the product can move the truncation by one
    }
}

// Bilinear density sampler of exe 140a6bd10 (the whole function) run in the Unicorn emulator on a fake map (byte i = (i * 37 + 11) & 255; exe_oracle_vectors3.tsv, densityBilinear rows):
// ours (DensityBilinear, the grass page loader's sampler) must be bit-equal, inside and outside the bounds.
TEST(GrassPlacement,DensityBilinearMatchesExeEmulatorVectors) {
    struct V { uint32_t w, h;  Rect b; float x, z, exe; };
    const V v[] = {
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 0.0f, 0.0f, 0.2666666805744171f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 2304.0f, 2304.0f, 0.8274510502815247f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 566.7839965820312f, 4548.09619140625f, 0.47000014781951904f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 4603.39208984375f, 4603.39208984375f, 0.38118433952331543f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 1382.4000244140625f, 3225.60009765625f, 0.6454901099205017f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 4608.0f, 2304.0f, 0.0f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, -460.79998779296875f, 2304.0f, 0.0f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 2304.0f, 5529.60009765625f, 0.0f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 46.08000183105469f, 4561.919921875f, 0.3615685701370239f},
        {9, 9, {0.0f, 0.0f, 4608.0f, 4608.0f}, 3548.159912109375f, 92.16000366210938f, 0.4485098123550415f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 0.0f, 0.0f, 0.4392157196998596f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 2304.0f, 2304.0f, 0.545098066329956f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 566.7839965820312f, 4548.09619140625f, 0.6217281818389893f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 4603.39208984375f, 4603.39208984375f, 0.41070228815078735f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 1382.4000244140625f, 3225.60009765625f, 0.48486384749412537f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 4608.0f, 2304.0f, 0.0f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, -460.79998779296875f, 2304.0f, 0.0f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 2304.0f, 5529.60009765625f, 0.0f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 46.08000183105469f, 4561.919921875f, 0.42964863777160645f},
        {129, 129, {0.0f, 0.0f, 4608.0f, 4608.0f}, 3548.159912109375f, 92.16000366210938f, 0.20913738012313843f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 100.0f, -50.0f, 0.4784314036369324f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 250.0f, 100.0f, 0.4176470935344696f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 136.89999389648438f, 246.10000610351562f, 0.4843098223209381f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 399.70001220703125f, 249.6999969482422f, 0.6084727048873901f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 190.0f, 160.0f, 0.68101966381073f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 400.0f, 100.0f, 0.0f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 70.0f, 100.0f, 0.0f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 250.0f, 310.0f, 0.0f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 103.0f, 247.0f, 0.43345099687576294f},
        {5, 4, {100.0f, -50.0f, 400.0f, 250.0f}, 331.0f, -44.0f, 0.30050984025001526f},
    };
    uint8_t px[129 * 129 + 4];
    for (const V& q : v) {
        for (uint32_t i = 0; i < q.w * q.h + 4; ++i) px[i] = uint8_t((i * 37 + 11) & 255);
        EXPECT_EQ(DensityBilinear(px, q.w, q.h, q.x, q.z, q.b), q.exe) << q.w << "x" << q.h << " x " << q.x << " z " << q.z;
    }
}

// loadPage count slice of exe 140a3b6b0 in the Unicorn emulator (exe_oracle_vectors3.tsv, pageCount rows; loader factor 1.0, page inside the layer bounds):
// the exe multiplies (density * factor) by (width * height); ours must give the same integer.
TEST(GrassPlacement, CandidateCountMatchesExeEmulatorVectors) {
    struct V { float density; Rect page; uint32_t exe; };
    const V v[] = {
        {0.004999999888241291f, {0.0f, 0.0f, 2304.0f, 2304.0f}, 26542u},
        {0.009999999776482582f, {0.0f, 0.0f, 2304.0f, 2304.0f}, 53084u},
        {0.004999999888241291f, {100.0f, 100.0f, 350.0f, 350.0f}, 312u},
        {0.004999999888241291f, {4608.0f, 0.0f, 6912.0f, 2304.0f}, 26542u},
        {0.0f, {0.0f, 0.0f, 2304.0f, 2304.0f}, 0u},
        {0.005595018155872822f, {0.0f, 0.0f, 1747.7830810546875f, 1828.635009765625f}, 17881u},
        {0.040025416761636734f, {0.0f, 0.0f, 1070.677001953125f, 617.5343627929688f}, 26463u},
        {0.018381277099251747f, {0.0f, 0.0f, 2359.493408203125f, 2331.9765625f}, 101139u},
        {0.006402702536433935f, {0.0f, 0.0f, 1881.2984619140625f, 2395.106201171875f}, 28849u},
        {0.029614223167300224f, {0.0f, 0.0f, 418.15673828125f, 983.4950561523438f}, 12179u},
        {0.02063325047492981f, {0.0f, 0.0f, 2398.580078125f, 1055.6165771484375f}, 52243u},
        {0.007499999832361937f, {3791.46142578125f, 10.0f, 4943.46142578125f, 1162.0f}, 9953u},
        {0.014999999664723873f, {149.98263549804688f, 10.0f, 649.9826049804688f, 510.0f}, 3749u},
        {0.013700000010430813f, {2257.47314453125f, 10.0f, 2507.47314453125f, 260.0f}, 856u},
        {0.019999999552965164f, {327.4200439453125f, 10.0f, 827.4200439453125f, 510.0f}, 5000u},
        {0.009999999776482582f, {659.848388671875f, 10.0f, 2963.848388671875f, 2314.0f}, 53084u},
        {0.0024999999441206455f, {156.8290252685547f, 10.0f, 2460.8291015625f, 2314.0f}, 13271u},
        {0.027400000020861626f, {2377.4794921875f, 10.0f, 3529.4794921875f, 1162.0f}, 36362u},
        {0.007499999832361937f, {3778.724365234375f, 10.0f, 4028.724365234375f, 260.0f}, 468u},
        {0.009999999776482582f, {3548.1611328125f, 10.0f, 5852.1611328125f, 2314.0f}, 53084u},
        {0.009999999776482582f, {672.1935424804688f, 10.0f, 1824.1934814453125f, 1162.0f}, 13271u},
        {0.0024999999441206455f, {3072.931884765625f, 10.0f, 3572.931884765625f, 510.0f}, 625u},
        {0.007499999832361937f, {3667.264892578125f, 10.0f, 5971.26513671875f, 2314.0f}, 39813u},
        {0.009999999776482582f, {1606.5770263671875f, 10.0f, 3910.576904296875f, 2314.0f}, 53084u},
        {0.0024999999441206455f, {3830.9248046875f, 10.0f, 6134.9248046875f, 2314.0f}, 13271u},
        {0.0024999999441206455f, {927.8274536132812f, 10.0f, 1427.8275146484375f, 510.0f}, 625u},
        {0.009999999776482582f, {729.3715209960938f, 10.0f, 1507.071533203125f, 787.7000122070312f}, 6048u},
        {0.006850000005215406f, {2138.36376953125f, 10.0f, 4442.36376953125f, 2314.0f}, 36362u},
        {0.014999999664723873f, {1569.515625f, 10.0f, 2347.215576171875f, 787.7000122070312f}, 9072u},
        {0.004999999888241291f, {2537.158203125f, 10.0f, 4841.158203125f, 2314.0f}, 26542u},
        {0.013700000010430813f, {3795.794921875f, 10.0f, 4045.794921875f, 260.0f}, 856u},
        {0.0012499999720603228f, {2456.27587890625f, 10.0f, 2956.27587890625f, 510.0f}, 312u},
        {0.004999999888241291f, {1456.65380859375f, 10.0f, 2234.353759765625f, 787.7000122070312f}, 3024u},
        {0.009999999776482582f, {1921.5804443359375f, 10.0f, 4225.58056640625f, 2314.0f}, 53084u},
        {0.0012499999720603228f, {2065.338134765625f, 10.0f, 2565.338134765625f, 510.0f}, 312u},
    };
    for (const V& q : v) {
        Layer l{}; l.density = q.density;
        EXPECT_EQ(CandidateCount(l, q.page), q.exe) << "density " << q.density << " page " << q.page.left << " " << q.page.right;
    }
}

// The whole PopulateBilinear (exe 140a41690) run in the Unicorn emulator (exe_oracle_vectors3.tsv, populateBilinear rows): a 4000-float table (LCG), the 129 x 129 map
// byte i = (i * 37 + 11) & 255 over (0, 0, 4608, 4608), slope(x, z) = x and height(x, z) = z. Ours must give the same blade count and the same output floats (FNV-1a of the bits);
// the cases cover the slope filter (max slope below / above the page), both height limits, the no-limit path and pages partly outside the map.
namespace {
float SlopeIsX(float x, float, void*) { return x; }
float HeightIsZ(float, float z, void*) { return z; }
uint64_t Fnv(const float* v, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) { uint32_t b; std::memcpy(&b, &v[i], 4); for (int k = 0; k < 4; ++k) { h ^= (b >> (8 * k)) & 255u; h *= 0x100000001b3ull; } }
    return h;
}
}
TEST(GrassPlacement, PopulateBilinearMatchesExeEmulatorVectors) {
    struct V { float max_slope, min_y, max_y; Rect page; uint32_t count, n; uint64_t fnv; };
    const V v[] = {
        {1000.0f, 0.0f, 0.0f, {500.0f, 500.0f, 1500.0f, 1500.0f}, 300u, 68u, 0x192ae98613f9997bull},
        {1000.0f, 600.0f, 1200.0f, {0.0f, 0.0f, 2304.0f, 2304.0f}, 500u, 25u, 0xb9c70881bda9080aull},
        {900.0f, 0.0f, 800.0f, {600.0f, 100.0f, 1000.0f, 700.0f}, 400u, 149u, 0x0e08fe64230cd869ull},
        {1000.0f, 700.0f, 0.0f, {200.0f, 500.0f, 900.0f, 1500.0f}, 500u, 203u, 0xc8056ac55dbe37d2ull},
        {1000.0f, 0.0f, 0.0f, {-300.0f, -300.0f, 300.0f, 300.0f}, 400u, 50u, 0xd89013b34425a2e4ull},
        {2000.0f, 0.0f, 0.0f, {900.0f, 1200.0f, 1100.0f, 1400.0f}, 450u, 233u, 0x1fc796b2c506fe32ull},
        {5000.0f, 0.0f, 3000.0f, {2304.0f, 2304.0f, 4608.0f, 4608.0f}, 580u, 79u, 0xd59aed896feb6acbull},
    };
    std::vector<float> tab(4000);
    uint32_t lcg = 12345u;
    for (float& t : tab) { lcg = lcg * 1664525u + 1013904223u; t = float(lcg >> 8) / 16777216.0f; }
    std::vector<uint8_t> map(129 * 129 + 4);
    for (size_t i = 0; i < map.size(); ++i) map[i] = uint8_t((i * 37 + 11) & 255);
    for (const V& q : v) {
        RandomTable rt; rt.Init(tab.data(), uint32_t(tab.size()));
        Layer l{}; l.density = 1.0f; l.max_slope = q.max_slope; l.min_y = q.min_y; l.max_y = q.max_y; l.map = map.data(); l.map_w = l.map_h = 129; l.map_bounds = {0.f, 0.f, 4608.f, 4608.f};
        std::vector<float> o(4 * q.count + 4);
        const uint32_t n = PopulateBilinear(rt, l, TerrainFns{SlopeIsX, HeightIsZ, nullptr}, q.page, q.count, o.data());
        EXPECT_EQ(n, q.n) << "page " << q.page.left << " " << q.page.top;
        EXPECT_EQ(Fnv(o.data(), 4 * size_t(n)), q.fnv) << "page " << q.page.left << " " << q.page.top;
    }
}
