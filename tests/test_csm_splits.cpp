#include <gtest/gtest.h>
#include <monkey_dust/render/csm_splits.h>
#include <cmath>

TEST(CsmSplits, ExeParametersGiveIncreasingSplitsEndingAtFar) {
    double s[5]; md::csm::SplitPoints(4, 1.0, 5000.0, 0.95, 0.5, s);
    EXPECT_DOUBLE_EQ(s[0], 1.0); EXPECT_DOUBLE_EQ(s[4], 5000.0);
    // hand value for i = 1: (0.95 * 5000^(1/4) + 0.05 * (1 + 4999 / 4)) * 0.5
    EXPECT_NEAR(s[1], (0.95 * std::pow(5000.0, 0.25) + 0.05 * (1.0 + 4999.0 / 4.0)) * 0.5, 1e-9);
    EXPECT_NEAR(s[2], (0.95 * std::pow(5000.0, 0.5) + 0.05 * (1.0 + 4999.0 / 2.0)) * 0.5, 1e-9);
    for (int i = 0; i < 4; ++i) EXPECT_LT(s[i], s[i + 1]);
}

TEST(CsmSplits, ScaleOneIsTheOgrePssmFormula) {
    double s[5]; md::csm::SplitPoints(4, 2.0, 400.0, 0.5, 1.0, s);   // OGRE: lambda * near * (far / near)^(i/n) + (1 - lambda) * (near + (i/n) * (far - near))
    for (int i = 1; i < 4; ++i) {
        const double f = i / 4.0;
        EXPECT_NEAR(s[i], 0.5 * 2.0 * std::pow(200.0, f) + 0.5 * (2.0 + f * 398.0), 1e-9);
    }
    md::csm::SplitPoints(4, 2.0, 400.0, 0.0, 1.0, s);                  // lambda 0 = uniform
    EXPECT_NEAR(s[2], 2.0 + 0.5 * 398.0, 1e-9);
}
