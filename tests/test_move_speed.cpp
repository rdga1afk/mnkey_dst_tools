// Movement mode cap of the exe's setMoveSpeed ([KSDP]): the numbers are the Unicorn emulator results (exe_oracle_vectors3.tsv, setMoveSpeed rows).
#include <gtest/gtest.h>
#include <monkey_dust/world/move_speed.h>

using md::world::MoveSpeedCap;

TEST(MoveSpeed, MatchesExeEmulatorVectors) {
    struct V { int mode; float exe; };
    const V v[] = { {-1, 77.f}, {0, 15.f}, {1, 55.f}, {2, 999.f}, {3, 999.f}, {4, 77.f}, {16, 77.f} };   // current cap 77, walk cap 15
    for (const V& x : v) EXPECT_EQ(MoveSpeedCap(x.mode, 77.f, 15.f), x.exe) << "mode " << x.mode;
}
