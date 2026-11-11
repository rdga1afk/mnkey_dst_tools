#include "upscale_pass.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cstdio>

bool UpscalePass::Init() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert";
    pd.frag_path = "shaders/upscale.frag";
    pd.layout.count  = 0;
    pd.layout.stride = 0;
    pd.raster.depth_test  = false;
    pd.raster.depth_write = false;
    pd.raster.cull_back   = false;
    pd.has_depth_target   = false;
    pd.frag_uniform_bufs  = 1;
    pd.frag_samplers      = 1;
    if (!pipeline_.Create(pd)) {
        fprintf(stderr, "[UpscalePass] pipeline create failed\n");
        return false;
    }
    {
        GpuPipeline::Desc fd = pd;
        fd.frag_path = "shaders/fxaa.frag";
        fxaa_ready_ = fxaa_pipeline_.Create(fd);
        if (!fxaa_ready_) fprintf(stderr, "[UpscalePass] FXAA pipeline create failed\n");
        fd.frag_path = "shaders/upscale_light.frag";   // #207: «Sharpening while moving» = Light
        light_ready_ = light_pipeline_.Create(fd);
        if (!light_ready_) fprintf(stderr, "[UpscalePass] light pipeline create failed\n");
        fd.frag_path = "shaders/sgsr1.frag";
        sgsr1_ready_ = sgsr1_pipeline_.Create(fd);
        if (!sgsr1_ready_) fprintf(stderr, "[UpscalePass] SGSR1 pipeline create failed\n");
    }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = GpuCreateSampler(dev, &si);
    ready_ = sampler_ != nullptr;
    return ready_;
}

void UpscalePass::DestroyTarget() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev && tex_) GpuReleaseTexture(dev, tex_);
    tex_ = nullptr; w_ = h_ = 0;
    if (dev && alt_tex_) GpuReleaseTexture(dev, alt_tex_);
    alt_tex_ = nullptr; alt_w_ = alt_h_ = 0;
}

void UpscalePass::Shutdown() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    DestroyTarget();
    if (dev && sampler_) GpuReleaseSampler(dev, sampler_);
    sampler_ = nullptr;
    pipeline_.Destroy(); fxaa_pipeline_.Destroy(); fxaa_ready_ = false; sgsr1_pipeline_.Destroy(); sgsr1_ready_ = false;
    ready_ = false;
}

void UpscalePass::Ensure(int w, int h, SDL_GPUTextureFormat fmt) {
    if (!ready_ || w <= 0 || h <= 0) return;
    if (tex_ && w == w_ && h == h_ && fmt == fmt_) return;
    if (alt_tex_ && w == alt_w_ && h == alt_h_ && fmt == alt_fmt_) {   // #194: back to the previous size: swap, nothing is created or destroyed
        std::swap(tex_, alt_tex_); std::swap(w_, alt_w_); std::swap(h_, alt_h_); std::swap(fmt_, alt_fmt_);
        return;
    }
    md::GpuDevice::Get().WarnIfActive("UpscalePass::Ensure");
    {   // the oldest target goes, the current one becomes the alt one
        md::GpuDeviceHandle d0 = md::GpuDevice::Get().SDLDevice();
        if (d0 && alt_tex_) GpuReleaseTexture(d0, alt_tex_);
        alt_tex_ = tex_; alt_w_ = w_; alt_h_ = h_; alt_fmt_ = fmt_;
        tex_ = nullptr; w_ = h_ = 0;
    }
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTextureCreateInfo ti{};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = fmt;
    ti.width                = (uint32_t)w;
    ti.height               = (uint32_t)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_ = GpuCreateTexture(dev, &ti);
    if (tex_) { w_ = w; h_ = h; fmt_ = fmt; }
    else fprintf(stderr, "[UpscalePass] scene target %dx%d create failed\n", w, h);
}

void UpscalePass::Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex, float sharpness, int sharp_mode) {
    if (!ready_ || !tex_ || !out_tex) return;
    const bool light = sharp_mode == 1 && light_ready_;
    if (sharp_mode == 2) sharpness = 0.f;
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd           = cmd;
    cpd.color_tex[0]  = out_tex;
    cpd.depth_tex     = nullptr;
    cpd.load_color    = false; // the upscale overwrites the whole swapchain image
    cpd.color_dont_care = true;   // the pass writes every pixel of the swapchain image
    cb.BeginColorPass(cpd);
    cb.BindPipeline(light ? &light_pipeline_ : &pipeline_);
    SDL_GPUTextureSamplerBinding tsb{ tex_, sampler_ };
    cb.BindFragmentSamplers(0, &tsb, 1);
    struct UpscaleUBO { float texel_x, texel_y, sharp, _pad; } ubo;
    ubo.texel_x = 1.f / (float)w_;
    ubo.texel_y = 1.f / (float)h_;
    ubo.sharp   = sharpness;
    ubo._pad    = 0.f;
    cb.PushFragmentUniforms(0, &ubo, sizeof(ubo));
    cb.Draw(3, 0);
    cb.EndPass();
}

void UpscalePass::ApplySgsr1(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex) {   // #198 S1
    if (!ready_ || !sgsr1_ready_ || !tex_ || !out_tex) return;
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd; cpd.color_tex[0] = out_tex; cpd.depth_tex = nullptr; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&sgsr1_pipeline_);
    SDL_GPUTextureSamplerBinding tsb{ tex_, sampler_ };
    cb.BindFragmentSamplers(0, &tsb, 1);
    float u[4] = { 1.f / (float)w_, 1.f / (float)h_, (float)w_, (float)h_ };
    cb.PushFragmentUniforms(0, u, sizeof(u));
    cb.Draw(3, 0);
    cb.EndPass();
}

void UpscalePass::ApplyFxaa(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out_tex) {
    if (!ready_ || !fxaa_ready_ || !tex_ || !out_tex) return;
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd; cpd.color_tex[0] = out_tex; cpd.depth_tex = nullptr; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&fxaa_pipeline_);
    SDL_GPUTextureSamplerBinding tsb{ tex_, sampler_ };
    cb.BindFragmentSamplers(0, &tsb, 1);
    float u[4] = { 1.f / (float)w_, 1.f / (float)h_, 0.f, 0.f };
    cb.PushFragmentUniforms(0, u, sizeof(u));
    cb.Draw(3, 0);
    cb.EndPass();
}
