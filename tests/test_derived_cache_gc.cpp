#include <gtest/gtest.h>
#include <monkey_dust/io/derived_cache.h>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

// Two keys of the same artifact are present: after the collection only the key this process asked for remains; a different artifact keeps its own key.
TEST(DerivedCacheGc, KeepsOnlyTheCurrentKeyOfARequestedArtifact) {
    const fs::path tmp = fs::temp_directory_path() / "md_cache_gc_test";
    fs::remove_all(tmp);
    setenv("XDG_CACHE_HOME", tmp.c_str(), 1);
    const uint8_t b[4] = { 1, 2, 3, 4 };
    ASSERT_TRUE(md::CacheWrite(0xAAull, "gc_test_artifact.bin", b, 4));   // old key
    ASSERT_TRUE(md::CacheWrite(0xBBull, "gc_test_other.bin", b, 4));      // another artifact, its own current key
    ASSERT_TRUE(md::CacheWrite(0xCCull, "gc_test_artifact.bin", b, 4));   // current key
    EXPECT_EQ(md::CacheCollectGarbage(), 1);
    EXPECT_FALSE(fs::exists(tmp / "monkeydust" / "00000000000000aa"));
    EXPECT_TRUE(fs::exists(tmp / "monkeydust" / "00000000000000cc" / "gc_test_artifact.bin"));
    EXPECT_TRUE(fs::exists(tmp / "monkeydust" / "00000000000000bb" / "gc_test_other.bin"));
    fs::remove_all(tmp);
}
