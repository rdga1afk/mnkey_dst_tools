#include <gtest/gtest.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/editor/editor_panel_registry.h>

// ── Batch 16: SequenceIgnoreChildFail · ShaderFeature · EditorPanel Selection ──

// ── SequenceIgnoreChildFail ───────────────────────────────────────────────────








// ── ShaderFeature enum ────────────────────────────────────────────────────────

TEST(Batch16ShaderFeature, Flags_AreDistinctPowersOfTwo) {
    EXPECT_EQ(SF_None,      0u);
    EXPECT_EQ(SF_Skinned,   1u);
    EXPECT_EQ(SF_Shadows,   2u);
    EXPECT_EQ(SF_AlphaTest, 4u);
}

TEST(Batch16ShaderFeature, Combination_Works) {
    uint32_t combo = SF_Skinned | SF_Shadows;
    EXPECT_TRUE(combo & SF_Skinned);
    EXPECT_TRUE(combo & SF_Shadows);
    EXPECT_FALSE(combo & SF_AlphaTest);
}

TEST(Batch16ShaderFeature, PipelineDesc_DefaultIsNone) {
    GpuPipeline::Desc d;
    EXPECT_EQ(d.shader_features, static_cast<uint32_t>(SF_None));
}

TEST(Batch16ShaderFeature, PipelineDesc_CanSetFeatures) {
    GpuPipeline::Desc d;
    d.shader_features = SF_Skinned | SF_Shadows;
    EXPECT_TRUE(d.shader_features & SF_Skinned);
    EXPECT_TRUE(d.shader_features & SF_Shadows);
}

// ── EditorPanelRegistry selection callbacks ───────────────────────────────────

namespace {
    static MdEntity g_selected = MdEntity::Null();
    static bool         g_deselected = false;

    void OnSel(MdEntity e)  { g_selected    = e; }
    void OnDesel()               { g_deselected  = true; }
    void DrawPanel()             {}
}

struct Batch16EditorFixture : ::testing::Test {
    void SetUp() override {
        EditorPanelRegistry::Get().Clear();
        g_selected   = MdEntity::Null();
        g_deselected = false;
    }
    void TearDown() override { EditorPanelRegistry::Get().Clear(); }
};

TEST_F(Batch16EditorFixture, Register_WithCallbacks_Stored) {
    EditorPanelRegistry::Get().Register(1, "Inspector", DrawPanel, OnSel, OnDesel);
    EXPECT_TRUE(EditorPanelRegistry::Get().IsRegistered(1));
}

TEST_F(Batch16EditorFixture, NotifyEntitySelected_CallsAllPanels) {
    EditorPanelRegistry::Get().Register(1, "Inspector", DrawPanel, OnSel, nullptr);
    EditorPanelRegistry::Get().Register(2, "Props",     DrawPanel, OnSel, nullptr);

    MdEntity e = MdRegistry::Get().Create();
    EditorPanelRegistry::Get().NotifyEntitySelected(e);
    EXPECT_EQ(g_selected, e);
    MdRegistry::Get().Destroy(e);
}

TEST_F(Batch16EditorFixture, NotifyEntityDeselected_CallsAllPanels) {
    EditorPanelRegistry::Get().Register(1, "Inspector", DrawPanel, nullptr, OnDesel);
    EditorPanelRegistry::Get().NotifyEntityDeselected();
    EXPECT_TRUE(g_deselected);
}

TEST_F(Batch16EditorFixture, Register_NoCallbacks_BackwardCompat) {
    // Old-style registration without callbacks compiles and works
    EditorPanelRegistry::Get().Register(5, "Console", DrawPanel);
    EXPECT_TRUE(EditorPanelRegistry::Get().IsRegistered(5));
    // Notify does nothing (no callbacks registered)
    EditorPanelRegistry::Get().NotifyEntityDeselected();  // must not crash
}

TEST_F(Batch16EditorFixture, ReRegister_UpdatesCallbacks) {
    EditorPanelRegistry::Get().Register(1, "P", DrawPanel, nullptr, nullptr);
    EditorPanelRegistry::Get().Register(1, "P", DrawPanel, OnSel,   OnDesel);

    MdEntity e = MdRegistry::Get().Create();
    EditorPanelRegistry::Get().NotifyEntitySelected(e);
    EXPECT_EQ(g_selected, e);
    MdRegistry::Get().Destroy(e);
}

TEST_F(Batch16EditorFixture, NotifySelected_NullCallback_NoCrash) {
    EditorPanelRegistry::Get().Register(1, "P", nullptr, nullptr, nullptr);
    MdEntity e = MdRegistry::Get().Create();
    EditorPanelRegistry::Get().NotifyEntitySelected(e);  // must not crash
    MdRegistry::Get().Destroy(e);
}
