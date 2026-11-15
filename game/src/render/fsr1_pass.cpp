#include "fsr1_pass.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
uint32_t Bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
uint32_t PackHalf2(float a) {   // two equal IEEE halves (FsrRcasCon: packHalf2x16), round toward zero, normal numbers only
    uint32_t u = Bits(a); uint32_t sgn = (u >> 16) & 0x8000u; int e = (int)((u >> 23) & 0xFF) - 127 + 15; uint32_t m = (u >> 13) & 0x3FFu;
    uint32_t h = (e <= 0) ? sgn : (e >= 31 ? (sgn | 0x7C00u) : (sgn | ((uint32_t)e << 10) | m));
    return h | (h << 16);
}
}

void Fsr1Pass::EasuConstants(float iw, float ih, float sw, float sh, float ow, float oh, uint32_t con[16]) {   // FsrEasuCon(con0..con3, ...)
    const float rsx = 1.f / sw, rsy = 1.f / sh;
    con[0] = Bits(iw * (1.f / ow)); con[1] = Bits(ih * (1.f / oh)); con[2] = Bits(0.5f * iw * (1.f / ow) - 0.5f); con[3] = Bits(0.5f * ih * (1.f / oh) - 0.5f);
    con[4] = Bits(rsx); con[5] = Bits(rsy); con[6] = Bits(1.f * rsx); con[7] = Bits(-1.f * rsy);
    con[8] = Bits(-1.f * rsx); con[9] = Bits(2.f * rsy); con[10] = Bits(1.f * rsx); con[11] = Bits(2.f * rsy);
    con[12] = Bits(0.f * rsx); con[13] = con[14] = con[15] = 0;
}

void Fsr1Pass::RcasConstants(float sharpness01, uint32_t con[4]) {   // FsrRcasCon: the sharpness in stops, 0 = maximum; AMD's sample maps its 0..1 slider as 2 - 2 * s (as FSR 2)
    const float stops = 2.0f - 2.0f * sharpness01;
    const float lin = std::exp2(-stops);
    con[0] = Bits(lin); con[1] = PackHalf2(lin); con[2] = 0; con[3] = 0;
}

bool Fsr1Pass::Init() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert";
    pd.layout.count = 0; pd.layout.stride = 0;
    pd.raster.depth_test = false; pd.raster.depth_write = false; pd.raster.cull_back = false;
    pd.has_depth_target = false; pd.frag_uniform_bufs = 1; pd.frag_samplers = 1;
    pd.frag_path = "shaders/fsr1_easu.frag";
    if (!easu_.Create(pd)) { fprintf(stderr, "[Fsr1Pass] EASU pipeline create failed\n"); return false; }
    pd.frag_path = "shaders/fsr1_rcas.frag";
    if (!rcas_.Create(pd)) { fprintf(stderr, "[Fsr1Pass] RCAS pipeline create failed\n"); return false; }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = GpuCreateSampler(dev, &si);
    ready_ = sampler_ != nullptr;
    return ready_;
}

void Fsr1Pass::Shutdown() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev && tex_) GpuReleaseTexture(dev, tex_);
    tex_ = nullptr; w_ = h_ = 0;
    if (dev && sampler_) GpuReleaseSampler(dev, sampler_);
    sampler_ = nullptr; easu_.Destroy(); rcas_.Destroy(); ready_ = false;
}

void Fsr1Pass::Ensure(int w, int h, SDL_GPUTextureFormat fmt) {
    if (!ready_ || w <= 0 || h <= 0) return;
    if (tex_ && w == w_ && h == h_ && fmt == fmt_) return;
    md::GpuDevice::Get().WarnIfActive("Fsr1Pass::Ensure");
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (tex_) { GpuReleaseTexture(dev, tex_); tex_ = nullptr; }
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.width = (uint32_t)w; ti.height = (uint32_t)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_ = GpuCreateTexture(dev, &ti);
    if (tex_) { w_ = w; h_ = h; fmt_ = fmt; } else fprintf(stderr, "[Fsr1Pass] target %dx%d create failed\n", w, h);
}

void Fsr1Pass::Easu(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* src, int sw, int sh, int ow, int oh) {
    if (!ready_ || !tex_ || !src) return;
    GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd; cpd.color_tex[0] = tex_; cpd.depth_tex = nullptr; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&easu_);
    SDL_GPUTextureSamplerBinding b{ src, sampler_ };
    cb.BindFragmentSamplers(0, &b, 1);
    struct { uint32_t con[16]; float params[4]; } u{};
    EasuConstants((float)sw, (float)sh, (float)sw, (float)sh, (float)ow, (float)oh, u.con);
    u.params[0] = Srgb() ? 1.f : 0.f;   // the sampler decodes an sRGB target: EASU wants perceptual values
    cb.PushFragmentUniforms(0, &u, sizeof(u));
    cb.Draw(3, 0); cb.EndPass();
}

void Fsr1Pass::Rcas(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle out, float sharpness) {
    if (!ready_ || !tex_ || !out) return;
    GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd; cpd.color_tex[0] = out; cpd.depth_tex = nullptr; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&rcas_);
    SDL_GPUTextureSamplerBinding b{ tex_, sampler_ };
    cb.BindFragmentSamplers(0, &b, 1);
    struct { uint32_t con[4]; float params[4]; } u{};
    RcasConstants(sharpness, u.con);
    u.params[0] = Srgb() ? 1.f : 0.f;
    cb.PushFragmentUniforms(0, &u, sizeof(u));
    cb.Draw(3, 0); cb.EndPass();
}

