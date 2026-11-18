// BuildOverlayMask from the game folder against its hash reference (tests/data/install_hashes.tsv).
// No copy of the output is committed (#201: Kenshi-derived files are never committed); the hash is the guard.
#include "install_hash.h"
#include <monkey_dust/world/overlay_mask_build.h>

TEST(OverlayMaskBuild, OutputMatchesHashReference) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    std::vector<uint8_t> px; ASSERT_TRUE(md::BuildOverlayMask(md::KenshiDir(), px));
    ASSERT_EQ(px.size(), size_t(4096) * 4096 * (px.size() / (size_t(4096) * 4096)));   // a whole number of bytes per texel of the 4096 square
    MD_EXPECT_INSTALL_HASH("overlay_mask", px.data(), px.size());
}
