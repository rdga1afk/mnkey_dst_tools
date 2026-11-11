#include <gtest/gtest.h>
#include <monkey_dust/platform/job_graph.h>
#include <monkey_dust/platform/job_system.h>
#include <monkey_dust/ecs/md_registry.h>

using md::JobBatchDesc;
using md::JobGraph;

// Phase 2.1 (audit): stress JobGraph::Run()'s wave dispatch with two
// independent, genuinely disjoint-tag batches (StressA/StressB) iterating
// their own query many times across many Run() calls, then checking the
// final accumulated values are exactly correct — a real correctness
// regression test for wave dispatch under either backend, not a synthetic
// one. test_job_graph.cpp's own tests never actually exercise real worker
// threads (JobSystem::Init() is never called there); this file is the only
// one in the suite that does.
//
// Under flecs: stresses the REAL concurrent dispatch path — readonly_begin
// (true) + per-batch get_stage() + MdRegistryStageScope + query.iter(stage)
// .each() — the exact mechanism that replaced the SIGSEGV-in-ecs_iter_fini/
// ecs_query_next crash from 2026-07-11 (see job_graph.h's doc comment).
// Under ThreadSanitizer this reproduces the exact hazard class the fix
// addresses if stage-routing ever regresses (e.g. someone changes MdEach
// to call query.each() unconditionally instead of respecting
// t_stage_override).
//
// Under gaia: NOT a concurrency stress anymore (task #47/#48, 2026-09-08)
// — JobGraph::Run()'s gaia branch always dispatches every wave
// sequentially now, after a TSan-confirmed real data race was found in
// gaia's own World::lock()/unlock() (non-atomic m_structuralChangesLocked,
// touched by every .each() call regardless of query contents — see
// job_graph.cpp's doc comment on the gaia branch for the full writeup).
// This test still runs under gaia and still has real value: it validates
// that sequential multi-wave dispatch with disjoint batches produces
// exactly correct accumulated results, i.e. a regression test against
// dropped/duplicated/misordered batch execution, not against a data race
// that no longer has a code path to occur through.
//
// Build+run under TSan: -DMD_SANITIZE=tsan (see CMakeLists.txt's
// MD_SANITIZE option). Originally verified against a hand-rolled
// -fsanitize=thread build before that CMake option existed.

namespace {
struct StressA { float v = 0.f; };
struct StressB { float v = 0.f; };

gaia::ecs::Query& QueryA() {
    static auto q = MdRegistry::Get().Raw().query().all<StressA&>();
    return q;
}
gaia::ecs::Query& QueryB() {
    static auto q = MdRegistry::Get().Raw().query().all<StressB&>();
    return q;
}

void JobFn_StressA(void*) {
    MdEach(QueryA(), [](StressA& a) { a.v += 1.f; });
}
void JobFn_StressB(void*) {
    MdEach(QueryB(), [](StressB& b) { b.v += 1.f; });
}
}  // namespace

TEST(JobGraphStress, ConcurrentDisjointBatchesManyIterationsNoRace) {
    static constexpr int kEntities  = 64;
    static constexpr int kIterations = 300;

    // Pre-warm both queries once, single-threaded, BEFORE JobSystem::Init()
    // — job_graph.h's own probe findings: on-the-fly query construction
    // against a stage crashes; a query built once against the main world
    // before readonly_begin() is the only confirmed-safe shape.
    (void)QueryA();
    (void)QueryB();

    auto& reg = MdRegistry::Get();
    MdEntity entities[kEntities];
    for (int i = 0; i < kEntities; ++i) {
        entities[i] = reg.Create();
        reg.Handle(entities[i]).emplace<StressA>(StressA{(float)i});
        reg.Handle(entities[i]).emplace<StressB>(StressB{(float)i});
    }

    // Deliberately never call Shutdown(): JobSystem::Shutdown() (job_system.cpp)
    // joins worker threads and destroys mtx_/cv_work_/cv_done_/cv_cap_ but does
    // NOT reset num_workers_ to 0 — a real gap noticed while writing this test,
    // out of this audit's declared scope to fix (JobSystem lifecycle isn't one
    // of the 4 concurrency-audit points). Calling Init() then Shutdown() here
    // would leave num_workers_ > 0 with the mutex/conditions already destroyed
    // — any LATER test in this same process whose JobGraph wave takes the
    // `NumWorkers() > 0` branch would call Submit()/Flush() against destroyed
    // SDL sync primitives (this binary runs all tests in one process, not one
    // per ctest invocation — confirmed by tools/md_ctl test's single combined
    // PASSED/FAILED summary line, gtest's own format for one process running
    // many tests). Leaving the real workers alive and idle for the rest of
    // the process is harmless (they just block on cv_work_) and is exactly
    // this project's real production lifecycle (Init() once at startup,
    // Shutdown() once at exit — see job_system.h's own usage doc comment).
    auto& js = JobSystem::Get();
    if (js.NumWorkers() == 0) js.Init();
    ASSERT_GT(js.NumWorkers(), 0) << "need real worker threads to exercise the concurrent path";

    auto& g = JobGraph::Get();
    for (int iter = 0; iter < kIterations; ++iter) {
        JobBatchDesc da; da.writes[0] = 0xA57E55A1; da.write_count = 1;  // disjoint tags —
        JobBatchDesc db; db.writes[0] = 0xA57E55B2; db.write_count = 1;  // must co-schedule
        ASSERT_TRUE(g.AddBatch("stressA", da, JobFn_StressA, nullptr));
        ASSERT_TRUE(g.AddBatch("stressB", db, JobFn_StressB, nullptr));
        g.Run();
    }

    for (int i = 0; i < kEntities; ++i) {
        EXPECT_FLOAT_EQ(reg.Handle(entities[i]).get_mut<StressA>().v, (float)i + kIterations);
        EXPECT_FLOAT_EQ(reg.Handle(entities[i]).get_mut<StressB>().v, (float)i + kIterations);
        reg.Destroy(entities[i]);  // don't leak entities into later tests' world-wide queries
    }
}
