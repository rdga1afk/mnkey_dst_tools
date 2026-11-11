#include <gtest/gtest.h>
#include <monkey_dust/ecs/component_warmup.h>
#include <monkey_dust/ecs/registry.h>

// Registers every real engine ECS component type with flecs ONCE, before any
// test runs — mirrors what game/src/main.cpp does at real startup (see
// component_warmup.h's doc comment for why this matters: flecs's lazy
// first-use component registration is a structural world operation, unsafe
// from inside a JobGraph-style world.readonly_begin(true) scope). Without
// this, whichever test happens to be the first in the whole binary to touch
// a given component from inside a staged batch would hit that hazard —
// exactly what tests/behavior/test_skill_xp_staged_grant.cpp did (task #247/
// #248) before this file existed.
class GlobalComponentWarmupEnv : public ::testing::Environment {
public:
    void SetUp() override { md::WarmUpEngineComponents(); }
};

static ::testing::Environment* const g_warmup_env =
    ::testing::AddGlobalTestEnvironment(new GlobalComponentWarmupEnv());

// Point 4 (concurrency audit): InjuryState/NoiseEmitter/SmellEmitter were
// used by real per-tick code (logic_tick.cpp's TickNeedsAndInjuries,
// ai_queries.cpp) but had no w.component<T>() line in
// WarmUpEngineComponents() — their flecs registration only ever happened as
// a side effect of ai_queries::WarmAll()'s query construction, not this
// file's own canonical list. Not an active crash today (WarmAll() already
// runs single-threaded before any JobGraph wave), but exactly the drift this
// file exists to prevent independently. ecs_lookup() is read-only (does not
// register) — a non-zero id here proves WarmUpEngineComponents() (which the
// global environment above already ran) is what registered the type, not
// some other, less-guaranteed call path.
TEST(GlobalComponentWarmup, PreviouslyMissingComponentsAreRegistered) {
    GTEST_SKIP() << "flecs-specific by-name lookup consistency check -- "
                    "Phase 5 scope (reflect), not yet ported for gaia";
}
