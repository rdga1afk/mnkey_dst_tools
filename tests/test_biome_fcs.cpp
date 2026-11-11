// The biome table built from the FCS BIOMES records must be the table the offline generator wrote (game/data/biome_table.txt), text for
// text, and load into an identical registry. Runs only with KENSHI_DIR set and from the repo root (needs game/data/biome_table.txt and
// tmp_/kenshi_re/terrain_textures, which are private data not in the public repos).
#include <gtest/gtest.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/ground_pack.h>
#include <monkey_dust/io/fcs_gamedata.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
const char* TEX_DIR = "tmp_/kenshi_re/terrain_textures";
const char* TABLE = "game/data/biome_table.txt";

std::string Content(const std::string& text) { // the table without comments and blank lines
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        out += line + "\n";
    }
    return out;
}

std::vector<md::fcs::Record> BiomeRecords(const std::string& dir) {
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error err;
    EXPECT_TRUE(inst.LoadOrderFor(order, err)) << err.Message();
    std::vector<md::fcs::Record> out;
    for (const auto& f : order.files) {
        md::fcs::File file;
        EXPECT_TRUE(md::fcs::ReadFile(f.path, file, err)) << f.path << ": " << err.Message();
        for (auto& r : file.records) if (r.kind == md::fcs::ITEM_BIOMES) out.push_back(std::move(r));
    }
    return out;
}
} // namespace

TEST(BiomeFcs, TableMatchesGeneratedFile) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    if (!std::filesystem::exists(TABLE) || !std::filesystem::is_directory(TEX_DIR)) GTEST_SKIP() << "run from the repo root with the private data";
    std::ifstream f(TABLE, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    std::string expected = Content(ss.str());
    auto records = BiomeRecords(dir);
    ASSERT_GE(records.size(), 136u); // 136 in gamedata.base / Newwworld.mod / rebirth.mod (the generator's input) + any in other files (Dialogue.mod: 1)
    std::string built = Content(BiomeRegistry::BuildTableFromFcs(records, TEX_DIR));
    if (built != expected) {
        std::istringstream a(expected), b(built);
        std::string la, lb; int n = 0;
        while (n < 3 && (std::getline(a, la), std::getline(b, lb), a || b)) { if (la != lb) { ADD_FAILURE() << "first difference:\nfile : " << la << "\nbuilt: " << lb; ++n; } }
    }
    EXPECT_TRUE(built == expected);
}

TEST(BiomeFcs, RegistryIdentical) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    if (!std::filesystem::exists(TABLE) || !std::filesystem::is_directory(TEX_DIR)) GTEST_SKIP() << "run from the repo root with the private data";
    BiomeRegistry& reg = BiomeRegistry::Get();
    ASSERT_TRUE(reg.LoadFromFile(TABLE));
    int n = reg.BiomeCount(), nt = reg.GroundTexCount();
    std::vector<BiomeDef> defs;
    for (int i = 0; i < n; ++i) defs.push_back(reg.ForIndex(i));
    std::vector<std::string> tex, nml;
    for (int i = 0; i < nt; ++i) { tex.push_back(reg.GroundTexPath(i)); nml.push_back(reg.GroundNmlPath(i)); }
    ASSERT_TRUE(reg.LoadFromKenshi(dir, TEX_DIR));
    ASSERT_EQ(reg.BiomeCount(), n);
    ASSERT_EQ(reg.GroundTexCount(), nt);
    for (int i = 0; i < n; ++i) EXPECT_EQ(memcmp(&defs[i], &reg.ForIndex(i), sizeof(BiomeDef)), 0) << "biome " << i;
    for (int i = 0; i < nt; ++i) { EXPECT_EQ(tex[i], reg.GroundTexPath(i)); EXPECT_EQ(nml[i], reg.GroundNmlPath(i)); }
    printf("[BiomeFcs] %d biomes, %d textures identical\n", n, nt);
}

// Which packer inputs the merged BIOMES records leave out (the original reads 0 for those) and that the pack follows the record.
TEST(BiomeFcs, GroundPackFollowsRecords) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error err;
    ASSERT_TRUE(inst.LoadOrderFor(order, err)) << err.Message();
    md::fcs::GameDataContainer gd;
    ASSERT_TRUE(gd.Load(order, err)) << err.Message();   // merged, like the original's records
    std::vector<md::fcs::Record> records;
    for (const md::fcs::GameData* g : gd.OfType(md::fcs::ITEM_BIOMES)) {
        md::fcs::Record r; r.kind = g->kind; r.name = g->name; r.string_id = g->string_id;
        for (const auto& kv : g->floats) r.floats.emplace_back(kv.first, kv.second);
        for (const auto& kv : g->ints) r.ints.emplace_back(kv.first, kv.second);
        records.push_back(std::move(r));
    }
    ASSERT_FALSE(records.empty());
    const char* keys[] = { "slope min 1", "slope min 2", "slope min 3", "slope min grass", "slope max grass", "slope fade grass", "absorbance 0", "absorbance road", "brightness fix", "fade distance", "ground colour", "overlay mult road", "tiling X road" };
    int missing[sizeof(keys) / sizeof(keys[0])] = {};
    for (const auto& r : records) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
            bool has = false;
            for (const auto& kv : r.floats) if (kv.first == keys[k]) has = true;
            for (const auto& kv : r.ints) if (kv.first == keys[k]) has = true;
            if (!has) ++missing[k];
        }
        auto fl = [&](const char* key) { for (const auto& kv : r.floats) if (kv.first == key) return kv.second; for (const auto& kv : r.ints) if (kv.first == key) return (float)kv.second; return 0.f; };
        md::GroundPack p = md::PackGroundBiome(r);
        EXPECT_FLOAT_EQ(p.slopeMin[3], fl("slope min grass") * 0.01f) << r.name;
        EXPECT_FLOAT_EQ(p.slopeBlend[2], fl("slope fade 3") * 0.01f) << r.name;
        EXPECT_FLOAT_EQ(p.scalesC[3], fl("tiling Y road")) << r.name;
        EXPECT_FLOAT_EQ(p.absorbance[5], fl("absorbance road")) << r.name;
    }
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) printf("[ground_pack] %zu of %zu records without \"%s\"\n", (size_t)missing[k], records.size(), keys[k]);
}

// Memory of per-biome 6-layer arrays (ground design A): the install's real texture sizes, as loaded (DDS header: size, mips, block format).
namespace {
struct DdsInfo { uint32_t w = 0, h = 0, mips = 1; uint32_t bytes = 0; char fmt[8] = {}; };
bool ReadDds(const std::string& p, DdsInfo& d) {
    std::ifstream f(p, std::ios::binary); unsigned char h[148] = {};
    if (!f.read((char*)h, 148) && f.gcount() < 128) return false;
    if (memcmp(h, "DDS ", 4) != 0) return false;
    auto u32 = [&](int o) { uint32_t v; memcpy(&v, h + o, 4); return v; };
    d.h = u32(12); d.w = u32(16); d.mips = u32(28) ? u32(28) : 1; memcpy(d.fmt, h + 84, 4);
    uint32_t block = 0, bpp = 0;
    if (!memcmp(d.fmt, "DXT1", 4) || !memcmp(d.fmt, "ATI1", 4) || !memcmp(d.fmt, "BC4U", 4)) block = 8;
    else if (!memcmp(d.fmt, "DXT3", 4) || !memcmp(d.fmt, "DXT5", 4) || !memcmp(d.fmt, "ATI2", 4)) block = 16;
    else if (!memcmp(d.fmt, "DX10", 4)) block = 16;   // not expected; counted as BC7-size
    else bpp = u32(88);
    uint64_t total = 0; uint32_t w = d.w, hh = d.h;
    for (uint32_t m = 0; m < d.mips; ++m) { total += block ? (uint64_t)((w + 3) / 4) * ((hh + 3) / 4) * block : (uint64_t)w * hh * (bpp / 8); w = w > 1 ? w / 2 : 1; hh = hh > 1 ? hh / 2 : 1; }
    d.bytes = (uint32_t)total; return true;
}
}

TEST(BiomeFcs, LayerArrayMemory) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    ASSERT_TRUE(BiomeRegistry::Get().LoadFromKenshi(dir, TEX_DIR));
    const BiomeRegistry& R = BiomeRegistry::Get();
    std::map<std::string, DdsInfo> seen; uint64_t all_unique = 0, per_biome_total = 0, worst_biome = 0; int missing = 0;
    auto bytes_of = [&](const char* path) -> uint64_t { if (!path || !*path) { ++missing; return 0; } auto it = seen.find(path); if (it == seen.end()) { DdsInfo d; if (!ReadDds(path, d)) { ++missing; return 0; } it = seen.emplace(path, d).first; all_unique += d.bytes; } return it->second.bytes; };
    for (int b = 0; b < R.BiomeCount(); ++b) {
        const BiomeDef& d = R.ForIndex(b); uint64_t sum = 0;
        for (int t : { d.tex_base, d.tex_slope, d.tex_cliff, d.tex_grass, d.tex_dirt, d.tex_road }) sum += bytes_of(R.GroundTexPath(t)) + bytes_of(R.GroundNmlPath(t));
        per_biome_total += sum; if (sum > worst_biome) worst_biome = sum;
    }
    std::map<std::string, int> sizes; for (auto& kv : seen) sizes[std::to_string(kv.second.w) + "x" + std::to_string(kv.second.h) + " " + std::string(kv.second.fmt, 4) + " mips " + std::to_string(kv.second.mips)]++;
    printf("[layer_memory] %d biomes, %zu distinct texture files (%d unreadable/empty), distinct files %.1f MB; per-biome arrays for ALL biomes %.1f MB (largest single biome %.1f MB, average %.1f MB)\n",
           R.BiomeCount(), seen.size(), missing, all_unique / 1e6, per_biome_total / 1e6, worst_biome / 1e6, per_biome_total / 1e6 / (R.BiomeCount() ? R.BiomeCount() : 1));
    for (auto& kv : sizes) printf("[layer_memory]   %4d files %s\n", kv.second, kv.first.c_str());
}

// Distance tiers of the ground arrays: the tiling (texture repeats per 500 m) of every layer of every biome decides how many base texels a pixel covers.
TEST(BiomeFcs, GroundTilingRange) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    md::fcs::Install inst(dir);
    md::fcs::LoadOrder order; md::fcs::Error err;
    ASSERT_TRUE(inst.LoadOrderFor(order, err)) << err.Message();
    md::fcs::GameDataContainer gd;
    ASSERT_TRUE(gd.Load(order, err)) << err.Message();
    std::vector<float> all; float global_min = 1e9f; std::string who;
    for (const md::fcs::GameData* g : gd.OfType(md::fcs::ITEM_BIOMES)) {
        md::fcs::Record r; r.kind = g->kind; r.name = g->name;
        for (const auto& kv : g->floats) r.floats.emplace_back(kv.first, kv.second);
        for (const auto& kv : g->ints) r.ints.emplace_back(kv.first, kv.second);
        const md::GroundPack p = md::PackGroundBiome(r);
        for (const float* a : { p.scalesA, p.scalesB, p.scalesC }) for (int i = 0; i < 4; ++i) if (a[i] > 0.f) { all.push_back(a[i]); if (a[i] < global_min) { global_min = a[i]; who = r.name; } }
    }
    ASSERT_FALSE(all.empty());
    std::sort(all.begin(), all.end());
    printf("[tiling] %zu layer tilings over %zu biomes: min %.3f (%s), p5 %.3f, median %.3f, max %.3f\n", all.size(), (size_t)gd.OfType(md::fcs::ITEM_BIOMES).size(), all.front(), who.c_str(), all[all.size() / 20], all[all.size() / 2], all.back());
}
