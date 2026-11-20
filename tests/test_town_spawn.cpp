// MakeTownSpawnList (DE-364): synthetic records for the rules, and the install data for the facts (the Hub via rebirth.mod; Brink is owned by United Cities).
#include <gtest/gtest.h>
#include <cmath>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/town_nodes.h>
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

TEST(TownSpawn, PlanIsCappedByTheBudgetAndHonoursTheTestPlacer) {
    auto d = Synth(); const auto l = md::world::MakeTownSpawnList(d, "townB", 7);
    const size_t total = l.anonymous.size() + l.named.size(); ASSERT_GT(total, 4u);
    auto at_anchor = [](const md::world::TownSpawnEntry&, size_t, float& x, float& z) { x = 100.f; z = -50.f; return true; };   // test-only placement: everyone at the town anchor
    const auto all = md::world::PlanTownSpawn(l, 1000, at_anchor); EXPECT_EQ(all.size(), total);
    const auto cap = md::world::PlanTownSpawn(l, 4, at_anchor); ASSERT_EQ(cap.size(), 4u); EXPECT_EQ(cap[0].x, 100.f); EXPECT_EQ(cap[0].z, -50.f);
    auto none = [](const md::world::TownSpawnEntry&, size_t, float&, float&) { return false; };   // no position rule: nothing is placed
    EXPECT_TRUE(md::world::PlanTownSpawn(l, 1000, none).empty());
    auto odd = [](const md::world::TownSpawnEntry&, size_t i, float& x, float& z) { x = z = 0.f; return (i & 1) == 0; };
    EXPECT_EQ(md::world::PlanTownSpawn(l, 1000, odd).size(), (total + 1) / 2);
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

TEST(TownSpawnInstall, HubHasGeneralNodesInsideTheTownRadius) {
    GameDataContainer data; ASSERT_TRUE(LoadMerged(data));
    // the Hub: centre from the townlist (9647.7, 15038.8) m, `size radius` 350 m
    const float cx = 9647.7f, cz = 15038.8f, R = 350.f;
    std::vector<md::world::TownNode> nodes; std::string err;
    ASSERT_TRUE(md::world::LoadTownNodes(md::KenshiDir(), data, cx, cz, R, nodes, err)) << err;
    size_t gen = 0, shop = 0; for (const auto& n : nodes) (n.node_type == 1 ? gen : shop)++;
    std::printf("Hub nodes: %zu general, %zu shopkeeper\n", gen, shop);
    ASSERT_GT(gen, 0u);
    // a node sits inside its building, so within the radius plus a building's half size (nodes up to ~40 m from the placement origin)
    for (const auto& n : nodes) { const float dx = n.x - cx, dz = n.z - cz; EXPECT_LE(std::sqrt(dx * dx + dz * dz), R + 60.f) << n.building; }
    std::vector<md::world::TownNode> again; ASSERT_TRUE(md::world::LoadTownNodes(md::KenshiDir(), data, cx, cz, R, again, err));
    ASSERT_EQ(again.size(), nodes.size());
}

TEST(TownNodes, ChoiceIsTypeOneFirstThenTypeZeroAndCoversTheRange) {
    std::vector<md::world::TownNode> v(5); for (size_t i = 0; i < v.size(); ++i) { v[i].node_type = (i == 1 || i == 3) ? 1 : 0; v[i].x = (float)i; }
    for (uint32_t r : { 0u, 0x40000000u, 0x80000000u, 0xffffffffu }) { const auto* n = md::world::ChooseSpawnNode(v, r); ASSERT_NE(n, nullptr); EXPECT_EQ(n->node_type, 1); }
    EXPECT_EQ(md::world::ChooseSpawnNode(v, 0u)->x, 1.f); EXPECT_EQ(md::world::ChooseSpawnNode(v, 0xffffffffu)->x, 3.f);
    v[1].node_type = v[3].node_type = 0; EXPECT_EQ(md::world::ChooseSpawnNode(v, 0xffffffffu)->x, 4.f);   // no type 1: type 0
    EXPECT_EQ(md::world::ChooseSpawnNode({}, 7u), nullptr);
}

TEST(TownSpawnInstall, HubPlanIsPlacedOnNodesAndSeedStable) {
    GameDataContainer data; ASSERT_TRUE(LoadMerged(data));
    const float cx = 9647.7f, cz = 15038.8f, R = 350.f; std::vector<md::world::TownNode> nodes; std::string err;
    ASSERT_TRUE(md::world::LoadTownNodes(md::KenshiDir(), data, cx, cz, R, nodes, err)) << err;
    const auto list = md::world::MakeTownSpawnList(data, "18919-Newwworld.mod", 1);
    md::world::NodePlacer p1(nodes, 1), p2(nodes, 1), p3(nodes, 2);
    const auto a = md::world::PlanTownSpawn(list, md::world::kTownNpcBudget, p1), b = md::world::PlanTownSpawn(list, md::world::kTownNpcBudget, p2), c = md::world::PlanTownSpawn(list, md::world::kTownNpcBudget, p3);
    ASSERT_EQ(a.size(), list.anonymous.size() + list.named.size());   // 16 members, under the budget of 64
    ASSERT_EQ(a.size(), b.size()); bool same = true, differs = false;
    for (size_t i = 0; i < a.size(); ++i) { same = same && a[i].x == b[i].x && a[i].z == b[i].z; differs = differs || a[i].x != c[i].x; }
    EXPECT_TRUE(same); EXPECT_TRUE(differs);
    for (const auto& m : a) { const float dx = m.x - cx, dz = m.z - cz; EXPECT_LE(std::sqrt(dx * dx + dz * dz), R + 60.f); }
    std::printf("Hub plan: %zu members placed on type-1 nodes of %zu nodes\n", a.size(), nodes.size());
}
