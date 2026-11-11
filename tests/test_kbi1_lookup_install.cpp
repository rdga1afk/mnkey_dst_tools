// The KBI1 blend lookup built at run time from the install's blendinfo.dat equals the committed game/data/textures/md_kbi1_blend_lookup.png (private/md_bake_kbi1_lookup.py), pixel for pixel,
// with the biome order of the FCS-built table. Needs KENSHI_DIR and the repo as the working folder.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/biome_def.h>
#include <stb_image.h>
#include <filesystem>
#include <fstream>
#include <iterator>

TEST(Kbi1LookupInstall, EqualsCommittedPng) {
    const char* png = "game/data/textures/md_kbi1_blend_lookup.png";
    if (md::KenshiDir().empty() || !std::filesystem::exists(png)) GTEST_SKIP() << "needs KENSHI_DIR and the repo as working folder";
    ASSERT_TRUE(BiomeRegistry::Get().LoadFromKenshi(md::KenshiDir().c_str(), (md::KenshiDir() + "/data/newland/land/textures").c_str()));
    std::ifstream in(md::KenshiDir() + "/data/newland/land/blendinfo.dat", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int dim = 0; std::vector<uint8_t> px;
    ASSERT_TRUE(BiomeRegistry::Get().BuildKbi1Lookup(bytes, dim, px));
    int w = 0, h = 0, ch = 0;
    unsigned char* ref = stbi_load(png, &w, &h, &ch, 4);
    ASSERT_NE(ref, nullptr);
    ASSERT_EQ(w, dim); ASSERT_EQ(h, 2 * dim);
    size_t bad = 0;
    for (size_t i = 0; i < px.size(); ++i) if (px[i] != ref[i]) ++bad;
    stbi_image_free(ref);
    EXPECT_EQ(bad, 0u);
}
