#include <gtest/gtest.h>

// Phase 2 (prompt_/PROMPT_GAIA_MIGRATION.md §4): GATE 2 -- the mandatory
// regression test that reproduces the B3.4 bug SHAPE found in
// world_init.cpp::SpawnFaction (see that file's comment above the
// WorldTransform emplace): a T& fetched via Handle(e).get_mut<T>() is
// held across SEVERAL subsequent structural changes (emplace<U>() calls
// for unrelated component types) on the SAME entity, then dereferenced
// again. Under a Table-only backend this is a real bug -- Table storage
// relocates an entity's ENTIRE row on every archetype change, so the
// held reference silently dangles. GAIA_STORAGE(Sparse) (now on
// WorldTransform/AIAgent/CharBodyState/BodyBaseline/AgentBlackboard,
// see world_transform.h's doc comment) gives the payload a stable,
// entity-indexed address that archetype moves do not touch -- this test
// makes that mechanically checkable instead of just conventionally
// avoided (the actual reason this whole migration exists).
//
// GATE 2's own text: "тест проходить під ON, і його Table-варіант
// падає (доводить, що тест справді щось ловить)". TableTwinTransform
// below is that Table-variant proof: identical shape to WorldTransform,
// deliberately WITHOUT GAIA_STORAGE(Sparse), run through the exact same
// hold-across-structural-changes sequence -- its address MUST move, or
// the WorldTransform test above isn't actually proving anything.
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>

namespace {
// Same shape as WorldTransform, no GAIA_STORAGE -- defaults to Table.
struct TableTwinTransform {
    float    x, y, z;
    float    rot_y;
    uint32_t slot = 0xFFFFFFFFu;
};

// Unrelated NON-empty types purely to force archetype changes
// (SpawnFaction's real shape interleaves WorldTransform with
// LimbHealth/Renderable/AIAgent/... additions -- 5 here is comparable
// density, not load-bearing as an exact count). Deliberately not empty
// tags: Handle(e).emplace<T>() constructs T{} and passes it through
// gaia's add<T>(entity, value) -- for a truly zero-size T that hits the
// same "can't write an empty component's value" rejection gaia's own
// chunk-column accessors static_assert on, a different call path this
// test doesn't need to exercise (MdEach's own empty-tag handling is
// covered by test sites already using MdManagedTag, e.g.
// editor_hierarchy.cpp) -- real structural-change components in this
// codebase are never empty tags either.
struct StructuralTagA { int _pad = 0; };
struct StructuralTagB { int _pad = 0; };
struct StructuralTagC { int _pad = 0; };
struct StructuralTagD { int _pad = 0; };
struct StructuralTagE { int _pad = 0; };

template<typename Transform>
void HoldRefAcrossStructuralChanges(MdEntity e, const void*& addr_before, const void*& addr_after) {
    auto& reg = MdRegistry::Get();
    auto& tr = reg.Handle(e).template get_mut<Transform>();
    addr_before = &tr;
    tr.slot = 42u;

    reg.Handle(e).template emplace<StructuralTagA>();
    reg.Handle(e).template emplace<StructuralTagB>();
    reg.Handle(e).template emplace<StructuralTagC>();
    reg.Handle(e).template emplace<StructuralTagD>();
    reg.Handle(e).template emplace<StructuralTagE>();

    auto& tr2 = reg.Handle(e).template get_mut<Transform>();
    addr_after = &tr2;
}
} // namespace

TEST(GaiaSparseStorage, WorldTransformAddressStableAcrossStructuralChanges) {
    auto& reg = MdRegistry::Get();
    MdEntity e = reg.Create();
    reg.Handle(e).emplace<WorldTransform>(1.f, 2.f, 3.f, 0.f);

    const void* addr_before = nullptr;
    const void* addr_after  = nullptr;
    HoldRefAcrossStructuralChanges<WorldTransform>(e, addr_before, addr_after);

    EXPECT_EQ(addr_before, addr_after)
        << "GAIA_STORAGE(Sparse) must keep WorldTransform's address stable "
           "across structural changes on the same entity -- this is the "
           "entire reason Phase 2 exists.";
    EXPECT_EQ(reg.Handle(e).get_mut<WorldTransform>().slot, 42u)
        << "value must also survive intact, not just the address";

    reg.Destroy(e);
}

TEST(GaiaSparseStorage, TableVariantAddressMovesProvingTheTestCatchesSomething) {
    auto& reg = MdRegistry::Get();
    MdEntity e = reg.Create();
    reg.Handle(e).emplace<TableTwinTransform>(1.f, 2.f, 3.f, 0.f);

    const void* addr_before = nullptr;
    const void* addr_after  = nullptr;
    HoldRefAcrossStructuralChanges<TableTwinTransform>(e, addr_before, addr_after);

    EXPECT_NE(addr_before, addr_after)
        << "Table-storage address SHOULD move across structural changes -- "
           "if this assertion fails, the WorldTransform test above is not "
           "actually proving anything (GATE 2's own falsifiability "
           "requirement).";

    reg.Destroy(e);
}

