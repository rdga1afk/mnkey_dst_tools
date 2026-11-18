// BuildTerrainColourDds from the game folder's tiles: the DDS header, then the hash reference of the whole file (tests/data/install_hashes.tsv).
// No copy of the output is committed (#201: Kenshi-derived files are never committed); the hash is the guard.
// The C++ code has one pipeline (no second one to compare with).
#include "install_hash.h"
#include <monkey_dust/world/terrain_colour_dds.h>

TEST(TerrainColourDds, OutputMatchesHashReference) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    std::vector<uint8_t> dds; ASSERT_TRUE(md::BuildTerrainColourDds(md::KenshiDir(), dds));
    ASSERT_GT(dds.size(), size_t(128));
    EXPECT_EQ(std::memcmp(dds.data(), "DDS ", 4), 0);
    MD_EXPECT_INSTALL_HASH("terrain_colour_dds", dds.data(), dds.size());
}
