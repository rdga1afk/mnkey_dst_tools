#include <gtest/gtest.h>
#include <monkey_dust/scripting/lua_system.h>

// Each test calls Init/Shutdown to get a fresh Lua state.
// LuaSystem is a singleton so we must Shutdown between tests.

// ── Lifecycle ─────────────────────────────────────────────────────────────────
TEST(LuaSandbox, InitShutdownLifecycle) {
    LuaSystem& ls = LuaSystem::Get();
    EXPECT_TRUE(ls.Init(nullptr));
    EXPECT_TRUE(ls.IsReady());
    ls.Shutdown();
    EXPECT_FALSE(ls.IsReady());
}

// ── Instruction limit ─────────────────────────────────────────────────────────
TEST(LuaSandbox, InstructionLimitKillsInfiniteLoop) {
    LuaSystem& ls = LuaSystem::Get();
    ASSERT_TRUE(ls.Init(nullptr));

    // A tight loop that will exceed the 50 000 instruction limit
    bool ok = ls.Exec("local i=0 while true do i=i+1 end");
    EXPECT_FALSE(ok); // hook should have triggered an error

    ls.Shutdown();
}

// ── Memory limit ─────────────────────────────────────────────────────────────
TEST(LuaSandbox, NormalScriptStaysWithinMemoryLimit) {
    LuaSystem& ls = LuaSystem::Get();
    ASSERT_TRUE(ls.Init(nullptr));

    EXPECT_TRUE(ls.Exec("local t = {} for i=1,1000 do t[i]=i end"));
    EXPECT_LT(ls.MemUsed(), 8u * 1024u * 1024u);

    ls.Shutdown();
}

// ── Sandbox restrictions ──────────────────────────────────────────────────────
TEST(LuaSandbox, IoLibraryNotAccessible) {
    LuaSystem& ls = LuaSystem::Get();
    ASSERT_TRUE(ls.Init(nullptr));

    // io should be nil — assert passes when nil, errors when not nil
    EXPECT_TRUE(ls.Exec("assert(io == nil, 'io should be blocked')"));

    ls.Shutdown();
}

TEST(LuaSandbox, OsLibraryNotAccessible) {
    LuaSystem& ls = LuaSystem::Get();
    ASSERT_TRUE(ls.Init(nullptr));

    EXPECT_FALSE(ls.Exec("assert(os ~= nil)"));

    ls.Shutdown();
}

TEST(LuaSandbox, LoadFunctionBlocked) {
    LuaSystem& ls = LuaSystem::Get();
    ASSERT_TRUE(ls.Init(nullptr));

    // load() should be nil; calling it should error
    EXPECT_FALSE(ls.Exec("assert(load ~= nil)"));

    ls.Shutdown();
}

TEST(LuaSandbox, DofileBlocked) {
    LuaSystem& ls = LuaSystem::Get();
    ASSERT_TRUE(ls.Init(nullptr));

    EXPECT_FALSE(ls.Exec("assert(dofile ~= nil)"));

    ls.Shutdown();
}

// ── Valid script execution ────────────────────────────────────────────────────
TEST(LuaSandbox, SimpleArithmeticSucceeds) {
    LuaSystem& ls = LuaSystem::Get();
    ASSERT_TRUE(ls.Init(nullptr));

    EXPECT_TRUE(ls.Exec("local x = 2 + 2; assert(x == 4)"));

    ls.Shutdown();
}
