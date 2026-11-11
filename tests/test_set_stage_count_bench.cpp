#include <gtest/gtest.h>
#include <monkey_dust/ecs/registry.h>
#include <chrono>
#include <cstdio>

// gaia-ecs migration Phase 4: this benchmark is flecs-stage-specific
// (set_stage_count/readonly_begin/get_stage). Not applicable under gaia --
// job_graph.cpp's gaia branch needs no stage-equivalent at all (confirmed
// via probe: disjoint-component concurrent queries against the same World
// are safe as long as each query is pre-warmed once before threads start,
// same precondition MdEach/ai_queries.h's `static auto q = ...` convention
// already guarantees everywhere in this codebase). See job_graph.cpp's
// MD_ECS_GAIA branch comment for the full finding.
TEST(SetStageCountBench, SkippedUnderGaiaBackend) {
    GTEST_SKIP() << "flecs-stage-specific benchmark -- gaia's job_graph.cpp "
                    "branch needs no stage mechanism at all, see its comment";
}
