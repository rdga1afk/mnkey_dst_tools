#include "hzb_pass.h"
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cstdio>
#include <cstring>

HzbPass& HzbPass::Get() { static HzbPass s; return s; }

bool HzbPass::Init() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/upscale.vert"; pd.frag_path = "shaders/hzb_down.frag";
    pd.layout.count = 0; pd.layout.stride = 0;
    pd.raster.depth_test = false; pd.raster.depth_write = false; pd.raster.cull_back = false;
    pd.has_depth_target = false; pd.frag_uniform_bufs = 1; pd.frag_samplers = 1;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    if (!pipe_.Create(pd)) { fprintf(stderr, "[HzbPass] pipeline create failed\n"); return false; }
    SDL_GPUSamplerCreateInfo si{}; si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samp_ = GpuCreateSampler(dev, &si);
    SDL_GPUTextureCreateInfo ti{}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT; ti.width = W; ti.height = H; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_ = GpuCreateTexture(dev, &ti);
    for (int i = 0; i < kRing; ++i) { SDL_GPUTransferBufferCreateInfo tb{}; tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tb.size = W * H * 4; tb_[i] = SDL_CreateGPUTransferBuffer(dev, &tb); }
    ready_ = samp_ && tex_ && tb_[0];
    return ready_;
}

void HzbPass::Shutdown() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev) { for (int i = 0; i < kRing; ++i) if (tb_[i]) SDL_ReleaseGPUTransferBuffer(dev, tb_[i]); if (tex_) GpuReleaseTexture(dev, tex_); if (samp_) GpuReleaseSampler(dev, samp_); }
    pipe_.Destroy(); ready_ = false; for (auto& t : tb_) t = nullptr; tex_ = nullptr; samp_ = nullptr;
}

bool HzbPass::Tick(float culled_share) { ++abs_; return culled_share >= 0.02f || (abs_ % 8) == 0; }

void HzbPass::Record(md::GpuCommandBufferHandle cmd, const GpuDepthTexture& depth, const float* vp16, const float* cam3, float near_m, float far_m) {
    if (!ready_ || !depth.SDLTexture()) return;
    const int cell = (depth.Width() + W - 1) / W;
    GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd; cpd.color_tex[0] = tex_; cpd.depth_tex = nullptr; cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&pipe_);
    SDL_GPUTextureSamplerBinding tsb{ depth.SDLTexture(), samp_ };
    cb.BindFragmentSamplers(0, &tsb, 1);
    float u[4] = { (float)cell, near_m, far_m, 0.f };
    cb.PushFragmentUniforms(0, u, sizeof(u));
    cb.Draw(3, 0);
    cb.EndPass();
    const int slot = (int)(frame_ % kRing); cur_slot_ = slot;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion r{}; r.texture = tex_; r.w = W; r.h = H; r.d = 1;
    SDL_GPUTextureTransferInfo d{}; d.transfer_buffer = tb_[slot]; d.pixels_per_row = W; d.rows_per_layer = H;
    SDL_DownloadFromGPUTexture(cp, &r, &d);
    SDL_EndGPUCopyPass(cp);
    memcpy(vp_[slot], vp16, 64); memcpy(cam_[slot], cam3, 12);
    rec_[slot] = abs_ + 1; rec_idx_[slot] = frame_;
    ++frame_;
}

HzbPass::View HzbPass::Latest() {
    View v;
    if (!ready_) return v;
    int best = -1; uint64_t best_abs = 0;
    for (int i = 0; i < kRing; ++i) {
        if (!rec_[i]) continue;
        const uint64_t a = rec_[i] - 1;
        if (a + kLatency > abs_ || abs_ - a > (uint64_t)kMaxAge) continue;   // not complete yet / too old
        if (best < 0 || a > best_abs) { best = i; best_abs = a; }
    }
    if (best < 0) return v;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (cpu_frame_ != rec_idx_[best]) {   // copy once: the mapped pointer is only valid while mapped
        const float* m = (const float*)SDL_MapGPUTransferBuffer(dev, tb_[best], false);
        if (!m) return v;
        memcpy(cpu_, m, sizeof(cpu_)); SDL_UnmapGPUTransferBuffer(dev, tb_[best]); cpu_frame_ = rec_idx_[best];
    }
    v.far_m = cpu_; memcpy(v.vp, vp_[best], 64); memcpy(v.cam, cam_[best], 12); v.valid = true;
    return v;
}
