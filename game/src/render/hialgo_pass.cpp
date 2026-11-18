#include "hialgo_pass.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cstdio>
#include <utility>

bool HialgoPass::Init() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert"; pd.layout.count = 0; pd.layout.stride = 0;
    pd.raster.depth_test = false; pd.raster.depth_write = false; pd.raster.cull_back = false; pd.has_depth_target = false;
    pd.frag_uniform_bufs = 1; pd.frag_samplers = 1;
    pd.frag_path = "shaders/fxaa.frag";        if (!fxaa_.Create(pd)) { fprintf(stderr, "[HialgoPass] FXAA pipeline failed\n"); return false; }
    pd.frag_path = "shaders/hialgo_blur.frag"; if (!blur_.Create(pd)) { fprintf(stderr, "[HialgoPass] blur pipeline failed\n"); return false; }
    pd.frag_path = "shaders/hialgo_up.frag";   if (!up_.Create(pd))   { fprintf(stderr, "[HialgoPass] upscale pipeline failed\n"); return false; }
    SDL_GPUSamplerCreateInfo si{};
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;  linear_  = GpuCreateSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; nearest_ = GpuCreateSampler(dev, &si);
    ready_ = linear_ && nearest_;
    return ready_;
}

void HialgoPass::Shutdown() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev) {
        if (a_) GpuReleaseTexture(dev, a_);
        if (b_) GpuReleaseTexture(dev, b_);
        if (alt_a_) GpuReleaseTexture(dev, alt_a_);
        if (alt_b_) GpuReleaseTexture(dev, alt_b_);
        if (linear_) GpuReleaseSampler(dev, linear_);
        if (nearest_) GpuReleaseSampler(dev, nearest_);
    }
    a_ = b_ = alt_a_ = alt_b_ = nullptr; linear_ = nearest_ = nullptr; w_ = h_ = alt_w_ = alt_h_ = 0;
    fxaa_.Destroy(); blur_.Destroy(); up_.Destroy(); ready_ = false;
}

void HialgoPass::Ensure(int w, int h, SDL_GPUTextureFormat fmt) {
    if (!ready_ || w <= 0 || h <= 0) return;
    if (a_ && b_ && w == w_ && h == h_ && fmt == fmt_) return;
    if (alt_a_ && alt_b_ && w == alt_w_ && h == alt_h_ && fmt == alt_fmt_) {   // back to the previous size: swap
        std::swap(a_, alt_a_); std::swap(b_, alt_b_); std::swap(w_, alt_w_); std::swap(h_, alt_h_); std::swap(fmt_, alt_fmt_);
        return;
    }
    md::GpuDevice::Get().WarnIfActive("HialgoPass::Ensure");
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (alt_a_) GpuReleaseTexture(dev, alt_a_);
    if (alt_b_) GpuReleaseTexture(dev, alt_b_);
    alt_a_ = a_; alt_b_ = b_; alt_w_ = w_; alt_h_ = h_; alt_fmt_ = fmt_;   // the current pair becomes the alt one
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.width = (uint32_t)w; ti.height = (uint32_t)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    a_ = GpuCreateTexture(dev, &ti); b_ = GpuCreateTexture(dev, &ti);
    if (a_ && b_) { w_ = w; h_ = h; fmt_ = fmt; } else { fprintf(stderr, "[HialgoPass] targets %dx%d create failed\n", w, h); w_ = h_ = 0; }
}

void HialgoPass::Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle src, int src_w, int src_h, md::GpuTextureHandle out, int out_w, int out_h,
                       int fxaa_profile, int stair_taps, bool align_x2, bool catmull, SDL_GPUTexture* haze_depth) {
    if (!ready_ || !a_ || !b_ || w_ != src_w || h_ != src_h || !src || !out) return;
    GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd; cpd.cmd = cmd; cpd.depth_tex = nullptr; cpd.load_color = false;
    {   // 1. FXAA on the internal frame (gamma: the frame is the tonemapped LDR composite)
        cpd.color_tex[0] = a_; cb.BeginColorPass(cpd); cb.BindPipeline(&fxaa_);
        SDL_GPUTextureSamplerBinding t{ src, linear_ }; cb.BindFragmentSamplers(0, &t, 1);
        const float u[8] = { 1.f / (float)src_w, 1.f / (float)src_h, 0.f, 0.f,
                             fxaa_profile == 1 ? 0.15f : 0.75f, fxaa_profile == 1 ? 0.20f : 0.166f, 0.0833f, 0.f };
        cb.PushFragmentUniforms(0, u, sizeof(u)); cb.Draw(3, 0); cb.EndPass();
    }
    SDL_GPUTexture* last = a_; SDL_GPUTexture* spare = b_;
    if (md::HeatHazePass::Get().Needed()) { md::HeatHazePass::Get().Run(cmd, last, spare, haze_depth, src_w, src_h); std::swap(last, spare); }
    if (stair_taps > 0) {   // 2. stair-only blur
        cpd.color_tex[0] = spare; cb.BeginColorPass(cpd); cb.BindPipeline(&blur_);
        SDL_GPUTextureSamplerBinding t{ last, linear_ }; cb.BindFragmentSamplers(0, &t, 1);
        const float u[4] = { 1.f / (float)src_w, 1.f / (float)src_h, (float)(stair_taps > 31 ? 31 : stair_taps), 0.f };
        cb.PushFragmentUniforms(0, u, sizeof(u)); cb.Draw(3, 0); cb.EndPass();
        last = spare;
    }
    {   // 3. neighbour-clamped upscale + clamped sharpening onto the swapchain
        cpd.color_tex[0] = out; cpd.color_dont_care = true; cb.BeginColorPass(cpd); cb.BindPipeline(&up_);
        SDL_GPUTextureSamplerBinding t{ last, linear_ }; cb.BindFragmentSamplers(0, &t, 1);
        const float u[8] = { (float)src_w, (float)src_h, (float)out_w, (float)out_h, catmull ? 1.f : 0.f, 1.f, align_x2 ? 1.f : 0.f, 0.f };
        cb.PushFragmentUniforms(0, u, sizeof(u)); cb.Draw(3, 0); cb.EndPass();
    }
}
