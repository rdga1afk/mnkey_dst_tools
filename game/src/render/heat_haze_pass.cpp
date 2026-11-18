#include "heat_haze_pass.h"
#ifdef MD_HLSL_RUNTIME
#include "kenshi_hlsl.h"
#include "spirv_combine.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/render/asset_cache.h>
#include <monkey_dust/world/weather.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstring>

namespace md {

HeatHazePass& HeatHazePass::Get() { static HeatHazePass p; return p; }

bool HeatHazePass::Wanted() { static const bool w = getenv("MD_HEATHAZE") && (getenv("MD_HEATHAZE")[0] == '1' || getenv("MD_HEATHAZE")[0] == '2'); return w; }

namespace {
// full-screen triangle with the StdQuad_vp varyings of the program (TEXCOORD0: (0,0) top-left; TEXCOORD1 = the frustum ray, unused by the program)
const char* kVert = R"(void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 texCoord : TEXCOORD0, out float3 ray : TEXCOORD1)
{ const float2 p = float2((id << 1) & 2, id & 2); pos = float4(p * 2.0 - 1.0, 0.0, 1.0); texCoord = float2(p.x, 1.0 - p.y); ray = float3(0.0, 0.0, 0.0); })";

SDL_GPUTexture* Make(SDL_GPUDevice* dev, int w, int h, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags use) {
    SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.usage = use; ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ti);
}
}   // namespace

bool HeatHazePass::Init(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt) {
    dev_ = dev; fmt_ = fmt;
    const std::string kd = KenshiDir();
    if (!dev || kd.empty()) { MD_LOG(MD_LOG_WARNING, "heat haze: no device / KENSHI_DIR"); return false; }
    const std::string dir = kd + "/data/materials/", src = dir + "post/heathaze.hlsl";
    hlsl::Compiled vc, fc;
    if (!hlsl::Compile(kVert, {}, 'v', vc) || !vc.valid) { MD_LOG(MD_LOG_WARNING, "heat haze: vertex: %s", vc.log.c_str()); return false; }
    // The program assigns to its `uniform float gameTime` (DX9 allows it, glslang does not): the compatibility rule of path1_adapter.md s.2 for a global, applied here to the two exact statements
    // (in memory, the install file is not touched); any other text of the file => fail closed.
    std::string text; { std::ifstream f(src, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); text = ss.str(); }
    auto sub = [&](const char* a, const char* b) { const size_t i = text.find(a); if (i == std::string::npos) return false; text.replace(i, strlen(a), b); return true; };
    if (!sub("uniform float gameTime;", "uniform float gameTime__u; static float gameTime;") || !sub("gameTime *= 100;", "gameTime = gameTime__u * 100;")) { MD_LOG(MD_LOG_WARNING, "heat haze: heathaze.hlsl is not the expected text: off"); return false; }
    if (!hlsl::Compile(text, { src }, 'f', fc) || !fc.valid) { MD_LOG(MD_LOG_WARNING, "heat haze: fragment: %s %s", fc.log.c_str(), fc.valid_log.c_str()); return false; }
    std::vector<hlsl::UniformMember> mem; uint32_t sz = 0;
    if (!hlsl::GlobalBlockMembers(fc.spv, mem, sz) || !sz) { MD_LOG(MD_LOG_WARNING, "heat haze: no uniform block"); return false; }
    ub_.assign(sz + 16, 0);
    for (const auto& m : mem) { if (m.name == "gameTime__u") off_time_ = (int)m.offset; else if (m.name == "heatHaze") off_haze_ = (int)m.offset; }
    if (off_time_ < 0 || off_haze_ < 0) { MD_LOG(MD_LOG_WARNING, "heat haze: uniforms gameTime / heatHaze not found"); return false; }
    SDL_GPUShaderCreateInfo sv = {}; sv.code = (const Uint8*)vc.spv.data(); sv.code_size = vc.spv.size() * 4; sv.entrypoint = "main"; sv.format = SDL_GPU_SHADERFORMAT_SPIRV; sv.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_ = SDL_CreateGPUShader(dev, &sv);
    SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)fc.spv.data(); sf.code_size = fc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = 4; sf.num_uniform_buffers = 1;
    fs_ = SDL_CreateGPUShader(dev, &sf);
    if (!vs_ || !fs_) { MD_LOG(MD_LOG_WARNING, "heat haze: shader create failed"); return false; }
    SDL_GPUColorTargetDescription ct = {}; ct.format = fmt;
    SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = fs_; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gi.target_info.color_target_descriptions = &ct; gi.target_info.num_color_targets = 1;
    pipe_ = SDL_CreateGPUGraphicsPipeline(dev, &gi);
    SDL_GPUSamplerCreateInfo si = {}; si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    point_ = SDL_CreateGPUSampler(dev, &si);                // heathaze.compositor: depth "filtering none", clamp
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; linear_ = SDL_CreateGPUSampler(dev, &si);   // base: bilinear, clamp
    GpuTexture* f = AssetCache::Get().GetOrLoadDDS2D((dir + "FlowHAZE.dds").c_str());       // the material's default sampling: repeat, linear
    GpuTexture* p = AssetCache::Get().GetOrLoadDDS2D((dir + "Perturber.dds").c_str());
    if (f) { flow_ = f->SDLTexture(); flow_smp_ = f->SDLSampler(); }
    if (p) { perturb_ = p->SDLTexture(); perturb_smp_ = p->SDLSampler(); }
    neutral_depth_ = Make(dev, 1, 1, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);   // contents 0 = "sky" -> 1 in the program
    if (neutral_depth_) { SDL_GPUCommandBuffer* c = SDL_AcquireGPUCommandBuffer(dev); if (c) { SDL_GPUColorTargetInfo cti = {}; cti.texture = neutral_depth_; cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE; SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(c, &cti, 1, nullptr); if (rp) SDL_EndGPURenderPass(rp); SDL_SubmitGPUCommandBuffer(c); } }
    ready_ = pipe_ && point_ && linear_ && flow_ && perturb_ && flow_smp_ && perturb_smp_ && neutral_depth_;
    if (!ready_) MD_LOG(MD_LOG_WARNING, "heat haze: pipeline / sampler / texture missing"); else MD_LOG(MD_LOG_INFO, "heat haze ready");
    return ready_;
}

void HeatHazePass::Shutdown() {
    if (!dev_) return;
    if (pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, pipe_);
    if (vs_) SDL_ReleaseGPUShader(dev_, vs_);
    if (fs_) SDL_ReleaseGPUShader(dev_, fs_);
    if (point_) SDL_ReleaseGPUSampler(dev_, point_);
    if (linear_) SDL_ReleaseGPUSampler(dev_, linear_);
    if (scratch_) SDL_ReleaseGPUTexture(dev_, scratch_);
    if (neutral_depth_) SDL_ReleaseGPUTexture(dev_, neutral_depth_);
    pipe_ = nullptr; vs_ = nullptr; fs_ = nullptr; point_ = linear_ = nullptr; scratch_ = nullptr; neutral_depth_ = nullptr; sw_ = sh_ = 0; ready_ = false; dev_ = nullptr;   // the DDS textures belong to the asset cache
}

void HeatHazePass::Tick(float haze_scaled, float sun_y) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    float dt = last_s_ < 0.0 ? 0.f : (float)(now - last_s_);
    last_s_ = now; if (dt > 0.1f) dt = 0.1f;
    time_ += dt;
    amp_ = weather::HeatHazeStep(amp_, haze_scaled, sun_y, dt);
}

void HeatHazePass::EnsureScratch(int w, int h) {
    if (!ready_ || w <= 0 || h <= 0 || (scratch_ && w == sw_ && h == sh_)) return;
    if (scratch_) SDL_ReleaseGPUTexture(dev_, scratch_);
    scratch_ = Make(dev_, w, h, fmt_, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    sw_ = scratch_ ? w : 0; sh_ = scratch_ ? h : 0;
}

void HeatHazePass::Run(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src, SDL_GPUTexture* dst, SDL_GPUTexture* depth, int, int) {
    if (!ready_ || !cmd || !src || !dst) return;
    SDL_GPUColorTargetInfo cti = {}; cti.texture = dst; cti.load_op = SDL_GPU_LOADOP_DONT_CARE; cti.store_op = SDL_GPU_STOREOP_STORE;   // the triangle covers the target
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, nullptr);
    if (!rp) return;
    memcpy(&ub_[off_time_], &time_, 4); memcpy(&ub_[off_haze_], &amp_, 4);
    const SDL_GPUTextureSamplerBinding tb[4] = { { flow_, flow_smp_ }, { perturb_, perturb_smp_ }, { depth ? depth : neutral_depth_, point_ }, { src, linear_ } };
    SDL_BindGPUGraphicsPipeline(rp, pipe_);
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 4);
    SDL_PushGPUFragmentUniformData(cmd, 0, ub_.data(), (Uint32)ub_.size() - 16);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

}   // namespace md
#else
namespace md {
HeatHazePass& HeatHazePass::Get() { static HeatHazePass p; return p; }
bool HeatHazePass::Wanted() { return false; }
bool HeatHazePass::Init(SDL_GPUDevice*, SDL_GPUTextureFormat) { return false; }
void HeatHazePass::Shutdown() {}
void HeatHazePass::Tick(float, float) {}
void HeatHazePass::EnsureScratch(int, int) {}
void HeatHazePass::Run(SDL_GPUCommandBuffer*, SDL_GPUTexture*, SDL_GPUTexture*, SDL_GPUTexture*, int, int) {}
}
#endif
