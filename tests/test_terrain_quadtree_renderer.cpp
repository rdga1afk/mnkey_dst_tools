#include <gtest/gtest.h>
#include <monkey_dust/render/gpu_hal.h>

// Updated 2026-09-06 for the flat-LOD replacement (docs/TERRAIN_FLAT_LOD_PLAN.md,
// 2026-09-05): end-to-end smoke test for the production TerrainQuadtreeRenderer
// -- draws ONE node (filled grid, indexed, vertex-buffer-less) into a real
// offscreen G-buffer render target and checks every touched pixel decodes to
// a finite, in-world-bounds position, same invariant class
// TerrainPatchRendererTest/TerrainClipmapRendererTest already check. Skirts
// and geomorph (morph/skirt_depth fields) no longer exist -- every tile is
// emitted at the same depth, so there's no density mismatch to hide and no
// blend to verify (see terrain_quadtree.h's own doc comment).

#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/terrain_quadtree_renderer.h>
#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/terrain_gen.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr int kRTWidth  = 256;
constexpr int kRTHeight = 256;

void MakePerspectiveVP(float eye_x, float eye_y, float eye_z,
                        float target_x, float target_y, float target_z,
                        float aspect, float* out16) {
    float fx = target_x - eye_x, fy = target_y - eye_y, fz = target_z - eye_z;
    float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= flen; fy /= flen; fz /= flen;
    float upx = 0, upy = 1, upz = 0;
    float sx = fy * upz - fz * upy, sy = fz * upx - fx * upz, sz = fx * upy - fy * upx;
    float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
    sx /= slen; sy /= slen; sz /= slen;
    float ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;

    float view[16] = {
        sx, ux, -fx, 0,
        sy, uy, -fy, 0,
        sz, uz, -fz, 0,
        -(sx * eye_x + sy * eye_y + sz * eye_z),
        -(ux * eye_x + uy * eye_y + uz * eye_z),
        (fx * eye_x + fy * eye_y + fz * eye_z), 1,
    };

    float fov = 60.0f * 3.14159265f / 180.0f;
    float f = 1.0f / std::tan(fov * 0.5f);
    float nearp = 1.0f, farp = 50000.0f;
    float proj[16] = {};
    proj[0] = f / aspect; proj[5] = f;
    proj[10] = (farp + nearp) / (nearp - farp); proj[11] = -1.0f;
    proj[14] = (2.0f * farp * nearp) / (nearp - farp);

    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += proj[k * 4 + r] * view[c * 4 + k];
            out16[c * 4 + r] = sum;
        }
    }
}
} // namespace

class TerrainQuadtreeRendererTest : public ::testing::Test {
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
        window_ = SDL_CreateWindow("md_quadtree_renderer", 64, 64,
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
        if (!hmap_.Init(dev_)) GTEST_SKIP() << "TerrainWorldHeightmap::Init failed";
        if (!renderer_.Init(dev_)) GTEST_SKIP() << "TerrainQuadtreeRenderer::Init failed";
        if (!ground_.Init()) GTEST_SKIP() << "TerrainRenderer::Init failed";
        // DrawNode's boundary-mask sampling (terrain_gbuffer_mini.frag) needs
        // real tex_ground/tex_ground_nml bound or it silently no-ops the draw
        // (GetSharedGroundSamplers slots 1/4 -- tex_ground_array/tex_ground_
        // nml_array -- null-checked, terrain_quadtree_renderer.cpp:356-359).
        // Load the same production texture array scene_render.cpp does --
        // InitGroundTextureArray reads BiomeRegistry, which must be loaded
        // first (scene_render.cpp:266-267 does the same before its own call).
        if (!BiomeRegistry::Get().LoadFromFile("game/data/biome_table.txt"))
            GTEST_SKIP() << "BiomeRegistry::LoadFromFile failed";
        if (!ground_.InitGroundTextureArray())
            GTEST_SKIP() << "TerrainRenderer::InitGroundTextureArray failed";

        ASSERT_TRUE(color_rt_.InitRenderTarget(kRTWidth, kRTHeight, GpuSamplerDesc{},
                                                SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT));
        depth_rt_.Init(kRTWidth, kRTHeight, false);
    }

    void TearDown() override {
        if (!window_) return;
        color_rt_.Shutdown();
        depth_rt_.Shutdown();
        ground_.Shutdown();
        renderer_.Shutdown(dev_);
        hmap_.Shutdown(dev_);
        md::GpuDevice::Get().Shutdown();
        SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    // Renders one node and returns the downloaded RGBA32F G-buffer.
    std::vector<float> RenderNode(const TerrainQuadtree::VisibleNode& node,
                                   float cam_x, float cam_y, float cam_z, const float* vp) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = color_rt_.SDLTexture();
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.clear_color = {0.f, 0.f, 0.f, 0.f};
        SDL_GPUDepthStencilTargetInfo ds{};
        ds.texture = depth_rt_.SDLTexture();
        ds.load_op = SDL_GPU_LOADOP_CLEAR;
        ds.store_op = SDL_GPU_STOREOP_STORE;
        ds.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        ds.clear_depth = 1.f;

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &ds);
        EXPECT_NE(rp, nullptr) << SDL_GetError();

        renderer_.DrawNode(rp, cmd, hmap_, vp, node, cam_x, cam_y, cam_z, ground_);

        SDL_EndGPURenderPass(rp);

        size_t px_count = (size_t)kRTWidth * kRTHeight;
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbi.size = (Uint32)(px_count * 4 * sizeof(float));
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
        EXPECT_NE(tb, nullptr) << SDL_GetError();

        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = color_rt_.SDLTexture();
        src.w = kRTWidth; src.h = kRTHeight; src.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = tb;
        dst.pixels_per_row = kRTWidth;
        dst.rows_per_layer = kRTHeight;
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);

        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        EXPECT_NE(fence, nullptr) << SDL_GetError();
        SDL_WaitForGPUFences(dev_, true, &fence, 1);
        SDL_ReleaseGPUFence(dev_, fence);

        void* mapped = SDL_MapGPUTransferBuffer(dev_, tb, false);
        EXPECT_NE(mapped, nullptr) << SDL_GetError();
        std::vector<float> data(px_count * 4);
        std::memcpy(data.data(), mapped, data.size() * sizeof(float));
        SDL_UnmapGPUTransferBuffer(dev_, tb);
        SDL_ReleaseGPUTransferBuffer(dev_, tb);
        return data;
    }

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* dev_ = nullptr;
    TerrainWorldHeightmap hmap_;
    TerrainQuadtreeRenderer renderer_;
    TerrainRenderer ground_;
    GpuTexture color_rt_;
    GpuDepthTexture depth_rt_;
};

TEST_F(TerrainQuadtreeRendererTest, SingleNodeRendersFiniteInBoundsGBuffer) {
    float origin_x = hmap_.WorldExtent() * 0.5f;
    float origin_z = hmap_.WorldExtent() * 0.5f;
    float cam_x = origin_x + 150.0f, cam_z = origin_z + 150.0f;
    float cam_y = hmap_.HeightMax() + 500.0f;

    float vp[16];
    MakePerspectiveVP(cam_x, cam_y, cam_z, origin_x + 150.0f, hmap_.HeightMin(), origin_z + 100.0f,
                       (float)kRTWidth / (float)kRTHeight, vp);

    TerrainQuadtree::VisibleNode node{};
    node.origin_x = origin_x;
    node.origin_z = origin_z;
    node.size = 460.8f;
    // #398: depth must match size (CHUNK_SIZE/2^depth) -- each depth now
    // binds its OWN clipmap level (own texelSize), unlike the pre-clipmap
    // world-wide texture where any depth/size combination worked. depth=0
    // is CHUNK_SIZE's real depth (28.8m texelSize, matching size/16 here).
    node.depth = 0;

    std::vector<float> data = RenderNode(node, cam_x, cam_y, cam_z, vp);

    int touched = 0;
    float worldExtent = hmap_.WorldExtent();
    size_t px_count = (size_t)kRTWidth * kRTHeight;
    for (size_t i = 0; i < px_count; ++i) {
        float wx = data[i * 4 + 0], wy = data[i * 4 + 1], wz = data[i * 4 + 2], packedN = data[i * 4 + 3];
        bool empty = (wx == 0.f && wy == 0.f && wz == 0.f && packedN == 0.f);
        if (empty) continue;
        ++touched;
        ASSERT_TRUE(std::isfinite(wx)) << "pixel " << i << " non-finite worldX";
        ASSERT_TRUE(std::isfinite(wy)) << "pixel " << i << " non-finite worldY";
        ASSERT_TRUE(std::isfinite(wz)) << "pixel " << i << " non-finite worldZ";
        ASSERT_GE(wx, -1.0f); ASSERT_LE(wx, worldExtent + 1.0f);
        ASSERT_GE(wz, -1.0f); ASSERT_LE(wz, worldExtent + 1.0f);
        ASSERT_GE(wy, hmap_.HeightMin() - 10.0f);
        ASSERT_LE(wy, hmap_.HeightMax() + 1.0f);
    }
    EXPECT_GT(touched, 0) << "no pixel was touched -- nothing rendered at all";
}
