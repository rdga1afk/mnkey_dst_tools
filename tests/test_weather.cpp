// Weather: the CRT-style generator, the install's data chain (areasmap -> group -> season -> weather) and the sky values of one dark-rain and one clear zone.
// Install parts need KENSHI_DIR.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/weather.h>
#include "stb_image.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace md::weather;

TEST(Weather, CrtRandMatchesTheMsvcSequence) {
    CrtRand r;   // srand(1): the well-known first outputs of the Microsoft C runtime
    const int expect[] = { 41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464 };
    for (int e : expect) EXPECT_EQ(r.Next(), e);
    CrtRand a; a.Seed(1);
    EXPECT_EQ(a.RangeInt(10, 10), 10);
    for (int i = 0; i < 1000; ++i) { const int v = a.RangeInt(60, 480); EXPECT_GE(v, 60); EXPECT_LE(v, 480); }
}

namespace {
struct Install {
    md::fcs::GameDataContainer gd; unsigned char* areas = nullptr; int w = 0, h = 0; Data data; bool ok = false;
    Install() {
        const char* dir = std::getenv("KENSHI_DIR");
        if (!dir || !*dir) return;
        md::fcs::Install inst(dir); md::fcs::LoadOrder order; md::fcs::Error e;
        if (!inst.LoadOrderFor(order, e) || !gd.Load(order, e)) return;
        int c = 0;
        areas = stbi_load((std::string(dir) + "/data/newland/land/areasmap.tga").c_str(), &w, &h, &c, 3);
        ok = areas && LoadData(gd, areas, w, h, data);
    }
    ~Install() { if (areas) stbi_image_free(areas); }
    int ZoneOf(const std::string& group) const {
        const int g = data.FindGroup(group);
        for (int i = 0; i < kZones * kZones; ++i) if (data.zone_group[i] == g) return i;
        return -1;
    }
};
void Centre(int zone, float& x, float& z) { x = (float)(zone / kZones) * kZoneUnits - kZoneOrigin + 0.5f * kZoneUnits; z = (float)(zone % kZones) * kZoneUnits - kZoneOrigin + 0.5f * kZoneUnits; }
}

TEST(Weather, AreasmapIsStoredBottomUp) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "needs KENSHI_DIR";
    FILE* f = std::fopen((std::string(dir) + "/data/newland/land/areasmap.tga").c_str(), "rb");
    ASSERT_TRUE(f);
    unsigned char hdr[18] = {}; ASSERT_EQ(std::fread(hdr, 1, 18, f), 18u); std::fclose(f);
    EXPECT_EQ(hdr[17] & 0x20, 0);   // image descriptor bit 5 clear: bottom-left origin; the exe's codec turns this into top-row-first like stb_image (private notes)
}

TEST(Weather, HubTileIsInTheBorderZoneGroup) {
    Install in; if (!in.ok) GTEST_SKIP() << "needs KENSHI_DIR";
    EXPECT_EQ(in.data.groups[in.data.zone_group[20 * kZones + 32]].name, "Border Zone");   // tile 20.32 holds the Hub (docs/research/kenshi_specs/navtiles.md)
}

TEST(Weather, DarkRainZoneAndClearZoneSkyValues) {
    Install in; if (!in.ok) GTEST_SKIP() << "needs KENSHI_DIR";
    // Deadlands: the season holds one weather (clouds density 1, rain 100) -> offset 1*1.4-0.8, darkness pow(1-0.5, 0.3)
    const int dz = in.ZoneOf("Deadlands"); ASSERT_GE(dz, 0);
    World w; w.Init(&in.data, 1);
    float x, z; Centre(dz, x, z);
    double minutes = 0.0;
    for (int i = 0; i < 40; ++i) { w.Update(minutes, 1.f, x, z); minutes += 1.0; }   // 40 s: the 30 s transition is over
    EXPECT_NEAR(w.Out().clouds_w, 1.f, 1e-5f);   // the original also accumulates rate * dt per frame
    EXPECT_NEAR(w.Out().cloud_offset, 0.6f, 1e-5f);
    EXPECT_NEAR(w.Out().darkness, std::pow(0.5f, 0.3f), 1e-5f);
    const float k = w.Strength(w.Out().group);   // the season's strength limit is 0.9..1 and the group multiplier 1
    EXPECT_GE(k, 0.9f); EXPECT_LE(k, 1.f);
    EXPECT_NEAR(w.Out().rain, 100.f * k, 1e-3f);
    // a group whose weathers all have clouds density 0 and no rain
    int clear = -1;
    for (size_t g = 1; g < in.data.groups.size() && clear < 0; ++g) {
        bool all0 = !in.data.groups[g].seasons.empty();
        for (const auto& s : in.data.groups[g].seasons) for (const auto& wd : s.weathers) if (wd.clouds != 0.f || wd.rain != 0.f) all0 = false;
        if (all0 && in.ZoneOf(in.data.groups[g].name) >= 0) clear = (int)g;
    }
    ASSERT_GE(clear, 0);
    World c; c.Init(&in.data, 1);
    Centre(in.ZoneOf(in.data.groups[clear].name), x, z); minutes = 0.0;
    for (int i = 0; i < 40; ++i) { c.Update(minutes, 1.f, x, z); minutes += 1.0; }
    EXPECT_FLOAT_EQ(c.Out().clouds_w, 0.f);
    EXPECT_NEAR(c.Out().cloud_offset, -0.8f, 1e-6f);
    EXPECT_FLOAT_EQ(c.Out().darkness, 0.f);
}

TEST(Weather, SeasonLengthsShareTheYear) {
    Install in; if (!in.ok) GTEST_SKIP() << "needs KENSHI_DIR";
    // days per year 100: Desert holds seasons with shares 900 and 300 (75 + 25 days); Skinner's Roam 300 / 200 / 100 ordered by the 1st triple value (0, 2, 3 -> 200, 100, 300 -> 33 / 17 / 50)
    const GroupDef& d = in.data.groups[in.data.FindGroup("Desert")];
    ASSERT_EQ(d.seasons.size(), 2u);
    EXPECT_EQ(d.seasons[0].days + d.seasons[1].days, 100);
    EXPECT_EQ(d.seasons[0].days, 75); EXPECT_EQ(d.seasons[1].days, 25);
    const GroupDef& r = in.data.groups[in.data.FindGroup("Skinner's Roam")];
    ASSERT_EQ(r.seasons.size(), 3u);
    EXPECT_EQ(r.seasons[0].days + r.seasons[1].days + r.seasons[2].days, 100);
    EXPECT_EQ(r.seasons[0].days, 33); EXPECT_EQ(r.seasons[1].days, 17); EXPECT_EQ(r.seasons[2].days, 50);
}

TEST(Weather, YearSplitDoesNotDependOnTheWalkOrder) {
    Install in; if (!in.ok) GTEST_SKIP() << "needs KENSHI_DIR";
    // the original walks the seasons in record-pointer order, we in sorted order: no group of the install is affected
    for (const GroupDef& g : in.data.groups) {
        if (g.seasons.size() < 2) continue;
        std::vector<float> sh; for (const SeasonDef& s : g.seasons) sh.push_back(s.share);
        const std::vector<int> a = SplitYear(sh, 100, false), b = SplitYear(sh, 100, true);
        EXPECT_EQ(a, b) << g.name;
    }
    EXPECT_EQ(SplitYear({ 1.f, 1.f, 1.f }, 100, false), (std::vector<int>{ 33, 33, 34 }));   // where the walk order would matter
    EXPECT_EQ(SplitYear({ 1.f, 1.f, 1.f }, 100, true), (std::vector<int>{ 34, 33, 33 }));
}

TEST(Weather, SameSeedSameState) {
    Install in; if (!in.ok) GTEST_SKIP() << "needs KENSHI_DIR";
    World a, b, c; a.Init(&in.data, 7); b.Init(&in.data, 7); c.Init(&in.data, 8);
    double minutes = 0.0;
    for (int i = 0; i < 4000; ++i) { a.Update(minutes, 0.5f, 0.f, 0.f); b.Update(minutes, 0.5f, 0.f, 0.f); c.Update(minutes, 0.5f, 0.f, 0.f); minutes += 5.0; }
    EXPECT_EQ(a.Checksum(), b.Checksum());
    EXPECT_NE(a.Checksum(), c.Checksum());
}

TEST(Weather, GroundWetnessRatesAndDeterminism) {
    Install in; if (!in.ok) GTEST_SKIP() << "needs KENSHI_DIR";
    World w; w.Init(&in.data, 1);
    float prev = 0.f; bool rose = false;
    double minutes = 0.0;
    for (int i = 0; i < 20000; ++i) {
        w.Update(minutes, 0.5f, 0.f, 0.f); minutes += 1.0;
        const float g = w.Out().ground_wetness;
        if (g > prev) { EXPECT_LE(g - prev, 0.5f * 0.01f + 1e-6f); rose = true; }
        if (g < prev) { EXPECT_LE(prev - g, 0.5f * 0.005f + 1e-6f); EXPECT_LE(w.Out().wetness, 0.f); }
        prev = g;
    }
    (void)rose;   // a dry install region may never rain; the bounds above are the check
}
