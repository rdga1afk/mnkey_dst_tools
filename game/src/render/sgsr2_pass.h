#pragma once
// Sgsr2Pass -- dynres 0.6: Snapdragon GSR 2 (temporal upscale, 2-pass fragment variant, BSD-3, shaders/sgsr2_*.frag).
// Convert (internal-size depth + current/previous view-projection -> motion + depth clip, RGBA16F) then Upscale (scene colour + motion + previous
// output -> the display-size history, RGBA16F, ping-pong) then a plain copy of the history onto the swapchain. No jitter; motion comes from the depth only
// (static world: ground / buildings; NPCs and grass get the camera motion only for now).
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>

class Sgsr2Pass {
public:
    bool Init();
    void Shutdown();
    bool Ready() const { return ready_; }
    // (Re)create the motion target (render size) and both history targets (display size). Call outside a frame (like UpscalePass::Ensure).
    // Once per frame BEFORE any camera matrix is built: the next Halton (2, 3) phase of 16, scaled to the render size, into g_proj_jitter_ndc. Zero when `on` is false.
    void BeginFrame(bool on);
    void Ensure(int render_w, int render_h, int out_w, int out_h);
    // scene = internal-size colour, depth = internal-size sampleable depth (standard Z), vp = this frame's view-projection WITH the jitter (column-major, y-up NDC)
    // rcas != nullptr and rcas_sharp > 0: FSR 1 RCAS (shared Fsr1Pass pipeline) sharpens the history onto `out` instead of the plain copy; `srgb_out` = the swapchain is sRGB
    void Apply(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* scene, SDL_GPUTexture* depth, const float vp[16], md::GpuTextureHandle out, class Fsr1Pass* rcas = nullptr, float rcas_sharp = 0.f, bool srgb_out = false);
private:
    GpuPipeline convert_, upscale_, lite_, blit_;
    SDL_GPUTexture* motion_ = nullptr; SDL_GPUTexture* zero_ = nullptr; bool zero_ready_ = false; SDL_GPUTexture* hist_[2] = {};
    SDL_GPUSampler* nearest_ = nullptr; SDL_GPUSampler* linear_ = nullptr;
    float jit_px_[2] = {};   // the jitter of this frame in render pixels (-0.5..0.5): the UBO jitterOffset, and the part removed from the view-projection
    int jit_n_ = 0;
    int rw_ = 0, rh_ = 0, ow_ = 0, oh_ = 0, cur_ = 0;
    double prev_vp_[16] = {}; bool have_prev_ = false;
    bool ready_ = false;
};
