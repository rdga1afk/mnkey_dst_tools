#include <gtest/gtest.h>
#include <monkey_dust/world/toroidal_update.h>

// docs/archive/DAGOR_IMPLEMENTATION_PROMPT.md Рівень 2, КРОК 4 -- correctness
// checks for the toroidal-update primitive: does it actually flag only
// the regions that went out of bounds, does it cover the full new-origin
// window without gaps/overlaps, does full invalidation trigger correctly.

using md::IPoint2;
using md::ToroidalQuadRegion;
using md::ToroidalUpdate;
using md::ToroidalWrap;

namespace {

// Sums the area of all emitted regions -- used both to sanity-check
// against the expected total-window area and (for a zero-movement or
// pure-repeat call) to confirm no double coverage.
int TotalArea(const ToroidalQuadRegion* regions, int count) {
    int area = 0;
    for (int i = 0; i < count; ++i) { area += regions[i].size.x * regions[i].size.y; }
    return area;
}

} // namespace

TEST(ToroidalUpdate, ZeroMovementEmitsNothing) {
    IPoint2 cur(1000, 1000), main(1000, 1000);
    ToroidalQuadRegion regions[4];
    int n = ToroidalUpdate(IPoint2(1000, 1000), &cur, &main, 256, 200, regions);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(cur, IPoint2(1000, 1000));
}

TEST(ToroidalUpdate, LargeMovementTriggersFullInvalidation) {
    IPoint2 cur(1000, 1000), main(1000, 1000);
    ToroidalQuadRegion regions[4];
    // movement of 500 texels >= full_update_texels_threshold(200) -> full reset.
    int n = ToroidalUpdate(IPoint2(1500, 1000), &cur, &main, 256, 200, regions);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(regions[0].viewport_lt, IPoint2(0, 0));
    EXPECT_EQ(regions[0].size, IPoint2(256, 256));
    EXPECT_EQ(cur, IPoint2(1500, 1000));
    EXPECT_EQ(main, IPoint2(1500, 1000));
}

TEST(ToroidalUpdate, SmallMovementCoversExactlyTheMovedStrip) {
    const int texSize = 256;
    IPoint2 cur(0, 0), main(0, 0);
    ToroidalQuadRegion regions[4];
    // Move 10 texels on X only -- expected total newly-covered area is a
    // 10-wide x texSize-tall strip (the only part of the window that
    // wasn't already valid from the previous origin).
    int n = ToroidalUpdate(IPoint2(10, 0), &cur, &main, texSize, 200, regions);
    ASSERT_GT(n, 0);
    ASSERT_LE(n, 4);
    int area = TotalArea(regions, n);
    EXPECT_EQ(area, 10 * texSize);
    EXPECT_EQ(cur, IPoint2(10, 0));
    // main_origin must NOT move on a partial update (only cur_origin does).
    EXPECT_EQ(main, IPoint2(0, 0));
}

TEST(ToroidalUpdate, DiagonalMovementCoversLShapedRegionNoOverlap) {
    const int texSize = 256;
    IPoint2 cur(0, 0), main(0, 0);
    ToroidalQuadRegion regions[4];
    int n = ToroidalUpdate(IPoint2(8, 8), &cur, &main, texSize, 200, regions);
    ASSERT_GT(n, 0);
    // Expected area: full window minus the (texSize-8)x(texSize-8) block
    // that stays valid (already-cached from the previous origin).
    int expected = (texSize * texSize) - ((texSize - 8) * (texSize - 8));
    EXPECT_EQ(TotalArea(regions, n), expected);

    // No two emitted regions should overlap in viewport space -- check
    // pairwise AABB intersection is empty.
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const auto& a = regions[i];
            const auto& b = regions[j];
            bool disjoint = a.viewport_lt.x + a.size.x <= b.viewport_lt.x ||
                             b.viewport_lt.x + b.size.x <= a.viewport_lt.x ||
                             a.viewport_lt.y + a.size.y <= b.viewport_lt.y ||
                             b.viewport_lt.y + b.size.y <= a.viewport_lt.y;
            EXPECT_TRUE(disjoint) << "regions " << i << " and " << j << " overlap";
        }
    }
}

TEST(ToroidalUpdate, RepeatedSmallStepsMatchOneBigStep) {
    // Moving 5 texels twice should leave cur_origin at the same place as
    // moving 10 texels once (origin bookkeeping correctness, not just
    // areas).
    const int texSize = 256;
    IPoint2 curA(0, 0), mainA(0, 0);
    ToroidalQuadRegion regionsA[4];
    ToroidalUpdate(IPoint2(5, 0), &curA, &mainA, texSize, 200, regionsA);
    ToroidalUpdate(IPoint2(10, 0), &curA, &mainA, texSize, 200, regionsA);

    IPoint2 curB(0, 0), mainB(0, 0);
    ToroidalQuadRegion regionsB[4];
    ToroidalUpdate(IPoint2(10, 0), &curB, &mainB, texSize, 200, regionsB);

    EXPECT_EQ(curA, curB);
    EXPECT_EQ(mainA, mainB);
}

TEST(ToroidalUpdate, WrapIsIdempotentWithinRange) {
    EXPECT_EQ(ToroidalWrap(IPoint2(5, 5), 256), IPoint2(5, 5));
    EXPECT_EQ(ToroidalWrap(IPoint2(-1, -1), 256), IPoint2(255, 255));
    EXPECT_EQ(ToroidalWrap(IPoint2(256, 300), 256), IPoint2(0, 44));
}
