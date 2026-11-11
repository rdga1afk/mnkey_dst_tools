// The map files the engine now reads straight from the Kenshi install (data/newland/land) must have exactly the pixels of the offline
// copies in game/data/textures that it used before. Runs only with KENSHI_DIR set and from the repo root; a pair whose offline copy was removed from the repo (#201) is skipped.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include "stb_image.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

struct Pair { const char* kenshi_name; const char* offline; int channels; };

TEST(KenshiLand, MapsHaveTheSamePixelsAsOfflineCopies) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    // channels = what the engine asks stb for (biomemap is loaded as RGB, the others as stored)
    const Pair pairs[] = {
        {"blendmap.png",              "game/data/textures/md_biome_blend.png", 4},
        {"overlaymaps/ambientmap.png", "game/data/textures/md_ambientmap.png", 4},
        {"biomemap.png",              "game/data/textures/md_biomemap.png",    3},
    };
    for (const Pair& p : pairs) {
        const std::string k = std::string(dir) + "/data/newland/land/" + p.kenshi_name;
        if (!std::filesystem::exists(p.offline)) continue;
        if (!std::filesystem::exists(k)) { ADD_FAILURE() << "missing " << k << " or " << p.offline; continue; }
        int kw = 0, kh = 0, kc = 0, ow = 0, oh = 0, oc = 0;
        unsigned char* a = stbi_load(k.c_str(), &kw, &kh, &kc, p.channels);
        unsigned char* b = stbi_load(p.offline, &ow, &oh, &oc, p.channels);
        ASSERT_TRUE(a && b) << p.kenshi_name;
        EXPECT_EQ(kw, ow); EXPECT_EQ(kh, oh);
        const size_t n = (size_t)kw * kh * p.channels;
        const bool same = kw == ow && kh == oh && memcmp(a, b, n) == 0;
        std::printf("[KenshiLand] %-26s %dx%d x%d: %s\n", p.kenshi_name, kw, kh, p.channels, same ? "identical" : "DIFFERENT");
        EXPECT_TRUE(same) << p.kenshi_name;
        stbi_image_free(a); stbi_image_free(b);
    }
}

TEST(KenshiLand, FileLookupFallsBackWhenInstallLacksTheFile) {
    setenv("KENSHI_DIR", "/nonexistent", 1);
    EXPECT_EQ(md::KenshiLandFile("blendmap.png", "fallback.png"), "fallback.png");
    unsetenv("KENSHI_DIR");
}

} // namespace
