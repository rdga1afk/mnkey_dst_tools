#include <gtest/gtest.h>
#include <monkey_dust/ecs/md_registry.h>

namespace {
struct RegTestPos  { float x = 0.f, y = 0.f; };
struct RegTestFlag  { bool set = false; };
struct RegTestCacheProbe { int v = 0; };
} // namespace

TEST(MdRegistry, CreateDestroyValid) {
    auto& reg = MdRegistry::Get();
    MdEntity e = reg.Create();
    EXPECT_TRUE(reg.Valid(e));
    reg.Destroy(e);
    EXPECT_FALSE(reg.Valid(e));
}

TEST(MdRegistry, EmplaceGetTryGet) {
    auto& reg = MdRegistry::Get();
    MdEntity e = reg.Create();

    reg.Handle(e).emplace<RegTestPos>(1.f, 2.f);
    EXPECT_FLOAT_EQ(reg.Handle(e).get_mut<RegTestPos>().x, 1.f);
    EXPECT_FLOAT_EQ(reg.Handle(e).get_mut<RegTestPos>().y, 2.f);

    RegTestPos* p = reg.Handle(e).try_get_mut<RegTestPos>();
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->x, 1.f);

    EXPECT_EQ(reg.Handle(e).try_get_mut<RegTestFlag>(), nullptr);

    reg.Destroy(e);
}

TEST(MdRegistry, AllOfAnyOf) {
    auto& reg = MdRegistry::Get();
    MdEntity e = reg.Create();
    reg.Handle(e).emplace<RegTestPos>();

    EXPECT_TRUE((reg.Handle(e).has<RegTestPos>()));
    bool all_of_both = (reg.Handle(e).has<RegTestPos>() && reg.Handle(e).has<RegTestFlag>());
    EXPECT_FALSE(all_of_both);
    bool any_of_both = (reg.Handle(e).has<RegTestPos>() || reg.Handle(e).has<RegTestFlag>());
    EXPECT_TRUE(any_of_both);

    reg.Destroy(e);
}

TEST(MdRegistry, RemoveReplacePatch) {
    auto& reg = MdRegistry::Get();
    MdEntity e = reg.Create();
    reg.Handle(e).emplace<RegTestPos>(1.f, 1.f);

    reg.Replace<RegTestPos>(e, 5.f, 6.f);
    EXPECT_FLOAT_EQ(reg.Handle(e).get_mut<RegTestPos>().x, 5.f);

    reg.Patch<RegTestPos>(e, [](RegTestPos& pos) { pos.x += 1.f; });
    EXPECT_FLOAT_EQ(reg.Handle(e).get_mut<RegTestPos>().x, 6.f);

    reg.Remove<RegTestPos>(e);
    EXPECT_FALSE((reg.Handle(e).has<RegTestPos>()));

    reg.Destroy(e);
}

TEST(MdRegistry, GetOrEmplaceCreatesThenReuses) {
    auto& reg = MdRegistry::Get();
    MdEntity e = reg.Create();

    if (!reg.Handle(e).has<RegTestPos>()) reg.Handle(e).emplace<RegTestPos>(3.f, 4.f);
    auto& first = reg.Handle(e).get_mut<RegTestPos>();
    EXPECT_FLOAT_EQ(first.x, 3.f);

    if (!reg.Handle(e).has<RegTestPos>()) reg.Handle(e).emplace<RegTestPos>(9.f, 9.f);
    auto& second = reg.Handle(e).get_mut<RegTestPos>();
    EXPECT_FLOAT_EQ(second.x, 3.f);  // already existed — args ignored, same as entt

    reg.Destroy(e);
}

TEST(MdRegistry, ViewEachIteratesEmplaced) {
    auto& reg = MdRegistry::Get();
    MdEntity a = reg.Create();
    MdEntity b = reg.Create();
    reg.Handle(a).emplace<RegTestFlag>(RegTestFlag{true});
    reg.Handle(b).emplace<RegTestFlag>(RegTestFlag{false});

    int seen = 0;
    static auto q_p3_test_md_registry_1 = reg.Raw().query().all<RegTestFlag&>();
    MdEach(q_p3_test_md_registry_1, [&](MdEntity /*e*/, RegTestFlag& f) {
        if (f.set) ++seen;
        f.set = true;
    });
    EXPECT_EQ(seen, 1);
    EXPECT_TRUE(reg.Handle(a).get_mut<RegTestFlag>().set);
    EXPECT_TRUE(reg.Handle(b).get_mut<RegTestFlag>().set);

    reg.Destroy(a);
    reg.Destroy(b);
}

// B3.3: View<T...>() caches its underlying view per component signature
// (prep for flecs::query<T...>, which is meant to be built once and
// reused, not rebuilt every call). These two tests exist specifically to
// prove that caching stays correct under the two ways it could go wrong:
// missing entities added after the cache was primed, and stale entities
// surviving a Clear().
TEST(MdRegistry, ViewCacheSeesEntitiesAddedAfterFirstCall) {
    auto& reg = MdRegistry::Get();

    // First call primes whatever cache exists for this signature.
    MdEntity a = reg.Create();
    reg.Handle(a).emplace<RegTestCacheProbe>(1);
    int seen = 0;
    static auto q_p3_test_md_registry_2 = reg.Raw().query().all<RegTestCacheProbe&>();
    MdEach(q_p3_test_md_registry_2, [&](RegTestCacheProbe&) { ++seen; });
    EXPECT_EQ(seen, 1);

    // Second call — must still see an entity created after the first call.
    MdEntity b = reg.Create();
    reg.Handle(b).emplace<RegTestCacheProbe>(2);
    seen = 0;
    static auto q_p3_test_md_registry_3 = reg.Raw().query().all<RegTestCacheProbe&>();
    MdEach(q_p3_test_md_registry_3, [&](RegTestCacheProbe&) { ++seen; });
    EXPECT_EQ(seen, 2);

    reg.Destroy(a);
    reg.Destroy(b);
}

TEST(MdRegistry, ViewCacheSurvivesClearAndRepopulate) {
    auto& reg = MdRegistry::Get();

    MdEntity a = reg.Create();
    reg.Handle(a).emplace<RegTestCacheProbe>(1);
    int seen = 0;
    static auto q_p3_test_md_registry_4 = reg.Raw().query().all<RegTestCacheProbe&>();
    MdEach(q_p3_test_md_registry_4, [&](RegTestCacheProbe&) { ++seen; });
    EXPECT_GE(seen, 1);

    reg.Clear();  // wipes every entity/component registry-wide

    MdEntity c = reg.Create();
    reg.Handle(c).emplace<RegTestCacheProbe>(3);
    seen = 0;
    static auto q_p3_test_md_registry_5 = reg.Raw().query().all<RegTestCacheProbe&>();
    MdEach(q_p3_test_md_registry_5, [&](RegTestCacheProbe&) { ++seen; });
    EXPECT_EQ(seen, 1);  // only the post-clear entity should be visible

    reg.Destroy(c);
}

// Point 1 (concurrency audit): Clear() is not stage-aware — these two tests
// prove the debug-build guard fires exactly when a JobGraph-style stage
// override is active, and stays silent otherwise (no false positives on the
// overwhelming majority of Clear() calls, which run with no stage active).
TEST(MdRegistry, ClearLogsErrorWhenStageActive) {
    static int s_error_count = 0;
    s_error_count = 0;
    MdLogSetHook([](int level, const char*) { if (level == MD_LOG_ERROR) ++s_error_count; });

    // gaia has no separate "stage" object the way flecs does (Phase 1
    // stub, see md_registry.h) -- MdRegistryStageScope's gaia constructor
    // takes the World reference directly, just to set t_stage_override
    // for this exact guard-check, not for any real routing.
    auto& raw = Registry::Get();
    {
        MdRegistryStageScope scope(raw);
        MdRegistry::Get().Clear();
    }

    MdLogSetHook(nullptr);
    EXPECT_GE(s_error_count, 1);
}

TEST(MdRegistry, ClearNoErrorWhenNoStageActive) {
    static int s_error_count = 0;
    s_error_count = 0;
    MdLogSetHook([](int level, const char*) { if (level == MD_LOG_ERROR) ++s_error_count; });

    MdRegistry::Get().Clear();  // normal call path — no stage override set

    MdLogSetHook(nullptr);
    EXPECT_EQ(s_error_count, 0);
}

TEST(MdRegistry, RawEscapeHatchMatchesRegistryGet) {
    // MdRegistry wraps the SAME world singleton as Registry::Get() — an
    // entity created via one is visible/valid via the other.
    MdEntity e(MdRegistry::Get().Raw().add());
    EXPECT_TRUE(Registry::Get().valid(e.Raw()));
    Registry::Get().del(e.Raw());
    EXPECT_FALSE(MdRegistry::Get().Valid(e));
}
