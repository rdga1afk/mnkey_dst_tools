// DecodeDdsRgba + a 2x box filter of the game folder's human_male_body_diffuse.dds (2048 DXT5) against its hash reference (tests/data/install_hashes.tsv).
// No copy of the output is committed (#201: Kenshi-derived files are never committed); the hash is the guard.
#include "install_hash.h"
#include <monkey_dust/io/dds_decode.h>

TEST(DdsDecode, BodyTextureMatchesHashReference) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    int w, h; std::vector<uint8_t> px;
    ASSERT_TRUE(md::DecodeDdsRgba(md::KenshiDir() + "/data/character/skins/human_male/human_male_body_diffuse.dds", w, h, px));
    ASSERT_EQ(w, 2048);
    md::BoxDownsample(px, w, h, 1);
    ASSERT_EQ(px.size(), size_t(w) * h * 4);
    MD_EXPECT_INSTALL_HASH("body_texture_1024_rgba", px.data(), px.size());
}
