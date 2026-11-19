#pragma once
// Heat haze post effect (DE-338): the install's post/heathaze.hlsl run as it is, after FXAA (compositors.cfg order: FXAA, HeatHaze).
// Inputs as materials/compositors/post.compositor: flow map, perturbation, the G-buffer depth (the live ground's R32F target) and the frame. On by default, MD_HEATHAZE=0 turns it off.
// The pass is skipped while the amplitude is exactly 0: the program then samples the same coordinate twice and blends them 50/50, i.e. returns its input (ctest HeatHazeZeroIsIdentity).
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/size_slots.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <cstdlib>
#include <string>
#include <vector>

namespace md {

class HeatHazePass {
public:
    static HeatHazePass& Get();
    static bool Wanted();                                  // on unless MD_HEATHAZE=0 (2 = also run at amplitude 0: the check that the program returns its input there)
    bool Init(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt);   // fmt = the colour format it draws into (the swapchain format); false (logged) when a program or texture is missing
    void Shutdown();
    bool Ready() const { return ready_; }
    // Once per frame: haze_scaled = SkyOutput.haze (record x strength), sun_y = sunDirection.y; the amplitude follows the exe ramp (HeatHazeStep).
    void Tick(float haze_scaled, float sun_y, double game_hours);   // game_hours = the game clock in hours since day 0: the program's gameTime is that clock minus its value at the first call (exe 14066ff60 / 14066db50: day * 24 + hour minus the value at load)
    bool Needed() const { return ready_ && (amp_ != 0.f || Force()); }
    static bool Force() { static const bool f = getenv("MD_HEATHAZE") && getenv("MD_HEATHAZE")[0] == '2'; return f; }
    float Amplitude() const { return amp_; }
    // The intermediate the previous pass writes into (outside a frame, like HialgoPass::Ensure); only the strict-order path at scale 1.0 needs it.
    void EnsureScratch(int w, int h);
    SDL_GPUTexture* Scratch() const { return scratch_; }
    // src (w x h, sampled bilinear) -> dst (colour target of the format given to Init); depth = the live ground's R32F target (same size as src) or null (then 0 = sky = 1).
    void Run(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src, SDL_GPUTexture* dst, SDL_GPUTexture* depth, int w, int h);

private:
    SDL_GPUDevice* dev_ = nullptr; SDL_GPUTextureFormat fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUShader* vs_ = nullptr; SDL_GPUShader* fs_ = nullptr; SDL_GPUGraphicsPipeline* pipe_ = nullptr;
    SDL_GPUSampler* point_ = nullptr; SDL_GPUSampler* linear_ = nullptr;
    SDL_GPUTexture* flow_ = nullptr; SDL_GPUTexture* perturb_ = nullptr; SDL_GPUSampler* flow_smp_ = nullptr; SDL_GPUSampler* perturb_smp_ = nullptr;
    SDL_GPUTexture* scratch_ = nullptr; SDL_GPUTexture* neutral_depth_ = nullptr; int sw_ = 0, sh_ = 0;
    struct Scr { SDL_GPUTexture* t = nullptr; int w = 0, h = 0; bool Empty() const { return !t; } };
    Scr alt_[4]; int alt_rr_ = 0;   // DE-362: the scratch targets of the other dynres levels
    std::vector<unsigned char> ub_; int off_time_ = -1, off_haze_ = -1;
    float amp_ = 0.f, time_ = 0.f; double last_s_ = -1.0, start_hours_ = 0.0; bool have_start_ = false; bool ready_ = false;
};

}   // namespace md
