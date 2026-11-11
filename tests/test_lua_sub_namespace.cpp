#include <gtest/gtest.h>
#include <monkey_dust/scripting/lua_system.h>

extern "C" {
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
}

// EDITOR_AUTOMATION_PLAN_v1.md Phase 2: LuaSystem::RegisterSubNamespaceFunction
// is the mechanism behind md.editor.*/md.ecs.* (real nested tables) — a
// deliberate, separate registration path from RegisterNamespaceFunction's
// flat md.foo() convention (every pre-existing binding). Tests the
// mechanism in isolation, without any editor/ECS dependency.

namespace {

int g_last_arg = 0;

int TestFn(lua_State* L) {
    g_last_arg = (int)lua_tointeger(L, 1);
    lua_pushinteger(L, g_last_arg * 2);
    return 1;
}

} // namespace

class LuaSubNamespaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        LuaSystem::Get().Init(nullptr);
        g_last_arg = 0;
    }
    void TearDown() override {
        LuaSystem::Get().Shutdown();
    }
};

TEST_F(LuaSubNamespaceTest, CreatesNestedTableAndIsCallable) {
    LuaSystem::Get().RegisterSubNamespaceFunction("editor", "test_fn", TestFn);
    ASSERT_TRUE(LuaSystem::Get().Exec("result = md.editor.test_fn(21)"));
    EXPECT_EQ(g_last_arg, 21);
}

TEST_F(LuaSubNamespaceTest, TwoSubTablesCoexist) {
    LuaSystem::Get().RegisterSubNamespaceFunction("editor", "fn_a", TestFn);
    LuaSystem::Get().RegisterSubNamespaceFunction("ecs", "fn_b", TestFn);
    ASSERT_TRUE(LuaSystem::Get().Exec(
        "assert(type(md.editor) == 'table')\n"
        "assert(type(md.ecs) == 'table')\n"
        "assert(type(md.editor.fn_a) == 'function')\n"
        "assert(type(md.ecs.fn_b) == 'function')\n"
    ));
}

TEST_F(LuaSubNamespaceTest, MultipleFieldsInSameSubTable) {
    LuaSystem::Get().RegisterSubNamespaceFunction("editor", "one", TestFn);
    LuaSystem::Get().RegisterSubNamespaceFunction("editor", "two", TestFn);
    ASSERT_TRUE(LuaSystem::Get().Exec(
        "assert(type(md.editor.one) == 'function')\n"
        "assert(type(md.editor.two) == 'function')\n"
    ));
}

TEST_F(LuaSubNamespaceTest, DoesNotClobberExistingFlatNamespaceFunction) {
    // RegisterNamespaceFunction (flat md.foo) and RegisterSubNamespaceFunction
    // (nested md.sub.foo) must coexist on the SAME md table without either
    // wiping the other's fields out.
    LuaSystem::Get().RegisterNamespaceFunction("flat_fn", TestFn);
    LuaSystem::Get().RegisterSubNamespaceFunction("editor", "nested_fn", TestFn);
    ASSERT_TRUE(LuaSystem::Get().Exec(
        "assert(type(md.flat_fn) == 'function')\n"
        "assert(type(md.editor.nested_fn) == 'function')\n"
    ));
}
