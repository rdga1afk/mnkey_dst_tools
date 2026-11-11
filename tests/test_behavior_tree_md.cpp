#include <gtest/gtest.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/scripting/flow_graph.h>

// Minimal EngineContext for BT tick calls


// ── C11: SequenceStateless ────────────────────────────────────────────────────



// ── C12: TimerStart OnlyIncrease ─────────────────────────────────────────────



// ── C13: FrameFlag ────────────────────────────────────────────────────────────



// ── C14: WeightedSelector ────────────────────────────────────────────────────



// ── C15: AwarenessCheck ───────────────────────────────────────────────────────



// ── C16: AlertnessCheck ───────────────────────────────────────────────────────


// ── C17: MoodCheck ────────────────────────────────────────────────────────────



// ── C18: RoleSystem ───────────────────────────────────────────────────────────





// ── C19: WithdrawState ────────────────────────────────────────────────────────




// ── C20: FlowDurableTrigger ───────────────────────────────────────────────────

TEST(FlowDurableTriggerTest, C20_AcquireAndRelease) {
    FlowGraph fg;
    fg.Init();

    uint8_t slot = fg.AcquireDurable(0xDEADu, 5.0f);
    ASSERT_NE(slot, FLOW_INVALID_DURABLE);
    EXPECT_EQ(fg.durable[slot].ref_count, 1u);
    EXPECT_FLOAT_EQ(fg.durable[slot].duration, 5.0f);

    fg.Release(slot);
    EXPECT_EQ(fg.durable[slot].ref_count, 0u);
}

TEST(FlowDurableTriggerTest, C20_AddRefPreventsEarlyFree) {
    FlowGraph fg;
    fg.Init();

    uint8_t slot = fg.AcquireDurable(0xBEEFu, 2.0f);
    fg.AddRef(slot);                       // ref_count = 2
    EXPECT_EQ(fg.durable[slot].ref_count, 2u);

    fg.Release(slot);                      // ref_count = 1
    EXPECT_EQ(fg.durable[slot].ref_count, 1u);
    EXPECT_GT(fg.durable[slot].duration, 0.f); // still alive

    fg.Release(slot);                      // ref_count = 0 → freed
    EXPECT_EQ(fg.durable[slot].ref_count, 0u);
}

TEST(FlowDurableTriggerTest, C20_PoolFullReturnsInvalid) {
    FlowGraph fg;
    fg.Init();

    // Fill all slots
    for (int i = 0; i < FlowGraph::MAX_DURABLE_TRIGGERS; ++i)
        fg.AcquireDurable(static_cast<uint32_t>(i), 10.0f);

    uint8_t slot = fg.AcquireDurable(0xCAFEu, 1.0f);
    EXPECT_EQ(slot, FLOW_INVALID_DURABLE);
}
