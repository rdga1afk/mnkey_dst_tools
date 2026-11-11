#include <gtest/gtest.h>
#include <monkey_dust/platform/job_graph.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/core/fnv.h>

using md::JobBatchDesc;
using md::JobGraph;

// ── Conflicts() ──────────────────────────────────────────────────────────────

TEST(JobGraph, ConflictsNoOverlapIsFalse) {
    JobBatchDesc a; a.reads[0] = 1; a.read_count = 1;
    JobBatchDesc b; b.reads[0] = 2; b.read_count = 1;
    EXPECT_FALSE(JobGraph::Conflicts(a, b));
}

TEST(JobGraph, ConflictsWriteVsReadIsTrue) {
    JobBatchDesc a; a.writes[0] = 1; a.write_count = 1;
    JobBatchDesc b; b.reads[0]  = 1; b.read_count  = 1;
    EXPECT_TRUE(JobGraph::Conflicts(a, b));
    EXPECT_TRUE(JobGraph::Conflicts(b, a));  // symmetric
}

TEST(JobGraph, ConflictsWriteVsWriteIsTrue) {
    JobBatchDesc a; a.writes[0] = 1; a.write_count = 1;
    JobBatchDesc b; b.writes[0] = 1; b.write_count = 1;
    EXPECT_TRUE(JobGraph::Conflicts(a, b));
}

TEST(JobGraph, ConflictsReadVsReadIsFalse) {
    JobBatchDesc a; a.reads[0] = 1; a.read_count = 1;
    JobBatchDesc b; b.reads[0] = 1; b.read_count = 1;
    EXPECT_FALSE(JobGraph::Conflicts(a, b));  // two readers never conflict
}

// ── AddBatch / Run() ─────────────────────────────────────────────────────────

TEST(JobGraph, RunExecutesRegisteredBatchesInOrder) {
    static int s_order[4];
    static int s_count;
    s_count = 0;

    auto fn_a = +[](void*) { s_order[s_count++] = 1; };
    auto fn_b = +[](void*) { s_order[s_count++] = 2; };
    auto fn_c = +[](void*) { s_order[s_count++] = 3; };

    auto& g = JobGraph::Get();
    JobBatchDesc empty;
    ASSERT_TRUE(g.AddBatch("a", empty, fn_a, nullptr));
    ASSERT_TRUE(g.AddBatch("b", empty, fn_b, nullptr));
    ASSERT_TRUE(g.AddBatch("c", empty, fn_c, nullptr));
    g.Run();

    ASSERT_EQ(s_count, 3);
    EXPECT_EQ(s_order[0], 1);
    EXPECT_EQ(s_order[1], 2);
    EXPECT_EQ(s_order[2], 3);
}

TEST(JobGraph, RunClearsBatchesAfterExecution) {
    static int s_calls = 0;
    s_calls = 0;
    auto fn = +[](void*) { ++s_calls; };

    auto& g = JobGraph::Get();
    JobBatchDesc empty;
    g.AddBatch("once", empty, fn, nullptr);
    g.Run();
    EXPECT_EQ(s_calls, 1);

    // A second Run() with nothing (re-)registered should be a no-op.
    g.Run();
    EXPECT_EQ(s_calls, 1);
}

TEST(JobGraph, AddBatchRejectsMaxBatchesOverflow) {
    auto& g = JobGraph::Get();
    JobBatchDesc empty;
    auto noop = +[](void*) {};
    int ok = 0;
    for (int i = 0; i < JobGraph::MAX_BATCHES + 4; ++i) {
        char name[16]; snprintf(name, sizeof(name), "b%d", i);
        if (g.AddBatch(name, empty, noop, nullptr)) ++ok;
    }
    EXPECT_EQ(ok, JobGraph::MAX_BATCHES);
    g.Run();  // drain so later tests in this binary start from a clean slate
}

// Point 3 (concurrency audit): registering a reader BEFORE its writer must
// log MD_LOG_ERROR (escalated from WARNING) — proves the registration-order
// validation loop actually fires, not just that it exists in source.
TEST(JobGraph, RunLogsErrorOnWrongRegistrationOrder) {
    static int s_error_count = 0;
    s_error_count = 0;
    MdLogSetHook([](int level, const char*) { if (level == MD_LOG_ERROR) ++s_error_count; });

    auto& g = JobGraph::Get();
    auto noop = +[](void*) {};
    JobBatchDesc reader; reader.reads[0] = 555; reader.read_count = 1;
    JobBatchDesc writer; writer.writes[0] = 555; writer.write_count = 1;
    // Wrong order: reader registered BEFORE its writer.
    ASSERT_TRUE(g.AddBatch("reader", reader, noop, nullptr));
    ASSERT_TRUE(g.AddBatch("writer", writer, noop, nullptr));
    g.Run();

    MdLogSetHook(nullptr);
    EXPECT_GE(s_error_count, 1);
}

TEST(JobGraph, RunNoErrorOnCorrectRegistrationOrder) {
    static int s_error_count = 0;
    s_error_count = 0;
    MdLogSetHook([](int level, const char*) { if (level == MD_LOG_ERROR) ++s_error_count; });

    auto& g = JobGraph::Get();
    auto noop = +[](void*) {};
    JobBatchDesc writer; writer.writes[0] = 556; writer.write_count = 1;
    JobBatchDesc reader; reader.reads[0]  = 556; reader.read_count  = 1;
    // Correct order: writer registered BEFORE its reader.
    ASSERT_TRUE(g.AddBatch("writer", writer, noop, nullptr));
    ASSERT_TRUE(g.AddBatch("reader", reader, noop, nullptr));
    g.Run();

    MdLogSetHook(nullptr);
    EXPECT_EQ(s_error_count, 0);
}

// ── Wave-based dispatch (task #7 completion) ─────────────────────────────────
// JobSystem::Init() is never called in this test binary (NumWorkers()==0),
// so Run() always falls back to its inline-sequential path regardless of
// wave grouping — these tests exercise the wave[] computation itself
// (which runs unconditionally), not the JobSystem dispatch branch.

TEST(JobGraph, RunWithNoConflictsBothExecute) {
    static bool s_a_ran, s_b_ran;
    s_a_ran = s_b_ran = false;
    auto fn_a = +[](void*) { s_a_ran = true; };
    auto fn_b = +[](void*) { s_b_ran = true; };

    auto& g = JobGraph::Get();
    JobBatchDesc a; a.reads[0] = 10; a.read_count = 1;   // disjoint resource tags
    JobBatchDesc b; b.reads[0] = 20; b.read_count = 1;
    ASSERT_TRUE(g.AddBatch("a", a, fn_a, nullptr));
    ASSERT_TRUE(g.AddBatch("b", b, fn_b, nullptr));
    g.Run();

    EXPECT_TRUE(s_a_ran);
    EXPECT_TRUE(s_b_ran);
}

TEST(JobGraph, RunWithConflictPreservesOrder) {
    static int s_order[2];
    static int s_count;
    s_count = 0;
    auto fn_writer = +[](void*) { s_order[s_count++] = 1; };
    auto fn_reader = +[](void*) { s_order[s_count++] = 2; };

    auto& g = JobGraph::Get();
    JobBatchDesc writer; writer.writes[0] = 99; writer.write_count = 1;
    JobBatchDesc reader; reader.reads[0]  = 99; reader.read_count  = 1;
    ASSERT_TRUE(g.AddBatch("writer", writer, fn_writer, nullptr));
    ASSERT_TRUE(g.AddBatch("reader", reader, fn_reader, nullptr));
    g.Run();

    ASSERT_EQ(s_count, 2);
    EXPECT_EQ(s_order[0], 1);  // writer's wave must run before reader's
    EXPECT_EQ(s_order[1], 2);
}

// Point 2 (concurrency audit): allows_concurrent=false must force
// serialization even when declared tags are fully disjoint (Conflicts()
// would otherwise say these two are independent) — proves the flag is an
// UNCONDITIONAL constraint, not something Conflicts() alone could catch.
TEST(JobGraph, AllowsConcurrentFalseForcesSerializationDespiteDisjointTags) {
    static int s_order[2];
    static int s_count;
    s_count = 0;
    auto fn_first  = +[](void*) { s_order[s_count++] = 1; };
    auto fn_second = +[](void*) { s_order[s_count++] = 2; };

    auto& g = JobGraph::Get();
    JobBatchDesc first;  first.reads[0]  = 111; first.read_count  = 1;
    first.allows_concurrent = false;
    JobBatchDesc second; second.reads[0] = 222; second.read_count = 1;  // disjoint tag

    ASSERT_FALSE(JobGraph::Conflicts(first, second));  // Conflicts() alone sees no dependency

    ASSERT_TRUE(g.AddBatch("first", first, fn_first, nullptr));
    ASSERT_TRUE(g.AddBatch("second", second, fn_second, nullptr));
    g.Run();

    ASSERT_EQ(s_count, 2);
    EXPECT_EQ(s_order[0], 1);  // first's wave must fully complete before second's
    EXPECT_EQ(s_order[1], 2);
}

TEST(JobGraph, AddBatchRejectsNullNameOrFn) {
    auto& g = JobGraph::Get();
    JobBatchDesc empty;
    auto noop = +[](void*) {};
    EXPECT_FALSE(g.AddBatch(nullptr, empty, noop, nullptr));
    EXPECT_FALSE(g.AddBatch("", empty, noop, nullptr));
    EXPECT_FALSE(g.AddBatch("x", empty, nullptr, nullptr));
    g.Run();  // nothing valid was registered, but keep state clean
}
