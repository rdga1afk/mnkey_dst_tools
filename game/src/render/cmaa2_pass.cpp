// Intel CMAA 2 host side (DE-358), written for our SDL_GPU HAL after the sequence of GameTechDev/CMAA2 vaCMAA2DX11.cpp (Apache-2.0, THIRD_PARTY.md):
// edge detection (16x16 groups, 14x14 outputs per group, 2x2 pixels per thread), dispatch-arguments pass, indirect candidate processing, dispatch-arguments pass, indirect apply.
#include "cmaa2_pass.h"
#include "kenshi_hlsl.h"
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef MD_CMAA2_HLSL
#define MD_CMAA2_HLSL "tools/shaders/cmaa2/CMAA2.hlsl"
#endif

namespace {
const char* kEntry[4] = { "EdgesColor2x2CS", "ComputeDispatchArgsCS", "ProcessCandidatesCS", "DeferredColorApply2x2CS" };
const Uint32 kThreads[4][3] = { { 16, 16, 1 }, { 1, 1, 1 }, { 128, 1, 1 }, { 4, 32, 1 } };

std::string ShaderPath() {
    const char* e = getenv("MD_CMAA2_HLSL");
    return e ? e : MD_CMAA2_HLSL;
}

SDL_GPUBuffer* MakeBuffer(SDL_GPUDevice* dev, Uint32 bytes, SDL_GPUBufferUsageFlags usage) {
    SDL_GPUBufferCreateInfo bi{}; bi.usage = usage; bi.size = bytes;
    return SDL_CreateGPUBuffer(dev, &bi);
}
SDL_GPUTexture* MakeTexture(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt, int w, int h, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ti{}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.usage = usage;
    return SDL_CreateGPUTexture(dev, &ti);
}
}

bool Cmaa2Pass::Init() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    const std::string src = ShaderPath();
    for (int i = 0; i < 4; ++i) {
        const std::string glue = "#define CMAA2_UAV_STORE_TYPED 1\n#define CMAA2_UAV_STORE_CONVERT_TO_SRGB 0\n#define CMAA2_UAV_STORE_TYPED_UNORM_FLOAT 1\n#define CMAA2_EDGE_DETECTION_LUMA_PATH 1\n#define " + std::string(kEntry[i]) + " main\n#include \"" + src + "\"\n";
        md::hlsl::Compiled c;
        if (!md::hlsl::Compile(glue, { src }, 'c', c) || !c.valid) { fprintf(stderr, "[Cmaa2] %s: %s\n", kEntry[i], c.log.c_str()); Shutdown(); return false; }
        SDL_GPUComputePipelineCreateInfo ci{};
        ci.code = (const Uint8*)c.spv.data(); ci.code_size = c.spv.size() * 4; ci.entrypoint = "main"; ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.num_readonly_storage_textures = 1; ci.num_readwrite_storage_textures = 3; ci.num_readwrite_storage_buffers = 5;
        ci.threadcount_x = kThreads[i][0]; ci.threadcount_y = kThreads[i][1]; ci.threadcount_z = kThreads[i][2];
        pipe_[i] = SDL_CreateGPUComputePipeline(dev, &ci);
        if (!pipe_[i]) { fprintf(stderr, "[Cmaa2] pipeline %s: %s\n", kEntry[i], SDL_GetError()); Shutdown(); return false; }
    }
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert"; pd.frag_path = "shaders/upscale.frag";
    pd.layout.count = 0; pd.layout.stride = 0;
    pd.raster.depth_test = false; pd.raster.depth_write = false; pd.raster.cull_back = false;
    pd.has_depth_target = false; pd.frag_uniform_bufs = 1; pd.frag_samplers = 1;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    if (!copy_pipeline_.Create(pd)) { Shutdown(); return false; }
    pd.color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    if (!blit_pipeline_.Create(pd)) { Shutdown(); return false; }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = GpuCreateSampler(dev, &si);
    ready_ = sampler_ != nullptr;
    return ready_;
}

void Cmaa2Pass::DestroyTargets() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return;
    for (SDL_GPUTexture** t : { &in_, &out_, &edges_, &heads_ }) { if (*t) SDL_ReleaseGPUTexture(dev, *t); *t = nullptr; }
    for (SDL_GPUBuffer** b : { &cand_, &loc_, &item_, &ctrl_, &indirect_ }) { if (*b) SDL_ReleaseGPUBuffer(dev, *b); *b = nullptr; }
    w_ = h_ = 0; ctrl_zeroed_ = false;
}

void Cmaa2Pass::Shutdown() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    DestroyTargets();
    if (dev) for (SDL_GPUComputePipeline*& p : pipe_) { if (p) SDL_ReleaseGPUComputePipeline(dev, p); p = nullptr; }
    copy_pipeline_.Destroy(); blit_pipeline_.Destroy();
    if (dev && sampler_) GpuReleaseSampler(dev, sampler_);
    sampler_ = nullptr; ready_ = false;
}

bool Cmaa2Pass::EnsureTargets(int w, int h) {
    if (in_ && w == w_ && h == h_) return true;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    DestroyTargets();
    const SDL_GPUBufferUsageFlags rw = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    const Uint32 px = (Uint32)w * (Uint32)h;
    in_ = MakeTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ);
    out_ = MakeTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
    edges_ = MakeTexture(dev, SDL_GPU_TEXTUREFORMAT_R32_UINT, (w + 1) / 2, h, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
    heads_ = MakeTexture(dev, SDL_GPU_TEXTUREFORMAT_R32_UINT, (w + 1) / 2, (h + 1) / 2, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
    cand_ = MakeBuffer(dev, px / 4 * 4, rw);
    item_ = MakeBuffer(dev, px / 2 * 8, rw);
    loc_ = MakeBuffer(dev, (px + 3) / 6 * 4, rw);
    ctrl_ = MakeBuffer(dev, 64, rw);
    indirect_ = MakeBuffer(dev, 16, rw | SDL_GPU_BUFFERUSAGE_INDIRECT);
    if (!in_ || !out_ || !edges_ || !heads_ || !cand_ || !item_ || !loc_ || !ctrl_ || !indirect_) { fprintf(stderr, "[Cmaa2] targets %dx%d: %s\n", w, h, SDL_GetError()); DestroyTargets(); return false; }
    w_ = w; h_ = h;
    return true;
}

bool Cmaa2Pass::Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle scene, int w, int h, md::GpuTextureHandle out_tex) {
    if (!ready_ || !scene || !out_tex || w <= 0 || h <= 0 || !EnsureTargets(w, h)) return false;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!ctrl_zeroed_) {   // the counters start at 0 and the second dispatch-arguments pass clears them for the next frame
        SDL_GPUTransferBufferCreateInfo ti{}; ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = 64;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
        if (!tb) return false;
        void* p = SDL_MapGPUTransferBuffer(dev, tb, false); memset(p, 0, 64); SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{ tb, 0 }; SDL_GPUBufferRegion dst{ ctrl_, 0, 64 };
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        ctrl_zeroed_ = true;
    }
    {   // scene (swapchain format) -> RGBA8 input image, one bilinear tap at the texel centres = a copy
        GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd;
        cpd.cmd = cmd; cpd.color_tex[0] = in_; cpd.depth_tex = nullptr; cpd.load_color = false; cpd.color_dont_care = true;
        cb.BeginColorPass(cpd); cb.BindPipeline(&copy_pipeline_);
        SDL_GPUTextureSamplerBinding tsb{ scene, sampler_ }; cb.BindFragmentSamplers(0, &tsb, 1);
        const float u[4] = { 1.f / (float)w, 1.f / (float)h, 0.f, 0.f }; cb.PushFragmentUniforms(0, u, sizeof(u));
        cb.Draw(3, 0); cb.EndPass();
    }
    {   // the output starts as the input: only the blended pixels are written on top
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation s{ in_, 0, 0, 0, 0, 0 }, d{ out_, 0, 0, 0, 0, 0 };
        SDL_CopyGPUTextureToTexture(cp, &s, &d, (Uint32)w, (Uint32)h, 1, false);
        SDL_EndGPUCopyPass(cp);
    }
    SDL_GPUStorageTextureReadWriteBinding rwt[3] = { { out_, 0, 0, false }, { edges_, 0, 0, false }, { heads_, 0, 0, false } };
    SDL_GPUStorageBufferReadWriteBinding rwb[5] = { { cand_, false }, { loc_, false }, { item_, false }, { ctrl_, false }, { indirect_, false } };
    auto pass = [&](int pi, int gx, int gy, bool indirect) {
        SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cmd, rwt, 3, rwb, 5);
        SDL_BindGPUComputePipeline(cp, pipe_[pi]);
        SDL_GPUTexture* ro = in_; SDL_BindGPUComputeStorageTextures(cp, 0, &ro, 1);
        if (indirect) SDL_DispatchGPUComputeIndirect(cp, indirect_, 0); else SDL_DispatchGPUCompute(cp, (Uint32)gx, (Uint32)gy, 1);
        SDL_EndGPUComputePass(cp);
    };
    pass(0, (w + 27) / 28, (h + 27) / 28, false);   // 14 x 14 outputs per group, two pixels each way per thread
    pass(1, 2, 1, false);
    pass(2, 0, 0, true);
    pass(1, 1, 2, false);
    pass(3, 0, 0, true);
    {   // the result onto the swapchain
        GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd;
        cpd.cmd = cmd; cpd.color_tex[0] = out_tex; cpd.depth_tex = nullptr; cpd.load_color = false; cpd.color_dont_care = true;
        cb.BeginColorPass(cpd); cb.BindPipeline(&blit_pipeline_);
        SDL_GPUTextureSamplerBinding tsb{ out_, sampler_ }; cb.BindFragmentSamplers(0, &tsb, 1);
        const float u[4] = { 1.f / (float)w, 1.f / (float)h, 0.f, 0.f }; cb.PushFragmentUniforms(0, u, sizeof(u));
        cb.Draw(3, 0); cb.EndPass();
    }
    return true;
}
