// DecodeDdsRgba + a 2x box filter reproduce game/data/textures/md_human_body.png (the 1024 copy of the install's human_male_body_diffuse.dds, 2048 DXT5). Needs KENSHI_DIR and the repo as the working folder.
#include <gtest/gtest.h>
#include <monkey_dust/io/dds_decode.h>
#include <monkey_dust/io/kenshi_dir.h>
#include "stb_image.h"
#include <cmath>
#include <filesystem>

TEST(DdsDecode, BodyTextureMatchesPngCopy) {
    const char* png = "game/data/textures/md_human_body.png";
    if (md::KenshiDir().empty() || !std::filesystem::exists(png)) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    int w, h; std::vector<uint8_t> px;
    ASSERT_TRUE(md::DecodeDdsRgba(md::KenshiDir() + "/data/character/skins/human_male/human_male_body_diffuse.dds", w, h, px));
    ASSERT_EQ(w, 2048);
    md::BoxDownsample(px, w, h, 1);
    int pw, ph, pc; unsigned char* ref = stbi_load(png, &pw, &ph, &pc, 4);
    ASSERT_NE(ref, nullptr); ASSERT_EQ(pw, w); ASSERT_EQ(ph, h);
    double sum = 0; int mx = 0;
    for (size_t i = 0; i < (size_t)w * h * 4; ++i) { const int d = std::abs((int)px[i] - (int)ref[i]); sum += d; if (d > mx) mx = d; }
    std::printf("[DdsDecode] mean abs diff %.3f / 255, max %d\n", sum / ((double)w * h * 4), mx);
    EXPECT_LT(sum / ((double)w * h * 4), 1.5);
    stbi_image_free(ref);
}
