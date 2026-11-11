#pragma once
// UpscalePass -- resolution_scale (#253): the 3D scene is rendered into an internal-resolution colour target of the
// swapchain's own format (so every already-built pipeline stays compatible) and this pass upscales it to the real
// swapchain (bilinear + optional cheap sharpen). Sized/recreated by Ensure() BEFORE the frame's command buffer is
// acquired (same rule as the depth / G-buffer resize checks).
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>

class UpscalePass {
public:
    bool Init();
    void Shutdown();
    // (Re)create the scene colour target at w x h in `fmt` (the swapchain format). No-op if unchanged.
    void Ensure(int w, int h, SDL_GPUTextureFormat fmt);
    bool Ready() const { return ready_; }
    bool HasTarget(int w, int h) const { return tex_ && w == w_ && h == h_; }
    bool Cached(int w, int h, SDL_GPUTextureFormat fmt) const { return (tex_ && w == w_ && h == h_ && fmt == fmt_) || (alt_tex_ && w == alt_w_ && h == alt_h_ && fmt == alt_fmt_); }   // #194: the previous size is kept
    md::GpuTextureHandle Tex() const { return tex_; }
    SDL_GPUSampler* Sampler() const { return sampler_; }
    int W() const { return w_; }
    int H() const { return h_; }
    // Upscale the scene target into `out_tex` (the real swapchain) in its own render pass.
    // `sharp_mode` (#207, owner 31.10): 0 Full = 5 taps + adaptive sharpen, 1 Light = 3 taps unsharp mask (upscale_light.frag), 2 Off = one bilinear tap
    void Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex, float sharpness, int sharp_mode = 0);
    // #180 FXAA 3.11 (Kenshi's own setup, shaders/fxaa.frag): the scene target (same size as the swapchain) onto `out_tex`
    void ApplyFxaa(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex);
    // #198 S1: Snapdragon GSR 1 (shaders/sgsr1.frag) instead of the bilinear + sharpen of Apply(): the scene target (internal size) onto `out_tex`
    void ApplySgsr1(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex);
    bool Sgsr1Ready() const { return sgsr1_ready_; }

private:
    void DestroyTarget();
    GpuPipeline pipeline_, light_pipeline_, fxaa_pipeline_, sgsr1_pipeline_;
    bool fxaa_ready_ = false, sgsr1_ready_ = false, light_ready_ = false;
    md::GpuTextureHandle tex_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTextureFormat fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    int w_ = 0, h_ = 0;
    md::GpuTextureHandle alt_tex_ = nullptr; SDL_GPUTextureFormat alt_fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID; int alt_w_ = 0, alt_h_ = 0;
    bool ready_ = false;
};
