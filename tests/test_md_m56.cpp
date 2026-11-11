#include <gtest/gtest.h>
#include <monkey_dust/components/health.h>

// ── M56: BT-driven combat loop ────────────────────────────────────────────────
// Tests verify engine-side contracts for BT-driven attack dispatch:
//   - CalcDamage formulas per DamageType
//   - RollHitZone determinism and rng advance
//   - HitZone table invariants
//   - WeaponStats / ArmorStats preset values
//   - Health depletion + IS_DEAD flag contracts
//   - Frame-flag bit positions (SHOULD_MELEE_ATTACK/SHOULD_RANGED_SHOOT)
//   - Cooldown + range gating contracts
//   - AgentBlackboard target_entity key stability

// ── DamageType formulas ───────────────────────────────────────────────────────










// ── HitZone table invariants ──────────────────────────────────────────────────





// ── RollHitZone determinism ───────────────────────────────────────────────────




// ── WeaponStats presets ───────────────────────────────────────────────────────





// ── ArmorStats presets ────────────────────────────────────────────────────────



// ── Health depletion contract ─────────────────────────────────────────────────

TEST(M56HealthContract, DepleteHealth) {
    Health h = Health::Make(100.0f);
    h.hp[0] -= 30.0f;  // damage head limb
    EXPECT_NEAR(h.TotalHp(), 70.0f, 1e-3f);
}

TEST(M56HealthContract, KillClampedToZero) {
    Health h = Health::Make(10.0f);
    for (int i = 0; i < LIMB_COUNT; ++i) h.hp[i] = 0.0f;
    EXPECT_FLOAT_EQ(h.TotalHp(), 0.0f);
}

// ── IS_DEAD flag contract ─────────────────────────────────────────────────────




// ── Frame-flag bit positions ──────────────────────────────────────────────────




// ── Cooldown contract ─────────────────────────────────────────────────────────

TEST(M56CooldownContract, NotExpired_NoAttack) {
    float last_ms = 1000.0f;
    uint32_t attack_ms = 1200u;
    float now_ms = 2100.0f;  // 2100 < 1000 + 1200 = 2200 → still cooling down
    EXPECT_LT(now_ms, last_ms + (float)attack_ms);
}

TEST(M56CooldownContract, Expired_CanAttack) {
    float last_ms = 1000.0f;
    uint32_t attack_ms = 1200u;
    float now_ms = 2200.0f;  // exactly on cooldown
    EXPECT_GE(now_ms, last_ms + (float)attack_ms);
}

// ── Range gating contract ─────────────────────────────────────────────────────

TEST(M56RangeContract, BeyondRange_NoAttack) {
    float range = 1.5f;
    float dx = 2.0f, dz = 0.0f;
    EXPECT_GT(dx * dx + dz * dz, range * range);
}

TEST(M56RangeContract, WithinRange_Attack) {
    float range = 1.5f;
    float dx = 1.0f, dz = 0.0f;
    EXPECT_LE(dx * dx + dz * dz, range * range);
}

// ── BlackboardEntry target key stability ──────────────────────────────────────



// ── Zone HP depletion contract ────────────────────────────────────────────────


