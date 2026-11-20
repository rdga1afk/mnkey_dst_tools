// MakeTownSpawnList (DE-364): synthetic records for the rules, and the install data for the facts (the Hub via rebirth.mod; Brink is owned by United Cities).
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/town_spawn.h>

using md::fcs::GameData;
using md::fcs::GameDataContainer;
namespace {
using Ref = std::pair<std::string, std::array<int32_t, 3>>;
void Add(GameDataContainer& d, int kind, const std::string& id, const std::string& name) { GameData g; g.kind = kind; g.string_id = id; g.name = name; d.records[id] = g; d.order.push_back(id); }
void SetRefs(GameDataContainer& d, const std::string& id, const std::string& cat, std::vector<Ref> v) { d.records[id].references.push_back({ cat, std::move(v) }); }
bool Same(const md::world::TownSpawnList& a, const md::world::TownSpawnList& b) {
    if (a.owner != b.owner || a.templates != b.templates || a.anonymous.size() != b.anonymous.size() || a.named.size() != b.named.size()) return false;
    for (size_t i = 0; i < a.anonymous.size(); ++i) if (a.anonymous[i].character != b.anonymous[i].character || a.anonymous[i].category != b.anonymous[i].category) return false;
    return true;
}
GameDataContainer Synth() {
    GameDataContainer d;
    Add(d, 10, "nofac", "No Faction"); Add(d, 10, "fac", "Fac"); Add(d, 13, "townA", "Town A"); Add(d, 13, "townB", "Town B"); Add(d, 13, "townC", "Town C");
    Add(d, 52, "tpl", "Tpl"); Add(d, 1, "boss", "Boss"); Add(d, 1, "guard", "Guard"); Add(d, 1, "dog", "Dog"); Add(d, 1, "hero", "Hero");
    d.records["hero"].bools["unique"] = true;
    SetRefs(d, "fac", "residents", { { "tpl", { 0, 0, 0 } } });
    SetRefs(d, "townB", "faction", { { "fac", { 0, 0, 0 } } });
    SetRefs(d, "townC", "faction", { { "fac", { 0, 0, 0 } } }); d.records["townC"].bools["residents override"] = true;
    SetRefs(d, "tpl", "leader", { { "boss", { 1, 100, 0 } } });
    SetRefs(d, "tpl", "squad", { { "guard", { 2, 6, 0 } }, { "hero", { 1, 100, 0 } } });
    SetRefs(d, "tpl", "animals", { { "dog", { 3, 100, 0 } } });
    return d;
}
}  // namespace

TEST(TownSpawn, NoFactionReferenceIsNofacAndEmpty) {
    auto d = Synth(); const auto l = md::world::MakeTownSpawnList(d, "townA", 1);
    ASSERT_TRUE(l.found); EXPECT_EQ(l.owner, "nofac"); EXPECT_TRUE(l.templates.empty()); EXPECT_TRUE(l.anonymous.empty()); EXPECT_TRUE(l.named.empty());
    EXPECT_FALSE(md::world::MakeTownSpawnList(d, "tpl", 1).found);
    EXPECT_FALSE(md::world::MakeTownSpawnList(d, "nothing", 1).found);
}
TEST(TownSpawn, OwnerResidentsAreExpandedByMinMax) {
    auto d = Synth(); const auto l = md::world::MakeTownSpawnList(d, "townB", 7);
    ASSERT_TRUE(l.found); EXPECT_EQ(l.owner, "fac"); ASSERT_EQ(l.templates.size(), 1u);
    int boss = 0, guard = 0, dog = 0; for (const auto& e : l.anonymous) { boss += e.character == "boss"; guard += e.character == "guard"; dog += e.character == "dog"; }
    EXPECT_EQ(boss, 1); EXPECT_GE(guard, 2); EXPECT_LE(guard, 6); EXPECT_EQ(dog, 3);   // max 100 = no random count: min
    ASSERT_EQ(l.named.size(), 1u); EXPECT_EQ(l.named[0].character, "hero");               // unique character listed apart
    EXPECT_EQ(l.anonymous.front().category, "leader"); EXPECT_EQ(l.anonymous.front().state, 2);
}
TEST(TownSpawn, ResidentsOverrideSwitchesTheOwnerListOff) {
    auto d = Synth(); const auto l = md::world::MakeTownSpawnList(d, "townC", 7); EXPECT_TRUE(l.templates.empty()); EXPECT_TRUE(l.anonymous.empty());
}
TEST(TownSpawn, SameSeedSameListAndTheSeedChangesTheRandomCounts) {
    auto d = Synth(); const auto a = md::world::MakeTownSpawnList(d, "townB", 5), b = md::world::MakeTownSpawnList(d, "townB", 5);
    EXPECT_TRUE(Same(a, b));
    bool differs = false; for (uint32_t s = 6; s < 40 && !differs; ++s) differs = !Same(a, md::world::MakeTownSpawnList(d, "townB", s));
    EXPECT_TRUE(differs);
}

namespace {
bool LoadMerged(GameDataContainer& data) {
    md::fcs::Install inst(md::KenshiDir()); md::fcs::LoadOrder order; md::fcs::Error err;
    return inst.LoadOrderFor(order, err) && data.Load(order, err);
}
}  // namespace
TEST(TownSpawnInstall, HubHasRebirthResidentsAndBrinkHasUnitedCitiesResidents) {
    GameDataContainer data; ASSERT_TRUE(LoadMerged(data));
    // Newwworld.mod alone leaves The Hub without references (owner «nofac», empty list); rebirth.mod, a core mod of the install's load order, gives it `faction` 42022-rebirth.mod,
    // `residents override` and four own `residents` templates (44422, 44426, 51709, 56097)
    const auto hub = md::world::MakeTownSpawnList(data, "18919-Newwworld.mod", 1);
    ASSERT_TRUE(hub.found); EXPECT_EQ(hub.owner, "42022-rebirth.mod");
    const std::vector<std::string> hub_want = { "44422-rebirth.mod", "44426-rebirth.mod", "51709-rebirth.mod", "56097-rebirth.mod" };
    EXPECT_EQ(hub.templates, hub_want);   // override: the owner's own residents are not added
    EXPECT_FALSE(hub.anonymous.empty());
    for (const auto& e : hub.anonymous) ASSERT_NE(data.Get(e.character), nullptr) << e.character;
    for (const auto& e : hub.named) ASSERT_NE(data.Get(e.character), nullptr) << e.character;
    std::printf("Hub: %zu templates, %zu anonymous, %zu named\n", hub.templates.size(), hub.anonymous.size(), hub.named.size());
    EXPECT_TRUE(Same(hub, md::world::MakeTownSpawnList(data, "18919-Newwworld.mod", 1)));
    const auto b = md::world::MakeTownSpawnList(data, "962-gamedata.base", 3);
    ASSERT_TRUE(b.found); EXPECT_EQ(b.owner, "defaultEmpireFactionSID"); const auto* own = data.Get(b.owner); const auto* tw = data.Get("962-gamedata.base"); ASSERT_NE(own, nullptr);
    std::vector<std::string> want; if (const auto* r = tw->Refs("residents")) for (const auto& x : *r) want.push_back(x.first);   // the town's own list first, then the owner's (merged load order)
    if (const auto* r = own->Refs("residents")) for (const auto& x : *r) want.push_back(x.first);
    EXPECT_EQ(b.templates, want); EXPECT_GE(b.templates.size(), 4u);
    ASSERT_FALSE(b.anonymous.empty());
    for (const auto& e : b.anonymous) { ASSERT_NE(data.Get(e.character), nullptr) << e.character; EXPECT_FALSE(e.name.empty()); }
    for (const auto& e : b.named) ASSERT_NE(data.Get(e.character), nullptr);
    std::printf("Brink: %zu templates, %zu anonymous, %zu named\n", b.templates.size(), b.anonymous.size(), b.named.size());
    const auto b2 = md::world::MakeTownSpawnList(data, "962-gamedata.base", 3);
    EXPECT_TRUE(Same(b, b2));
}
