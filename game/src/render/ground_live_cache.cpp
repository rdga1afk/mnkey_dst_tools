#include "ground_live_cache.h"
#include "kenshi_hlsl.h"
#include <monkey_dust/platform/md_log.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace md {
namespace {

const char* kVert = R"(float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2.0 - 1.0, 0.0, 1.0); })";
// ring pixel -> strip texel: st_off.xy = world texel - ring pixel - (strip origin - apron) of the drawn sub-rectangle, st_off.z = 1 when the strip was baked transposed
const char* kStore = R"(Texture2D<float4> sRgb : register(s0);
Texture2D<float4> sAux : register(s1);
uniform float4 st_off;
struct StoreOut { float4 a : SV_Target0; float4 b : SV_Target1; };
StoreOut main(float4 fc : SV_Position)
{
    int2 l = int2(fc.xy) + int2(st_off.xy);
    int2 p = st_off.z > 0.5 ? l.yx : l;
    StoreOut o; o.a = sRgb.Load(int3(p, 0)); o.b = float4(sAux.Load(int3(p, 0)).rgb, 1.0); return o;
})";

SDL_GPUTexture* MakeTarget(SDL_GPUDevice* dev, int w, int h, SDL_GPUTextureFormat fmt) {
    SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ti);
}

}   // namespace

bool GroundLiveCache::Init(SDL_GPUDevice* dev) {
    dev_ = dev;
    const int sw = kMaxLong + 2 * kApron, sh = kMaxShort + 2 * kApron;
    const SDL_GPUTextureFormat sf[3] = { SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R32_FLOAT };
    for (int i = 0; i < 3; ++i) if (!(strip_[i] = MakeTarget(dev, sw, sh, sf[i]))) return false;
    for (auto& d : dec_) if (!(d = MakeTarget(dev, sw, sh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT))) return false;
    for (auto& r : ring_) for (auto& t : r) if (!(t = MakeTarget(dev, GroundCacheRings::kSize, GroundCacheRings::kSize, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM))) return false;
    hlsl::Compiled vc, fc;
    if (!hlsl::Compile(kVert, {}, 'v', vc) || !hlsl::Compile(kStore, {}, 'f', fc)) { MD_LOG(MD_LOG_WARNING, "ground cache: store shader: %s%s", vc.log.c_str(), fc.log.c_str()); return false; }
    SDL_GPUShaderCreateInfo si = {}; si.entrypoint = "main"; si.format = SDL_GPU_SHADERFORMAT_SPIRV;
    si.code = (const Uint8*)vc.spv.data(); si.code_size = vc.spv.size() * 4; si.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_ = SDL_CreateGPUShader(dev, &si);
    si.code = (const Uint8*)fc.spv.data(); si.code_size = fc.spv.size() * 4; si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; si.num_samplers = 2; si.num_uniform_buffers = 1;
    fs_ = SDL_CreateGPUShader(dev, &si);
    if (!vs_ || !fs_) return false;
    SDL_GPUColorTargetDescription ct[2] = {}; ct[0].format = ct[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = fs_; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 2;
    pipe_ = SDL_CreateGPUGraphicsPipeline(dev, &gi);
    SDL_GPUSamplerCreateInfo smp = {}; smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_NEAREST; smp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = SDL_CreateGPUSampler(dev, &smp);
    ready_ = pipe_ && nearest_;
    MD_LOG(ready_ ? MD_LOG_INFO : MD_LOG_WARNING, "ground cache: rings %d x %d^2 RGBA8 A/B %s", GroundCacheRings::kRings, GroundCacheRings::kSize, ready_ ? "ready" : "failed");
    return ready_;
}

void GroundLiveCache::Shutdown() {
    if (!dev_) return;
    if (pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, pipe_);
    if (vs_) SDL_ReleaseGPUShader(dev_, vs_);
    if (fs_) SDL_ReleaseGPUShader(dev_, fs_);
    if (nearest_) SDL_ReleaseGPUSampler(dev_, nearest_);
    for (auto& r : ring_) for (auto& t : r) { if (t) SDL_ReleaseGPUTexture(dev_, t); t = nullptr; }
    for (auto& t : strip_) { if (t) SDL_ReleaseGPUTexture(dev_, t); t = nullptr; }
    for (auto& t : dec_) { if (t) SDL_ReleaseGPUTexture(dev_, t); t = nullptr; }
    pipe_ = nullptr; vs_ = fs_ = nullptr; nearest_ = nullptr; ready_ = false; dev_ = nullptr;
}

void GroundLiveCache::Store(SDL_GPUCommandBuffer* cmd, int r, const CacheRect& s, bool transposed) {
    if (!ready_ || s.Empty() || r < 0 || r >= GroundCacheRings::kRings) return;
    constexpr int N = GroundCacheRings::kSize;
    SDL_GPUColorTargetInfo cti[2] = {};
    for (int i = 0; i < 2; ++i) { cti[i].texture = ring_[r][i]; cti[i].load_op = SDL_GPU_LOADOP_LOAD; cti[i].store_op = SDL_GPU_STOREOP_STORE; }
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, cti, 2, nullptr);
    if (!rp) return;
    SDL_BindGPUGraphicsPipeline(rp, pipe_);
    const SDL_GPUTextureSamplerBinding tb[2] = { { dec_[0], nearest_ }, { dec_[1], nearest_ } };
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 2);
    auto wrap = [](int32_t v) { return ((v % N) + N) % N; };
    // split the strip at the ring's wrap: x pieces [x0, next multiple of N), ..., the same along z (a strip is at most N long, so at most 2 x 2 pieces)
    for (int32_t x = s.x0; x < s.x1;) {
        const int32_t xe = std::min<int32_t>(s.x1, x + (N - wrap(x)));
        for (int32_t z = s.z0; z < s.z1;) {
            const int32_t ze = std::min<int32_t>(s.z1, z + (N - wrap(z)));
            const int32_t rx = wrap(x), rz = wrap(z);
            const float off[4] = { (float)(x - rx - (s.x0 - kApron)), (float)(z - rz - (s.z0 - kApron)), transposed ? 1.f : 0.f, 0.f };
            const SDL_Rect sc = { rx, rz, xe - x, ze - z }; SDL_SetGPUScissor(rp, &sc);
            SDL_PushGPUFragmentUniformData(cmd, 0, off, sizeof(off));
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            z = ze;
        }
        x = xe;
    }
    SDL_EndGPURenderPass(rp);
}

void GroundLiveCache::DumpTick(SDL_GPUCommandBuffer* cmd, uint64_t frame) {
    static const uint64_t at = getenv("MD_GROUND_CACHE_DUMP") ? (uint64_t)std::atoll(getenv("MD_GROUND_CACHE_DUMP")) : 0;
    if (!at || !ready_) return;
    constexpr int N = GroundCacheRings::kSize;
    if (frame == at) {
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        for (int r = 0; r < GroundCacheRings::kRings; ++r) {
            SDL_GPUTransferBufferCreateInfo bi = {}; bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; bi.size = (Uint32)N * N * 4;
            dump_[r] = SDL_CreateGPUTransferBuffer(dev_, &bi); if (!dump_[r]) continue;
            SDL_GPUTextureRegion rg = {}; rg.texture = ring_[r][0]; rg.w = N; rg.h = N; rg.d = 1;
            SDL_GPUTextureTransferInfo ti = { dump_[r], 0, (Uint32)N, (Uint32)N };
            SDL_DownloadFromGPUTexture(cp, &rg, &ti);
        }
        SDL_EndGPUCopyPass(cp);
        dump_frame_ = frame;
    } else if (dump_frame_ && frame == dump_frame_ + 3) {   // the buffers are read three frames later (the frame ring), so no stall
        for (int r = 0; r < GroundCacheRings::kRings; ++r) {
            if (!dump_[r]) continue;
            const unsigned char* px = (const unsigned char*)SDL_MapGPUTransferBuffer(dev_, dump_[r], false);
            char path[96]; snprintf(path, sizeof(path), "temp_/ground_cache_ring%d.ppm", r);
            if (FILE* fp = px ? fopen(path, "wb") : nullptr) {
                fprintf(fp, "P6\n%d %d\n255\n", N, N);
                for (int i = 0; i < N * N; ++i) fwrite(px + 4 * i, 1, 3, fp);
                fclose(fp); MD_LOG(MD_LOG_INFO, "ground cache: wrote %s", path);
            }
            if (px) SDL_UnmapGPUTransferBuffer(dev_, dump_[r]);
            SDL_ReleaseGPUTransferBuffer(dev_, dump_[r]); dump_[r] = nullptr;
        }
        dump_frame_ = 0;
    }
}

}   // namespace md

