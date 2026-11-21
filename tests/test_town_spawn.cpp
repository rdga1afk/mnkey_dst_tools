// MakeTownSpawnList (DE-364): synthetic records for the rules, and the install data for the facts (the Hub via rebirth.mod; Brink is owned by United Cities).
#include <gtest/gtest.h>
#include <cmath>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/town_nodes.h>
#include <monkey_dust/world/town_spawn.h>
#include <monkey_dust/world/npc_look.h>
#include <map>

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

TEST(TownRadius, ExeTypeRadiiTimesMultiplier) {   // the exe radius virtual (read ok): the constants per type, the multiplier clamp of the town init
    EXPECT_FLOAT_EQ(md::world::TownRadiusM(2, 1.f), 250.f); EXPECT_FLOAT_EQ(md::world::TownRadiusM(3, 1.f), 93.75f);
    EXPECT_FLOAT_EQ(md::world::TownRadiusM(4, 1.f), 87.5f); EXPECT_FLOAT_EQ(md::world::TownRadiusM(9, 1.f), 43.75f);
    EXPECT_FLOAT_EQ(md::world::TownRadiusM(1, 1.f), 125.f); EXPECT_FLOAT_EQ(md::world::TownRadiusM(8, 1.f), 0.f); EXPECT_FLOAT_EQ(md::world::TownRadiusM(10, 3.f), 0.f);
    EXPECT_FLOAT_EQ(md::world::TownRadiusM(2, 0.2f), 50.f); EXPECT_FLOAT_EQ(md::world::TownRadiusM(2, 0.05f), 250.f); EXPECT_FLOAT_EQ(md::world::TownRadiusM(2, 9.f), 1250.f);
}

TEST(TownSpawnInstall, HubRadiusFromTypeAndMultiplier) {   // what the exe radius is for the Hub record (printed: the facts the DE-027 replacement rests on)
    GameDataContainer data; ASSERT_TRUE(LoadMerged(data));
    const auto* t = data.Get("18919-Newwworld.mod"); ASSERT_NE(t, nullptr);
    const auto ti = t->ints.find("type"); const auto mi = t->floats.find("town radius mult"); const auto si = t->floats.find("size radius");
    const int type = ti == t->ints.end() ? -1 : ti->second; const float mult = mi == t->floats.end() ? 1.f : mi->second;
    std::printf("Hub: type %d, town radius mult %.3f (%s), size radius %.1f, exe radius %.1f m\n", type, mult, mi == t->floats.end() ? "absent" : "set", si == t->floats.end() ? -1.f : si->second, md::world::TownRadiusM(type, mult));
    EXPECT_GT(md::world::TownRadiusM(type, mult), 0.f);
}

static int HubIndex(const std::vector<md::world::TownSite>& t) { for (size_t i = 0; i < t.size(); ++i) if (t[i].id == "18919-Newwworld.mod") return (int)i; return -1; }

TEST(TownSpawnInstall, HubMembershipByTypeRadius) {
    GameDataContainer data; ASSERT_TRUE(LoadMerged(data));
    std::vector<md::world::TownSite> towns; std::string err; ASSERT_TRUE(md::world::LoadTownSites(md::KenshiDir(), data, towns, err)) << err;
    const int hub = HubIndex(towns); ASSERT_GE(hub, 0);
    std::vector<md::world::TownNode> nodes; ASSERT_TRUE(md::world::LoadTownNodes(md::KenshiDir(), data, towns, hub, nodes, err)) << err;
    std::vector<md::world::TownNode> again0;
    size_t t0 = 0, t1 = 0, t2 = 0; for (const auto& n : nodes) (n.node_type == 0 ? t0 : n.node_type == 1 ? t1 : t2)++;
    size_t total = 0; ASSERT_TRUE(md::world::LoadTownNodes(md::KenshiDir(), data, towns, hub, again0, err, &total)) << err;
    std::printf("Hub: %zu towns in the list, radius %.1f m, %zu buildings in all, %zu with nodes, nodes type0 %zu type1 %zu type2 %zu\n", towns.size(), md::world::TownRadiusM(towns[hub].type, towns[hub].mult), total, md::world::TownBuildingCount(nodes), t0, t1, t2);
    ASSERT_GT(t1, 0u);
    const float R = md::world::TownRadiusM(towns[hub].type, towns[hub].mult);
    for (const auto& n : nodes) {   // a node sits inside its building: the radius plus a building's half size; and the nearest holding town is the Hub
        const float dx = n.x - towns[hub].x, dz = n.z - towns[hub].z; EXPECT_LE(std::sqrt(dx * dx + dz * dz), R + 60.f) << n.building;
    }
    std::vector<md::world::TownNode> again; ASSERT_TRUE(md::world::LoadTownNodes(md::KenshiDir(), data, towns, hub, again, err));
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
    std::vector<md::world::TownSite> towns; std::vector<md::world::TownNode> nodes; std::string err; ASSERT_TRUE(md::world::LoadTownSites(md::KenshiDir(), data, towns, err)) << err;
    const int hub = HubIndex(towns); ASSERT_GE(hub, 0);
    const float cx = towns[hub].x, cz = towns[hub].z, R = md::world::TownRadiusM(towns[hub].type, towns[hub].mult);
    ASSERT_TRUE(md::world::LoadTownNodes(md::KenshiDir(), data, towns, hub, nodes, err)) << err;
    const auto list = md::world::MakeTownSpawnList(data, "18919-Newwworld.mod", 1);
    md::world::NodePlacer p1(nodes, 1), p2(nodes, 1), p3(nodes, 2);
    const auto a = md::world::PlanTownSpawn(list, md::world::kTownNpcBudget, p1), b = md::world::PlanTownSpawn(list, md::world::kTownNpcBudget, p2), c = md::world::PlanTownSpawn(list, md::world::kTownNpcBudget, p3);
    ASSERT_EQ(a.size(), list.anonymous.size() + list.named.size());   // 16 members, under the budget of 64
    ASSERT_EQ(a.size(), b.size()); bool same = true, differs = false;
    for (size_t i = 0; i < a.size(); ++i) { same = same && a[i].x == b[i].x && a[i].z == b[i].z; differs = differs || a[i].x != c[i].x; }
    EXPECT_TRUE(same); EXPECT_TRUE(differs);
    for (const auto& m : a) { const float dx = m.x - cx, dz = m.z - cz; EXPECT_LE(std::sqrt(dx * dx + dz * dz), R + 60.f); }
    std::printf("Hub plan: %zu members placed on type-1 nodes of %zu nodes\n", a.size(), nodes.size());
    for (size_t i = 0; i < a.size() && i < 5; ++i) std::printf("member %zu at (%.1f, %.1f) node y %.2f\n", i + 1, a[i].x, a[i].z, a[i].y);
    for (const auto& m : a) EXPECT_TRUE(m.has_y);
    float lo = 1e9f, hi = -1e9f; for (const auto& n : nodes) { lo = std::min(lo, n.y); hi = std::max(hi, n.y); } std::printf("node y range %.1f .. %.1f\n", lo, hi);
}

TEST(TownNodes, TownOfPointIsTheNearestHoldingTown) {
    std::vector<md::world::TownSite> t = { {"a", 0.f, 0.f, 2, 1.f}, {"b", 300.f, 0.f, 3, 1.f}, {"c", 10.f, 0.f, 8, 1.f} };   // a: 250 m, b: 93.75 m, c: type 8 = no radius
    EXPECT_EQ(md::world::TownOfPoint(t, 5.f, 0.f), 0);        // c is nearer but holds nothing
    EXPECT_EQ(md::world::TownOfPoint(t, 200.f, 0.f), 0);      // 200 m from a: a's (b is 100 m away, beyond its 93.75 m)
    EXPECT_EQ(md::world::TownOfPoint(t, 300.f, 0.f), 1);      // 300 m from a: not a's; b holds it
    EXPECT_EQ(md::world::TownOfPoint(t, 240.f, 0.f), 1);      // both hold it, b is nearer
    EXPECT_EQ(md::world::TownOfPoint(t, 0.f, 500.f), -1);     // nobody
}

TEST(TownNodes, SquadSpawnNodeOrderIsZeroTwoOneInItsOwnBuilding) {
    auto N = [](int placed, int type, float x) { md::world::TownNode n; n.placed = placed; n.node_type = type; n.x = x; return n; };
    std::vector<md::world::TownNode> v = { N(0, 1, 1.f), N(1, 1, 2.f), N(1, 2, 3.f), N(1, 2, 4.f), N(2, 0, 5.f), N(2, 2, 6.f), N(2, 1, 7.f) };
    EXPECT_EQ(md::world::SquadSpawnNode(v, 2, 0u)->x, 5.f);                 // 0 first
    const auto* g = md::world::SquadSpawnNode(v, 1, 0xffffffffu); ASSERT_NE(g, nullptr); EXPECT_EQ(g->node_type, 2); EXPECT_EQ(g->x, 4.f);   // no 0: 2 before 1
    EXPECT_EQ(md::world::SquadSpawnNode(v, 0, 7u)->node_type, 1);           // only 1
    EXPECT_EQ(md::world::SquadSpawnNode(v, 9, 7u), nullptr);                // no building: the origin
    for (uint32_t r : { 0u, 0x80000000u, 0xffffffffu }) EXPECT_EQ(md::world::SquadSpawnNode(v, 1, r)->placed, 1);
}

TEST(TownNodes, HousingWalksPrioritiesFiveToZero) {
    std::vector<md::world::WantedBuilding> table = { {10, 0, 5}, {11, 3, 1}, {12, 5, 2}, {13, 3, 1} };
    const auto h = md::world::AssignHousing(table, 6);
    ASSERT_EQ(h.size(), 6u);
    EXPECT_EQ(h[0], 12); EXPECT_EQ(h[1], 12);   // priority 5 first, count 2
    EXPECT_EQ(h[2], 11); EXPECT_EQ(h[3], 13);   // priority 3 in table order
    EXPECT_EQ(h[4], 10); EXPECT_EQ(h[5], 10);   // priority 0
    EXPECT_EQ(md::world::AssignHousing(table, 6), h);   // deterministic
    const auto few = md::world::AssignHousing(table, 1); EXPECT_EQ(few[0], 12);
    const auto many = md::world::AssignHousing({ {1, 5, 1} }, 3); EXPECT_EQ(many[0], 1); EXPECT_EQ(many[1], -1); EXPECT_EQ(many[2], -1);   // a squad with no house
}

TEST(TownNodes, GeneralChoiceIgnoresGuardPostNodes) {
    md::world::TownNode g; g.node_type = 2; g.x = 9.f; md::world::TownNode z; z.node_type = 0; z.x = 4.f;
    std::vector<md::world::TownNode> v = { g, z };
    EXPECT_EQ(md::world::ChooseSpawnNode(v, 0xffffffffu)->x, 4.f);
    v = { g }; EXPECT_EQ(md::world::ChooseSpawnNode(v, 0u), nullptr);
}

// Template housing (DE-364): templates are served by priority 5..0, each takes the first free placed building of its `building` set.
TEST(TownNodes, TemplatesTakeFreeWantedBuildingsByPriority) {
    GameDataContainer d;
    Add(d, 10, "nofac", "No Faction"); Add(d, 13, "town", "Town"); Add(d, 52, "lo", "Lo"); Add(d, 52, "hi", "Hi"); Add(d, 52, "none", "None");
    SetRefs(d, "town", "residents", { { "lo", { 1, 1, 0 } }, { "hi", { 1, 5, 0 } }, { "none", { 1, 100, 0 } } });   // lo: priority 1, hi: 5, none: stored 100 -> 0
    SetRefs(d, "lo", "building", { { "bA", { 0, 0, 0 } }, { "bB", { 0, 0, 0 } } });
    SetRefs(d, "hi", "building", { { "bA", { 0, 0, 0 } } });
    const auto l = md::world::MakeTownSpawnList(d, "town", 1);
    ASSERT_EQ(l.priorities.size(), 3u);
    EXPECT_EQ(l.priorities[0], 1); EXPECT_EQ(l.priorities[1], 5); EXPECT_EQ(l.priorities[2], 0);
    std::vector<md::world::TownNode> nodes(3);
    nodes[0].building = "bA"; nodes[0].placed = 4; nodes[0].node_type = 1; nodes[0].x = 10.f;
    nodes[1].building = "bB"; nodes[1].placed = 7; nodes[1].node_type = 1; nodes[1].x = 20.f;
    nodes[2].building = "bC"; nodes[2].placed = 9; nodes[2].node_type = 1; nodes[2].x = 30.f;
    const auto h = md::world::AssignTemplateHomes(d, l, nodes);
    ASSERT_EQ(h.size(), 3u);
    EXPECT_EQ(h[1], 4);    // hi (priority 5) takes bA first
    EXPECT_EQ(h[0], 7);    // lo wanted bA too, but it is taken: bB
    EXPECT_EQ(h[2], -1);   // no `building` set
    EXPECT_EQ(md::world::AssignTemplateHomes(d, l, nodes), h);   // deterministic
    md::world::NodePlacer placer(nodes, 3); placer.list = &l; placer.homes = &h;
    md::world::TownSpawnEntry e; e.template_id = "hi"; float x = 0, z = 0, y = 0;
    ASSERT_TRUE(placer(e, 0, x, z, y)); EXPECT_FLOAT_EQ(x, 10.f);
    e.template_id = "none"; ASSERT_TRUE(placer(e, 1, x, z, y));   // no home: any general node
    EXPECT_TRUE(x == 10.f || x == 20.f || x == 30.f);
}

// Look of the Hub residents: race source order, same seed same look, unresolved ones are reported (no stand-in).
TEST(NpcLookInstall, HubResidentsRaceSplitAndSeedStable) {
    GameDataContainer data; ASSERT_TRUE(LoadMerged(data));
    const auto hub = md::world::MakeTownSpawnList(data, "18919-Newwworld.mod", 1);
    ASSERT_TRUE(hub.found);
    std::map<std::string, int> split; int unresolved = 0, n = 0;
    for (const auto& e : hub.anonymous) {
        const auto* t = data.Get(e.template_id); std::string fac = hub.owner;
        if (t) if (const auto* f = t->Refs("faction")) if (!f->empty()) fac = (*f)[0].first;
        const auto a = md::world::ResolveNpcLook(data, e.character, e.template_id, fac, 7);
        const auto b = md::world::ResolveNpcLook(data, e.character, e.template_id, fac, 7);
        ++n;
        EXPECT_EQ(a.race, b.race); EXPECT_EQ(a.female, b.female); EXPECT_EQ(a.head, b.head); EXPECT_EQ(a.hair, b.hair); EXPECT_EQ(a.hair_argb, b.hair_argb);
        if (!a.resolved) { ++unresolved; std::printf("  unresolved: %s (%s, faction %s)\n", e.character.c_str(), e.name.c_str(), fac.c_str()); continue; }
        { const auto* rr = data.Get(a.race); ++split[a.race + " (" + (rr ? rr->name : std::string("?")) + ")" + (a.female ? " F" : " M")]; }
        EXPECT_FALSE(a.mesh.empty()) << a.race;
        std::printf("  %s: %s %s head=%s hair=%s beard=%s col=%08x\n", e.name.c_str(), a.race.c_str(), a.female ? "F" : "M", a.head.c_str(), a.hair.c_str(), a.beard.c_str(), a.hair_argb);
    }
    for (const auto& kv : split) std::printf("Hub race split: %s x%d\n", kv.first.c_str(), kv.second);
    std::printf("Hub residents %d, unresolved %d\n", n, unresolved);
    EXPECT_GT(n, 0);
}
