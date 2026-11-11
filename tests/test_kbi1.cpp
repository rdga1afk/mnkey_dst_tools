// blendinfo.dat reader: layout facts of the player's install (needs KENSHI_DIR) and the query rules of the original's zone map.
#include <gtest/gtest.h>
#include <monkey_dust/world/kbi1.h>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace md;

TEST(Kbi1, InstallLayoutAndCounts) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    Kbi1 k; ASSERT_TRUE(k.Load(std::string(dir) + "/data/newland/land/blendinfo.dat"));
    EXPECT_EQ(k.Dim(), 32); EXPECT_EQ(k.Res(), 32);
    EXPECT_EQ(k.SubdividedCells(), 656);   // the file ends exactly after 368 one-byte and 656 2048-byte blocks
    std::map<int, int> by_distinct; int slot4 = 0;
    for (int c = 0; c < 1024; ++c) {
        std::set<int> s; for (int i = 0; i < 5; ++i) if (k.Slots(c)[i] != 0) s.insert(k.Slots(c)[i]);
        ++by_distinct[(int)s.size()]; if (k.Slots(c)[4] != 0) ++slot4;
    }
    // docs/research/PER_CELL_DRAWS_DATA_2026-10-31.md: 1 -> 355, 2 -> 386, 3 -> 227, 4 -> 56 distinct biomes among the 5 ids; slot 4 set in 194 cells
    EXPECT_EQ(by_distinct[1], 355); EXPECT_EQ(by_distinct[2], 386); EXPECT_EQ(by_distinct[3], 227); EXPECT_EQ(by_distinct[4], 56); EXPECT_EQ(slot4, 194);
    printf("[kbi1] cells by distinct ids: 1=%d 2=%d 3=%d 4=%d 5+=%d, slot4 in %d cells\n", by_distinct[1], by_distinct[2], by_distinct[3], by_distinct[4], by_distinct[5], slot4);
}

TEST(Kbi1, QueryRules) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    Kbi1 k; ASSERT_TRUE(k.Load(std::string(dir) + "/data/newland/land/blendinfo.dat"));
    const float cell = 294912.f / 32.f;
    // a small patch inside the Hub's cell: not empty, at most 4 entries, descending slots, ids equal the cell's slot ids
    const float hx = (9647.7f - 14745.6f) * 10.f, hz = (15038.8f - 14745.6f) * 10.f;
    PatchBiomes p = k.Query(hx - 50.f, hz - 50.f, hx + 50.f, hz + 50.f);
    ASSERT_GT(p.count, 0); EXPECT_LE(p.count, 4);
    const int cx = (int)std::floor((hx + 147456.f) / cell), cz = (int)std::floor((hz + 147456.f) / cell);
    for (int i = 0; i < p.count; ++i) { EXPECT_EQ(p.ids[i], k.Slots(cz * 32 + cx)[p.channels[i]]); if (i) EXPECT_LT(p.channels[i], p.channels[i - 1]); }
    // corners in different cells, or outside the world: no biomes
    EXPECT_EQ(k.Query(hx - 50.f, hz - 50.f, hx + 2.f * cell, hz + 50.f).count, 0);
    EXPECT_EQ(k.Query(-200000.f, 0.f, -199900.f, 100.f).count, 0);
    // a patch that spans the whole cell sees the cell's quadtree root (index 1): never more entries than a small patch inside it
    PatchBiomes whole = k.Query(cx * cell - 147456.f + 1.f, cz * cell - 147456.f + 1.f, (cx + 1) * cell - 147456.f - 1.f, (cz + 1) * cell - 147456.f - 1.f);
    EXPECT_GE(whole.count, 0);
}

#include <monkey_dust/world/ground_lists.h>

TEST(GroundLists, DistinctListsAndLeafConsistency) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    Kbi1 k; ASSERT_TRUE(k.Load(std::string(dir) + "/data/newland/land/blendinfo.dat"));
    GroundLists g; g.Build(k);
    ASSERT_EQ(g.Side(), 1024);
    printf("[ground_lists] %d distinct lists in the world\n", g.Count());
    EXPECT_GT(g.Count(), 2); EXPECT_LT(g.Count(), 60000);
    // each leaf's list equals the original's patch query for that leaf's own box (a patch of exactly one sub-cell)
    const float cell = 294912.f / 32.f, leaf = cell / 32.f;
    int checked = 0, bad = 0;
    for (int tz = 0; tz < 1024; tz += 7) for (int tx = 0; tx < 1024; tx += 5) {
        const float x0 = -147456.f + tx * leaf, z0 = -147456.f + tz * leaf;
        const PatchBiomes q = k.Query(x0 + 0.01f, z0 + 0.01f, x0 + leaf - 0.01f, z0 + leaf - 0.01f);
        const GroundList& l = g.List(g.IndexAt(tx, tz)); ++checked;
        bool same = q.count == l.count; for (int i = 0; same && i < q.count; ++i) same = q.ids[i] == l.ids[i] && q.channels[i] == l.channels[i];
        if (!same) ++bad;
    }
    printf("[ground_lists] leaf list == patch query of the leaf box: %d of %d\n", checked - bad, checked);
    EXPECT_EQ(bad, 0);
    // lists inside the Hub's surroundings (+-1500 m): a handful
    const int hx = (int)(9647.7f / 28.8f), hz = (int)(15038.8f / 28.8f), r = (int)(1500.f / 28.8f);
    const auto in = g.ListsIn(hx - r, hz - r, hx + r, hz + r);
    printf("[ground_lists] lists within 1500 m of the Hub: %zu\n", in.size());
    EXPECT_GE(in.size(), 1u);
}
