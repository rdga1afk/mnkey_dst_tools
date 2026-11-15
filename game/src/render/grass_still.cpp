#include "grass_still.h"
#include <cstdio>
#include <cstdlib>

namespace md::grassstill {

namespace {
struct State {
    GroundLivePass ov; bool tried = false, ok = false;
    SDL_GPUTexture* base = nullptr; int bw = 0, bh = 0; SDL_GPUTextureFormat bfmt = SDL_GPU_TEXTUREFORMAT_INVALID;
    bool base_valid = false, dirty = false;
    SDL_GPUDevice* dev = nullptr;
} S;

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
    if (!S.ov.EnsureTargets(gw, gh)) return false;
    if (S.base && (S.bw != gw || S.bh != gh || S.bfmt != fmt)) { SDL_ReleaseGPUTexture(dev, S.base); S.base = nullptr; S.base_valid = false; }
    if (!S.base) {
        SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = (Uint32)gw; ti.height = (Uint32)gh; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        S.base = SDL_CreateGPUTexture(dev, &ti); if (!S.base) return false;
        S.bw = gw; S.bh = gh; S.bfmt = fmt; S.base_valid = false;
    }
    if (!S.base_valid) { Copy(gf.cmd, ground, S.base, gw, gh); S.base_valid = true; S.dirty = false; }
    else if (S.dirty) Copy(gf.cmd, S.base, ground, gw, gh);
    for (int i = 0; i < 3; ++i) gf.target[i] = S.ov.Target(i);
    gf.depth = S.ov.DepthStencil(); gf.w = gw; gf.h = gh; gf.clear = true;
    grasspass::Record(gf);
    S.ov.Decode(gf.cmd);
    if (!S.ov.Light(gf.cmd, f, lp)) return false;
    SDL_GPUColorTargetInfo cti = {}; cti.texture = ground; cti.load_op = SDL_GPU_LOADOP_LOAD; cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dti = {}; dti.texture = S.ov.DepthStencil(); dti.load_op = SDL_GPU_LOADOP_DONT_CARE; dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE; dti.cycle = false;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(gf.cmd, &cti, 1, &dti);
    if (!rp) return false;
    const bool ok = S.ov.DrawFog(rp, gf.cmd, f, fp, fmt, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT);
    SDL_EndGPURenderPass(rp);
    S.dirty = true;
    return ok;
}

void Restore(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* ground) {
    if (S.dirty && S.base && ground && cmd) Copy(cmd, S.base, ground, S.bw, S.bh);
    S.dirty = false; S.base_valid = false;
}

void Shutdown() {
    if (S.base && S.dev) SDL_ReleaseGPUTexture(S.dev, S.base);
    S.ov.Shutdown(); S = State{};
}

}  // namespace md::grassstill
