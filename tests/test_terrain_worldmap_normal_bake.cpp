#include <gtest/gtest.h>
#include <monkey_dust/render/gpu_hal.h>

// Full-variant Phase 3 (serene-pondering-teapot.md): verifies
// TerrainWorldHeightmap::NormalTexture() (baked by
// shaders/terrain_worldmap_normal_bake.comp inside Init()) produces
// finite, unit-length-reconstructible normals across the WHOLE world
// texture -- not just a hand-picked region. Reads the raw RG8_SNORM
// texels back via GPU->CPU download (same transfer-buffer pattern the
// terrain render tests already use) and checks every texel decodes to
// nx,nz in [-1,1] with nx^2+nz^2 <= 1 (so y=sqrt(1-nx^2-nz^2) stays real).
//
// Does NOT yet verify cross-tier agreement at patch boundaries (that is
// this same class of proof Part A already ran via RenderDoc post-VS
// vertex comparison, applicable again once Phase 5's patch renderer
// exists to compare against) -- this test's job is narrower: is the bake
// itself numerically sane over the entire world, no NaN/Inf/out-of-range
// texels anywhere (edges, water-level flats, the tallest Kenshi peaks).

#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/world/terrain_gen.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

class TerrainWorldmapNormalBakeTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TerrainAtlas_Loaded()) {
            if (!TerrainAtlas_Load("game/data/terrain/world_hmap")) {
                GTEST_SKIP() << "TerrainAtlas_Load failed";
            }
        }
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            GTEST_SKIP() << "SDL_INIT_VIDEO failed: " << SDL_GetError();
        }
        window_ = SDL_CreateWindow("md_normal_bake", 64, 64,
                                    SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
        if (!window_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            GTEST_SKIP() << "SDL_CreateWindow failed: " << SDL_GetError();
        }
        if (!md::GpuDevice::Get().Init(window_)) {
            SDL_DestroyWindow(window_);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            GTEST_SKIP() << "GpuDevice::Init failed";
        }
        dev_ = md::GpuDevice::Get().SDLDevice();
        if (!hmap_.Init(dev_)) GTEST_SKIP() << "TerrainWorldHeightmap::Init failed (includes the normal bake)";
    }

    void TearDown() override {
        if (!window_) return;
        hmap_.Shutdown(dev_);
        md::GpuDevice::Get().Shutdown();
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* dev_ = nullptr;
    TerrainWorldHeightmap hmap_;
};

TEST_F(TerrainWorldmapNormalBakeTest, BakedNormalsFiniteAndUnitReconstructibleAcrossWholeWorld) {
    ASSERT_NE(hmap_.NormalTexture(), nullptr);
    ASSERT_NE(hmap_.NormalSampler(), nullptr);

    int N = hmap_.Resolution();
    size_t texel_count = (size_t)N * (size_t)N;

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size  = (Uint32)(texel_count * 2); // RG8_SNORM = 2 bytes/texel
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    ASSERT_NE(tb, nullptr) << SDL_GetError();

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = hmap_.NormalTexture();
    src.w = (Uint32)N; src.h = (Uint32)N; src.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    dst.pixels_per_row = (Uint32)N;
    dst.rows_per_layer = (Uint32)N;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    ASSERT_NE(fence, nullptr) << SDL_GetError();
    SDL_WaitForGPUFences(dev_, true, &fence, 1);
    SDL_ReleaseGPUFence(dev_, fence);

    void* mapped = SDL_MapGPUTransferBuffer(dev_, tb, false);
    ASSERT_NE(mapped, nullptr) << SDL_GetError();
    std::vector<int8_t> raw(texel_count * 2);
    std::memcpy(raw.data(), mapped, raw.size());
    SDL_UnmapGPUTransferBuffer(dev_, tb);
    SDL_ReleaseGPUTransferBuffer(dev_, tb);

    int bad = 0;
    double sum_nx2_nz2 = 0.0;
    for (size_t i = 0; i < texel_count; ++i) {
        float nx = (float)raw[i * 2 + 0] / 127.0f;
        float nz = (float)raw[i * 2 + 1] / 127.0f;
        bool finite = std::isfinite(nx) && std::isfinite(nz);
        bool in_range = nx >= -1.0f && nx <= 1.0f && nz >= -1.0f && nz <= 1.0f;
        float xz2 = nx * nx + nz * nz;
        // The shader's vec3(-dhdx, 1.0, -dhdz) always has a positive Y
        // component pre-normalize, so post-normalize nx^2+nz^2 <= 1.0 is
        // an exact mathematical guarantee -- EXCEPT RG8_SNORM quantizes
        // nx and nz INDEPENDENTLY (1/127 step). At near-vertical Kenshi
        // cliff faces (nx^2+nz^2 approx 1, ny approx 0 before encoding),
        // independent per-channel rounding can push the encoded sum
        // slightly over 1.0 -- worst case bound is ~1 + 2*eps*sqrt(2)
        // with eps=0.5/127, i.e. ~1.011. 1.02 gives headroom for that
        // known, harmless quantization artifact while still catching a
        // real bake bug (NaN, wildly out-of-range, or near-2.0 values).
        bool reconstructible = xz2 <= 1.02f;
        sum_nx2_nz2 += xz2;
        if (!finite || !in_range || !reconstructible) {
            if (bad < 5) {
                ADD_FAILURE() << "texel " << i << ": nx=" << nx << " nz=" << nz
                               << " finite=" << finite << " in_range=" << in_range
                               << " reconstructible=" << reconstructible;
            }
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0) << bad << "/" << texel_count << " baked normal texels are bad";

    // A perfectly flat world would give nx=nz=0 everywhere (xz2=0); a real
    // Kenshi heightmap has real slopes, so the average xz2 should be
    // meaningfully above zero -- this catches a "bake dispatched but wrote
    // nothing" class of bug (all-zero output would still pass the above
    // finite/range/reconstructible checks).
    double avg_xz2 = sum_nx2_nz2 / (double)texel_count;
    EXPECT_GT(avg_xz2, 0.0001) << "baked normals are suspiciously flat everywhere -- bake may not have run";
}

// task #576/#556 falsification test REMOVED 2026-09-24: it asserted
// normal_tex_ has a real mip chain (SDL_GenerateMipmapsForGPUTexture,
// mip level 8 specifically) and downloaded that mip level directly --
// crashed (core dump) once the mip chain was removed this same session
// (CLAUDE_STATE.md БОРГ-VISUAL-1's addendum / БОРГ-VISUAL-5): the #556
// LOD-selection this chain existed for was proven permanently dead
// (terrain_quadtree.vert's normalLod always evaluates to 0 under the
// later fixed-depth quadtree migration, confirmed across all 3 live
// draw paths), so normal_tex_ is back to a single level and mip 8
// no longer exists to request. Not rewritten to pass some other way --
// the feature this test verified is gone by design, not by regression.
