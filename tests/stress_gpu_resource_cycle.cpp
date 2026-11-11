#include <filesystem>
#include <gtest/gtest.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/gpu_resource_tracker.h>

#include <monkey_dust/render/gpu_device.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <cstdio>
#include <cstring>

// GPU resource leak-detection stress test — create/destroy thousands of each
// of the 5 public HAL wrapper types (GpuStaticBuffer, GpuTexture, GpuPipeline,
// GpuComputePipeline, GpuDepthTexture) and assert GpuResourceTracker's live
// count returns to its pre-cycle baseline. Scoped to these 5 classes'
// Init/Create + Shutdown/Destroy — NOT the ~100 raw SDL_CreateGPU*/
// SDL_ReleaseGPU* call sites elsewhere in the renderer (ssao/
// motion_blur/etc.) — see gpu_resource_tracker.h's doc comment.
//
// Raw SDL3 calls only (no window.h — Rule M-A restricts that header to
// Main.cpp/EditorMain.cpp). Skips gracefully (GTEST_SKIP, not a failure) if
// no GPU/display is available — e.g. a headless CI runner with no
// SDL_VIDEODRIVER=dummy or Vulkan ICD set up. Real value on a dev machine
// with an actual GPU; harmless no-op otherwise.
class GpuResourceStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            GTEST_SKIP() << "SDL_INIT_VIDEO failed: " << SDL_GetError();
        }
        window_ = SDL_CreateWindow("md_stress_gpu", 64, 64,
                                    SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
        if (!window_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            GTEST_SKIP() << "SDL_CreateWindow failed: " << SDL_GetError();
        }
        if (!md::GpuDevice::Get().Init(window_)) {
            SDL_DestroyWindow(window_);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            GTEST_SKIP() << "GpuDevice::Init failed (no usable GPU/Vulkan ICD)";
        }
    }

    void TearDown() override {
        if (!window_) return;  // SetUp already skipped
        md::GpuDevice::Get().Shutdown();
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    SDL_Window* window_ = nullptr;
};

TEST_F(GpuResourceStressTest, BufferCreateDestroyCycleLeavesNoLeaks) {
    const int baseline = md::GpuResourceTracker::Get().LiveBuffers();
    constexpr int kIterations = 2000;
    static const float data[16] = {};
    for (int i = 0; i < kIterations; ++i) {
        GpuStaticBuffer buf;
        buf.Init(/*GL_ARRAY_BUFFER-equivalent target*/ 0u, data, sizeof(data));
        buf.Shutdown();
    }
    EXPECT_EQ(md::GpuResourceTracker::Get().LiveBuffers(), baseline)
        << "GpuStaticBuffer leak: live count did not return to baseline after "
        << kIterations << " create/destroy cycles";
}

TEST_F(GpuResourceStressTest, DepthTextureCreateDestroyCycleLeavesNoLeaks) {
    const int baseline = md::GpuResourceTracker::Get().LiveDepthTextures();
    constexpr int kIterations = 500;  // depth textures are heavier than buffers
    for (int i = 0; i < kIterations; ++i) {
        GpuDepthTexture tex;
        tex.Init(128, 128, false);
        tex.Shutdown();
    }
    EXPECT_EQ(md::GpuResourceTracker::Get().LiveDepthTextures(), baseline)
        << "GpuDepthTexture leak: live count did not return to baseline after "
        << kIterations << " create/destroy cycles";
}

TEST_F(GpuResourceStressTest, TextureCreateDestroyCycleLeavesNoLeaks) {
    const int baseline = md::GpuResourceTracker::Get().LiveTextures();
    constexpr int kIterations = 500;
    static uint8_t rgba[8 * 8 * 4] = {};
    GpuSamplerDesc s{};
    for (int i = 0; i < kIterations; ++i) {
        GpuTexture tex;
        tex.InitFromMemory(rgba, 8, 8, s);
        tex.Shutdown();
    }
    EXPECT_EQ(md::GpuResourceTracker::Get().LiveTextures(), baseline)
        << "GpuTexture leak: live count did not return to baseline after "
        << kIterations << " create/destroy cycles";
}

// GpuPipeline/GpuComputePipeline are intentionally NOT stress-cycled here:
// Create() compiles real SPIR-V shaders from disk (working-directory-relative
// paths) and populates a process-wide pipeline cache keyed by path+features
// (PipeCache_Put, see gpu_hal_pipeline.cpp) — thousands of iterations would
// mostly hit that cache (not exercise real create/destroy) and the ones that
// don't would be a real multi-second compile stall per the project's own
// documented ~4.5s-per-pipeline precedent (gpu_device.cpp), unacceptable for
// a unit test. The tracker instrumentation is still in place and exercised
// indirectly by every real pipeline the engine creates during normal game/
// editor runs (GpuResourceTracker::Get().LivePipelines() /
// LiveComputePipelines() are queryable at any time for manual diagnosis).


// DXT1 (BC1) files, as Kenshi's plant textures: 8 bytes a 4x4 block; before the BC1 path InitFromDDS refused them.
TEST_F(GpuResourceStressTest, Dds2dLoadsBc1WithFullMipChain) {
    std::filesystem::create_directories("temp_");   // gitignored: absent in a fresh checkout (CI)
    const char* path = "temp_/test_bc1_8x8.dds";
    uint8_t file[128 + 32 + 8 + 8 + 8] = {};
    auto w32 = [&](size_t o, uint32_t v) { memcpy(file + o, &v, 4); };
    memcpy(file, "DDS ", 4); w32(4, 124); w32(12, 8); w32(16, 8); w32(28, 4); w32(76, 32); w32(80, 4); w32(84, 0x31545844u);   // 8x8, 4 mips (8, 4, 2, 1)
    FILE* f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fwrite(file, 1, sizeof(file), f); fclose(f);
    const int baseline = md::GpuResourceTracker::Get().LiveTextures();
    GpuTexture tex; GpuSamplerDesc s{};
    ASSERT_TRUE(tex.InitFromFile(path, s));
    EXPECT_EQ(md::GpuResourceTracker::Get().LiveTextures(), baseline + 1);
    tex.Shutdown();
    EXPECT_EQ(md::GpuResourceTracker::Get().LiveTextures(), baseline);
    remove(path);
}
