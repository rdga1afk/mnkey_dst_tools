// Per-biome layer arrays on the GPU: six-layer diffuse + normal arrays from the install, resident size under a budget. Needs KENSHI_DIR and a GPU (skips otherwise).
#include <gtest/gtest.h>
#include <monkey_dust/render/biome_layer_arrays.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/world/biome_def.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>

using namespace md;

TEST(BiomeLayerArraysGpu, BuildsUnderBudget) {
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "KENSHI_DIR not set";
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) GTEST_SKIP() << "SDL_INIT_VIDEO failed";
    SDL_Window* win = SDL_CreateWindow("md_biome_arrays", 64, 64, SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (!win) { SDL_QuitSubSystem(SDL_INIT_VIDEO); GTEST_SKIP() << "no window"; }
    if (!GpuDevice::Get().Init(win)) { SDL_DestroyWindow(win); SDL_QuitSubSystem(SDL_INIT_VIDEO); GTEST_SKIP() << "no GPU"; }
    const std::string tex_dir = std::string(dir) + "/data/newland/land/textures";
    ASSERT_TRUE(BiomeRegistry::Get().LoadFromKenshi(dir, tex_dir.c_str()));
    const int n = BiomeRegistry::Get().BiomeCount();
    ASSERT_GT(n, 4);
    {
        BiomeLayerArrays arrays; arrays.SetBudgetBytes(128ull << 20);   // small budget so the eviction is exercised
        int built = 0; uint64_t frame = 0;
        for (int b = 0; b < n && built < 8; ++b) {
            ++frame;
            const BiomeLayerSet* s = arrays.Acquire(b, frame);
            if (!s) continue;
            ++built;
            EXPECT_NE(s->diffuse.SDLTexture(), nullptr);
            EXPECT_NE(s->normal.SDLTexture(), nullptr);
            // the budget holds except for the biome in use this frame
            EXPECT_LE(arrays.Cache().ResidentBytes(), (128ull << 20) + 120ull * 1024 * 1024);
        }
        printf("[biome_arrays] built %d biomes, resident %d (%.1f MB), builds %llu, evictions %llu\n", built, arrays.Cache().Resident(), arrays.Cache().ResidentBytes() / 1e6,
               (unsigned long long)arrays.Cache().Builds(), (unsigned long long)arrays.Cache().Evictions());
        EXPECT_GE(built, 4);
        EXPECT_GT(arrays.Cache().Evictions(), 0u);
    }
    {   // distance tiers: the same biomes as tails (4 or 6 mips dropped) are a fraction of the memory and of the load time
        BiomeLayerArrays arrays; arrays.SetBudgetBytes(256ull << 20);
        for (int tier : { 0, 2, 3 }) {
            arrays.Clear(); uint64_t frame = 0; int built = 0; const auto t0 = SDL_GetTicks();
            for (int b = 0; b < n && built < 8; ++b) { ++frame; const BiomeLayerSet* s = arrays.Acquire(b, frame, tier); if (!s) continue; ++built; EXPECT_NE(s->diffuse.SDLTexture(), nullptr); }
            const auto ms = SDL_GetTicks() - t0;
            printf("[biome_arrays] tier %d (%d mips dropped): %d biomes, resident %d (%.1f MB by the estimate), %.1f ms per biome\n", tier, tier * 2, built, arrays.Cache().Resident(), arrays.Cache().ResidentBytes() / 1e6, built ? (double)ms / built : 0.0);
            EXPECT_GE(built, 4);
        }
    }
    SDL_DestroyWindow(win); SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
