// Uncompressed greyscale TIFF reader (engine/src/io/tiff.cpp): synthetic files, refusals, and -- only with KENSHI_DIR set and from the repo
// root -- every sample of Kenshi's fullmap.tif[::2] against the world heightmap the engine uses today (game/data/terrain/world_hmap.r16).
#include <gtest/gtest.h>
#include <monkey_dust/io/tiff.h>
#include <monkey_dust/world/terrain_gen.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); }
void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xff); }

struct Tag { uint16_t tag, typ; uint32_t count, val; };

// A w x h 16-bit image whose pixel (x, y) holds 100*y + x, in `strips` strips; `compression` is written into the tag.
std::string WriteTiff(const char* name, uint32_t w, uint32_t h, uint32_t bits, uint32_t compression, uint32_t rows_per_strip, bool offsets_array) {
    std::vector<uint8_t> b = {'I', 'I', '*', 0, 0, 0, 0, 0};
    const uint32_t bpp = bits / 8;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t v = 100 * y + x;
            if (bpp == 2) Put16(b, (uint16_t)v); else b.push_back((uint8_t)v);
        }
    const uint32_t n_strips = (h + rows_per_strip - 1) / rows_per_strip;
    uint32_t arr_at = 0;
    if (offsets_array) { // LONG array right after the pixels
        arr_at = (uint32_t)b.size();
        for (uint32_t s = 0; s < n_strips; ++s) Put32(b, 8 + s * rows_per_strip * w * bpp);
    }
    const uint32_t ifd_at = (uint32_t)b.size();
    std::vector<Tag> tags = {{256, 3, 1, w}, {257, 3, 1, h}, {258, 3, 1, bits}, {259, 3, 1, compression},
                             {273, 4, offsets_array ? n_strips : 1, offsets_array ? arr_at : 8}, {277, 3, 1, 1}, {278, 3, 1, rows_per_strip}};
    Put16(b, (uint16_t)tags.size());
    for (const Tag& t : tags) { Put16(b, t.tag); Put16(b, t.typ); Put32(b, t.count); Put32(b, t.val); }
    Put32(b, 0);
    for (int i = 0; i < 4; ++i) b[4 + i] = (ifd_at >> (8 * i)) & 0xff;
    const std::string path = (std::filesystem::temp_directory_path() / (std::string("md_tiff_") + name + ".tif")).string();
    std::ofstream(path, std::ios::binary).write((const char*)b.data(), (std::streamsize)b.size());
    return path;
}

} // namespace

TEST(Tiff, WindowsAndSteps16Bit) {
    const std::string p = WriteTiff("one_strip", 4, 3, 16, 1, 3, false);
    md::tiff::Image img; std::string err;
    ASSERT_TRUE(img.Open(p.c_str(), err)) << err;
    EXPECT_EQ(img.Width(), 4u); EXPECT_EQ(img.Height(), 3u); EXPECT_EQ(img.Bits(), 16u);
    std::vector<uint16_t> out;
    ASSERT_TRUE(img.Window(1, 1, 2, 2, 1, out, err)) << err;
    EXPECT_EQ(out, (std::vector<uint16_t>{101, 102, 201, 202}));
    ASSERT_TRUE(img.Window(0, 0, 2, 2, 2, out, err)) << err;
    EXPECT_EQ(out, (std::vector<uint16_t>{0, 2, 200, 202}));
    // a window running past the edge is clamped to the last pixel, like Okran's
    ASSERT_TRUE(img.Window(2, 1, 3, 1, 2, out, err)) << err;
    EXPECT_EQ(out, (std::vector<uint16_t>{102, 103, 103}));
    std::filesystem::remove(p);
}

TEST(Tiff, SeveralStrips) {
    const std::string p = WriteTiff("strips", 5, 7, 16, 1, 3, true); // strips of 3, 3, 1 rows
    md::tiff::Image img; std::string err;
    ASSERT_TRUE(img.Open(p.c_str(), err)) << err;
    std::vector<uint16_t> out;
    ASSERT_TRUE(img.Window(0, 0, 5, 7, 1, out, err)) << err;
    for (uint32_t y = 0; y < 7; ++y)
        for (uint32_t x = 0; x < 5; ++x) EXPECT_EQ(out[y * 5 + x], 100 * y + x) << x << "," << y;
    std::filesystem::remove(p);
}

TEST(Tiff, EightBitIsWidenedBy257) {
    const std::string p = WriteTiff("eight", 4, 2, 8, 1, 2, false);
    md::tiff::Image img; std::string err;
    ASSERT_TRUE(img.Open(p.c_str(), err)) << err;
    std::vector<uint16_t> out;
    ASSERT_TRUE(img.Window(0, 1, 4, 1, 1, out, err)) << err;
    EXPECT_EQ(out, (std::vector<uint16_t>{100 * 257, 101 * 257, 102 * 257, 103 * 257}));
    std::filesystem::remove(p);
}

TEST(Tiff, RefusesWhatItCannotRead) {
    md::tiff::Image img; std::string err;
    const std::string packed = WriteTiff("packbits", 4, 3, 16, 32773, 3, false);
    EXPECT_FALSE(img.Open(packed.c_str(), err));
    EXPECT_NE(err.find("compression 32773"), std::string::npos) << err;
    const std::string rgb_bits = WriteTiff("bits32", 4, 3, 32, 1, 3, false);
    EXPECT_FALSE(img.Open(rgb_bits.c_str(), err));
    const std::string junk = (std::filesystem::temp_directory_path() / "md_tiff_junk.tif").string();
    std::ofstream(junk, std::ios::binary) << "MM\0*not a little-endian tiff";
    EXPECT_FALSE(img.Open(junk.c_str(), err));
    EXPECT_EQ(err, "not a little-endian TIFF");
    EXPECT_FALSE(img.Open("/nonexistent/x.tif", err));
    for (auto* f : {&packed, &rgb_bits, &junk}) std::filesystem::remove(*f);
}

// fullmap.tif[::2], zone by zone (129x129 samples each, stride 2 in the 16385^2 image) == world_hmap.r16 (8256^2, zone-tiled, 8-byte header)
TEST(TiffData, FullmapMatchesWorldHmapR16) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    const std::string tif = std::string(dir) + "/data/newland/land/fullmap.tif";
    const char* r16_path = "game/data/terrain/world_hmap.r16";
    if (!std::filesystem::exists(tif) || !std::filesystem::exists(r16_path)) GTEST_SKIP() << "need " << tif << " and " << r16_path;

    md::tiff::Image img; std::string err;
    ASSERT_TRUE(img.Open(tif.c_str(), err)) << err;
    ASSERT_EQ(img.Width(), 16385u); ASSERT_EQ(img.Height(), 16385u); ASSERT_EQ(img.Bits(), 16u);

    constexpr int ZONES = 64, VERTS = 129, SIDE = ZONES * VERTS;
    std::ifstream r16(r16_path, std::ios::binary);
    uint32_t magic = 0, zones = 0;
    r16.read((char*)&magic, 4); r16.read((char*)&zones, 4);
    ASSERT_EQ(magic, 0x3631524Du); ASSERT_EQ(zones, 64u);
    std::vector<uint16_t> ref((size_t)SIDE * SIDE);
    r16.read((char*)ref.data(), (std::streamsize)ref.size() * 2);
    ASSERT_TRUE(r16.good());

    size_t mismatches = 0, total = 0;
    std::vector<uint16_t> blk;
    for (int zy = 0; zy < ZONES; ++zy)
        for (int zx = 0; zx < ZONES; ++zx) {
            ASSERT_TRUE(img.Window((uint32_t)zx * 256, (uint32_t)zy * 256, VERTS, VERTS, 2, blk, err)) << err;
            for (int row = 0; row < VERTS; ++row)
                for (int col = 0; col < VERTS; ++col) {
                    ++total;
                    if (blk[(size_t)row * VERTS + col] != ref[(size_t)(zy * VERTS + row) * SIDE + zx * VERTS + col]) ++mismatches;
                }
        }
    std::printf("[TiffData] %zu samples compared, %zu mismatches\n", total, mismatches);
    EXPECT_EQ(total, (size_t)SIDE * SIDE);
    EXPECT_EQ(mismatches, 0u);
}

// The atlas the engine builds from the Kenshi install (KENSHI_DIR set) must be the atlas it builds from the offline world_hmap.r16
// (KENSHI_DIR pointing nowhere -> fallback), height for height.
TEST(TiffData, AtlasFromKenshiEqualsAtlasFromR16) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    const std::string kenshi = dir;
    if (!std::filesystem::exists(kenshi + "/data/newland/land/fullmap.tif") || !std::filesystem::exists("game/data/terrain/world_hmap.r16"))
        GTEST_SKIP() << "need fullmap.tif and game/data/terrain/world_hmap.r16";
    constexpr int ZONES = 64, VERTS = 129;
    std::vector<float> a((size_t)ZONES * ZONES * VERTS * VERTS), b(a.size());
    auto snapshot = [&](std::vector<float>& dst) {
        size_t i = 0;
        for (int zy = 0; zy < ZONES; ++zy)
            for (int zx = 0; zx < ZONES; ++zx)
                for (int row = 0; row < VERTS; ++row)
                    for (int col = 0; col < VERTS; ++col) dst[i++] = TerrainAtlas_GetHeight(zx, zy, col, row);
    };
    setenv("KENSHI_DIR", kenshi.c_str(), 1);
    ASSERT_TRUE(TerrainAtlas_Load("game/data/terrain/world_hmap"));
    snapshot(a);
    setenv("KENSHI_DIR", "/nonexistent", 1);
    ASSERT_TRUE(TerrainAtlas_Load("game/data/terrain/world_hmap"));
    snapshot(b);
    setenv("KENSHI_DIR", kenshi.c_str(), 1);
    size_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) if (memcmp(&a[i], &b[i], 4) != 0) ++diff;
    std::printf("[TiffData] atlas: %zu heights compared, %zu different\n", a.size(), diff);
    EXPECT_EQ(diff, 0u);
}
