#pragma once
// UpscalePass -- resolution_scale (#253): the 3D scene is rendered into an internal-resolution colour target of the
// swapchain's own format (so every already-built pipeline stays compatible) and this pass upscales it to the real
// swapchain (bilinear + optional cheap sharpen). Sized/recreated by Ensure() BEFORE the frame's command buffer is
// acquired (same rule as the depth / G-buffer resize checks).
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/size_slots.h>

class UpscalePass {
public:
    bool Init();
    void Shutdown();
    // (Re)create the scene colour target at w x h in `fmt` (the swapchain format). No-op if unchanged.
    void Ensure(int w, int h, SDL_GPUTextureFormat fmt);
    bool Ready() const { return ready_; }
    bool HasTarget(int w, int h) const { return tex_ && w == w_ && h == h_; }
    bool Cached(int w, int h, SDL_GPUTextureFormat fmt) const { if (tex_ && w == w_ && h == h_ && fmt == fmt_) return true; for (const Set& s : alt_) if (s.tex && s.w == w && s.h == h && s.fmt == fmt) return true; return false; }   // DE-362: the sets of the other dynres levels are kept
    md::GpuTextureHandle Tex() const { return tex_; }
    SDL_GPUSampler* Sampler() const { return sampler_; }
    int W() const { return w_; }
    int H() const { return h_; }
    // Upscale the scene target into `out_tex` (the real swapchain) in its own render pass.
    // `sharp_mode` (#207, owner 31.10): 0 Full = 5 taps + adaptive sharpen, 1 Light = 3 taps unsharp mask (upscale_light.frag), 2 Off = one bilinear tap
    void Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex, float sharpness, int sharp_mode = 0);
    // #198 S1: Snapdragon GSR 1 (shaders/sgsr1.frag) instead of the bilinear + sharpen of Apply(): the scene target (internal size) onto `out_tex`
    void ApplySgsr1(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex);
    bool Sgsr1Ready() const { return sgsr1_ready_; }

private:
    void DestroyTarget();
    GpuPipeline pipeline_, light_pipeline_, sgsr1_pipeline_;
    bool sgsr1_ready_ = false, light_ready_ = false;
    md::GpuTextureHandle tex_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTextureFormat fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    int w_ = 0, h_ = 0;
    struct Set { md::GpuTextureHandle tex = nullptr; SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_INVALID; int w = 0, h = 0; bool Empty() const { return !tex; } };
    Set alt_[4]; int alt_rr_ = 0;   // DE-362: up to 4 more sizes (5 dynres levels with the active one)
    bool ready_ = false;
};
