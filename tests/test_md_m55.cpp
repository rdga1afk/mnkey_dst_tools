#include <gtest/gtest.h>
#include <monkey_dust/ecs/registry.h>
#include <cmath>

// ── M55: SenseSystem real perception ─────────────────────────────────────────
// Tests verify engine-side contracts for perception:
//   - SenseComponent field layout and default values
//   - ViewCone math (cone activation formula)
//   - SenseRegistry API (Load/Find/At)
//   - activation[]/last_activated_ms state machine contracts
//   - threshold_hi/lo semantics
//
// SenseSystemUpdate() is game-side (depends on lcf::IS_PLAYER + WorldTransform)
// and is tested indirectly through contracts.

// ── ViewCone math helpers (mirrored from sense_system.h for engine tests) ────

static constexpr float SENSE_PI     = 3.14159265f;
static constexpr float SENSE_AUDIO_RADIUS_M = 15.0f;


// ── SenseComponent defaults ───────────────────────────────────────────────────






// ── Visual activation math ────────────────────────────────────────────────────








// ── Audio activation math ─────────────────────────────────────────────────────

TEST(M55AudioMath, BeyondRadius_ZeroActivation) {
    float dist = SENSE_AUDIO_RADIUS_M + 0.1f;
    float aud = (dist < SENSE_AUDIO_RADIUS_M) ? (1.0f - dist / SENSE_AUDIO_RADIUS_M) : 0.0f;
    EXPECT_EQ(aud, 0.0f);
}

TEST(M55AudioMath, AtOrigin_MaxActivation) {
    float dist = 0.0f;
    float aud = (dist < SENSE_AUDIO_RADIUS_M) ? (1.0f - dist / SENSE_AUDIO_RADIUS_M) : 0.0f;
    EXPECT_FLOAT_EQ(aud, 1.0f);
}

TEST(M55AudioMath, HalfRadius_HalfActivation) {
    float dist = SENSE_AUDIO_RADIUS_M * 0.5f;
    float aud = (dist < SENSE_AUDIO_RADIUS_M) ? (1.0f - dist / SENSE_AUDIO_RADIUS_M) : 0.0f;
    EXPECT_NEAR(aud, 0.5f, 1e-5f);
}

// ── SenseRegistry ─────────────────────────────────────────────────────────────




// ── Threshold state machine invariants ───────────────────────────────────────





// ── last_known_x/z update contract ───────────────────────────────────────────



// ── SenseType enum bounds ─────────────────────────────────────────────────────





// ── angle wrap invariant ──────────────────────────────────────────────────────

TEST(M55AngleWrap, WrapPI_StaysInRange) {
    auto wrap = [](float a) {
        while (a >  SENSE_PI) a -= 2.0f * SENSE_PI;
        while (a < -SENSE_PI) a += 2.0f * SENSE_PI;
        return a;
    };
    EXPECT_NEAR(wrap(SENSE_PI + 0.01f), -SENSE_PI + 0.01f, 1e-4f);
    EXPECT_NEAR(wrap(-SENSE_PI - 0.01f), SENSE_PI - 0.01f, 1e-4f);
    EXPECT_NEAR(wrap(0.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(wrap(2.0f * SENSE_PI), 0.0f, 1e-4f);
}

TEST(M55AngleWrap, NorthAngle_IsZero) {
    // Target directly north (+Z), NPC facing north: angle diff = 0
    float bearing = atan2f(0.0f, 10.0f);  // atan2(dx=0, dz=10) = 0
    float forward = 0.0f;
    auto wrap = [](float a) {
        while (a >  SENSE_PI) a -= 2.0f * SENSE_PI;
        while (a < -SENSE_PI) a += 2.0f * SENSE_PI;
        return a;
    };
    float diff = fabsf(wrap(bearing - forward)) * (180.0f / SENSE_PI);
    EXPECT_NEAR(diff, 0.0f, 1e-4f);
}

TEST(M55AngleWrap, TargetBehind_Angle180) {
    // Target directly south (-Z), NPC facing north (rot_y=0)
    float bearing = atan2f(0.0f, -10.0f);  // pi
    float forward = 0.0f;
    auto wrap = [](float a) {
        while (a >  SENSE_PI) a -= 2.0f * SENSE_PI;
        while (a < -SENSE_PI) a += 2.0f * SENSE_PI;
        return a;
    };
    float diff = fabsf(wrap(bearing - forward)) * (180.0f / SENSE_PI);
    EXPECT_NEAR(diff, 180.0f, 1e-3f);
}
