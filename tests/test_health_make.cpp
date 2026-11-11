// #185 audit: the F3 inspector "Add Component > Health" used Health{100.f, 100.f}, which brace-initialises hp[0], hp[1] (hp[] is an array) and leaves max[] zero.
// Health::Make(cur, max) is the intended constructor: every limb gets an equal share of both totals.
#include <gtest/gtest.h>
#include <monkey_dust/components/health.h>

TEST(HealthMake, SplitsCurrentAndMaxEqually) {
    const Health h = Health::Make(100.f, 100.f);
    EXPECT_FLOAT_EQ(h.TotalHp(), 100.f);
    EXPECT_FLOAT_EQ(h.TotalMax(), 100.f);
    EXPECT_FLOAT_EQ(h.HpFraction(), 1.f);
    EXPECT_FALSE(h.incapacitated);
}

TEST(HealthMake, BraceInitWithTwoFloatsFillsOnlyTheFirstLimbs) {
    // Documents the pitfall fixed in editor_inspector.cpp: not a valid way to build a Health.
    const Health h{100.f, 100.f};
    EXPECT_FLOAT_EQ(h.TotalMax(), 0.f);
}

// Limb API (Sever / AttachProsthetic / IsSevered / HasProsthetic): kept for the medicine/combat weeks, state covered so it is not dead-and-unchecked.
TEST(LimbHealthState, SeverZeroesLimbAndIncapacitates) {
    Health h = Health::Make(120.f);
    h.Sever(2);
    EXPECT_TRUE(h.IsSevered(2));
    EXPECT_FALSE(h.HasProsthetic(2));
    EXPECT_FLOAT_EQ(h.hp[2], 0.f);
    EXPECT_FLOAT_EQ(h.max[2], 0.f);
    EXPECT_TRUE(h.incapacitated);
    EXPECT_FALSE(h.IsSevered(0));
}

TEST(LimbHealthState, ProstheticOnlyOnSeveredLimb) {
    Health h = Health::Make(120.f);
    h.AttachProsthetic(3, 10.f);          // limb not severed: ignored
    EXPECT_FALSE(h.HasProsthetic(3));
    h.Sever(3);
    h.AttachProsthetic(3, 10.f);
    EXPECT_TRUE(h.HasProsthetic(3));
    EXPECT_TRUE(h.IsSevered(3));          // SEVERED stays set, PROSTHETIC added
    EXPECT_FLOAT_EQ(h.max[3], 10.f);
    EXPECT_FLOAT_EQ(h.hp[3], 8.f);
    EXPECT_FALSE(h.incapacitated);        // other limbs alive, this one hp > 0
}

TEST(LimbHealthState, OutOfRangeLimbIgnored) {
    Health h = Health::Make(120.f);
    h.Sever(-1);
    h.Sever(LIMB_COUNT);
    EXPECT_FALSE(h.incapacitated);
    EXPECT_FLOAT_EQ(h.TotalHp(), 120.f);
}
