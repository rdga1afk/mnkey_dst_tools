#include <gtest/gtest.h>
#include <monkey_dust/render/backend/granite_backend.h>

// RENDER-BACKEND-STAGE-5 (docs/archive/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.2).
// No live Vulkan device/window here -- Init()/Shutdown() deliberately don't
// own the Granite device (see granite_backend.h's doc comment), so in this
// test binary's default build (USE_GRANITE=OFF) md::GraniteBackend::Get().
// IsBuilt() is false and Init() correctly reports failure without touching
// any GPU state. This covers the callback-dispatch contract itself (the
// part that's actually new code in this class) -- the underlying Vulkan
// device/WSI init path (md::GraniteBackend, monkey_dust/render/
// granite_backend.h) already has its own live coverage in probes/
// granite_m2_wsi and probes/granite_m3_imgui_dynamic_rendering.

using md::render_backend::BackendStageFn;
using md::render_backend::GraniteBackend;
using md::render_backend::RenderFrameParams;

TEST(GraniteBackendTest, InitFailsWithoutPriorGraniteDeviceInit) {
    GraniteBackend backend;
    // md::GraniteBackend::Get() was never Init()'d with a window in this
    // test binary -- Init() must report failure, not crash or silently
    // succeed.
    EXPECT_FALSE(backend.Init());
}

TEST(GraniteBackendTest, BackendNameIsGranite) {
    GraniteBackend backend;
    EXPECT_STREQ(backend.BackendName(), "Granite");
}

TEST(GraniteBackendTest, GetCapsReturnsAllFalseThisPhase) {
    GraniteBackend backend;
    auto caps = backend.GetCaps();
    EXPECT_FALSE(caps.supports_deferred);
    EXPECT_FALSE(caps.supports_gpu_culling);
    EXPECT_FALSE(caps.supports_shadows);
    EXPECT_FALSE(caps.supports_post_process);
}

TEST(GraniteBackendTest, StageMethodsAreSafeNoOpsWithoutRegisteredCallbacks) {
    GraniteBackend backend;
    RenderFrameParams params;
    backend.SetFrameParams(params);
    // None of these have a callback registered -- must log a warning and
    // return, never crash/assert (§3 CONSTRAINT: "БЕЗ assert()").
    EXPECT_NO_FATAL_FAILURE(backend.UploadTransforms());
    EXPECT_NO_FATAL_FAILURE(backend.RunGpuCulling());
    EXPECT_NO_FATAL_FAILURE(backend.RunGpuSkinning());
    EXPECT_NO_FATAL_FAILURE(backend.RenderShadowPass());
    EXPECT_NO_FATAL_FAILURE(backend.RenderGBufferPass());
    EXPECT_NO_FATAL_FAILURE(backend.RenderDeferredLighting());
    EXPECT_NO_FATAL_FAILURE(backend.RunSsaoPass());
    EXPECT_NO_FATAL_FAILURE(backend.RunPostProcessChain());
    EXPECT_NO_FATAL_FAILURE(backend.RenderHud());
    EXPECT_NO_FATAL_FAILURE(backend.RenderEditorOverlay());
}

namespace {
struct CallbackProbe {
    int calls = 0;
    const RenderFrameParams* last_params = nullptr;
};

void RecordCall(void* user, const RenderFrameParams& params) {
    auto* probe = static_cast<CallbackProbe*>(user);
    probe->calls++;
    probe->last_params = &params;
}
}  // namespace

TEST(GraniteBackendTest, ShadowPassCallbackDispatchesWithFrameParams) {
    GraniteBackend backend;
    CallbackProbe probe;
    backend.SetShadowPassCallback(&RecordCall, &probe);

    RenderFrameParams params;
    params.now_s = 42.f;
    backend.SetFrameParams(params);
    backend.RenderShadowPass();

    ASSERT_EQ(probe.calls, 1);
    EXPECT_FLOAT_EQ(probe.last_params->now_s, 42.f);
}

TEST(GraniteBackendTest, UploadSkinCallbackCoversUploadTransformsOnly) {
    // Matches SdlGpuBackend's own documented split: UploadTransforms()
    // dispatches the callback, RunGpuSkinning() is a deliberate no-op
    // folded into the same call (see granite_backend.h's doc comment).
    GraniteBackend backend;
    CallbackProbe probe;
    backend.SetUploadSkinCallback(&RecordCall, &probe);

    backend.UploadTransforms();
    EXPECT_EQ(probe.calls, 1);

    backend.RunGpuSkinning();
    EXPECT_EQ(probe.calls, 1);  // unchanged -- no-op, not a second dispatch
}

TEST(GraniteBackendTest, DeferredPassCallbackCoversAllThreeFoldedStages) {
    // Matches SdlGpuBackend: RunSsaoPass()/RunPostProcessChain() are
    // deliberate no-ops folded into RenderDeferredLighting().
    GraniteBackend backend;
    CallbackProbe probe;
    backend.SetDeferredPassCallback(&RecordCall, &probe);

    backend.RenderDeferredLighting();
    EXPECT_EQ(probe.calls, 1);

    backend.RunSsaoPass();
    backend.RunPostProcessChain();
    EXPECT_EQ(probe.calls, 1);  // unchanged -- both are no-ops
}

TEST(GraniteBackendTest, EditorOverlayCallbackDispatches) {
    // Unlike SdlGpuBackend (no callback slot exists for RenderEditorOverlay
    // there yet), GraniteBackend gets this slot in Крок 3 specifically for
    // the permanent ImGui bridge (tools/editor/editor_granite_imgui_bridge.cpp).
    GraniteBackend backend;
    CallbackProbe probe;
    backend.SetEditorOverlayCallback(&RecordCall, &probe);

    backend.RenderEditorOverlay();
    EXPECT_EQ(probe.calls, 1);
}
