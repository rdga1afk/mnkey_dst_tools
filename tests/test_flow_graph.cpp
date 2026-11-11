#include <gtest/gtest.h>
#include <monkey_dust/scripting/flow_graph.h>
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/core/fnv.h>

static MdWorldRef s_reg;

// s_reg is a standalone local World, not Registry::Get() -- can't reuse
// MdRegistry::Create()/Handle(e).destruct() (those operate on the global
// registry, a different World instance). Mirrors md_registry.h's own
// per-backend Create()/destruct() pattern directly against s_reg instead.
static MdEntity MakeTestEntity() { return MdEntity(s_reg.add()); }
static void DestroyTestEntity(MdEntity e) { s_reg.del(e.Raw()); }

// ── Init ─────────────────────────────────────────────────────────────────────

TEST(FlowGraph, InitClearsState) {
    FlowGraph fg;
    fg.Init();
    EXPECT_EQ(fg.node_count, 0);
    EXPECT_EQ(fg.conn_count, 0);
    EXPECT_EQ(fg.var_count, 0);
    EXPECT_EQ(fg.pending_head, 0);
    EXPECT_EQ(fg.pending_tail, 0);
    EXPECT_EQ(fg.action_count, 0);
}

// ── Variables ────────────────────────────────────────────────────────────────

TEST(FlowGraph, SetVarGetVar) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("hp");
    fg.SetVar(k, 100.f);
    EXPECT_FLOAT_EQ(fg.GetVar(k), 100.f);
}

TEST(FlowGraph, GetVarMissingReturnsDefault) {
    FlowGraph fg; fg.Init();
    EXPECT_FLOAT_EQ(fg.GetVar(0xDEADBEEFu, -7.f), -7.f);
}

TEST(FlowGraph, SetVarUpdatesExistingNoNewEntry) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("x");
    fg.SetVar(k, 1.f);
    fg.SetVar(k, 2.f);
    EXPECT_EQ(fg.var_count, 1);
    EXPECT_FLOAT_EQ(fg.GetVar(k), 2.f);
}

TEST(FlowGraph, VarTableCapacity) {
    FlowGraph fg; fg.Init();
    for (int i = 0; i < FlowGraph::MAX_VARS; ++i)
        fg.SetVar(static_cast<uint32_t>(i + 1), (float)i);
    EXPECT_EQ(fg.var_count, FlowGraph::MAX_VARS);
    fg.SetVar(0xFFFFFFFFu, 999.f);  // silently dropped
    EXPECT_EQ(fg.var_count, FlowGraph::MAX_VARS);
}

// ── Ring buffer ───────────────────────────────────────────────────────────────

TEST(FlowGraph, FireTriggerEnqueues) {
    FlowGraph fg; fg.Init();
    EXPECT_EQ(fg.pending_head, fg.pending_tail);
    fg.FireTrigger(md::fnv1a("start"), 0.0);
    EXPECT_NE(fg.pending_head, fg.pending_tail);
}

TEST(FlowGraph, FireTriggerRingBufferFull) {
    FlowGraph fg; fg.Init();
    // Ring buffer holds MAX_PENDING-1 items (one slot wasted for full/empty distinction)
    for (int i = 0; i < FlowGraph::MAX_PENDING - 1; ++i)
        fg.FireTrigger(static_cast<uint32_t>(i + 1), 999.0);  // future → won't fire
    int count = 0;
    int h = fg.pending_head, t = fg.pending_tail;
    while (h != t) { h = (h + 1) & (FlowGraph::MAX_PENDING - 1); ++count; }
    EXPECT_EQ(count, FlowGraph::MAX_PENDING - 1);

    // One more must be silently dropped
    fg.FireTrigger(0xFFFFu, 999.0);
    count = 0;
    h = fg.pending_head; t = fg.pending_tail;
    while (h != t) { h = (h + 1) & (FlowGraph::MAX_PENDING - 1); ++count; }
    EXPECT_EQ(count, FlowGraph::MAX_PENDING - 1);
}

// ── Tick / node dispatch ──────────────────────────────────────────────────────

TEST(FlowGraph, FutureTriggerDoesNotFireYet) {
    FlowGraph fg; fg.Init();
    MdEntity ctx = MakeTestEntity();

    fg.nodes[0] = { md::fnv1a("evt"), 0, 0, 0 };
    fg.node_count = 1;
    fg.FireTrigger(md::fnv1a("evt"), 10.0);  // fires at t=10

    fg.Tick(0.0, ctx, s_reg);  // now=0 → nothing processed
    // Ring still has the pending trigger
    EXPECT_NE(fg.pending_head, fg.pending_tail);

    DestroyTestEntity(ctx);
}

TEST(FlowGraph, ActionNodeCallsCallback) {
    FlowGraph fg; fg.Init();
    MdEntity ctx = MakeTestEntity();

    static int calls = 0;
    calls = 0;
    uint32_t aid = md::fnv1a("my_action");
    fg.RegisterAction(aid, [](uint32_t, double, MdEntity, MdWorldRef&) {
        ++calls;
    });
    fg.nodes[0] = { aid, 2, 0, 0 };  // type=2 = Action
    fg.node_count = 1;

    fg.FireTrigger(aid, 0.0);
    fg.Tick(1.0, ctx, s_reg);
    EXPECT_EQ(calls, 1);

    DestroyTestEntity(ctx);
}

TEST(FlowGraph, EventNodePropagatesConnection) {
    FlowGraph fg; fg.Init();
    MdEntity ctx = MakeTestEntity();

    uint32_t ev_id  = md::fnv1a("source_event");
    uint32_t act_id = md::fnv1a("sink_action");

    static int sink_calls = 0;
    sink_calls = 0;
    fg.RegisterAction(act_id, [](uint32_t, double, MdEntity, MdWorldRef&) {
        ++sink_calls;
    });

    fg.nodes[0] = { ev_id,  0, 1, 0 };  // Event, 1 connection
    fg.nodes[1] = { act_id, 2, 0, 0 };  // Action
    fg.node_count = 2;

    fg.conns[0] = { ev_id, act_id, 0, 0, {0, 0} };
    fg.conn_count = 1;

    fg.FireTrigger(ev_id, 0.0);
    fg.Tick(1.0, ctx, s_reg);
    // Event fires → propagates → Action callback
    EXPECT_EQ(sink_calls, 1);

    DestroyTestEntity(ctx);
}

TEST(FlowGraph, ActionTableCapacity) {
    FlowGraph fg; fg.Init();
    for (int i = 0; i < FlowGraph::MAX_ACTIONS; ++i)
        fg.RegisterAction(static_cast<uint32_t>(i + 1), nullptr);
    EXPECT_EQ(fg.action_count, FlowGraph::MAX_ACTIONS);
    fg.RegisterAction(0xFFFFu, nullptr);  // silently dropped
    EXPECT_EQ(fg.action_count, FlowGraph::MAX_ACTIONS);
}

TEST(FlowGraph, RegisterActionOverwritesExisting) {
    FlowGraph fg; fg.Init();
    MdEntity ctx = MakeTestEntity();

    static int counter = 0;
    counter = 0;
    uint32_t id = md::fnv1a("overwrite_test");

    fg.RegisterAction(id, [](uint32_t, double, MdEntity, MdWorldRef&) {
        counter += 10;
    });
    fg.RegisterAction(id, [](uint32_t, double, MdEntity, MdWorldRef&) {
        counter += 1;
    });
    EXPECT_EQ(fg.action_count, 1);  // same id → overwrite, not new entry

    fg.nodes[0] = { id, 2, 0, 0 };
    fg.node_count = 1;
    fg.FireTrigger(id, 0.0);
    fg.Tick(1.0, ctx, s_reg);
    EXPECT_EQ(counter, 1);  // second registration wins

    DestroyTestEntity(ctx);
}

// ── M45: Typed FlowVar ────────────────────────────────────────────────────────

TEST(FlowGraph, SetGetVarBool) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("is_alert");
    fg.SetVarBool(k, true);
    EXPECT_TRUE(fg.GetVarBool(k));
    EXPECT_EQ(fg.GetVarType(k), FlowVarType::Bool);
    // GetVar (float coercion): true → 1.0f
    EXPECT_FLOAT_EQ(fg.GetVar(k), 1.f);
}

TEST(FlowGraph, SetGetVarBoolFalse) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("door_open");
    fg.SetVarBool(k, false);
    EXPECT_FALSE(fg.GetVarBool(k));
    EXPECT_FLOAT_EQ(fg.GetVar(k), 0.f);
}

TEST(FlowGraph, SetGetVarInt) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("kill_count");
    fg.SetVarInt(k, 42);
    EXPECT_EQ(fg.GetVarInt(k), 42);
    EXPECT_EQ(fg.GetVarType(k), FlowVarType::Int);
    EXPECT_FLOAT_EQ(fg.GetVar(k), 42.f);
}

TEST(FlowGraph, SetGetVarStr) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("item_name");
    fg.SetVarStr(k, "sword");
    const char* s = fg.GetVarStr(k);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "sword");
    EXPECT_EQ(fg.GetVarType(k), FlowVarType::Str);
}

TEST(FlowGraph, StrVarTruncatesAt15Chars) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("long_str");
    fg.SetVarStr(k, "1234567890123456789");  // 19 chars → truncated to 15
    const char* s = fg.GetVarStr(k);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(strlen(s), 15u);
}

TEST(FlowGraph, StrVarCoercesToFloatNonEmpty) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("tag");
    fg.SetVarStr(k, "boss");
    EXPECT_FLOAT_EQ(fg.GetVar(k), 1.f);  // non-empty → truthy
    fg.SetVarStr(k, "");
    EXPECT_FLOAT_EQ(fg.GetVar(k), 0.f);  // empty → falsy
}

TEST(FlowGraph, IntCoercesToBool) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("flag");
    fg.SetVarInt(k, 5);
    EXPECT_TRUE(fg.GetVarBool(k));
    fg.SetVarInt(k, 0);
    EXPECT_FALSE(fg.GetVarBool(k));
}

TEST(FlowGraph, BoolCoercesToInt) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("ready");
    fg.SetVarBool(k, true);
    EXPECT_EQ(fg.GetVarInt(k), 1);
    fg.SetVarBool(k, false);
    EXPECT_EQ(fg.GetVarInt(k), 0);
}

TEST(FlowGraph, GetVarTypeMissingKeyReturnsFloat) {
    FlowGraph fg; fg.Init();
    EXPECT_EQ(fg.GetVarType(0xDEADu), FlowVarType::Float);
}

TEST(FlowGraph, GetVarStrMissingKeyReturnsDefault) {
    FlowGraph fg; fg.Init();
    EXPECT_STREQ(fg.GetVarStr(0xDEADu, "fallback"), "fallback");
}

TEST(FlowGraph, UpdateBoolVarNoNewEntry) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("toggle");
    fg.SetVarBool(k, true);
    fg.SetVarBool(k, false);
    EXPECT_EQ(fg.var_count, 1);
    EXPECT_FALSE(fg.GetVarBool(k));
}

TEST(FlowGraph, TypeChangesOnReassign) {
    FlowGraph fg; fg.Init();
    uint32_t k = md::fnv1a("morphed");
    fg.SetVar(k, 3.14f);
    EXPECT_EQ(fg.GetVarType(k), FlowVarType::Float);
    fg.SetVarInt(k, 7);
    EXPECT_EQ(fg.GetVarType(k), FlowVarType::Int);
    EXPECT_EQ(fg.var_count, 1);  // same slot, type changed
}
