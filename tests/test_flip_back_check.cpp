#include <gtest/gtest.h>
#include "../game/src/render/flip_back_check.h"

namespace {
const double kCam[3] = {9647.7, 30.0, 15038.8}, kDir[3] = {0, 0, 1};
void Feed(md::FlipBackCheck& c, const int* vals, int n, double move_per_frame = 0.0) {
    for (int i = 0; i < n; ++i) {
        const double cam[3] = {kCam[0] + move_per_frame * i, kCam[1], kCam[2]};
        c.BeginFrame(cam, kDir); c.Push(7, vals[i]);
    }
}
}

TEST(FlipBackCheck, AbaWithAFixedViewIsAViolation) {
    md::FlipBackCheck c; const int v[] = {2, 2, 3, 2};
    Feed(c, v, 4);
    EXPECT_EQ(c.Violations(), 1u);
}

TEST(FlipBackCheck, SteadyAndMonotoneChangesAreSilent) {
    md::FlipBackCheck a; const int steady[] = {2, 2, 2, 2, 2, 2}; Feed(a, steady, 6); EXPECT_EQ(a.Violations(), 0u);
    md::FlipBackCheck b; const int step[] = {1, 1, 2, 2, 3, 3, 3}; Feed(b, step, 7); EXPECT_EQ(b.Violations(), 0u);
}

TEST(FlipBackCheck, AMovingCameraMayLegitimatelyFlipBack) {
    md::FlipBackCheck c; const int v[] = {2, 2, 3, 2};
    Feed(c, v, 4, 1.0);   // 1 m per frame: the view is not still across the three frames
    EXPECT_EQ(c.Violations(), 0u);
}

TEST(FlipBackCheck, TurningTheViewAlsoCancels) {
    md::FlipBackCheck c; const int v[] = {2, 3, 2};
    for (int i = 0; i < 3; ++i) { const double d[3] = {std::sin(i * 0.01), 0, std::cos(i * 0.01)}; c.BeginFrame(kCam, d); c.Push(1, v[i]); }   // 0.57 deg per frame
    EXPECT_EQ(c.Violations(), 0u);
}

TEST(FlipBackCheck, ABeyondTheHistoryAndGapsDoNotCount) {
    md::FlipBackCheck c; const int v[] = {2, 3, 3, 3, 3, 3, 3, 3, 3, 2};   // the 2 is 9 frames after the first: out of the 8-frame history
    Feed(c, v, 10); EXPECT_EQ(c.Violations(), 0u);
    md::FlipBackCheck g;   // a key missing for a frame breaks the chain
    g.BeginFrame(kCam, kDir); g.Push(5, 2); g.BeginFrame(kCam, kDir); g.Push(5, 3); g.BeginFrame(kCam, kDir); /* no push */ g.BeginFrame(kCam, kDir); g.Push(5, 2);
    EXPECT_EQ(g.Violations(), 0u);
}
