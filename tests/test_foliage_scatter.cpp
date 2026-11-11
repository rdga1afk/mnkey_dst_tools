// Foliage scatter (#120): the random-number generator against the reference generator, and the Hub's MEDIUM meshes of the C++ port against the python stand-in tool
// (tests/data/hub_spike/hub_medium.txt, private/md_foliage_place.py: the later file wins a reference value, cluster counts are ints, float32 cell maths). The second test needs KENSHI_DIR; run from the repo root.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/foliage_scatter.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace md::world;

TEST(FoliageScatter, MtMatchesReferenceGenerator) {
    Mt mt(5489);
    EXPECT_EQ(mt.NextU32(), 3499211612u);
    EXPECT_EQ(mt.NextU32(), 581869302u);
    Mt a(123456789u); std::mt19937 b(123456789u);
    for (int i = 0; i < 3000; ++i) ASSERT_EQ(a.NextU32(), b()) << i;
}

TEST(FoliageScatter, RangeAndIntFollowTheExeFormulas) {
    Mt a(42), b(42);
    for (int i = 0; i < 200; ++i) {   // F041815: a + (b - a) x u / 2^32 in double, then float
        const double u = (double)b.NextU32();
        EXPECT_EQ(a.Range(-1.f, 1.f), (float)(u * (1.0 / 4294967296.0) * (double)(1.f - -1.f) + (double)-1.f));
    }
    Mt c(7), d(7);
    for (int i = 0; i < 200; ++i) {   // F041775: outputs masked to n's bits until one is not above n
        const uint32_t n = 5 + (uint32_t)i;
        uint32_t m = (n >> 1) | n; m |= m >> 2; m |= m >> 4; m |= m >> 8; m |= m >> 16;
        uint32_t v; do { v = d.NextU32() & m; } while (v > n);
        EXPECT_EQ(c.Int(n), v);
    }
    EXPECT_EQ(ParseIntOgre("3606-Newwworld.mod"), 3606);
    EXPECT_EQ(ParseIntOgre("abc"), 0);
}

TEST(FoliageScatter, HubMediumMatchesPythonStandIn) {
    const char* kd = std::getenv("KENSHI_DIR");
    if (!kd || !*kd) GTEST_SKIP() << "KENSHI_DIR not set";
    md::fcs::Install inst(kd); md::fcs::LoadOrder order; md::fcs::Error e; md::fcs::GameDataContainer data;
    data.replace_reference_lists = false;
    ASSERT_TRUE(inst.LoadOrderFor(order, e) && data.Load(order, e)) << e.Message();
    Foliage f = LoadFoliage(data);
    std::vector<Town> towns;
    { std::ifstream tf("tests/data/hub_spike/hub_towns.txt"); std::string l; while (std::getline(tf, l)) { if (l.empty() || l[0] == '#') continue; std::istringstream is(l); Town t; if (is >> t.x >> t.z >> t.no_foliage >> t.kind) towns.push_back(t); } }
    ASSERT_GT(towns.size(), 100u);
    const double cxu = 9647.7 * 10 - 147456, czu = 15038.8 * 10 - 147456, ru = 1000.0;   // 100 m round the Hub
    const std::vector<int> hub_biomes = { 0xff2828 };   // Swampfeeder Canyon: the one biome the python stand-in knows
    std::vector<Placed> got;
    for (int zz = 32; zz <= 32; ++zz)
        for (int zx = 20; zx <= 21; ++zx) {
            std::vector<Placed> z; std::string err;
            ASSERT_TRUE(ScatterZone(kd, f, zx, zz, 1, towns, z, err, &hub_biomes)) << err;
            for (const Placed& p : z) if (((double)p.x - cxu) * ((double)p.x - cxu) + ((double)p.z - czu) * ((double)p.z - czu) <= ru * ru) got.push_back(p);
        }
    struct Row { std::string mesh; double x, y, z, s; };
    std::vector<Row> want;
    { std::ifstream wf("tests/data/hub_spike/hub_medium.txt"); std::string l; while (std::getline(wf, l)) { std::istringstream is(l); Row r; std::string qw, q0, qy, q1, tex, flag; if (is >> r.mesh >> r.x >> r.y >> r.z >> r.s >> qw >> q0 >> qy >> q1 >> tex >> flag && flag == "0") want.push_back(r); } }
    ASSERT_EQ(got.size(), want.size());
    size_t bad = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        std::string m = f.meshes[got[i].mesh].mesh; for (char& c : m) if (c == '\\') c = '/';
        const double x = (got[i].x + 147456.0) / 10.0, y = got[i].y / 10.0, z = (got[i].z + 147456.0) / 10.0;
        if (m != want[i].mesh || std::fabs(x - want[i].x) > 0.02 || std::fabs(y - want[i].y) > 0.02 || std::fabs(z - want[i].z) > 0.02 || std::fabs(got[i].scale - want[i].s) > 0.002) { if (bad++ < 5) ADD_FAILURE() << i << " " << m << " " << x << " " << y << " " << z << " " << got[i].scale << " vs " << want[i].mesh << " " << want[i].x << " " << want[i].y << " " << want[i].z << " " << want[i].s; }
    }
    EXPECT_EQ(bad, 0u);
}

TEST(FoliageScatter, TownsMatchTheStandInList) {
    const char* kd = std::getenv("KENSHI_DIR");
    if (!kd || !*kd || !std::filesystem::exists(std::string(kd) + "/data/gamedata.base")) GTEST_SKIP() << "needs a Kenshi install (KENSHI_DIR), run from the repo root";
    md::fcs::Install inst(kd); md::fcs::LoadOrder order; md::fcs::Error e; md::fcs::GameDataContainer data;
    ASSERT_TRUE(inst.LoadOrderFor(order, e) && data.Load(order, e)) << e.Message();
    std::vector<Town> got, want; std::string err;
    ASSERT_TRUE(LoadTowns(kd, data, got, err)) << err;
    std::ifstream tf("tests/data/hub_spike/hub_towns.txt"); std::string l;
    while (std::getline(tf, l)) { if (l.empty() || l[0] == '#') continue; std::istringstream is(l); Town t; if (is >> t.x >> t.z >> t.no_foliage >> t.kind) want.push_back(t); }
    ASSERT_EQ(got.size(), want.size());
    for (size_t i = 0; i < got.size(); ++i) { EXPECT_EQ(got[i].x, want[i].x) << i; EXPECT_EQ(got[i].z, want[i].z) << i; EXPECT_EQ(got[i].no_foliage, want[i].no_foliage) << i; EXPECT_EQ(got[i].kind, want[i].kind) << i; }
}

TEST(FoliageScatter, GrassBladesOfTheHubCircle) {
    const char* kd = std::getenv("KENSHI_DIR");
    if (!kd || !*kd || !std::filesystem::exists(std::string(kd) + "/data/gamedata.base")) GTEST_SKIP() << "needs a Kenshi install (KENSHI_DIR), run from the repo root";
    md::fcs::Install inst(kd); md::fcs::LoadOrder order; md::fcs::Error e; md::fcs::GameDataContainer data;
    ASSERT_TRUE(inst.LoadOrderFor(order, e) && data.Load(order, e)) << e.Message();
    Foliage f = LoadFoliage(data);
    std::vector<Blade> b; std::string err;
    const float cx = (float)(9647.7 * 10 - 147456), cz = (float)(15038.8 * 10 - 147456);
    ASSERT_TRUE(ScatterGrass(kd, f, cx, cz, 500.f, 1.f, 1u, 100000, b, err)) << err;
    // the python stand-in (private/md_hub_grass.py, tests/data/hub_spike/hub_grass.txt) keeps 1418 blades in the same 50 m circle
    EXPECT_GT(b.size(), 700u); EXPECT_LT(b.size(), 2800u);
    for (const Blade& x : b) { const float dx = x.x - cx, dz = x.z - cz; ASSERT_LE(dx * dx + dz * dz, 500.f * 500.f * 1.0001f); ASSERT_GT(x.size, 0.f); }
}

// A4 worst-case suite (#203): the zones with the most MEDIUM foliage places (every 2nd zone each way); writes MD_FOLIAGE_RANK=<file> (TSV: zone x, zone z, places, mean world x, z in metres). Skipped unless set.
TEST(FoliageScatter, RankZonesByMediumPlaces) {
    const char* kd = std::getenv("KENSHI_DIR"); const char* out = std::getenv("MD_FOLIAGE_RANK");
    if (!kd || !*kd || !out) GTEST_SKIP() << "KENSHI_DIR / MD_FOLIAGE_RANK not set";
    md::fcs::Install inst(kd); md::fcs::LoadOrder order; md::fcs::Error fe;
    ASSERT_TRUE(inst.LoadOrderFor(order, fe)) << fe.Message();
    md::fcs::GameDataContainer data; data.replace_reference_lists = true;
    ASSERT_TRUE(data.Load(order, fe)) << fe.Message();
    Foliage f = LoadFoliage(data); std::vector<Town> towns; std::string err;
    ASSERT_TRUE(LoadTowns(kd, data, towns, err)) << err;
    struct Z { int x, z; size_t n; double mx, mz; };
    std::vector<Z> zs;
    for (int zz = 0; zz < 64; zz += 2) for (int zx = 0; zx < 64; zx += 2) {
        std::vector<Placed> pl;
        if (!ScatterZone(kd, f, zx, zz, 1, towns, pl, err)) continue;
        double sx = 0, sz = 0; for (const Placed& p : pl) { sx += (p.x + 147456.0) / 10.0; sz += (p.z + 147456.0) / 10.0; }
        zs.push_back({ zx, zz, pl.size(), pl.empty() ? 0 : sx / pl.size(), pl.empty() ? 0 : sz / pl.size() });
    }
    std::sort(zs.begin(), zs.end(), [](const Z& a, const Z& b) { return a.n > b.n; });
    FILE* o = fopen(out, "w"); ASSERT_TRUE(o != nullptr);
    for (size_t i = 0; i < zs.size() && i < 15; ++i) fprintf(o, "%d\t%d\t%zu\t%.1f\t%.1f\n", zs[i].x, zs[i].z, zs[i].n, zs[i].mx, zs[i].mz);
    fclose(o);
}

TEST(FoliageScatter, DumpMeshMaterialTypes) {   // #205: which foliage records are FOLIAGE-mode (4), TERRAIN-mode (2) or other, and whether a leaves mesh exists
    const char* kd = std::getenv("KENSHI_DIR"); const char* out = std::getenv("MD_FOLIAGE_DUMP");
    if (!kd || !*kd || !out) GTEST_SKIP() << "KENSHI_DIR / MD_FOLIAGE_DUMP not set";
    md::fcs::Install inst(kd); md::fcs::LoadOrder order; md::fcs::Error fe;
    ASSERT_TRUE(inst.LoadOrderFor(order, fe)) << fe.Message();
    md::fcs::GameDataContainer data; data.replace_reference_lists = true;
    ASSERT_TRUE(data.Load(order, fe)) << fe.Message();
    Foliage f = LoadFoliage(data);
    FILE* o = fopen(out, "w"); ASSERT_TRUE(o != nullptr);
    for (const FoliageMesh& m : f.meshes) fprintf(o, "%d\t%s\t%s\t%s\t%s\n", m.material_type, m.record.c_str(), m.mesh.c_str(), m.leaves_mesh.c_str(), m.texture.c_str());
    fclose(o);
}
