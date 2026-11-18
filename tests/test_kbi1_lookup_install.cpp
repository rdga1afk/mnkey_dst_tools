// The KBI1 blend lookup built at run time from the game folder's blendinfo.dat (with the biome order of the FCS-built table) against its hash reference (tests/data/install_hashes.tsv).
// No copy of the output is committed (#201: Kenshi-derived files are never committed); the hash is the guard.
#include "install_hash.h"
#include <monkey_dust/world/biome_def.h>
#include <filesystem>
#include <iterator>

TEST(Kbi1LookupInstall, OutputMatchesHashReference) {
    ASSERT_FALSE(md::KenshiDir().empty()) << "needs KENSHI_DIR";
    ASSERT_TRUE(BiomeRegistry::Get().LoadFromKenshi(md::KenshiDir().c_str(), (md::KenshiDir() + "/data/newland/land/textures").c_str()));
    std::ifstream in(md::KenshiDir() + "/data/newland/land/blendinfo.dat", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int dim = 0; std::vector<uint8_t> px;
    ASSERT_TRUE(BiomeRegistry::Get().BuildKbi1Lookup(bytes, dim, px));
    ASSERT_GT(dim, 0); ASSERT_EQ(px.size(), size_t(dim) * size_t(2 * dim) * 4);
    std::string all(reinterpret_cast<const char*>(&dim), sizeof dim); all.append(reinterpret_cast<const char*>(px.data()), px.size());
    MD_EXPECT_INSTALL_HASH("kbi1_blend_lookup", all.data(), all.size());
}
