#pragma once
// GroundSplitPass -- #163 5b-2: the ground resolve in its own render pass (before the main pass) into a colour target of the scene format (alpha 1 = written), and the draw
// that copies it into the main pass (shaders/ground_composite.frag). Without it the ground colour is mixed with NPCs and water in the scene colour (no history possible).
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_compute.h>
#include <monkey_dust/render/gpu_sampler_texture.h>

class GroundSplitPass {
public:
    bool Init();
    void Shutdown();
    bool Ready() const { return ready_; }
    // (Re)create the ground target(s) at w x h in `fmt` (the scene colour format); history = a second one for the ping-pong of the ground reuse. Call before the frame's command
    // buffer is acquired. True when something was (re)created: the history is then gone.
    bool Ensure(int w, int h, SDL_GPUTextureFormat fmt, bool history);
    bool Cached(int w, int h, SDL_GPUTextureFormat fmt) const { return (target_[0].SDLTexture() && target_[0].Width() == w && target_[0].Height() == h && fmt_ == fmt) || (alt_target_[0].SDLTexture() && alt_target_[0].Width() == w && alt_target_[0].Height() == h && alt_fmt_ == fmt); }   // #194: the previous size is kept
    bool HasTarget(int w, int h) const { return target_[0].SDLTexture() && w == target_[0].Width() && h == target_[0].Height(); }
    // this frame's target / the previous frame's (after Flip); Composite reads this frame's
    SDL_GPUTexture* Tex() const { return target_[idx_].SDLTexture(); }
    SDL_GPUTexture* HistTex() const { return target_[1 - idx_].SDLTexture(); }
    // #194: the frame the size changed the history is the last ground target of the old size (read at its own size by the reuse resolve)
    bool CrossSize() const { return xs_tex_ != nullptr; }
    SDL_GPUTexture* ReuseHistTex() const { return xs_tex_ ? xs_tex_ : HistTex(); }
    SDL_GPUSampler* Sampler() const { return sampler_; }
    SDL_GPUSampler* LinearSampler() const { return linear_sampler_; }   // the history read of the ground reuse (Catmull-Rom taps)
    void Flip() { if (target_[1].SDLTexture()) idx_ = 1 - idx_; }   // once per frame, before the ground pass
    // inside the main pass: copy the written ground pixels into the scene colour.
    void Composite(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd);
    // #140 L4 (Kenshi auto-exposure, shaders/kenshi_lum_adapt.comp): the ground target holds the colour before the exposure. Adapt = one compute dispatch outside any pass (after the ground pass,
    // before the main pass): the mean luminance of this frame's ground target adapted from the last result. override_mean >= 0 replaces the measurement (the oracle).
    void Adapt(md::GpuCommandBufferHandle cmd, float dt, float lo, float hi, float override_mean);
    void ResetAdaptation() { lum_valid_ = false; }
    bool AdaptReady() const { return adapt_ready_; }
    bool IsFloat() const { return fmt_ == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; }
    SDL_GPUTextureFormat Fmt() const { return fmt_; }
    SDL_GPUTexture* StencilDepthTex(int w, int h);   // #207 stencil split: a w x h D32_FLOAT_S8_UINT depth-stencil target (created on demand, recreated on a size change); null on failure
    SDL_GPUTexture* LumTex() const { return lum_tex_[lum_idx_]; }   // the adapted luminance, 1x1 R32F
    void CompositeExposed(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd);

private:
    GpuPipeline pipeline_, pipeline_exp_;
    GpuComputePipeline adapt_pipeline_;
    SDL_GPUTexture* lum_tex_[2] = {};
    SDL_GPUTexture* ds_tex_ = nullptr; int ds_w_ = 0, ds_h_ = 0;   // #207
    int lum_idx_ = 0;
    bool lum_valid_ = false, adapt_ready_ = false;
    GpuColorTexture target_[2], alt_target_[2];
    SDL_GPUTextureFormat alt_fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    int idx_ = 0;
    SDL_GPUTexture* xs_tex_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUSampler* linear_sampler_ = nullptr;
    SDL_GPUTextureFormat fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    bool ready_ = false;
};
