// BuildSteepnessSmoothed (heights from the game folder's fullmap.tif + biomes from its FCS) against its hash reference (tests/data/install_hashes.tsv).
// No copy of the output is committed (#201: Kenshi-derived files are never committed); the hash is the guard.
#include "install_hash.h"
#include <monkey_dust/world/steepness_bake.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/terrain_gen.h>

TEST(SteepnessBake, OutputMatchesHashReference) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    ASSERT_TRUE(BiomeRegistry::Get().LoadFromKenshi(md::KenshiDir().c_str(), (md::KenshiDir() + "/data/newland/land/textures").c_str()));
    ASSERT_TRUE(TerrainAtlas_Load("game/data/terrain/world_hmap"));   // with KENSHI_DIR set the atlas is built from the install's fullmap.tif
    std::vector<uint8_t> g; ASSERT_TRUE(md::BuildSteepnessSmoothed(g));
    ASSERT_EQ(g.size(), size_t(md::STEEPNESS_SIZE) * md::STEEPNESS_SIZE);
    MD_EXPECT_INSTALL_HASH("steepness_smoothed", g.data(), g.size());
}
