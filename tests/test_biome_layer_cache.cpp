#include <gtest/gtest.h>
#include <monkey_dust/render/biome_layer_cache.h>

using md::BiomeLayerCache;

TEST(BiomeLayerCache, BudgetEvictsOldestUnusedAndKeepsThisFrame) {
    int live = 0; std::vector<int> freed;
    BiomeLayerCache c([&](int b) { ++live; return BiomeLayerCache::Built{ (void*)(intptr_t)(b + 1), 50ull << 20 }; }, [&](void* h) { --live; freed.push_back((int)(intptr_t)h - 1); });
    c.SetBudgetBytes(120ull << 20);   // two biomes fit, a third does not
    EXPECT_TRUE(c.Acquire(1, 1)); EXPECT_TRUE(c.Acquire(2, 1));
    EXPECT_EQ(c.Resident(), 2);
    EXPECT_TRUE(c.Acquire(3, 1));   // all three used this frame: nothing may go, the budget is exceeded
    EXPECT_EQ(c.Resident(), 3); EXPECT_EQ(c.Evictions(), 0u);
    EXPECT_TRUE(c.Acquire(2, 2)); EXPECT_TRUE(c.Acquire(3, 2));   // frame 2 uses 2 and 3
    EXPECT_TRUE(c.Acquire(4, 2));
    ASSERT_EQ(freed.size(), 1u);   // 1 (oldest, unused) goes; the budget stays exceeded because 2, 3 and 4 are in use this frame
    EXPECT_EQ(freed[0], 1);
    EXPECT_FALSE(c.Has(1)); EXPECT_TRUE(c.Has(2)); EXPECT_TRUE(c.Has(3)); EXPECT_TRUE(c.Has(4));
    EXPECT_EQ(c.Builds(), 4u);
    c.Clear(); EXPECT_EQ(live, 0);
}

TEST(BiomeLayerCache, UnbuildableBiomeIsNotCached) {
    BiomeLayerCache c([](int) { return BiomeLayerCache::Built{}; }, [](void*) {});
    EXPECT_EQ(c.Acquire(5, 1), nullptr); EXPECT_EQ(c.Resident(), 0); EXPECT_EQ(c.Builds(), 0u);
}
