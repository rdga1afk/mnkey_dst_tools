// Player selection rule of the exe's select / deselect core ([KSDP], read level): the last selected character cannot be deselected.
#include <gtest/gtest.h>
#include <monkey_dust/world/player_selection.h>

using md::world::PlayerSelection;

TEST(PlayerSelection, LastSelectedCannotBeDeselected) {
    PlayerSelection s;
    EXPECT_TRUE(s.Select(10, {1, 2, 3}));
    EXPECT_FALSE(s.Deselect(10));
    EXPECT_EQ(s.Count(), 1);
    EXPECT_TRUE(s.Select(11, {4, 5, 6}));
    EXPECT_TRUE(s.Deselect(10));
    EXPECT_FALSE(s.Has(10));
    EXPECT_FALSE(s.Deselect(11));
    EXPECT_EQ(s.Count(), 1);
}

TEST(PlayerSelection, DirtyAndLastPosition) {
    PlayerSelection s;
    EXPECT_FALSE(s.Dirty());
    EXPECT_FALSE(s.Select(0, {}));            // a missing object does nothing
    EXPECT_FALSE(s.Dirty());
    s.Select(7, {1, 2, 3});
    EXPECT_TRUE(s.Dirty());
    EXPECT_EQ(s.LastPos().y, 2.f);
    s.ClearDirty();
    EXPECT_FALSE(s.Select(7, {9, 9, 9}));     // already selected: no change, position kept
    EXPECT_FALSE(s.Dirty());
    EXPECT_EQ(s.LastPos().x, 1.f);
}
