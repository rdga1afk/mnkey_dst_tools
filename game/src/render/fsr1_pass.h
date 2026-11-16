#pragma once
// Fsr1Pass -- #156: FSR 1 (AMD FidelityFX FSR v1.0.2, MIT; shaders/fsr1): EASU (internal resolution -> display size, into an intermediate target of the swapchain format) and RCAS
// (sharpening, onto the swapchain), two fullscreen fragment passes (shaders/fsr1_easu.frag, fsr1_rcas.frag). No jitter, no motion vectors: a replacement of the bilinear upscale of UpscalePass.
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>

class Fsr1Pass {
public:
    bool Init();
    void Shutdown();
    bool Ready() const { return ready_; }
    // (Re)create the intermediate target at the display size in `fmt` (the swapchain format). Call outside a frame (like UpscalePass::Ensure).
    void Ensure(int w, int h, SDL_GPUTextureFormat fmt);
    // EASU: src = the scene colour (src_w x src_h) -> the display-size target Tex() (out_w x out_h)
    void Easu(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* src, int src_w, int src_h, int out_w, int out_h);
    // RCAS: Tex() -> the real swapchain `out`; sharpness 0..1 (1 = the strongest; AMD's default 0.2 stops = 0.9)
    // `src` = another display-size texture (the SGSR 2 history) instead of Tex(); `perceptual_in` = its values are linear and RCAS wants perceptual ones (sRGB swapchain)
    void Rcas(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out, float sharpness, SDL_GPUTexture* src = nullptr, int perceptual_in = -1);
    SDL_GPUTexture* Tex() const { return tex_; }
    int W() const { return w_; }
    int H() const { return h_; }
    // CPU side of FsrEasuCon / FsrRcasCon (ffx_fsr1.h), exposed for the test
    static void EasuConstants(float in_w, float in_h, float src_w, float src_h, float out_w, float out_h, uint32_t con[16]);
    static void RcasConstants(float sharpness01, uint32_t con[4]);
private:
    bool Srgb() const { return fmt_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB || fmt_ == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB; }
    GpuPipeline easu_, rcas_;
    SDL_GPUTexture* tex_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTextureFormat fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    int w_ = 0, h_ = 0;
    bool ready_ = false;
};
