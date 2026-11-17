#pragma once
// HialgoPass -- DE-312 (owner 17.11): the upscale path below 1.0 after HiAlgo's ideas, written from our notes (no HiAlgo shader text):
// FXAA (Kenshi or HiAlgo profile) on the internal-resolution LDR frame in gamma, optional stair-only blur, then the neighbour-clamped upscale with clamped sharpening onto the swapchain.
// md.set_upscaler("hialgo") (fractional scales) / "x2" (corner-aligned parity grid of the exact 2x case). Off by default.
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>

class HialgoPass {
public:
    bool Init();
    void Shutdown();
    bool Ready() const { return ready_; }
    // (Re)create the two internal-size targets in `fmt`. Outside a frame, before the command buffer is acquired.
    void Ensure(int w, int h, SDL_GPUTextureFormat fmt);
    // src = the internal-size scene target (UpscalePass::Tex()), out = the swapchain; src_w x src_h its size, out_w x out_h the swapchain size.
    // fxaa_profile 0 kenshi / 1 hialgo; stair_taps 0 = no stair blur; align_x2 = corner-aligned parity grid; catmull = 4x4 Catmull-Rom (else bilinear)
    void Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle src, int src_w, int src_h, md::GpuTextureHandle out, int out_w, int out_h,
               int fxaa_profile, int stair_taps, bool align_x2, bool catmull);
private:
    GpuPipeline fxaa_, blur_, up_;
    SDL_GPUTexture* a_ = nullptr; SDL_GPUTexture* b_ = nullptr; int w_ = 0, h_ = 0; SDL_GPUTextureFormat fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUSampler* linear_ = nullptr; SDL_GPUSampler* nearest_ = nullptr;
    bool ready_ = false;
};
