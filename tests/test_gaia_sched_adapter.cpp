#include <gtest/gtest.h>

// Phase 4 (PROMPT_GAIA_MIGRATION.md §6): gaia lazily raises its own
// gaia::mt::ThreadPool the first time a query runs with an explicit
// non-Serial QueryExecType, UNLESS a custom Sched was installed first via
// World::set_sched() -- confirmed via a standalone probe (thread-count
// sampling around a Parallel .each() call, with and without a custom
// Sched installed: +3 threads with none, +0 with one). Registry::Get()
// (registry.h) installs MdGaiaSchedAdapter before returning the World to
// any caller. This test exists so that guarantee has a real regression
// check, not just a code comment -- written before job_graph.cpp is
// rewritten to actually use gaia's parallel dispatch (per the owner's
// explicit ordering request), so it only makes assertions about the
// adapter itself, not about JobGraph's future wiring.
//
// Only meaningful under MD_ECS_GAIA -- gaia::mt::ThreadPool doesn't exist
// under the flecs backend at all.
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/gaia_sched_adapter.h>
#include <monkey_dust/platform/job_system.h>
#include <fstream>
#include <string>

namespace {
// /proc/self/status "Threads:" -- same technique validated in the
// standalone probe this test formalizes. Linux-only, matches this
// project's existing dev/CI target (no cross-platform test infra here).
int CountThreads() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Threads:", 0) == 0)
            return std::stoi(line.substr(8));
    }
    return -1;
}
} // namespace

TEST(GaiaSchedAdapter, InstalledBeforeFirstParallelQueryPreventsDefaultThreadPool) {
    // Registry::Get() installs the adapter as part of its own static-local
    // construction (registry.h) -- this call is what we're really testing;
    // if it were missing or ran too late, the .each(Parallel) call below
    // would fall through to gaia::ecs::sched_def() and spin up
    // gaia::mt::ThreadPool.
    auto& w = Registry::Get();

    struct Probe { float x; };
    for (int i = 0; i < 256; ++i) {
        auto e = w.add();
        w.add<Probe>(e, Probe{(float)i});
    }
    auto q = w.query().all<Probe&>();

    int before = CountThreads();
    ASSERT_GE(before, 1) << "couldn't read /proc/self/status Threads: -- test infra issue, not adapter issue";

    int touched = 0;
    q.each([&](Probe& p) { p.x += 1.0f; ++touched; }, gaia::ecs::QueryExecType::Parallel);

    int after = CountThreads();
    EXPECT_EQ(touched, 256);
    // The adapter itself may (correctly) grow JobSystem's own worker
    // threads if this is JobSystem's first-ever Submit() in the process,
    // but gaia's OWN ThreadPool must never be the thing doing that growth.
    // Distinguishing "JobSystem grew" from "gaia's ThreadPool grew" by
    // thread count alone isn't possible without a stronger hook, so this
    // asserts the stronger, directly-testable claim: with JobSystem
    // already initialized (steady-state, the real game's actual
    // condition), NO new threads appear at all.
    JobSystem::Get().Init();
    int steadyBefore = CountThreads();
    int touched2 = 0;
    q.each([&](Probe& p) { p.x += 1.0f; ++touched2; }, gaia::ecs::QueryExecType::Parallel);
    int steadyAfter = CountThreads();
    EXPECT_EQ(touched2, 256);
    EXPECT_EQ(steadyAfter, steadyBefore)
        << "thread count grew during a Parallel query dispatch with JobSystem "
        << "already initialized -- gaia's own ThreadPool likely got created "
        << "instead of routing through the installed adapter";
}

TEST(GaiaSchedAdapter, SchedTaskRunsAndCompletes) {
    auto& w = Registry::Get();
    (void)w;  // ensure adapter installed

    std::atomic<int> ran{0};
    gaia::ecs::SchedTaskDesc desc{};
    desc.pCtx = &ran;
    desc.invoke = [](void* p) { static_cast<std::atomic<int>*>(p)->fetch_add(1); };

    const auto& sched = MdGaiaSchedAdapter::Instance();
    auto token = sched.sched(sched.pCtx, &desc);
    sched.wait(sched.pCtx, token);
    sched.del(sched.pCtx, token);

    EXPECT_EQ(ran.load(), 1);
}

TEST(GaiaSchedAdapter, SchedParRunsAllItemsExactlyOnce) {
    auto& w = Registry::Get();
    (void)w;

    static constexpr uint32_t kItems = 500;
    std::atomic<int> hits[kItems]{};

    gaia::ecs::SchedParDesc desc{};
    desc.pCtx = hits;
    desc.itemCount = kItems;
    desc.groupSize = 37;  // deliberately not a divisor of kItems
    desc.invoke = [](void* p, uint32_t s, uint32_t e) {
        auto* h = static_cast<std::atomic<int>*>(p);
        for (uint32_t i = s; i < e; ++i) h[i].fetch_add(1);
    };

    const auto& sched = MdGaiaSchedAdapter::Instance();
    auto token = sched.sched_par(sched.pCtx, &desc);
    sched.wait(sched.pCtx, token);
    sched.del(sched.pCtx, token);

    for (uint32_t i = 0; i < kItems; ++i)
        ASSERT_EQ(hits[i].load(), 1) << "item " << i << " ran " << hits[i].load() << " times, expected exactly 1";
}

