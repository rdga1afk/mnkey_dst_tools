// The Q04 slice squad (engine/include/monkey_dust/world/slice_squad.h): selection rules, a move order, arrival, no overlap at the end, and a deterministic replay of 600 ticks
// with a committed state hash (a guard: the hash is this code's own output, #201 pattern; the rules inside are proven by test_steer / test_neighbour_avoid / test_move_speed).
#include <gtest/gtest.h>
#include <monkey_dust/world/slice_squad.h>
#include <cstdio>

using namespace md::world;

namespace {
void FillSquad(SliceSquad& s) {
    s.Add(0, 0, 0); s.Add(1.0f, 0, 0.2f); s.Add(-1.0f, 0, -0.4f); s.Add(30, 0, 30);
}
uint64_t Replay(int ticks) {
    SliceSquad s; FillSquad(s);
    s.Click(0, false); s.Click(1, true); s.Click(2, true);
    const float path[3][3] = {{10, 0, 0}, {10, 0, 12}, {20, 0, 12}};
    s.OrderMove(path, 3, 1);
    for (int t = 0; t < ticks; ++t) SliceTick(s, 0.1f);
    return SliceStateHash(s);
}
}  // namespace

TEST(SliceSquad, SelectionKeepsTheLastAndReplacesWithoutAdd) {
    SliceSquad s; FillSquad(s);
    EXPECT_TRUE(s.Click(0, false));
    EXPECT_TRUE(s.Click(1, true));
    EXPECT_EQ(s.sel.Count(), 2);
    EXPECT_TRUE(s.Click(1, true));          // add-click on a selected one removes it
    EXPECT_EQ(s.sel.Count(), 1);
    EXPECT_FALSE(s.Click(0, true));         // the last selected cannot be removed
    EXPECT_TRUE(s.Click(2, false));         // a plain click replaces the set
    EXPECT_TRUE(s.sel.Has(3));
    EXPECT_EQ(s.sel.Count(), 1);
}

TEST(SliceSquad, OnlySelectedMoveAndArrive) {
    SliceSquad s; FillSquad(s);
    s.Click(0, false); s.Click(1, true);
    const float path[2][3] = {{8, 0, 0}, {8, 0, 8}};
    s.OrderMove(path, 2, 0);
    for (int t = 0; t < 600; ++t) SliceTick(s, 0.1f);
    EXPECT_FALSE(s.c[0].moving); EXPECT_FALSE(s.c[1].moving);
    EXPECT_NEAR(s.c[0].pos[0], 8.f, 0.5f); EXPECT_NEAR(s.c[0].pos[2], 8.f, 0.5f);
    EXPECT_EQ(s.c[2].pos[0], -1.0f);        // not selected: unmoved
    EXPECT_EQ(s.c[3].pos[0], 30.f);
}

TEST(SliceSquad, ReplayIsDeterministicWithCommittedHash) {
    const uint64_t a = Replay(600), b = Replay(600);
    EXPECT_EQ(a, b);
    EXPECT_NE(Replay(30), a);
    EXPECT_EQ(a, 0xb04436f7386c353dULL);    // committed: a change of the rules or of the tick order changes it on purpose
    std::printf("slice replay hash (600 ticks): 0x%016llx\n", (unsigned long long)a);
}
