#include <gtest/gtest.h>
#include <monkey_dust/core/fnv.h>

TEST(FNV, EmptyStringIsOffsetBasis) {
    EXPECT_EQ(md::fnv1a(""), 2166136261u);
}

TEST(FNV, CompileTimeEqualsRuntime) {
    constexpr uint32_t ct = md::fnv1a("menace");
    uint32_t rt = md::fnv1a_rt("menace");
    EXPECT_EQ(ct, rt);
}

TEST(FNV, Deterministic) {
    EXPECT_EQ(md::fnv1a("director_stage"), md::fnv1a("director_stage"));
}

TEST(FNV, DifferentStringsProduceDifferentHashes) {
    EXPECT_NE(md::fnv1a("speed"), md::fnv1a("health"));
    EXPECT_NE(md::fnv1a("a"), md::fnv1a("b"));
}

TEST(FNV, KnownVectors) {
    // Pre-validated against reference FNV-1a 32-bit implementation.
    EXPECT_EQ(md::fnv1a("foobar"), 0xBF9CF968u);
    EXPECT_EQ(md::fnv1a("hello"),  0x4F9F2CABu);
}

TEST(FNV, SingleCharDiffersFromEmpty) {
    EXPECT_NE(md::fnv1a("a"), md::fnv1a(""));
    EXPECT_NE(md::fnv1a("z"), md::fnv1a(""));
}
