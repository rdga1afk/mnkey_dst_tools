#include "ground_split_pass.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/kenshi_hdr.h>
#include <cmath>
#include <cstdio>

bool GroundSplitPass::Init() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert";
    pd.frag_path = "shaders/ground_composite.frag";
    pd.layout.count  = 0;
    pd.layout.stride = 0;
    pd.raster.depth_test  = false;
    pd.raster.depth_write = false;
    pd.raster.cull_back   = false;
    pd.has_depth_target   = true;   // the main pass has a depth attachment
    pd.frag_uniform_bufs  = 0;
    pd.frag_samplers      = 1;
    if (!pipeline_.Create(pd)) { fprintf(stderr, "[GroundSplitPass] pipeline create failed\n"); return false; }
    pd.frag_path = "shaders/ground_composite_exp.frag";
    pd.frag_samplers = 2;
    adapt_ready_ = pipeline_exp_.Create(pd);
    if (adapt_ready_) {
        GpuComputePipeline::Desc cd{};
        cd.glsl_path = "shaders/kenshi_lum_adapt.comp";
        cd.num_uniform_buffers = 1; cd.num_samplers = 2; cd.num_readwrite_storage_textures = 1;
        cd.threadcount_x = 256; cd.threadcount_y = 1; cd.threadcount_z = 1;
        adapt_ready_ = adapt_pipeline_.Create(cd);
        for (int i = 0; adapt_ready_ && i < 2; ++i) {
            SDL_GPUTextureCreateInfo ti = {};
            ti.type = SDL_GPU_TEXTURETYPE_2D; ti.width = ti.height = 1; ti.layer_count_or_depth = 1; ti.num_levels = 1;
            ti.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            ti.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            lum_tex_[i] = GpuCreateTexture(dev, &ti);
            adapt_ready_ = lum_tex_[i] != nullptr;
        }
        if (!adapt_ready_) fprintf(stderr, "[GroundSplitPass] auto-exposure resources failed (mode 4 falls back to the fixed exposure)\n");
    } else fprintf(stderr, "[GroundSplitPass] exposed composite pipeline create failed\n");
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = GpuCreateSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    linear_sampler_ = GpuCreateSampler(dev, &si);
    ready_ = sampler_ != nullptr && linear_sampler_ != nullptr;
    return ready_;
}

void GroundSplitPass::Shutdown() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev && sampler_) GpuReleaseSampler(dev, sampler_);
    sampler_ = nullptr;
    if (dev && linear_sampler_) GpuReleaseSampler(dev, linear_sampler_);
    linear_sampler_ = nullptr;
    target_[0].Shutdown(); target_[1].Shutdown(); alt_target_[0].Shutdown(); alt_target_[1].Shutdown();
    for (auto& t : lum_tex_) { if (dev && t) GpuReleaseTexture(dev, t); t = nullptr; }
    if (dev && ds_tex_) GpuReleaseTexture(dev, ds_tex_);
    ds_tex_ = nullptr; ds_w_ = ds_h_ = 0;
    pipeline_exp_.Destroy(); adapt_pipeline_.Destroy();
    pipeline_.Destroy();
    ready_ = false; adapt_ready_ = false;
}

SDL_GPUTexture* GroundSplitPass::StencilDepthTex(int w, int h) {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev || w < 2 || h < 2) return nullptr;
    if (ds_tex_ && ds_w_ == w && ds_h_ == h) return ds_tex_;
    if (ds_tex_) { GpuReleaseTexture(dev, ds_tex_); ds_tex_ = nullptr; }
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET; ti.width = (Uint32)w; ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ds_tex_ = SDL_CreateGPUTexture(dev, &ti);
    ds_w_ = w; ds_h_ = h;
    if (!ds_tex_) fprintf(stderr, "[GroundSplitPass] stencil depth texture create failed: %s\n", SDL_GetError());
    return ds_tex_;
}

bool GroundSplitPass::Ensure(int w, int h, SDL_GPUTextureFormat fmt, bool history) {
    if (!ready_ || w < 2 || h < 2) return false;
    xs_tex_ = nullptr;
    const bool same = target_[0].SDLTexture() && target_[0].Width() == w && target_[0].Height() == h && fmt_ == fmt;
    if (same && (!history || target_[1].SDLTexture())) {
        if (!history && target_[1].SDLTexture()) { target_[1].Shutdown(); idx_ = 0; return true; }
        return false;
    }
    const SDL_GPUTextureUsageFlags u = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (history && target_[1].SDLTexture() && fmt_ == fmt) xs_tex_ = target_[idx_].SDLTexture();   // #194: the last ground target of the old size is the history of this frame
    if (alt_target_[0].SDLTexture() && alt_target_[0].Width() == w && alt_target_[0].Height() == h && alt_fmt_ == fmt) {   // #194: back to the previous size: swap, nothing is created or destroyed
        std::swap(target_[0], alt_target_[0]); std::swap(target_[1], alt_target_[1]); std::swap(fmt_, alt_fmt_);
        idx_ = 0;
        if (history && !target_[1].SDLTexture()) target_[1].Init(w, h, fmt, u);
        if (!history && target_[1].SDLTexture()) target_[1].Shutdown();
        return xs_tex_ == nullptr;
    }
    alt_target_[0].Shutdown(); alt_target_[1].Shutdown();   // the oldest set goes, the current one becomes the alt one
    std::swap(target_[0], alt_target_[0]); std::swap(target_[1], alt_target_[1]); alt_fmt_ = fmt_;
    idx_ = 0; fmt_ = fmt;
    target_[0].Init(w, h, fmt, u);
    if (history) target_[1].Init(w, h, fmt, u);
    return xs_tex_ == nullptr;
}

void GroundSplitPass::Composite(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd) {
    if (!ready_ || !Tex() || !rp) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&pipeline_);
    SDL_GPUTextureSamplerBinding b = { Tex(), sampler_ };
    pv.BindFragmentSamplers(0, &b, 1);
    pv.Draw(3, 1, 0, 0);
}

void GroundSplitPass::Adapt(md::GpuCommandBufferHandle cmd, float dt, float lo, float hi, float override_mean) {
    if (!adapt_ready_ || !Tex()) return;
    GpuComputeStorageBindings sb;
    sb.cmd = cmd;
    const int dst = 1 - lum_idx_;
    sb.rw_textures[0] = { lum_tex_[dst], false };
    sb.num_rw_textures = 1;
    GpuComputePass pass;
    pass.Begin(&adapt_pipeline_, sb);
    if (!pass.SDLPass()) return;
    SDL_GPUTextureSamplerBinding b[2] = { { Tex(), sampler_ }, { lum_tex_[lum_idx_], sampler_ } };
    pass.BindSamplers(0, b, 2);
    float ubo[8] = { 1.f - std::exp(-dt * md::kKenshiAutoexpRate), lo, hi, override_mean, lum_valid_ ? 0.f : 1.f, 0.f, 0.f, 0.f };
    pass.PushUniforms(0, ubo, sizeof(ubo));
    pass.Dispatch(1, 1, 1);
    pass.End();
    lum_idx_ = dst; lum_valid_ = true;
}

void GroundSplitPass::CompositeExposed(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd) {
    if (!adapt_ready_ || !Tex() || !rp) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&pipeline_exp_);
    SDL_GPUTextureSamplerBinding b[2] = { { Tex(), sampler_ }, { lum_tex_[lum_idx_], sampler_ } };
    pv.BindFragmentSamplers(0, b, 2);
    pv.Draw(3, 1, 0, 0);
}
