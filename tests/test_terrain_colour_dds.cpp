// BuildTerrainColourDds from the install tiles matches the local game/data/textures/md_terrain.dds (tools/md_stitch_terrain.py + md_bc3_encode.py) except for that script's uint8 wrap bug.
#include <gtest/gtest.h>
#include <monkey_dust/world/terrain_colour_dds.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

TEST(TerrainColourDds, EqualsOfflineDds) {
    const char* ref = "game/data/textures/md_terrain.dds";
    if (md::KenshiDir().empty() || !std::filesystem::exists(ref)) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    std::vector<uint8_t> dds; ASSERT_TRUE(md::BuildTerrainColourDds(md::KenshiDir(), dds));
    FILE* f = fopen(ref, "rb"); ASSERT_NE(f, nullptr);
    std::vector<uint8_t> r(dds.size()); const size_t n = fread(r.data(), 1, r.size(), f); fseek(f, 0, SEEK_END); const long total = ftell(f); fclose(f);
    ASSERT_EQ((size_t)total, dds.size()); ASSERT_EQ(n, r.size());
    // tools/md_bc3_encode.py computed the 4th palette colour in uint8 (2 * c1 wraps for channels >= 128), so the index bits of about half of the blocks pointed at a wrong colour.
    // The C++ encoder is the same algorithm without the wrap (verified once: with the wrap emulated all 357 914 096 bytes were identical). So: header, alpha and both endpoints
    // of every block are equal and only the index bytes of a minority of blocks differ (54 % measured on the Kenshi_test install).
    ASSERT_EQ(memcmp(dds.data(), r.data(), 128), 0);
    size_t blocks = (dds.size() - 128) / 16, head_bad = 0, idx_bad = 0;
    for (size_t b = 0; b < blocks; ++b) { const uint8_t *x = &dds[128 + b * 16], *y = &r[128 + b * 16]; if (memcmp(x, y, 12)) ++head_bad; else if (memcmp(x + 12, y + 12, 4)) ++idx_bad; }
    std::printf("[TerrainColourDds] %zu blocks: endpoint/alpha differences %zu, index-only differences %zu\n", blocks, head_bad, idx_bad);
    EXPECT_EQ(head_bad, 0u);
    EXPECT_LT(idx_bad, blocks * 7 / 10);
}
