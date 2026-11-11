// The C++ zone reader must give the same placed building parts as the offline extraction that fed the Hub spike
// (tests/data/hub_spike/hub_spike.txt, circle 300 m around the Hub). Runs with KENSHI_DIR set, from the repo root.
#include <gtest/gtest.h>
#include <monkey_dust/world/zone_buildings.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

TEST(ZoneBuildings, HubMatchesSpikeInput) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error fe;
    ASSERT_TRUE(inst.LoadOrderFor(order, fe)) << fe.Message();
    md::fcs::GameDataContainer data;
    data.replace_reference_lists = true;
    ASSERT_TRUE(data.Load(order, fe)) << fe.Message();
    std::vector<md::world::PlacedPart> got; std::string err;
    ASSERT_TRUE(md::world::LoadBuildingsInCircle(dir, data, 9647.7f, 15038.8f, 300.0f, got, err)) << err;

    if (const char* dump = std::getenv("MD_ZB_DUMP")) { FILE* o = fopen(dump, "w"); if (o) { for (const auto& g : got) fprintf(o, "%.4f\t%.4f\t%.4f\t%.4f\t%s\n", g.x, g.y, g.z, g.y_off, g.mesh.c_str()); fclose(o); } }
    struct Row { std::string mesh; float v[8]; };
    std::vector<Row> want;
    std::ifstream f("tests/data/hub_spike/hub_spike.txt");
    ASSERT_TRUE(f.good());
    for (std::string line; std::getline(f, line);) {
        const size_t tab = line.find('\t'); ASSERT_NE(tab, std::string::npos);
        std::istringstream s(line.substr(tab + 1)); Row r; r.mesh = line.substr(0, tab); for (float& x : r.v) s >> x; want.push_back(r);
    }
    std::printf("[ZoneBuildings] want %zu rows, got %zu\n", want.size(), got.size());
    // match every wanted row to an unused got row with the same mesh and position within 1 cm
    { size_t same_at = 0, n = std::min(got.size(), want.size()); for (size_t i = 0; i < n; ++i) if (got[i].mesh == want[i].mesh && std::fabs(got[i].x - want[i].v[0]) < 0.01f && std::fabs(got[i].z - want[i].v[2]) < 0.01f) ++same_at; std::printf("[ZoneBuildings] same row at same index: %zu of %zu\n", same_at, n); }
    std::vector<bool> used(got.size(), false);
    size_t missing = 0, bad_other = 0; float worst = 0;
    for (const Row& w : want) {
        bool hit = false;
        for (size_t i = 0; i < got.size() && !hit; ++i) {
            if (used[i]) continue;
            const auto& g = got[i];
            std::string gm = g.mesh; // spike file keeps "./data/..." exactly as stored
            if (gm != w.mesh) continue;
            const float d = std::fabs(g.x - w.v[0]) + std::fabs(g.y - w.v[1]) + std::fabs(g.z - w.v[2]);
            if (d > 0.01f) continue;
            float dq = std::fabs(g.scale - w.v[3]);
            for (int k = 0; k < 4; ++k) dq += std::fabs(g.quat[k] - w.v[4 + k]);
            if (dq > 1e-4f) { ++bad_other; if (bad_other <= 3) std::printf("ROT %s want q %.5f %.5f %.5f %.5f s %.3f got q %.5f %.5f %.5f %.5f s %.3f\n", w.mesh.c_str(), w.v[4], w.v[5], w.v[6], w.v[7], w.v[3], g.quat[0], g.quat[1], g.quat[2], g.quat[3], g.scale); continue; }
            used[i] = true; hit = true; worst = std::fmax(worst, d);
        }
        if (!hit) { ++missing; std::printf("MISS %s %.2f %.2f %.2f\n", w.mesh.c_str(), w.v[0], w.v[1], w.v[2]); }
    }
    for (size_t i = 0; i < got.size(); ++i) if (!used[i]) std::printf("EXTRA %s %.2f %.2f %.2f\n", got[i].mesh.c_str(), got[i].x, got[i].y, got[i].z);
    std::printf("[ZoneBuildings] unmatched %zu, worst position diff %.5f m\n", missing, worst);
    EXPECT_EQ(missing, 0u);
    // Reference lists replace per category (data.replace_reference_lists); with Okran's per-target rule 17 extra parts of Storm_House_06 / _09 appear.
    size_t extra = 0; for (bool u : used) if (!u) ++extra;
    EXPECT_EQ(extra, 0u);
}

// One zone at a time (the streaming unit): the parts of the zones touching the Hub circle, filtered to the circle, are exactly the circle result.
TEST(ZoneBuildings, PerZoneUnionEqualsCircle) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error fe;
    ASSERT_TRUE(inst.LoadOrderFor(order, fe)) << fe.Message();
    md::fcs::GameDataContainer data;
    data.replace_reference_lists = true;
    ASSERT_TRUE(data.Load(order, fe)) << fe.Message();
    std::vector<md::world::PlacedPart> circle, zones; std::string err;
    ASSERT_TRUE(md::world::LoadBuildingsInCircle(dir, data, 9647.7f, 15038.8f, 300.0f, circle, err)) << err;
    for (int zx = 0; zx < 64; ++zx) for (int zz = 0; zz < 64; ++zz) {
        const float x0 = zx * 460.8f, z0 = zz * 460.8f;   // zone square in metres; keep zones the circle touches
        if (x0 > 9647.7f + 300.f || x0 + 460.8f < 9647.7f - 300.f || z0 > 15038.8f + 300.f || z0 + 460.8f < 15038.8f - 300.f) continue;
        ASSERT_TRUE(md::world::LoadZoneBuildings(dir, data, zx, zz, zones, err)) << err;
    }
    size_t inside = 0;
    for (const auto& p : zones) { const float dx = p.x - 9647.7f, dz = p.z - 15038.8f; if (dx * dx + dz * dz <= 300.f * 300.f) ++inside; }
    std::printf("[ZoneBuildings] circle %zu parts, zones total %zu, of them inside the circle %zu\n", circle.size(), zones.size(), inside);
    EXPECT_EQ(inside, circle.size());
}

// A4 worst-case suite (#203): rank the places of the world by the number of building parts within 300 m (the town range of the game); writes
// MD_TOWN_RANK=<file> (TSV: rank, x, z, parts_300m, zone files). Not a check: skipped unless the variable is set.
TEST(ZoneBuildings, RankTownsBy300mParts) {
    const char* dir = std::getenv("KENSHI_DIR"); const char* out = std::getenv("MD_TOWN_RANK");
    if (!dir || !*dir || !out) GTEST_SKIP() << "KENSHI_DIR / MD_TOWN_RANK not set";
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error fe;
    ASSERT_TRUE(inst.LoadOrderFor(order, fe)) << fe.Message();
    md::fcs::GameDataContainer data; data.replace_reference_lists = true;
    ASSERT_TRUE(data.Load(order, fe)) << fe.Message();
    struct P { float x, z; };
    std::vector<P> all;
    for (int zz = 0; zz < 64; ++zz) for (int zx = 0; zx < 64; ++zx) {
        std::vector<md::world::PlacedPart> parts; std::string err;
        if (md::world::LoadZoneBuildings(dir, data, zx, zz, parts, err)) for (const auto& p : parts) all.push_back({ p.x, p.z });
    }
    std::printf("[TownRank] %zu building parts in the world\n", all.size());
    // 100 m cells; count within 300 m of every part (a part is a candidate centre)
    const float R = 300.0f, R2 = R * R;
    std::map<std::pair<int, int>, std::vector<int>> grid;
    for (size_t i = 0; i < all.size(); ++i) grid[{ (int)std::floor(all[i].x / 100.0f), (int)std::floor(all[i].z / 100.0f) }].push_back((int)i);
    struct C { float x, z; int n; };
    std::vector<C> cands;
    for (size_t i = 0; i < all.size(); i += 7) {   // every 7th part is enough as a centre candidate
        int n = 0; const int cx = (int)std::floor(all[i].x / 100.0f), cz = (int)std::floor(all[i].z / 100.0f);
        for (int dz = -3; dz <= 3; ++dz) for (int dx = -3; dx <= 3; ++dx) { auto it = grid.find({ cx + dx, cz + dz }); if (it == grid.end()) continue;
            for (int j : it->second) { const float ddx = all[j].x - all[i].x, ddz = all[j].z - all[i].z; if (ddx * ddx + ddz * ddz <= R2) ++n; } }
        cands.push_back({ all[i].x, all[i].z, n });
    }
    std::sort(cands.begin(), cands.end(), [](const C& a, const C& b) { return a.n > b.n; });
    FILE* o = fopen(out, "w"); ASSERT_TRUE(o != nullptr);
    std::vector<C> picked;
    for (const C& c : cands) {
        bool far = true; for (const C& p : picked) if (std::hypot(c.x - p.x, c.z - p.z) < 1000.0f) { far = false; break; }
        if (!far) continue;
        picked.push_back(c); fprintf(o, "%zu\t%.1f\t%.1f\t%d\n", picked.size(), c.x, c.z, c.n);
        if (picked.size() >= 12) break;
    }
    fclose(o);
}
