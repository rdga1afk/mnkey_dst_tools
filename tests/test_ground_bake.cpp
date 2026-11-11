// BuildGroundBaked (port of tools/md_bake_ground_layers.py) against the python run on the same inputs at a small size.
// MD_GB_DUMP=<dir>: writes the registry as a biome_table.txt, the zone biome ids and the overlay mask png for the python (tools/md_bake_ground_layers.py, see docs/research/R3E_GROUND_BAKE.md).
// MD_GB_REF=<png>: the python output (side from the png) to compare with. Needs KENSHI_DIR and the repo as working folder.
#include <gtest/gtest.h>
#include <monkey_dust/world/ground_bake.h>
#include <monkey_dust/world/overlay_mask_build.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <stb_image.h>
#include <cstdio>
#include <cstdlib>

static void LoadAll() {
    ASSERT_TRUE(BiomeRegistry::Get().LoadFromKenshi(md::KenshiDir().c_str(), (md::KenshiDir() + "/data/newland/land/textures").c_str()));
    ASSERT_TRUE(TerrainAtlas_Load("game/data/terrain/world_hmap"));
}

TEST(GroundBake, DumpInputsForPython) {
    const char* dir = getenv("MD_GB_DUMP");
    if (md::KenshiDir().empty() || !dir) GTEST_SKIP() << "set MD_GB_DUMP";
    LoadAll(); if (HasFatalFailure()) return;
    std::vector<uint8_t> bytes; ASSERT_TRUE(md::LoadOverlayMask(md::KenshiDir(), bytes));
    std::string d = dir;
    FILE* f = fopen((d + "/biome_table.txt").c_str(), "w");
    BiomeRegistry& r = BiomeRegistry::Get();
    for (int i = 0; i < r.GroundTexCount(); ++i) fprintf(f, "tex %d|%s|x\n", i, r.GroundTexPath(i));
    for (int i = 0; i < r.BiomeCount(); ++i) {
        const BiomeDef& b = r.ForIndex(i);
        fprintf(f, "biome b%d %d %d %d %d %d %d 0 0 0 0 0 0 %d 0 0 %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", i,
                b.tex_base, b.tex_slope, b.tex_cliff, b.tex_grass, b.tex_dirt, b.tex_road, i,
                b.tile_base_x, b.tile_base_y, b.tile_slope_x, b.tile_slope_y, b.cliff_tiling_x, b.cliff_tiling_y, b.tile_dirt_x, b.tile_dirt_y, b.tile_grass_x, b.tile_grass_y, b.tile_road_x, b.tile_road_y,
                b.slope_min, b.slope_max, b.slope_fade, b.cliff_min, b.cliff_max, b.cliff_fade);
    }
    fclose(f);
    f = fopen((d + "/zones.txt").c_str(), "w");
    std::vector<uint8_t> dummy; { std::vector<uint8_t> g; md::BuildGroundBaked(md::KenshiDir(), 8, g); }   // loads the biome map
    for (int zy = 0; zy < 64; ++zy) for (int zx = 0; zx < 64; ++zx) fprintf(f, "%d\n", TerrainGen_ResolveBiome(zx, zy).biome_id);
    fclose(f);
    f = fopen((d + "/overlay.rgba").c_str(), "wb"); fwrite(bytes.data(), 1, bytes.size(), f); fclose(f);
}

TEST(GroundBake, MatchesPython) {
    const char* ref = getenv("MD_GB_REF");
    if (md::KenshiDir().empty() || !ref) GTEST_SKIP() << "set MD_GB_REF";
    LoadAll(); if (HasFatalFailure()) return;
    int w, h, c; unsigned char* py = stbi_load(ref, &w, &h, &c, 3); ASSERT_NE(py, nullptr); ASSERT_EQ(w, h);
    std::vector<uint8_t> mine; ASSERT_TRUE(md::BuildGroundBaked(md::KenshiDir(), w, mine));
    size_t n = (size_t)w * h * 3, eq = 0, big = 0; double sum = 0; int mx = 0;
    for (size_t i = 0; i < n; ++i) { const int d = std::abs((int)mine[i] - (int)py[i]); sum += d; mx = std::max(mx, d); eq += d == 0; big += d > 2; }
    std::printf("[GroundBake] %dx%d: equal %.2f%%, off by >2: %.4f%%, max %d, mean %.4f\n", w, h, 100.0 * eq / n, 100.0 * big / n, mx, sum / n);
    // what remains: the python decodes DXT through ImageMagick and sums in float32 cumsums, the port decodes itself and sums in double
    EXPECT_LT(sum / n, 0.5);
    EXPECT_LT((double)big / n, 0.001);
    stbi_image_free(py);
}

TEST(GroundBake, Bench) {
    const char* n = getenv("MD_GB_BENCH");
    if (md::KenshiDir().empty() || !n) GTEST_SKIP() << "set MD_GB_BENCH=<side>";
    LoadAll(); if (HasFatalFailure()) return;
    std::vector<uint8_t> px; ASSERT_TRUE(md::BuildGroundBaked(md::KenshiDir(), atoi(n), px));
}
