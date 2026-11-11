#include <gtest/gtest.h>
#include <monkey_dust/editor/cmd_registry.h>
#include <monkey_dust/ecs/registry.h>

// EDITOR_AUTOMATION_PLAN_v1.md Phase 0 falsification probe + Phase 1.1-1.2
// real design.
//
// Phase 0 hypothesis (still covered below): a host-owned command registry
// survives a panels hot-reload cycle with no stale pointers — register ->
// unregister-by-module -> re-register, no cross-module leakage, no stale
// array holes.
//
// Phase 1.1-1.2 adds: CmdArgs/CmdSchema validation (reject on mismatch
// rather than calling with wrong types) and registry-mediated undo — a
// command that registers undo+redo fns produces a DispatchOutcome carrying
// everything the caller needs to push onto ITS OWN CommandStack (engine/
// never touches that stack directly — see cmd_registry.h's file header).
// These tests exercise that hand-off directly against a local stand-in for
// CommandStack::Command (same {execute, undo, data[64]} shape), since the
// real CommandStack lives in tools/ (editor-only) and isn't linked here.

namespace {

int g_calls_a = 0;
int g_calls_b = 0;
CmdResult CmdA(const CmdArgs&, MdRegistry&, uint8_t[64]) { ++g_calls_a; return CmdResult{}; }
CmdResult CmdB(const CmdArgs&, MdRegistry&, uint8_t[64]) { ++g_calls_b; return CmdResult{}; }

constexpr uint32_t kModuleA = 1;
constexpr uint32_t kModuleB = 2;

CmdSchema NoArgSchema(bool no_undo = false) {
    CmdSchema s;
    s.count = 0;
    s.no_undo = no_undo;
    return s;
}

CmdArgs NoArgs() { return CmdArgs{}; }

} // namespace

TEST(EditorCmdRegistry, RegisterAndDispatch) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    g_calls_a = 0;
    MdRegistry& ecs = MdRegistry::Get();

    ASSERT_TRUE(reg.Register("Delete Selected", CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA));
    EXPECT_EQ(reg.Count(), 1);
    DispatchOutcome out = reg.Dispatch("Delete Selected", NoArgs(), ecs);
    EXPECT_TRUE(out.result.ok);
    EXPECT_EQ(g_calls_a, 1);
}

TEST(EditorCmdRegistry, DispatchUnknownNameFailsCleanly) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    MdRegistry& ecs = MdRegistry::Get();
    DispatchOutcome out = reg.Dispatch("Does Not Exist", NoArgs(), ecs);
    EXPECT_FALSE(out.result.ok);
}

TEST(EditorCmdRegistry, DispatchArgCountMismatchFailsCleanly) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    MdRegistry& ecs = MdRegistry::Get();
    reg.Register("Delete Selected", CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA);

    CmdArgs bad;
    bad.count = 1;
    bad.values[0].type = CmdArgType::I64;
    bad.values[0].i64 = 42;

    DispatchOutcome out = reg.Dispatch("Delete Selected", bad, ecs);
    EXPECT_FALSE(out.result.ok) << "schema expects 0 args; dispatch with 1 must be rejected";
}

TEST(EditorCmdRegistry, DispatchArgTypeMismatchFailsCleanly) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    MdRegistry& ecs = MdRegistry::Get();

    CmdSchema s;
    s.count = 1;
    s.args[0] = { "amount", CmdArgType::I64 };
    reg.Register("Set Amount", CmdA, nullptr, nullptr, s, kModuleA);

    CmdArgs wrong_type;
    wrong_type.count = 1;
    wrong_type.values[0].type = CmdArgType::F64; // schema wants I64
    wrong_type.values[0].f64 = 1.5;

    DispatchOutcome out = reg.Dispatch("Set Amount", wrong_type, ecs);
    EXPECT_FALSE(out.result.ok);
}

TEST(EditorCmdRegistry, ReRegisterSameNameUpdatesInPlace) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    g_calls_a = 0;
    g_calls_b = 0;
    MdRegistry& ecs = MdRegistry::Get();

    reg.Register("Delete Selected", CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA);
    reg.Register("Delete Selected", CmdB, nullptr, nullptr, NoArgSchema(true), kModuleA); // same name, same module
    EXPECT_EQ(reg.Count(), 1) << "re-registering the same name must not grow the array";

    reg.Dispatch("Delete Selected", NoArgs(), ecs);
    EXPECT_EQ(g_calls_a, 0);
    EXPECT_EQ(g_calls_b, 1) << "the updated execute fn must be the one called";
}

// The exact Phase 0 acceptance sequence: spawn/select/exec (simulated by
// direct dispatch) -> hot-reload cycle (UnregisterModule then re-Register,
// simulating the panels .so's shutdown->dlclose->dlopen->init) -> exec
// again. No crash, no stale entry, registry count identical before/after.
TEST(EditorCmdRegistry, SurvivesHotReloadCycle) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    g_calls_a = 0;
    MdRegistry& ecs = MdRegistry::Get();

    // First "load": panels .so registers its one command.
    reg.Register("Delete Selected", CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA);
    ASSERT_EQ(reg.Count(), 1);
    ASSERT_TRUE(reg.Dispatch("Delete Selected", NoArgs(), ecs).result.ok);
    ASSERT_EQ(g_calls_a, 1);

    // Simulated hot-reload: shutdown purges the module, dlclose/dlopen
    // happens (out of scope for this unit test, already-proven subsystem),
    // then init re-registers.
    reg.UnregisterModule(kModuleA);
    EXPECT_EQ(reg.Count(), 0) << "unregister must leave no stale entries behind";
    EXPECT_FALSE(reg.Dispatch("Delete Selected", NoArgs(), ecs).result.ok)
        << "dispatching after unload must fail cleanly, not call a freed pointer";

    reg.Register("Delete Selected", CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA);
    EXPECT_EQ(reg.Count(), 1) << "count must match pre-reload state, not accumulate";
    EXPECT_TRUE(reg.Dispatch("Delete Selected", NoArgs(), ecs).result.ok);
    EXPECT_EQ(g_calls_a, 2);
}

// Cross-module isolation: unregistering one module must not touch another
// module's commands — the real multi-command state Phase 1 will have.
TEST(EditorCmdRegistry, UnregisterModuleDoesNotTouchOtherModules) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    g_calls_a = 0;
    g_calls_b = 0;
    MdRegistry& ecs = MdRegistry::Get();

    reg.Register("Cmd A", CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA);
    reg.Register("Cmd B", CmdB, nullptr, nullptr, NoArgSchema(true), kModuleB);
    ASSERT_EQ(reg.Count(), 2);

    reg.UnregisterModule(kModuleA);
    EXPECT_EQ(reg.Count(), 1);
    EXPECT_FALSE(reg.Dispatch("Cmd A", NoArgs(), ecs).result.ok);
    EXPECT_TRUE(reg.Dispatch("Cmd B", NoArgs(), ecs).result.ok);
    EXPECT_EQ(g_calls_b, 1);
}

TEST(EditorCmdRegistry, MaxCommandsRejectsOverflowGracefully) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();

    char names[EditorCmdRegistry::MAX_COMMANDS + 1][16];
    for (int i = 0; i < EditorCmdRegistry::MAX_COMMANDS; ++i) {
        snprintf(names[i], sizeof(names[i]), "cmd_%d", i);
        ASSERT_TRUE(reg.Register(names[i], CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA));
    }
    EXPECT_EQ(reg.Count(), EditorCmdRegistry::MAX_COMMANDS);

    snprintf(names[EditorCmdRegistry::MAX_COMMANDS], sizeof(names[0]), "overflow");
    EXPECT_FALSE(reg.Register(names[EditorCmdRegistry::MAX_COMMANDS], CmdA, nullptr, nullptr,
                               NoArgSchema(true), kModuleA))
        << "registering past MAX_COMMANDS must fail, not overflow the fixed array";
    EXPECT_EQ(reg.Count(), EditorCmdRegistry::MAX_COMMANDS);

    reg.Clear();
}

// ── Phase 1.2: registry-mediated undo ───────────────────────────────────────
namespace {

// A tiny stand-in for the real CommandStack::Command (tools/editor/
// editor_history.h) — same {execute, undo, data[64]} shape, deliberately
// not the real type since that header is tools-only and not linked here.
// This proves DispatchOutcome hands the caller everything it needs with no
// adaptation beyond a straight field copy.
struct FakeUndoSlot {
    CmdRedoApplyFn execute = nullptr;
    CmdUndoApplyFn undo = nullptr;
    uint8_t data[64] = {};
};

int g_counter = 0;

// "Increment" command: adds args.values[0].i64 to g_counter, captures the
// PRE-execute value into undo_data so undo can restore it exactly (not just
// subtract, proving undo_data round-trips real state, not just an inverse
// delta).
CmdResult CmdIncrement(const CmdArgs& args, MdRegistry&, uint8_t undo_data[64]) {
    int64_t prev = g_counter;
    memcpy(undo_data, &prev, sizeof(prev));
    g_counter += static_cast<int>(args.values[0].i64);
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "counter=%d", g_counter);
    return r;
}

void UndoIncrement(void* data, MdRegistry&) {
    int64_t prev = 0;
    memcpy(&prev, data, sizeof(prev));
    g_counter = static_cast<int>(prev);
}

// Redo re-applies from the captured PRE-value + re-derives the same delta
// is not enough info alone — for this test, redo simply re-runs the known
// effect (set back to prev, then apply the fixed delta this test uses),
// proving redo operates on the captured blob, not a re-parsed CmdArgs.
constexpr int64_t kIncrementDelta = 5;
void RedoIncrement(void* data, MdRegistry&) {
    int64_t prev = 0;
    memcpy(&prev, data, sizeof(prev));
    g_counter = static_cast<int>(prev + kIncrementDelta);
}

} // namespace

TEST(EditorCmdRegistry, NoUndoCommandProducesNoOutcomeUndo) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    MdRegistry& ecs = MdRegistry::Get();

    reg.Register("Delete Selected", CmdA, nullptr, nullptr, NoArgSchema(true), kModuleA);
    DispatchOutcome out = reg.Dispatch("Delete Selected", NoArgs(), ecs);
    EXPECT_TRUE(out.result.ok);
    EXPECT_FALSE(out.has_undo) << "NO_UNDO schema must never produce undo/redo fn pointers";
}

TEST(EditorCmdRegistry, UndoCapableCommandRoundTripsThroughFakeStack) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    MdRegistry& ecs = MdRegistry::Get();
    g_counter = 10;

    CmdSchema s;
    s.count = 1;
    s.args[0] = { "delta", CmdArgType::I64 };
    s.no_undo = false;
    reg.Register("Increment", CmdIncrement, UndoIncrement, RedoIncrement, s, kModuleA);

    CmdArgs args;
    args.count = 1;
    args.values[0].type = CmdArgType::I64;
    args.values[0].i64 = kIncrementDelta;

    DispatchOutcome out = reg.Dispatch("Increment", args, ecs);
    ASSERT_TRUE(out.result.ok);
    ASSERT_TRUE(out.has_undo) << "command registered undo+redo and is not NO_UNDO";
    EXPECT_EQ(g_counter, 15);

    // Caller (tools/ in the real system) builds its own stack entry from
    // the outcome with a plain field copy — no adaptation.
    FakeUndoSlot slot;
    slot.execute = out.redo;
    slot.undo = out.undo;
    memcpy(slot.data, out.undo_data, sizeof(slot.data));

    slot.undo(slot.data, ecs);
    EXPECT_EQ(g_counter, 10) << "undo must restore the captured pre-execute value";

    slot.execute(slot.data, ecs);
    EXPECT_EQ(g_counter, 15) << "redo must reproduce the effect from the captured blob";
}

TEST(EditorCmdRegistry, UndoDataIsPerDispatchNotSharedAcrossCalls) {
    auto& reg = EditorCmdRegistry::Get();
    reg.Clear();
    MdRegistry& ecs = MdRegistry::Get();
    g_counter = 0;

    CmdSchema s;
    s.count = 1;
    s.args[0] = { "delta", CmdArgType::I64 };
    reg.Register("Increment", CmdIncrement, UndoIncrement, RedoIncrement, s, kModuleA);

    CmdArgs args;
    args.count = 1;
    args.values[0].type = CmdArgType::I64;
    args.values[0].i64 = kIncrementDelta;

    DispatchOutcome first = reg.Dispatch("Increment", args, ecs);  // 0 -> 5, captures prev=0
    DispatchOutcome second = reg.Dispatch("Increment", args, ecs); // 5 -> 10, captures prev=5

    int64_t prev_first = 0, prev_second = 0;
    memcpy(&prev_first, first.undo_data, sizeof(prev_first));
    memcpy(&prev_second, second.undo_data, sizeof(prev_second));
    EXPECT_EQ(prev_first, 0);
    EXPECT_EQ(prev_second, 5) << "each dispatch's undo_data must reflect its own pre-state";
}
