// BuildOverlayMask from the install equals the committed game/data/textures/md_overlay_mask.raw (made by tools/md_stitch_overlay_mask.py). Needs KENSHI_DIR and the repo as working folder.
#include <gtest/gtest.h>
#include <monkey_dust/world/overlay_mask_build.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <cstdio>
#include <cstring>
#include <filesystem>

TEST(OverlayMaskBuild, EqualsCommittedRaw) {
    const char* raw = "game/data/textures/md_overlay_mask.raw";
    if (md::KenshiDir().empty() || !std::filesystem::exists(raw)) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    std::vector<uint8_t> px; ASSERT_TRUE(md::BuildOverlayMask(md::KenshiDir(), px));
    FILE* f = fopen(raw, "rb"); ASSERT_NE(f, nullptr);
    uint32_t hd[2]; ASSERT_EQ(fread(hd, 4, 2, f), 2u); ASSERT_EQ(hd[0], 4096u);
    std::vector<uint8_t> ref(px.size()); ASSERT_EQ(fread(ref.data(), 1, ref.size(), f), ref.size()); fclose(f);
    size_t bad = 0; for (size_t i = 0; i < px.size(); ++i) bad += px[i] != ref[i];
    std::printf("[OverlayMask] differing bytes: %zu of %zu\n", bad, px.size());
    EXPECT_EQ(bad, 0u);
}
