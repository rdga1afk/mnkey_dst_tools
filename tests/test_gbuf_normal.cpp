// #205: round trip of the full-sphere octahedral normal of the terrain G-buffer (shaders/terrain_gbuf_pack.glsl, 8 + 8 bits). The CPU functions below are the same
// arithmetic as TS_GbufEncode / TS_GbufDecode; the test also checks that the shader file still holds the expressions this mirror copies.
#include <gtest/gtest.h>
#include <monkey_dust/io/data_files.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
struct V3 { float x, y, z; };
inline float Sgn(float v) { return v >= 0.f ? 1.f : -1.f; }
uint16_t Encode(V3 n) {
    float ox = n.x / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z)), oz = n.z / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z));
    if (n.y < 0.f) { const float fx = (1.f - std::fabs(oz)) * Sgn(ox), fz = (1.f - std::fabs(ox)) * Sgn(oz); ox = fx; oz = fz; }
    auto q = [](float o) { float v = std::floor(std::fmin(std::fmax(o * 0.5f + 0.5f, 0.f), 1.f) * 255.f + 0.5f); return (uint32_t)v; };
    return (uint16_t)(q(ox) | (q(oz) << 8));
}
V3 Decode(uint16_t w) {
    const float ox = (float)(w & 255u) * (2.f / 255.f) - 1.f, oz = (float)((w >> 8) & 255u) * (2.f / 255.f) - 1.f;
    V3 n{ ox, 1.f - std::fabs(ox) - std::fabs(oz), oz };
    if (n.y < 0.f) { n.x = (1.f - std::fabs(oz)) * Sgn(ox); n.z = (1.f - std::fabs(ox)) * Sgn(oz); }
    const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return { n.x / l, n.y / l, n.z / l };
}
}

TEST(GbufNormal, RoundTripFullSphere) {
    double worst = 0.0, worst_up = 0.0, worst_down = 0.0; V3 at{};
    const int N = 400;
    for (int i = 0; i <= N; ++i) {           // polar angle 0..pi: the poles (0, +-1, 0) are the first and last rows
        const float th = 3.14159265358979f * (float)i / (float)N;
        for (int j = 0; j < 4 * N; ++j) {
            const float ph = 6.28318530717959f * (float)j / (float)(4 * N);
            V3 n{ std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph) };
            const V3 d = Decode(Encode(n));
            const double dot = std::fmin(1.0, (double)n.x * d.x + (double)n.y * d.y + (double)n.z * d.z);
            const double deg = std::acos(dot) * 57.29577951308232;
            if (deg > worst) { worst = deg; at = n; }
            if (n.y >= 0.f) worst_up = std::fmax(worst_up, deg); else worst_down = std::fmax(worst_down, deg);
        }
    }
    std::printf("[GbufNormal] max angular error %.3f deg (upper hemisphere %.3f, lower %.3f) at (%.3f %.3f %.3f)\n", worst, worst_up, worst_down, at.x, at.y, at.z);
    EXPECT_LT(worst, 1.0);
    EXPECT_GT(Decode(Encode({ 0.f, -1.f, 0.f })).y, 0.99f - 2.f);   // the lower pole stays in the lower hemisphere
    EXPECT_LT(Decode(Encode({ 0.f, -1.f, 0.f })).y, -0.99f);
    EXPECT_GT(Decode(Encode({ 0.f, 1.f, 0.f })).y, 0.99f);
}

TEST(GbufNormal, ShaderStillHoldsTheMirroredMaths) {
    std::ifstream f(md::RepoPath("shaders/terrain_gbuf_pack.glsl"));
    ASSERT_TRUE(f.good()) << "run from the repo root";
    std::stringstream ss; ss << f.rdbuf(); const std::string s = ss.str();
    EXPECT_NE(s.find("vec2 o = N.xz / (abs(N.x) + abs(N.y) + abs(N.z));"), std::string::npos);
    EXPECT_NE(s.find("if (N.y < 0.0) o = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);"), std::string::npos);
    EXPECT_NE(s.find("N = vec3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);"), std::string::npos);
    EXPECT_NE(s.find("if (N.y < 0.0) N.xz = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);"), std::string::npos);
}
