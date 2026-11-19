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
    if (!cmaa_.Init()) { fprintf(stderr, "[HialgoPass] CMAA 2 init failed\n"); return false; }   // DE-360: the AA of this path (no CMAA 2 = this path stays off)
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
        for (Set& s : alt_) { if (s.a) GpuReleaseTexture(dev, s.a); if (s.b) GpuReleaseTexture(dev, s.b); }
        if (linear_) GpuReleaseSampler(dev, linear_);
        if (nearest_) GpuReleaseSampler(dev, nearest_);
    }
    a_ = b_ = nullptr; for (Set& s : alt_) s = Set{}; linear_ = nearest_ = nullptr; w_ = h_ = 0;
    cmaa_.Shutdown(); blur_.Destroy(); up_.Destroy(); ready_ = false;
}

void HialgoPass::Ensure(int w, int h, SDL_GPUTextureFormat fmt) {
    if (!ready_ || w <= 0 || h <= 0) return;
    if (cmaa_.Ready()) cmaa_.EnsureTargets(w, h);   // DE-362: the CMAA 2 targets of this level are made with the pair
    if (a_ && b_ && w == w_ && h == h_ && fmt == fmt_) return;
    {
        Set cur{ a_, b_, w_, h_, fmt_ };
        if (md::SlotSwapIn(cur, alt_, [&](const Set& s) { return s.w == w && s.h == h && s.fmt == fmt; })) { a_ = cur.a; b_ = cur.b; w_ = cur.w; h_ = cur.h; fmt_ = cur.fmt; return; }   // a stored size: swap
    }
    md::GpuDevice::Get().WarnIfActive("HialgoPass::Ensure");
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    {   // a new size: the active pair is stored, the oldest stored one goes when all slots are used
        Set cur{ a_, b_, w_, h_, fmt_ };
        md::SlotPushCur(cur, alt_, alt_rr_, [&](Set& s) { if (s.a) GpuReleaseTexture(dev, s.a); if (s.b) GpuReleaseTexture(dev, s.b); s = Set{}; });
        a_ = b_ = nullptr; w_ = h_ = 0;
    }
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.width = (uint32_t)w; ti.height = (uint32_t)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    a_ = GpuCreateTexture(dev, &ti); b_ = GpuCreateTexture(dev, &ti);
    if (a_ && b_) { w_ = w; h_ = h; fmt_ = fmt; } else { fprintf(stderr, "[HialgoPass] targets %dx%d create failed\n", w, h); w_ = h_ = 0; }
}

void HialgoPass::Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle src, int src_w, int src_h, md::GpuTextureHandle out, int out_w, int out_h,
                       int stair_taps, bool align_x2, bool catmull, SDL_GPUTexture* haze_depth, bool aa) {
    if (!ready_ || !a_ || !b_ || w_ != src_w || h_ != src_h || !src || !out) return;
    GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd; cpd.cmd = cmd; cpd.depth_tex = nullptr; cpd.load_color = false;
    // 1. CMAA 2 on the internal frame (gamma: the frame is the tonemapped LDR composite): src -> a_
    if (aa && !cmaa_.Apply(cmd, src, src_w, src_h, a_)) return;
    SDL_GPUTexture* last = aa ? a_ : src; SDL_GPUTexture* spare = aa ? b_ : a_;   // md.set_aa("off"): the stages after it read the source directly
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
