#include <gtest/gtest.h>
#include <monkey_dust/editor/editor_panel_registry.h>


struct Batch14PanelTest : ::testing::Test {
    EditorPanelRegistry& reg = EditorPanelRegistry::Get();
    void SetUp()    override { reg.Clear(); }
    void TearDown() override { reg.Clear(); }
};

TEST_F(Batch14PanelTest, B15_Register_IsRegistered) {
    EXPECT_FALSE(reg.IsRegistered(0));
    reg.Register(0, "Hierarchy", nullptr);
    EXPECT_TRUE(reg.IsRegistered(0));
    EXPECT_EQ(reg.Count(), 1);
}

TEST_F(Batch14PanelTest, B15_Register_StoreName) {
    reg.Register(5, "Inspector", nullptr);
    EXPECT_STREQ(reg.GetName(5), "Inspector");
    EXPECT_EQ(reg.GetName(99), nullptr);  // not registered
}

TEST_F(Batch14PanelTest, B15_Draw_InvokesCallback) {
    static int called = 0;
    called = 0;
    reg.Register(1, "Console", []{ ++called; });
    reg.Draw(1);
    EXPECT_EQ(called, 1);
    reg.Draw(99);  // no-op
    EXPECT_EQ(called, 1);
}

TEST_F(Batch14PanelTest, B15_Register_UpdatesExisting) {
    static int a_count = 0, b_count = 0;
    a_count = b_count = 0;
    reg.Register(2, "Assets", []{ ++a_count; });
    reg.Register(2, "AssetsV2", []{ ++b_count; });  // update same id
    EXPECT_EQ(reg.Count(), 1);
    EXPECT_STREQ(reg.GetName(2), "AssetsV2");
    reg.Draw(2);
    EXPECT_EQ(a_count, 0);
    EXPECT_EQ(b_count, 1);
}

TEST_F(Batch14PanelTest, B15_MaxPanels_SilentDrop) {
    for (int i = 0; i < EditorPanelRegistry::MAX_PANELS; ++i)
        reg.Register((uint8_t)i, "P", nullptr);
    EXPECT_EQ(reg.Count(), EditorPanelRegistry::MAX_PANELS);
    // Register one more — should be silently dropped
    reg.Register(255, "Overflow", nullptr);
    EXPECT_EQ(reg.Count(), EditorPanelRegistry::MAX_PANELS);
    EXPECT_FALSE(reg.IsRegistered(255));
}
