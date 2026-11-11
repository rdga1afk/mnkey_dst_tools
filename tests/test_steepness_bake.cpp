// BuildSteepnessSmoothed (heights + biomes from the install) matches the committed game/data/textures/md_ground_steepness_smoothed.png
// (tools/md_bake_ground_layers.py). Needs KENSHI_DIR and the repo as the working folder.
#include <gtest/gtest.h>
#include <monkey_dust/world/steepness_bake.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <stb_image.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

TEST(SteepnessBake, MatchesCommittedPng) {
    const char* png = "game/data/textures/md_ground_steepness_smoothed.png";
    if (md::KenshiDir().empty() || !std::filesystem::exists(png)) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    ASSERT_TRUE(BiomeRegistry::Get().LoadFromKenshi(md::KenshiDir().c_str(), (md::KenshiDir() + "/data/newland/land/textures").c_str()));
    ASSERT_TRUE(TerrainAtlas_Load("game/data/terrain/world_hmap"));
    std::vector<uint8_t> g; ASSERT_TRUE(md::BuildSteepnessSmoothed(g));
    int w, h, c; unsigned char* ref = stbi_load(png, &w, &h, &c, 1);
    ASSERT_NE(ref, nullptr); ASSERT_EQ(w, md::STEEPNESS_SIZE); ASSERT_EQ(h, md::STEEPNESS_SIZE);
    // The old png was baked from the old generated biome_table.txt; three biomes (ashlands_2, artery_2, blister_cliffs_2, load-order 4, 5, 7) have a different slope_min
    // there than in the FCS the game reads now (0.46/0.15/0.29 vs 0.02/0.05/0.18), so their zones and the zones whose corner blend reaches them are not comparable.
    bool skip[64][64] = {};
    for (int zy = 0; zy < 64; ++zy) for (int zx = 0; zx < 64; ++zx) {
        const int id = TerrainGen_ResolveBiome(zx, zy).biome_id;
        if (id == 4 || id == 5 || id == 7) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) if (zy + dy >= 0 && zy + dy < 64 && zx + dx >= 0 && zx + dx < 64) skip[zy + dy][zx + dx] = true;
    }
    size_t n = 0, d0 = 0, dbig = 0; int mx = 0; double sum = 0;
    for (int y = 0; y < md::STEEPNESS_SIZE; ++y) for (int x = 0; x < md::STEEPNESS_SIZE; ++x) {
        if (skip[y / 32][x / 32]) continue;
        const int d = std::abs((int)g[(size_t)y * md::STEEPNESS_SIZE + x] - (int)ref[(size_t)y * md::STEEPNESS_SIZE + x]);
        ++n; sum += d; mx = std::max(mx, d); d0 += d == 0; dbig += d > 2;
    }
    std::printf("[Steepness] comparable texels %zu: equal %.3f%%, off by >2: %zu, max %d, mean %.5f\n", n, 100.0 * d0 / n, dbig, mx, sum / n);
    EXPECT_GT(n, 3000000u);
    EXPECT_LT(sum / n, 0.01);
    EXPECT_LT(dbig, n / 10000);
    stbi_image_free(ref);
}
