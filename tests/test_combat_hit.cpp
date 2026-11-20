// Simple melee hit (DE-364): formulas of combat_hit.h against the exe read in docs/research/COMBAT_HIT_EXE_READ_2026-11-20.md.
// The install test needs KENSHI_DIR (no game data in the repo); it skips without it.
#include <gtest/gtest.h>
#include <monkey_dust/world/combat_hit.h>
#include <monkey_dust/world/combat_fight.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

using namespace md::world::combat;

namespace {
BodyState Body() {
    BodyState b;
    b.part_count = 3;
    b.parts[0].vital = true; b.parts[0].size = 2.f;
    b.parts[1].size = 1.f;
    b.parts[2].size = 1.f;
    return b;
}
HitInput Hit(float cut) { HitInput h; h.damage.cut = cut; h.attacker_level = 10; h.defender_block_level = 10; return h; }
}

TEST(CombatHit, SettingsKeysExistInInstall) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    md::fcs::Install inst(dir); md::fcs::LoadOrder order; md::fcs::Error e;
    md::fcs::GameDataContainer gd;
    ASSERT_TRUE(inst.LoadOrderFor(order, e) && gd.Load(order, e));
    HitConstants c;
    ASSERT_TRUE(LoadHitConstants(gd, c));
    EXPECT_FLOAT_EQ(c.attack_chance_factor, 0.05f);
    EXPECT_FLOAT_EQ(c.base_block_chance, 70.f);
    EXPECT_FLOAT_EQ(c.block_reduction_per_level, 1.5f);
    EXPECT_FLOAT_EQ(c.block_increase_per_level, 1.2f);
    EXPECT_FLOAT_EQ(c.ko_point_min, 10.f);
    // Values the load order's mods change (base data: resistance -0.5..0.5, ko point max 99, ko time base 30) only need to stay sane.
    EXPECT_LT(c.resistance_min, 0.f);
    EXPECT_GT(c.resistance_max, 0.f);
    EXPECT_LT(c.ko_point_min, c.ko_point_max);
    EXPECT_GT(c.ko_time_base, 0.f);
}

TEST(CombatHit, BlockChanceRisesAndFallsPerTenLevels) {
    HitConstants c;
    EXPECT_FLOAT_EQ(BlockChancePercent(c, 10, 10), 70.f);
    EXPECT_NEAR(BlockChancePercent(c, 10, 20), 55.f, 1e-4);   // 10 levels lower: -15
    EXPECT_NEAR(BlockChancePercent(c, 20, 10), 82.f, 1e-4);   // 10 levels higher: +12
    EXPECT_GT(BlockChancePercent(c, 30, 10), BlockChancePercent(c, 20, 10));
    EXPECT_LT(BlockChancePercent(c, 0, 20), BlockChancePercent(c, 10, 20));
    EXPECT_LE(BlockChancePercent(c, 99, 0), 95.f);
    EXPECT_FLOAT_EQ(BlockChancePercent(c, 0, 99, 5.f), 5.f);
}

TEST(CombatHit, AttackRollEqualLevelsIsFair) {
    HitConstants c;
    Rng r(1);
    int n = 0;
    for (int i = 0; i < 20000; ++i) { const float a = r.Next01(), b = r.Next01(); n += AttackRoll(c, 30, 30, a, b); }
    EXPECT_NEAR(n / 20000.0, 0.5, 0.02);
}

TEST(CombatHit, ArmourReducesDamageMonotonically) {
    HitConstants c;
    float prev = 1e9f;
    for (float red = 0.f; red <= 1.f; red += 0.25f) {
        HitInput h = Hit(40.f); h.defender_block_level = 0; h.attacker_level = 99;
        Rng r(7);
        HitResult res;
        int guard = 0;
        do {
            BodyState b = Body();
            for (int i = 0; i < 3; ++i) b.parts[i].armour.cut_reduction = red;
            res = ResolveHit(c, h, b, r);
        } while (res.blocked && ++guard < 1000);
        ASSERT_FALSE(res.blocked);
        EXPECT_LE(res.applied.cut, prev);
        prev = res.applied.cut;
    }
}

TEST(CombatHit, ToughnessResistanceMatchesTheBlend) {
    HitConstants c;
    const DamageRecord d{10, 10, 10, 10, 1};
    EXPECT_FLOAT_EQ(ApplyResistance(d, DamageResistance(c, 0.f)).cut, 15.f);   // 1.5x at toughness 0
    EXPECT_FLOAT_EQ(ApplyResistance(d, DamageResistance(c, 1.f)).cut, 5.f);    // 0.5x at toughness 100
}

TEST(CombatHit, KnockoutOnVitalPartDown) {
    HitConstants c;
    BodyState b = Body();
    b.parts[0].health = 5.f; b.parts[1].health = 1000.f; b.parts[2].health = 1000.f;
    b.parts[1].size = b.parts[2].size = 0.f;   // only part 0 can be picked
    HitInput h = Hit(50.f); h.defender_block_level = 0; h.attacker_level = 99;
    Rng r(3);
    HitResult res;
    int guard = 0;
    do { res = ResolveHit(c, h, b, r); } while (res.blocked && ++guard < 1000);
    ASSERT_FALSE(res.blocked);
    EXPECT_EQ(res.part, 0);
    EXPECT_TRUE(res.part_down);
    EXPECT_TRUE(res.knockout);
    EXPECT_FLOAT_EQ(res.knockout_time, KnockoutTime(c, 0.f));
    EXPECT_FLOAT_EQ(res.knockout_time, 4.f * 30.f);
}

TEST(CombatHit, SameSeedSameResult) {
    HitConstants c;
    for (int s = 0; s < 50; ++s) {
        BodyState a = Body(), b = Body();
        Rng ra(s), rb(s);
        const HitResult x = ResolveHit(c, Hit(30.f), a, ra), y = ResolveHit(c, Hit(30.f), b, rb);
        EXPECT_EQ(x.blocked, y.blocked); EXPECT_EQ(x.part, y.part); EXPECT_FLOAT_EQ(x.health_delta, y.health_delta);
    }
}

TEST(CombatHit, NoAllocationTypesArePod) {
    EXPECT_TRUE(std::is_trivially_copyable<BodyState>::value);
    EXPECT_TRUE(std::is_trivially_copyable<HitResult>::value);
}

// The exe attack roll and block chance run in the Unicorn emulator (tools_private/research/combat_oracle.py -> tools/tests/data/exe_combat.tsv).
// The settings the generator wrote are the base data's (0.05, 70, 1.5, 1.2). Attack rows compare the boolean exactly, block rows within 1e-4 relative (the exe's soft cap runs in double).
TEST(CombatHit, MatchesExeEmulatorVectors) {
    std::ifstream in(MD_TOOLS_TEST_DATA "/exe_combat.tsv"); std::string line; int attacks = 0, blocks = 0;
    HitConstants c;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("fn", 0) == 0) continue;
        std::istringstream ss(line); std::string f[6]; for (auto& x : f) std::getline(ss, x, '\t');
        const float x = std::stof(f[1]), y = std::stof(f[2]);
        if (f[0] == "attack") {
            EXPECT_EQ(AttackRoll(c, x, y, std::stof(f[3]), std::stof(f[4])), std::stoi(f[5]) != 0) << line;
            ++attacks;
        } else {
            const float want = std::stof(f[5]);
            EXPECT_NEAR(BlockChancePercent(c, x, y), want, 1e-4f * (want > 1.f ? want : 1.f)) << line;
            ++blocks;
        }
    }
    EXPECT_EQ(attacks, 56);
    EXPECT_EQ(blocks, 11);
}

// Fight loop (DE-364): off by default; a whole exchange at a fixed seed is reproducible and ends in a knock-out.
namespace {
Fighter Duelist(float cut, float level) {
    Fighter f; f.body = Body(); f.weapon.cut = cut; f.attack_level = level; f.block_level = level;
    for (int i = 0; i < 3; ++i) f.body.parts[i].health = 60.f;
    return f;
}
}

TEST(CombatFight, OffByDefaultDoesNothing) {
    HitConstants c; FightConfig cfg; Fighter a = Duelist(30, 20), b = Duelist(30, 20); Rng r(5);
    const FightResult res = RunFight(c, cfg, a, b, r);
    EXPECT_FALSE(res.ran);
    EXPECT_EQ(res.winner, -1);
    EXPECT_FLOAT_EQ(b.body.parts[0].health, 60.f);
    EXPECT_FALSE(cfg.enabled);
}

TEST(CombatFight, WholeExchangeAtFixedSeed) {
    HitConstants c; FightConfig cfg; cfg.enabled = true;
    auto run = [&](FightResult& out, Fighter& a, Fighter& b) { Rng r(12345); a = Duelist(40, 30); b = Duelist(40, 30); out = RunFight(c, cfg, a, b, r); };
    FightResult r1, r2; Fighter a1, b1, a2, b2;
    run(r1, a1, b1); run(r2, a2, b2);
    EXPECT_TRUE(r1.ran);
    ASSERT_GE(r1.winner, 0);
    EXPECT_EQ(r1.winner, r2.winner); EXPECT_EQ(r1.rounds, r2.rounds);
    EXPECT_EQ(r1.hits[0], r2.hits[0]); EXPECT_EQ(r1.hits[1], r2.hits[1]);
    const Fighter& loser = r1.winner == 0 ? b1 : a1;
    EXPECT_TRUE(loser.body.knocked_out);
    EXPECT_GT(r1.hits[r1.winner], 0);
    EXPECT_FALSE((r1.winner == 0 ? a1 : b1).body.knocked_out);
}

TEST(CombatFight, StrongerSideWinsMostDuels) {
    HitConstants c; FightConfig cfg; cfg.enabled = true; int strong = 0;
    for (int s = 0; s < 200; ++s) {
        Fighter a = Duelist(60, 60), b = Duelist(20, 10); Rng r(s);
        if (RunFight(c, cfg, a, b, r).winner == 0) ++strong;
    }
    EXPECT_GT(strong, 150);
}
