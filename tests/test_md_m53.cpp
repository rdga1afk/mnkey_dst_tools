#include <gtest/gtest.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <cmath>

// ── M53: FrameFlagDispatcher invariants ──────────────────────────────────────
// DispatchFrameFlags (game/src/ai/frame_flag_dispatch.h) cannot be tested
// directly from the engine test suite (it includes game-side NavAgent/Combat).
// These tests verify the engine-side contracts M53 relies on:
//   – correct lcf::* bit indices for lifecycle flags
//   – correct ff::* bit indices for dispatch
//   – AgentBlackboard K_TARGET roundtrip
//   – SenseComponent last_known_x/z accessible
//   – lcflags.bits is uint64_t (bit ops compile)

// ── lcflags constants ─────────────────────────────────────────────────────────




// ── ff:: dispatch table ───────────────────────────────────────────────────────








// ── AgentBlackboard K_TARGET roundtrip ───────────────────────────────────────



// ── SenseComponent last_known_x/z ────────────────────────────────────────────



// ── rot_y facing convention ───────────────────────────────────────────────────
// DispatchFrameFlags uses atan2f(dx, dz) to orient WorldTransform::rot_y.
// Verify the convention: North(+Z) = 0, East(+X) = PI/2.

TEST(M53Dispatch, FaceAngle_NorthIsZero) {
    float dx = 0.0f, dz = 10.0f;  // target is north (+Z from NPC)
    float rot = atan2f(dx, dz);
    EXPECT_NEAR(rot, 0.0f, 1e-5f);
}

TEST(M53Dispatch, FaceAngle_EastIsHalfPi) {
    float dx = 10.0f, dz = 0.0f;  // target is east (+X from NPC)
    float rot = atan2f(dx, dz);
    EXPECT_NEAR(rot, 3.14159265f / 2.0f, 1e-5f);
}

// ── lcflags.bits type ─────────────────────────────────────────────────────────



// ── DispatchFrameFlags: ECS despawn contract ──────────────────────────────────
// Verify that setting SHOULD_DESPAWN + calling a simulated dispatch removes entity.


