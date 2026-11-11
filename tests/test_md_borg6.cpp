#include <gtest/gtest.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>

// ── БОРГ-6: TransformSoA slot serialization ───────────────────────────────────
// Verifies the save-v5+ slot serialization contract:
//   Serialize: GetSlotForEntity(e) → NpcRecord::transform_soa_slot
//   Deserialize: AssignSlot(e, slot, x, z, faction) → ntr.slot = slot
// This means slot layout is preserved exactly across a save/load cycle.

struct Borg6Fixture : ::testing::Test {
    TransformSoA& soa = TransformSoA::Get();
    MdRegistry& reg = MdRegistry::Get();

    void SetUp() override { soa.Init(); }
    void TearDown() override { soa.Init(); }
};

// ── Basic Alloc / GetSlotForEntity roundtrip ──────────────────────────────────

TEST_F(Borg6Fixture, Alloc_ReturnsValidSlot) {
    MdEntity e = reg.Create();
    uint32_t slot = soa.Alloc(e, 10.f, 20.f, 1u);
    EXPECT_NE(slot, TransformSoA::INVALID_SLOT);
    reg.Destroy(e);
}

TEST_F(Borg6Fixture, GetSlotForEntity_FindsAllocatedSlot) {
    MdEntity e = reg.Create();
    uint32_t slot = soa.Alloc(e, 5.f, 7.f, 2u);
    EXPECT_EQ(soa.GetSlotForEntity(e), slot);
    reg.Destroy(e);
}

TEST_F(Borg6Fixture, GetSlotForEntity_UnknownEntity_ReturnsInvalid) {
    MdEntity e = reg.Create();
    EXPECT_EQ(soa.GetSlotForEntity(e), TransformSoA::INVALID_SLOT);
    reg.Destroy(e);
}

// ── AssignSlot: exact slot restoration (the serialization path) ───────────────

TEST_F(Borg6Fixture, AssignSlot_RestoresExactSlot) {
    MdEntity e = reg.Create();
    const uint32_t target_slot = 42u;
    soa.AssignSlot(e, target_slot, 3.f, 9.f, 0u);

    EXPECT_EQ(soa.GetSlotForEntity(e), target_slot);
    EXPECT_FLOAT_EQ(soa.px[target_slot], 3.f);
    EXPECT_FLOAT_EQ(soa.pz[target_slot], 9.f);
    EXPECT_EQ(soa.faction[target_slot], 0u);
    reg.Destroy(e);
}

TEST_F(Borg6Fixture, AssignSlot_SetsActivCount) {
    MdEntity e = reg.Create();
    soa.AssignSlot(e, 99u, 0.f, 0.f, 1u);
    EXPECT_GE(soa.active_count, 100);
    reg.Destroy(e);
}

TEST_F(Borg6Fixture, AssignSlot_OutOfRange_NoOp) {
    MdEntity e = reg.Create();
    int before = soa.active_count;
    soa.AssignSlot(e, TransformSoA::MAX_SLOTS + 1u, 0.f, 0.f, 0u);
    EXPECT_EQ(soa.active_count, before);
    reg.Destroy(e);
}

// ── slot_to_entity reverse map ────────────────────────────────────────────────

TEST_F(Borg6Fixture, AssignSlot_SlotToEntityReverseMap) {
    MdEntity e = reg.Create();
    soa.AssignSlot(e, 7u, 1.f, 2.f, 3u);
    EXPECT_EQ(soa.slot_to_entity[7], e);
    reg.Destroy(e);
}

TEST_F(Borg6Fixture, Alloc_SlotToEntityReverseMap) {
    MdEntity e = reg.Create();
    uint32_t slot = soa.Alloc(e, 0.f, 0.f, 0u);
    EXPECT_EQ(soa.slot_to_entity[slot], e);
    reg.Destroy(e);
}

// ── Save/load simulation: Alloc → GetSlotForEntity → AssignSlot ──────────────

TEST_F(Borg6Fixture, SaveLoadCycle_ExactSlotRestored) {
    MdEntity e1 = reg.Create();
    MdEntity e2 = reg.Create();

    // Simulate original session: allocate slots
    uint32_t slot1 = soa.Alloc(e1, 10.f, 20.f, 1u);
    uint32_t slot2 = soa.Alloc(e2, 30.f, 40.f, 2u);
    EXPECT_NE(slot1, slot2);

    // Serialize: record slot assignments (what NpcRecord stores)
    uint32_t saved_slot1 = soa.GetSlotForEntity(e1);
    uint32_t saved_slot2 = soa.GetSlotForEntity(e2);
    EXPECT_EQ(saved_slot1, slot1);
    EXPECT_EQ(saved_slot2, slot2);

    // Simulate load: fresh SoA + two new entities
    soa.Init();
    MdEntity r1 = reg.Create();
    MdEntity r2 = reg.Create();

    // Restore using AssignSlot (world_serializer.cpp:435-437)
    soa.AssignSlot(r1, saved_slot1, 10.f, 20.f, 1u);
    soa.AssignSlot(r2, saved_slot2, 30.f, 40.f, 2u);

    EXPECT_EQ(soa.GetSlotForEntity(r1), saved_slot1);
    EXPECT_EQ(soa.GetSlotForEntity(r2), saved_slot2);
    EXPECT_FLOAT_EQ(soa.px[saved_slot1], 10.f);
    EXPECT_FLOAT_EQ(soa.px[saved_slot2], 30.f);

    reg.Destroy(r1);
    reg.Destroy(r2);
}

// ── Init clears all slots ─────────────────────────────────────────────────────

TEST_F(Borg6Fixture, Init_ClearsActivCount) {
    MdEntity e = reg.Create();
    soa.Alloc(e, 1.f, 2.f, 0u);
    EXPECT_GT(soa.active_count, 0);
    soa.Init();
    EXPECT_EQ(soa.active_count, 0);
    reg.Destroy(e);
}

TEST_F(Borg6Fixture, Init_GetSlotForEntity_ReturnsInvalid) {
    MdEntity e = reg.Create();
    soa.Alloc(e, 1.f, 2.f, 0u);
    soa.Init();
    EXPECT_EQ(soa.GetSlotForEntity(e), TransformSoA::INVALID_SLOT);
    reg.Destroy(e);
}

// ── SlotCount accessor ────────────────────────────────────────────────────────

TEST_F(Borg6Fixture, SlotCount_ReflectsAllocations) {
    EXPECT_EQ(soa.SlotCount(), 0);
    MdEntity e1 = reg.Create();
    MdEntity e2 = reg.Create();
    soa.Alloc(e1, 0.f, 0.f, 0u);
    soa.Alloc(e2, 0.f, 0.f, 0u);
    EXPECT_EQ(soa.SlotCount(), 2);
    reg.Destroy(e1);
    reg.Destroy(e2);
}

// ── INVALID_SLOT constant ─────────────────────────────────────────────────────

TEST(Borg6Constants, InvalidSlotValue) {
    static_assert(TransformSoA::INVALID_SLOT == 0xFFFFFFFFu,
                  "INVALID_SLOT must be 0xFFFFFFFF for NpcRecord sentinel");
}

TEST(Borg6Constants, MaxSlotsCoversNpcCount) {
    static_assert(TransformSoA::MAX_SLOTS >= 65536,
                  "MAX_SLOTS must fit all possible active NPCs");
}
