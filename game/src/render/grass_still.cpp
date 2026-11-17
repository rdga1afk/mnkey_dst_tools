#include "grass_still.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/gpu_pipeline.h>

float g_grass_scale = 1.f;   // md.set_grass_scale: the grass overlay at this fraction of the ground resolution (1 = as before)

namespace md::grassstill {

namespace {
struct State {
    GroundLivePass ov; bool tried = false, ok = false;
    SDL_GPUTexture* base = nullptr; int bw = 0, bh = 0; SDL_GPUTextureFormat bfmt = SDL_GPU_TEXTUREFORMAT_INVALID;
    bool base_valid = false, dirty = false;
    SDL_GPUDevice* dev = nullptr;
    GpuPipeline pmin, pcomp; SDL_GPUSampler *smp_near = nullptr, *smp_lin = nullptr; bool pipes_tried = false, pipes_ok = false; SDL_GPUTextureFormat pcomp_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture *hdepth = nullptr, *hcol = nullptr; int hw = 0, hh = 0; SDL_GPUTextureFormat hfmt = SDL_GPU_TEXTUREFORMAT_INVALID;
} S;

void ReleaseHalf() {
    if (S.hdepth) SDL_ReleaseGPUTexture(S.dev, S.hdepth);
    if (S.hcol) SDL_ReleaseGPUTexture(S.dev, S.hcol);
    S.hdepth = S.hcol = nullptr; S.hw = S.hh = 0;
}

bool EnsureHalf(SDL_GPUDevice* dev, int hw, int hh, SDL_GPUTextureFormat fmt) {
    if (S.hdepth && S.hcol && S.hw == hw && S.hh == hh && S.hfmt == fmt) return true;
    ReleaseHalf();
    SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)hw; ti.height = (Uint32)hh; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT; S.hdepth = SDL_CreateGPUTexture(dev, &ti);
    ti.format = fmt; S.hcol = SDL_CreateGPUTexture(dev, &ti);
    if (!S.hdepth || !S.hcol) { ReleaseHalf(); return false; }
    S.hw = hw; S.hh = hh; S.hfmt = fmt; return true;
}

bool EnsurePipes(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt) {
    if (S.pipes_tried && S.pcomp_fmt == fmt) return S.pipes_ok;
    if (S.pipes_tried) { S.pmin.Destroy(); S.pcomp.Destroy(); }
    S.pipes_tried = true; S.pcomp_fmt = fmt; S.pipes_ok = false;
    GpuPipeline::Desc pd; pd.vert_path = "shaders/upscale.vert"; pd.layout.count = 0; pd.layout.stride = 0;
    pd.raster.depth_test = false; pd.raster.depth_write = false; pd.raster.cull_back = false; pd.has_depth_target = false;
    pd.frag_path = "shaders/grass_depth_min.frag"; pd.frag_samplers = 1; pd.color_format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    if (!S.pmin.Create(pd)) { fprintf(stderr, "[GrassStill] depth-min pipeline failed\n"); return false; }
    pd.frag_path = "shaders/grass_composite.frag"; pd.frag_samplers = 2; pd.color_format = fmt;
    if (!S.pcomp.Create(pd)) { fprintf(stderr, "[GrassStill] composite pipeline failed\n"); return false; }
    if (!S.smp_near) {
        SDL_GPUSamplerCreateInfo si{}; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; S.smp_near = SDL_CreateGPUSampler(dev, &si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; S.smp_lin = SDL_CreateGPUSampler(dev, &si);
    }
    S.pipes_ok = S.smp_near && S.smp_lin; return S.pipes_ok;
}

void Copy(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src, SDL_GPUTexture* dst, int w, int h) {
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation a = {}, b = {}; a.texture = src; b.texture = dst;
    SDL_CopyGPUTextureToTexture(cp, &a, &b, (Uint32)w, (Uint32)h, 1, false);
    SDL_EndGPUCopyPass(cp);
}
}  // namespace

bool Enabled() { static const bool on = !(getenv("MD_GRASS_EVERY_FRAME") && getenv("MD_GRASS_EVERY_FRAME")[0] == '0'); return on; }

bool Run(SDL_GPUDevice* dev, const std::string& kd, SDL_GPUTexture* ground, SDL_GPUTextureFormat fmt, int gw, int gh, const GroundLiveFrame& f,
         const GroundLiveLightParams& lp, const GroundLiveFogParams& fp, grasspass::Frame gf) {
    if (!S.tried) { S.tried = true; S.dev = dev; S.ok = S.ov.Init(dev, kd) && S.ov.LightReady() && S.ov.FogReady(); if (!S.ok) fprintf(stderr, "[GrassStill] overlay pass not ready\n"); }
    if (!S.ok || !ground || !gf.cmd) return false;
    const float sc = std::min(1.f, std::max(0.25f, g_grass_scale));
    const bool half = sc < 0.999f;
    const int ow = half ? std::max(1, (int)std::lround(gw * sc)) : gw, oh = half ? std::max(1, (int)std::lround(gh * sc)) : gh;   // the overlay's own size
    if (half && (!EnsurePipes(dev, fmt) || !EnsureHalf(dev, ow, oh, fmt))) return false;
    if (!S.ov.EnsureTargets(ow, oh)) return false;
    if (S.base && (S.bw != gw || S.bh != gh || S.bfmt != fmt)) { SDL_ReleaseGPUTexture(dev, S.base); S.base = nullptr; S.base_valid = false; }
    if (!S.base) {
        SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = (Uint32)gw; ti.height = (Uint32)gh; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        S.base = SDL_CreateGPUTexture(dev, &ti); if (!S.base) return false;
        S.bw = gw; S.bh = gh; S.bfmt = fmt; S.base_valid = false;
    }
    if (!S.base_valid) { Copy(gf.cmd, ground, S.base, gw, gh); S.base_valid = true; S.dirty = false; }
    else if (S.dirty && !half) Copy(gf.cmd, S.base, ground, gw, gh);   // the half-resolution composite below rewrites every pixel of the ground from the base
    if (half) {   // nearest scene depth of each 2x2 block: the depth the half-resolution blades are tested against
        GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd; cpd.cmd = gf.cmd; cpd.color_tex[0] = S.hdepth; cpd.depth_tex = nullptr; cpd.load_color = false;
        cb.BeginColorPass(cpd); cb.BindPipeline(&S.pmin);
        SDL_GPUTextureSamplerBinding b0{ gf.terrain_depth, S.smp_near }; cb.BindFragmentSamplers(0, &b0, 1);
        cb.Draw(3, 0); cb.EndPass();
        gf.terrain_depth = S.hdepth;
    }
    for (int i = 0; i < 3; ++i) gf.target[i] = S.ov.Target(i);
    gf.depth = S.ov.DepthStencil(); gf.w = ow; gf.h = oh; gf.clear = true;
    grasspass::Record(gf);
    S.ov.Decode(gf.cmd);
    if (!S.ov.Light(gf.cmd, f, lp)) return false;
    SDL_GPUColorTargetInfo cti = {}; cti.texture = half ? S.hcol : ground; cti.load_op = half ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD; cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dti = {}; dti.texture = S.ov.DepthStencil(); dti.load_op = SDL_GPU_LOADOP_DONT_CARE; dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE; dti.cycle = false;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(gf.cmd, &cti, 1, &dti);
    if (!rp) return false;
    const bool ok = S.ov.DrawFog(rp, gf.cmd, f, fp, fmt, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT);
    SDL_EndGPURenderPass(rp);
    if (half && ok) {
        GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd; cpd.cmd = gf.cmd; cpd.color_tex[0] = ground; cpd.depth_tex = nullptr; cpd.load_color = false;
        cb.BeginColorPass(cpd); cb.BindPipeline(&S.pcomp);
        SDL_GPUTextureSamplerBinding tb[2] = { { S.base, S.smp_near }, { S.hcol, S.smp_lin } }; cb.BindFragmentSamplers(0, tb, 2);
        cb.Draw(3, 0); cb.EndPass();
    }
    S.dirty = true;
    return ok;
}

bool GetDebugTargets(DebugTargets& t) {
    if (!S.ok || !S.ov.AlbedoRgb() || !S.ov.AuxB() || !S.ov.Width()) return false;
    t.albedo = S.ov.AlbedoRgb(); t.aux = S.ov.AuxB(); t.dist = S.ov.Raw(2); t.w = S.ov.Width(); t.h = S.ov.Height(); return true;
}

void Restore(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* ground) {
    if (S.dirty && S.base && ground && cmd) Copy(cmd, S.base, ground, S.bw, S.bh);
    S.dirty = false; S.base_valid = false;
}

void Shutdown() {
    if (S.dev) { ReleaseHalf(); if (S.smp_near) SDL_ReleaseGPUSampler(S.dev, S.smp_near); if (S.smp_lin) SDL_ReleaseGPUSampler(S.dev, S.smp_lin); }
    S.pmin.Destroy(); S.pcomp.Destroy();
    if (S.base && S.dev) SDL_ReleaseGPUTexture(S.dev, S.base);
    S.ov.Shutdown(); S = State{};
}

}  // namespace md::grassstill
