#pragma once
// Startup splash: window creation → SceneRender::Init() is ~5-8s of GPU
// pipeline/texture setup (see game_init.cpp's [STARTUP-TIMING] breakdown)
// during which the window was previously visible but never presented to,
// showing whatever garbage the compositor last had in that buffer --
// reads as a frozen/crashed process, not a loading game. This presents an
// the original Kenshi loading screen (docs/research/kenshi_specs/loading_screen.md): one of the install's
// loading_screen*.dds picked at random, the panel with "Loading" + dots and a random tip, all read from
// KENSHI_DIR at run time; without an install a plain dark screen. The top-right load bar is the exe's streaming ProgressBar (docs/research/kenshi_specs/loading_screen.md §6).
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/io/kenshi_loading_screen.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <string>
#include <vector>

class SplashScreen {
public:
    static SplashScreen& Get();

    void Init(md::GpuDeviceHandle dev);
    void SetProgress(float frac01);
    void Present();
    // Renders the same frame into an offscreen target of w x h and writes it as a PNG (gate / debugging); seed != 0 fixes the picture and the tip (the rule is the exe's, only the generator state is chosen). False without an install or on a GPU error.
    bool SaveFrame(const char* png_path, int w, int h, uint32_t seed);
    void Shutdown();
    bool IsReady() const { return ready_; }

private:
    GpuTexture bg_tex_;
    GpuTexture panel_tex_;           // 1x1 dark, stretched to the panel rectangle (the skin is not ported yet)
    GpuTexture title_tex_[4];        // "Loading" + 0..3 dots
    GpuTexture tip_tex_;
    GpuTexture bar_frame_[9], bar_empty_, bar_fill_;   // Kenshi_ProgressBarSkin (nine slices), Kenshi_ProgressBarEmptySkin / FilledSkin, cut from data/gui/gfx/Kenshi_UI.png at run time
    float      title_w_px_[4] = {}, title_h_px_ = 0.f, tip_w_px_ = 0.f, tip_h_px_ = 0.f, title_ref_w_px_ = 0.f;
    int        fixed_dots_ = -1;     // >= 0: the number of dots of a saved gate frame (SaveFrame with a seed); -1 = from the clock
    int        text_for_h_ = 0;      // window height the text textures were rasterised for
    std::vector<uint8_t> title_font_, tip_font_;
    std::string tip_;
    std::string root_;
    md::kenshi::LoadingScreenData data_;
    void Pick(uint32_t seed);
    void LoadBarSkins();
    void DrawInto(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* target, uint32_t w, uint32_t h);
    GpuPipeline pipeline_;
    bool  ready_    = false;
    float progress_ = 0.f;
};
