#include <gtest/gtest.h>
#include <monkey_dust/scripting/flow_graph.h>

// ── Helpers ───────────────────────────────────────────────────────────────────


// Advance the 1Hz gate by pumping enough delta_s. Each call fires once.

// ── P11: Population dynamics ──────────────────────────────────────────────────





// ── P12: Faction raids ────────────────────────────────────────────────────────






// ── P13: motivation_ticks inertia ────────────────────────────────────────────





// ── P16: Vec3 + EntityRef flow vars ─────────────────────────────────────────

struct Batch13FlowVarTest : ::testing::Test {
    FlowGraph fg;
    void SetUp() override { fg.Init(); }
};

TEST_F(Batch13FlowVarTest, B13_SetGetVec3_RoundTrip) {
    uint32_t key = md::fnv1a("spawn_pos");
    fg.SetVarVec3(key, 1.5f, 2.5f, 3.5f);
    float x = 0.f, y = 0.f, z = 0.f;
    fg.GetVarVec3(key, x, y, z);
    EXPECT_FLOAT_EQ(x, 1.5f);
    EXPECT_FLOAT_EQ(y, 2.5f);
    EXPECT_FLOAT_EQ(z, 3.5f);
}

TEST_F(Batch13FlowVarTest, B13_SetGetEntityRef_RoundTrip) {
    uint32_t key = md::fnv1a("target_entity");
    fg.SetVarEntity(key, 0xDEADBEEFu);
    EXPECT_EQ(fg.GetVarEntity(key), 0xDEADBEEFu);
}

TEST_F(Batch13FlowVarTest, B13_GetVarType_Vec3_EntityRef) {
    uint32_t kv3  = md::fnv1a("pos");
    uint32_t kent = md::fnv1a("ent");
    fg.SetVarVec3(kv3, 0.f, 0.f, 0.f);
    fg.SetVarEntity(kent, 42u);
    EXPECT_EQ(fg.GetVarType(kv3),  FlowVarType::Vec3);
    EXPECT_EQ(fg.GetVarType(kent), FlowVarType::EntityRef);
}

TEST_F(Batch13FlowVarTest, B13_GetVarVec3_MissingKey_ReturnsZero) {
    float x = 9.f, y = 9.f, z = 9.f;
    fg.GetVarVec3(md::fnv1a("nonexistent"), x, y, z);
    EXPECT_FLOAT_EQ(x, 0.f);
    EXPECT_FLOAT_EQ(y, 0.f);
    EXPECT_FLOAT_EQ(z, 0.f);
}

TEST_F(Batch13FlowVarTest, B13_GetVarEntity_MissingKey_ReturnsDefault) {
    EXPECT_EQ(fg.GetVarEntity(md::fnv1a("missing"), 99u), 99u);
}

// ── P17: FlowAlias ────────────────────────────────────────────────────────────

struct Batch13AliasTest : ::testing::Test {
    FlowGraph fg;
    void SetUp() override { fg.Init(); }
};

TEST_F(Batch13AliasTest, B13_RegisterResolveAlias_ByHash) {
    uint32_t alias_hash  = md::fnv1a("patrol_start");
    uint32_t target_hash = md::fnv1a("node_001_patrol");
    fg.RegisterAlias(alias_hash, target_hash);
    EXPECT_EQ(fg.ResolveAlias(alias_hash), target_hash);
}

TEST_F(Batch13AliasTest, B13_RegisterResolveAlias_ByName) {
    fg.RegisterAlias("main_entry", "node_spawn_01");
    uint32_t expected = md::fnv1a("node_spawn_01");
    EXPECT_EQ(fg.ResolveAlias(md::fnv1a("main_entry")), expected);
}

TEST_F(Batch13AliasTest, B13_ResolveAlias_NotFound_Identity) {
    uint32_t hash = md::fnv1a("unknown_alias");
    EXPECT_EQ(fg.ResolveAlias(hash), hash);
}

TEST_F(Batch13AliasTest, B13_RegisterAlias_UpdateExisting) {
    fg.RegisterAlias("shortcut", "node_a");
    fg.RegisterAlias("shortcut", "node_b");  // update
    uint32_t expected = md::fnv1a("node_b");
    EXPECT_EQ(fg.ResolveAlias(md::fnv1a("shortcut")), expected);
}
