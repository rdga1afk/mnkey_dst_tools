// G3 cheap half: per-category frame counters (engine/include/monkey_dust/render/frame_counters.h). Pure arithmetic, no GPU.
#include <gtest/gtest.h>
#include <monkey_dust/render/frame_counters.h>

TEST(FrameCounters, TotalSumsRows) {
    md::FrameCounters c;
    EXPECT_TRUE(c.Add("town", 10, 200, 40000));
    EXPECT_TRUE(c.Add("foliage", 5, 3000, 1500000));
    EXPECT_TRUE(c.Add("terrain", 120, 120, 61440));
    const md::FrameCounterRow t = c.Total();
    EXPECT_EQ(t.draws, 135);
    EXPECT_EQ(t.instances, 3320);
    EXPECT_EQ(t.triangles, 1601440);
}

TEST(FrameCounters, EmptyTotalIsZero) {
    md::FrameCounters c;
    const md::FrameCounterRow t = c.Total();
    EXPECT_EQ(t.draws, 0);
    EXPECT_EQ(t.instances, 0);
    EXPECT_EQ(t.triangles, 0);
}

TEST(FrameCounters, TableFullRejectsAndKeepsRows) {
    md::FrameCounters c;
    for (int i = 0; i < md::kFrameCounterMaxRows; ++i) EXPECT_TRUE(c.Add("x", 1, 1, 1));
    EXPECT_FALSE(c.Add("overflow", 100, 100, 100));
    EXPECT_EQ(c.count, md::kFrameCounterMaxRows);
    EXPECT_EQ(c.Total().draws, md::kFrameCounterMaxRows);
}

TEST(FrameCounters, TrisPerInstanceHandlesZeroAndLargeCounts) {
    md::FrameCounterRow none; none.triangles = 500;
    EXPECT_FLOAT_EQ(md::FrameCounters::TrisPerInstance(none), 0.f);
    md::FrameCounterRow r; r.instances = 4; r.triangles = 10;
    EXPECT_FLOAT_EQ(md::FrameCounters::TrisPerInstance(r), 2.5f);
    md::FrameCounterRow big; big.instances = 1000000; big.triangles = int64_t(3000000000LL);   // beyond int32
    EXPECT_FLOAT_EQ(md::FrameCounters::TrisPerInstance(big), 3000.f);
}
