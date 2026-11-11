#pragma once
// #173: the SkyX sky as a lat-long panorama (256 x 128, RGBA16F), redrawn when the sun has moved or every REDRAW_FRAMES frames (Dagor daSkies2: the sky is computed into a panorama and only read per frame).
// Update() records a render pass: call it with the frame's command buffer BEFORE the main scene pass is begun.
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>

class SkyPanorama {
public:
    static constexpr int W = 256, H = 128, REDRAW_FRAMES = 16, STRIPES = 4;   // STRIPES: the install dome's redraw is spread over this many frames
    static SkyPanorama& Get();
    bool Init();
    static void SetGameSpeed(float speed);   // the game speed multiplier (1 normal, 0 paused) that drives the star scroll of the install dome
    void Shutdown();
    void Update(md::GpuCommandBufferHandle cmd, const float sun_real[3], const float zenith[3], const float sun_colour[3]);   // redraws when due
    SDL_GPUTexture* Tex() const { return tex_; }
    SDL_GPUSampler* Sampler() const { return sampler_; }
    bool Ready() const { return ready_ && valid_; }
    int redraws() const { return redraws_; }
private:
    GpuPipeline pipe_;
    bool use_install_ = false;   // MD_SKY_INSTALL: SkyX_Skydome.hlsl of the install (path 1)
    GpuTexture star_, clouds_, clouds_normal_, clouds_tile_;   // data/materials/SkyX/*.dds
    SDL_GPUTexture* tex_ = nullptr; SDL_GPUSampler* sampler_ = nullptr;
    bool ready_ = false, valid_ = false, tried_ = false;
    float last_sun_[3] = { 0, 0, 0 }, cycle_sun_[3] = { 0, 0, 0 }, cycle_zenith_[3] = { 0, 0, 0 }, cycle_colour_[3] = { 0, 0, 0 }; int since_ = 0, redraws_ = 0, stripe_ = 0;   // stripe_: the band the running redraw draws next
};
