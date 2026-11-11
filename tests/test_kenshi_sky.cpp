// #140 L1: the Kenshi sky light port (kenshi_sky.h, Okran sky.rs F040798 / F040805, SkyX getColorAt). The same checks as Okran's own test plus values that follow from the formulas.
#include <gtest/gtest.h>
#include <monkey_dust/render/kenshi_sky.h>
#include <monkey_dust/render/kenshi_hdr.h>
#include <cmath>

TEST(KenshiSky, SunRisesAndSets) {
    md::KenshiSkySettings s;   // sunrise 5, sunset 23, latitude 54
    EXPECT_NEAR(s.SunDirection(5.f).y, 0.f, 1e-5f);
    EXPECT_NEAR(s.SunDirection(23.f).y, 0.f, 1e-5f);
    EXPECT_GT(s.SunDirection(14.f).y, 0.5f);
    EXPECT_LT(s.SunDirection(2.f).y, 0.f);
    EXPECT_NEAR(s.SunDirection(14.f).x, 0.f, 1e-5f);   // the middle of the day: overhead
    const md::KsVec3 d = s.SunDirection(9.f);
    EXPECT_NEAR(d.x * d.x + d.y * d.y + d.z * d.z, 1.f, 1e-5f);
}

TEST(KenshiSky, MainLight) {
    md::KenshiSkySettings s;
    const auto noon = s.GetMainLight(14.f), dusk = s.GetMainLight(22.8f), night = s.GetMainLight(2.f);
    EXPECT_GT(noon.colour.x, 0.f); EXPECT_GT(noon.colour.z, 0.f); EXPECT_GT(noon.w, 0.5f);
    EXPECT_GT(dusk.colour.x / std::max(dusk.colour.z, 1e-6f), noon.colour.x / noon.colour.z);   // a low sun is redder
    EXPECT_EQ(night.dir.y, 0.f);                                                                  // the light direction never goes below the horizon
    EXPECT_NEAR(noon.min_lum, 0.8f, 1e-5f);                                                       // day: exposure min
    EXPECT_NEAR(night.min_lum, 0.35f * 0.8f, 1e-5f);                                              // night: night darkness x exposure min
    EXPECT_EQ(night.colour.x, 0.f);                                                               // below y = -0.2 there is no sun colour
}

TEST(KenshiSky, FogAndClouds) {
    md::KenshiSkySettings s;
    const md::KsVec4 f = s.FogParams();
    EXPECT_FLOAT_EQ(f.x, 50000.f); EXPECT_FLOAT_EQ(f.y, 3000.f); EXPECT_FLOAT_EQ(f.z, 30000.f);   // View Distance 5000: D = 50000 units, fog 0.06 D .. 0.6 D
    const md::KsVec4 h = s.HorizonClouds(13.f);
    EXPECT_GE(h.x, 0.f); EXPECT_LE(h.x, 2.f); EXPECT_NEAR(h.w, 0.f + std::clamp(-0.8f + 0.5f, 0.f, 1.f), 1e-6f);
    const md::KsVec3 z = s.ZenithLight(13.f);
    EXPECT_GT(z.z, z.x);   // the zenith is blue
}

TEST(KenshiHdr, AdaptLuminance) {
    // hdrfp4.hlsl AdaptLuminance_fp4: last + (current - last) * (1 - exp(-dt * rate)), clamped
    const float dt = 1.f / 30.f;
    EXPECT_NEAR(md::KenshiAdaptLuminance(1.f, 1.2f, dt, md::kKenshiAutoexpRate, 0.8f, 1.2f), 1.f + 0.2f * (1.f - std::exp(-dt * 0.5f)), 1e-6f);
    EXPECT_FLOAT_EQ(md::KenshiAdaptLuminance(1.f, 0.f, 100.f, 0.5f, 0.8f, 1.2f), 0.8f);   // clamped to MIN_LUMINANCE
    EXPECT_FLOAT_EQ(md::KenshiAdaptLuminance(1.f, 9.f, 100.f, 0.5f, 0.8f, 1.2f), 1.2f);   // clamped to MAX_LUMINANCE
    EXPECT_NEAR(md::KenshiExposureOf(1.2f), 0.55f / 1.2f, 1e-6f);
}
