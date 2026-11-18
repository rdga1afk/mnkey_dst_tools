// StartSquadMembers on the merged load order (base plus the core mods of the install), as the game uses it for the new-game start; a missing record gives none.
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/start_squad.h>
#include <cstdio>

namespace {
bool LoadMerged(md::fcs::GameDataContainer& data) {
    md::fcs::Install inst(md::KenshiDir()); md::fcs::LoadOrder order; md::fcs::Error err;
    return inst.LoadOrderFor(order, err) && data.Load(order, err);
}
}  // namespace

TEST(StartSquad, CannibalHuntersMergedLoadOrder) {
    md::fcs::GameDataContainer data; ASSERT_TRUE(LoadMerged(data));
    const auto m = md::world::StartSquadMembers(data, "The Cannibal Hunters");
    std::printf("merged Cannibal Hunters: %zu members:", m.size());
    for (const auto& x : m) std::printf(" %s(%s)", x.id.c_str(), x.name.c_str());
    std::printf("\n");
    ASSERT_FALSE(m.empty());
    for (const auto& x : m) { EXPECT_FALSE(x.name.empty()) << x.id << " has no record"; }
    EXPECT_TRUE(md::world::StartSquadMembers(data, "no such record").empty());
}
