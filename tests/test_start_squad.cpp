// StartSquadMembers on the install's gamedata.base only (the mods of the load order re-list the references): The Cannibal Hunters = 4 + 1 characters, a record with no squad reference = none
// (the exe then takes the record 45550-gamedata.base, not ported), count 0 gives 1 (The Holy Sword).
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/start_squad.h>

namespace {
bool LoadBase(md::fcs::GameDataContainer& data) {
    md::fcs::Install inst(md::KenshiDir()); md::fcs::LoadOrder order; md::fcs::Error err;
    if (!inst.LoadOrderFor(order, err)) return false;
    md::fcs::LoadOrder base;
    for (const auto& f : order.files) if (f.name == "gamedata.base") base.files.push_back(f);
    return base.files.size() == 1 && data.Load(base, err);
}
}  // namespace

TEST(StartSquad, CannibalHuntersIs4Plus1FromTheBaseFile) {
    md::fcs::GameDataContainer data; ASSERT_TRUE(LoadBase(data));
    const auto m = md::world::StartSquadMembers(data, "The Cannibal Hunters");
    ASSERT_EQ(m.size(), 5u);
    EXPECT_EQ(m[0].id, "2917-gamedata.base");
    EXPECT_EQ(m[3].id, "2917-gamedata.base");
    EXPECT_EQ(m[4].id, "5214-gamedata.base");
    for (const auto& x : m) EXPECT_FALSE(x.name.empty());
    EXPECT_TRUE(md::world::StartSquadMembers(data, "Wanderer").empty());
    EXPECT_TRUE(md::world::StartSquadMembers(data, "no such record").empty());
}
