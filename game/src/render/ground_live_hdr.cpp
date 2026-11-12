#include "ground_live_hdr.h"
#include "kenshi_hlsl.h"
#include "spirv_combine.h"
#include <monkey_dust/io/data_files.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/render/ground_slot.h>
#include <monkey_dust/render/gpu_device.h>
#include "ground_live_driver.h"
#include "scene_render.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace md {

GroundLiveHdr& GroundLiveHdr::Get() { static GroundLiveHdr h; return h; }

bool GroundLiveHdr::Active() {
    static const bool want = !(getenv("MD_GROUND_LIVE_HDR") && getenv("MD_GROUND_LIVE_HDR")[0] == '0');
    if (!want || !GroundLiveLightOn() || !GroundLiveDriver::Wanted() || SceneRender::Get().terrain_shading_projected_.HasTranslatedResolve()) return false;
    static bool tried = false;
    if (!tried) { tried = true; if (!Get().Init(GpuDevice::Get().SDLDevice())) MD_LOG(MD_LOG_WARNING, "[GroundLiveHdr] off: the install HDR chain did not initialise (fixed exposure)"); }
    return Get().Ready();
}

namespace {

// full-screen triangle with the StdQuad_vp varyings (TEXCOORD0: (0,0) top-left, SDL_GPU clip y is up)
const char* kVert = R"(void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 texCoord : TEXCOORD0)
{ const float2 p = float2((id << 1) & 2, id & 2); pos = float4(p * 2.0 - 1.0, 0.0, 1.0); texCoord = float2(p.x, 1.0 - p.y); })";

const char* kEntry[] = { "Downsample2x2Luminance_fp4", "Downsample3x3_fp4", "AdaptLuminance_fp4", "Copy_fp4", "Downsample3x3Brightpass_fp4", "BloomBlurV_fp4", "BloomBlurH_fp4", "Composite_fp4" };
const int kNumSamplers[] = { 1, 1, 3, 1, 3, 1, 1, 3 };   // highest binding + 1 (probe md_hdr_live_dry: inRTT 0, inBloom 1, inLum 2; unused bindings get a dummy)
const char* kUniformName[] = { "texelSize", "BLOOM_THRESHOLD", "EXPOSURE_KEY", "BLOOM_BLUR_SIGMA", "frameTime", "MIN_LUMINANCE", "MAX_LUMINANCE", "BLOOM_MAGNITUDE",
                               "AUTOEXP_ADAPTATION_RATE", "md_BLOOM_MAGNITUDE", "md_EXPOSURE_KEY" };
const int kLumSize[5] = { 1, 4, 16, 64, 128 };   // rt_lum0..rt_lum4 (main.compositor Resolve_HDR)

std::string ReadAll(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

SDL_GPUTexture* MakeTex(SDL_GPUDevice* dev, int w, int h, SDL_GPUTextureFormat fmt) {
    SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ti);
}

}   // namespace

bool GroundLiveHdr::Init(SDL_GPUDevice* dev) {
    dev_ = dev;
    const std::string kd = KenshiDir();
    if (!dev || kd.empty()) { MD_LOG(MD_LOG_WARNING, "ground live hdr: no device / KENSHI_DIR"); return false; }
    const std::string dir = kd + "/data/materials/", src = dir + "post/hdrfp4.hlsl";
    std::string glue = ReadAll(DataPath("game/src/render/ground_live_hdr.hlsl"));
    if (glue.empty() || glue.find("@HDR@") == std::string::npos) { MD_LOG(MD_LOG_WARNING, "ground live hdr: glue missing"); return false; }
    glue.replace(glue.find("@HDR@"), 5, src);
    std::vector<std::string> deps = { src };
    for (const char* d : { "common", "post", "deferred", "forward" })
        for (const auto& e : std::filesystem::directory_iterator(dir + d))
            if (e.path().extension() == ".hlsl" && e.path().string() != src) deps.push_back(e.path().string());
    hlsl::Compiled vc;
    if (!hlsl::Compile(kVert, {}, 'v', vc)) { MD_LOG(MD_LOG_WARNING, "ground live hdr: vertex: %s", vc.log.c_str()); return false; }
    SDL_GPUShaderCreateInfo sv = {}; sv.code = (const Uint8*)vc.spv.data(); sv.code_size = vc.spv.size() * 4; sv.entrypoint = "main"; sv.format = SDL_GPU_SHADERFORMAT_SPIRV; sv.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_ = SDL_CreateGPUShader(dev, &sv); if (!vs_) return false;

    bloom_fmt_ = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
        ? SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT : SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;   // the compositor's PF_R11G11B10_FLOAT
    for (int i = 0; i < kProgs; ++i) {
        Prog& p = prog_[i];
        for (int& o : p.off) o = -1;
        const std::string pre = i == kComposite ? std::string("#define MD_HDR_COMPOSITE\n") : std::string("#define ") + kEntry[i] + " main\n";
        hlsl::Compiled fc;
        if (!hlsl::Compile(pre + glue, deps, 'f', fc)) { MD_LOG(MD_LOG_WARNING, "ground live hdr: %s: %s", kEntry[i], fc.log.c_str()); return false; }
        std::vector<hlsl::UniformMember> mem; uint32_t sz = 0;
        if (hlsl::GlobalBlockMembers(fc.spv, mem, sz)) {
            p.ub_size = sz; p.ub.assign(sz + 16, 0);
            for (const auto& m : mem) for (int u = 0; u < kUniforms; ++u) if (m.name == kUniformName[u]) p.off[u] = (int)m.offset;
        }
        SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)fc.spv.data(); sf.code_size = fc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        sf.num_samplers = (Uint32)kNumSamplers[i]; sf.num_uniform_buffers = p.ub_size ? 1 : 0;
        p.fs = SDL_CreateGPUShader(dev, &sf);
        if (!p.fs) { MD_LOG(MD_LOG_WARNING, "ground live hdr: %s: shader create failed", kEntry[i]); return false; }
        if (i == kComposite) continue;
        SDL_GPUColorTargetDescription ct = {}; ct.format = (i == kBright || i == kBlurV || i == kBlurH) ? bloom_fmt_ : SDL_GPU_TEXTUREFORMAT_R32_FLOAT;   // rt_lum*, rt_adaptedLum, rt_lastLum: PF_FLOAT32_R
        SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = p.fs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        gi.target_info.color_target_descriptions = &ct; gi.target_info.num_color_targets = 1;
        p.pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi);
        if (!p.pipe) { MD_LOG(MD_LOG_WARNING, "ground live hdr: %s: pipeline create failed", kEntry[i]); return false; }
    }
    SDL_GPUSamplerCreateInfo si = {}; si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    point_ = SDL_CreateGPUSampler(dev, &si);                // hdr.material "filtering none", clamp
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    linear_ = SDL_CreateGPUSampler(dev, &si);               // the material default (bilinear), clamp
    for (int i = 0; i < 5; ++i) lum_[i] = MakeTex(dev, kLumSize[i], kLumSize[i], SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
    adapted_ = MakeTex(dev, 1, 1, SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
    last_ = MakeTex(dev, 1, 1, SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
    ready_ = point_ && linear_ && adapted_ && last_ && lum_[0] && lum_[1] && lum_[2] && lum_[3] && lum_[4];
    if (!ready_) MD_LOG(MD_LOG_WARNING, "ground live hdr: sampler / texture create failed");
    else MD_LOG(MD_LOG_INFO, "ground live hdr ready (bloom format %d)", (int)bloom_fmt_);
    reset_ = true; last_cleared_ = false;
    return ready_;
}

void GroundLiveHdr::Shutdown() {
    if (!dev_) return;
    for (int i = 0; i < n_comp_pipes_; ++i) if (comp_pipes_[i].pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, comp_pipes_[i].pipe);
    n_comp_pipes_ = 0;
    for (auto& p : prog_) { if (p.pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, p.pipe); if (p.fs) SDL_ReleaseGPUShader(dev_, p.fs); p = Prog(); }
    if (vs_) SDL_ReleaseGPUShader(dev_, vs_);
    for (auto& t : lum_) { if (t) SDL_ReleaseGPUTexture(dev_, t); t = nullptr; }
    for (SDL_GPUTexture** t : { &adapted_, &last_, &bright_, &bloom_ }) { if (*t) SDL_ReleaseGPUTexture(dev_, *t); *t = nullptr; }
    if (point_) SDL_ReleaseGPUSampler(dev_, point_);
    if (linear_) SDL_ReleaseGPUSampler(dev_, linear_);
    vs_ = nullptr; point_ = linear_ = nullptr; hdr_ = nullptr; bw_ = bh_ = 0; ready_ = false; dev_ = nullptr;
}

bool GroundLiveHdr::EnsureSized(int w, int h) {
    const int bw = w / 4 > 0 ? w / 4 : 1, bh = h / 4 > 0 ? h / 4 : 1;   // target_width_scaled 0.25
    if (bright_ && bw == bw_ && bh == bh_) return true;
    if (bright_) SDL_ReleaseGPUTexture(dev_, bright_);
    if (bloom_) SDL_ReleaseGPUTexture(dev_, bloom_);
    bright_ = MakeTex(dev_, bw, bh, bloom_fmt_); bloom_ = MakeTex(dev_, bw, bh, bloom_fmt_);
    bw_ = bw; bh_ = bh;
    if (!bright_ || !bloom_) { MD_LOG(MD_LOG_WARNING, "ground live hdr: bloom targets %dx%d create failed", bw, bh); return false; }
    return true;
}

void GroundLiveHdr::Set(Prog& p, int u, const float* v, int n) {
    if (p.off[u] >= 0 && (uint32_t)p.off[u] + 4u * (uint32_t)n <= p.ub.size()) memcpy(&p.ub[p.off[u]], v, 4 * n);
}

void GroundLiveHdr::Quad(SDL_GPUCommandBuffer* cmd, Prog& p, SDL_GPUTexture* target, const SDL_GPUTextureSamplerBinding* tb, int ntb) {
    SDL_GPUColorTargetInfo cti = {}; cti.texture = target; cti.load_op = SDL_GPU_LOADOP_DONT_CARE; cti.store_op = SDL_GPU_STOREOP_STORE;   // the quad covers the whole target
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, nullptr);
    if (!rp) { MD_LOG(MD_LOG_WARNING, "ground live hdr: begin pass failed"); return; }
    SDL_BindGPUGraphicsPipeline(rp, p.pipe);
    SDL_BindGPUFragmentSamplers(rp, 0, tb, (Uint32)ntb);
    if (p.ub_size) SDL_PushGPUFragmentUniformData(cmd, 0, p.ub.data(), p.ub_size);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

bool GroundLiveHdr::Run(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* hdr, int w, int h, float dt, const GroundLiveHdrParams& prm) {
    if (!ready_ || !cmd || !hdr || w < 4 || h < 4) return false;
    if (!EnsureSized(w, h)) return false;
    hdr_ = hdr;
    if (!last_cleared_) {   // rt_lastLum before the first frame: 0 = log(1) (the exe's initial content is UNKNOWN)
        SDL_GPUColorTargetInfo cti = {}; cti.texture = last_; cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, nullptr); if (rp) SDL_EndGPURenderPass(rp);
        last_cleared_ = true;
    }
    // texelSize = inverse_texture_size 0 (the pass's input 0)
    const float ts_hdr[2] = { 1.f / w, 1.f / h };
    Set(prog_[kLum2x2], kTexelSize, ts_hdr, 2);
    { const SDL_GPUTextureSamplerBinding tb = { hdr, point_ }; Quad(cmd, prog_[kLum2x2], lum_[4], &tb, 1); }
    for (int i = 4; i >= 1; --i) {   // rt_lum3 <- lum4, ..., rt_lum0 <- lum1
        const float ts[2] = { 1.f / kLumSize[i], 1.f / kLumSize[i] };
        Set(prog_[kDown3x3], kTexelSize, ts, 2);
        const SDL_GPUTextureSamplerBinding tb = { lum_[i], point_ };
        Quad(cmd, prog_[kDown3x3], lum_[i - 1], &tb, 1);
    }
    {   // rt_adaptedLum <- AdaptLuminance(rt_lum0, rt_lastLum); rt_lastLum <- Copy(rt_adaptedLum)
        Prog& p = prog_[kAdapt];
        const float ft = reset_ ? 1e4f : dt;
        Set(p, kFrameTime, &ft, 1); Set(p, kMinLum, &prm.min_luminance, 1); Set(p, kMaxLum, &prm.max_luminance, 1);
        Set(p, kBloomMag, &prm.bloom_magnitude, 1); Set(p, kAdaptRate, &prm.adaptation_rate, 1);
        const SDL_GPUTextureSamplerBinding tb[3] = { { lum_[0], linear_ }, { lum_[0], point_ }, { last_, linear_ } };   // binding 1 unused by the entry
        Quad(cmd, p, adapted_, tb, 3);
        const SDL_GPUTextureSamplerBinding cb = { adapted_, linear_ };
        Quad(cmd, prog_[kCopy], last_, &cb, 1);
        reset_ = false;
    }
    {   // rt_brightpass <- Downsample3x3Brightpass(hdr, rt_adaptedLum)
        Prog& p = prog_[kBright];
        Set(p, kTexelSize, ts_hdr, 2); Set(p, kBloomThreshold, &prm.bloom_threshold, 1); Set(p, kExposureKey, &prm.exposure_key, 1);   // TONE_MAP_A / B: not read by ToneMap (left 0)
        const SDL_GPUTextureSamplerBinding tb[3] = { { hdr, point_ }, { lum_[0], point_ }, { adapted_, point_ } };
        Quad(cmd, p, bright_, tb, 3);
    }
    {   // rt_bloom <- HDR/BloomBlur: pass 1 BloomBlurV, pass 2 BloomBlurH; the compositor binds input 0 (rt_brightpass) to both passes and the second has no blend: it overwrites
        const float ts[2] = { 1.f / bw_, 1.f / bh_ };
        const SDL_GPUTextureSamplerBinding tb = { bright_, linear_ };
        for (int k : { kBlurV, kBlurH }) {
            Set(prog_[k], kTexelSize, ts, 2); Set(prog_[k], kBloomSigma, &prm.bloom_blur_sigma, 1);
            Quad(cmd, prog_[k], bloom_, &tb, 1);
        }
    }
    Set(prog_[kComposite], kMdBloomMag, &prm.bloom_magnitude, 1); Set(prog_[kComposite], kMdExposureKey, &prm.exposure_key, 1);
    return true;
}

bool GroundLiveHdr::Composite(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, SDL_GPUTextureFormat cfmt, SDL_GPUTextureFormat dfmt) {
    if (!ready_ || !rp || !hdr_ || !bloom_) return false;
    SDL_GPUGraphicsPipeline* pipe = nullptr;
    for (int i = 0; i < n_comp_pipes_ && !pipe; ++i) if (comp_pipes_[i].c == cfmt && comp_pipes_[i].d == dfmt) pipe = comp_pipes_[i].pipe;
    if (!pipe) {
        if (n_comp_pipes_ >= 4) return false;
        SDL_GPUColorTargetDescription ct = {}; ct.format = cfmt;
        SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = prog_[kComposite].fs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        gi.target_info.color_target_descriptions = &ct; gi.target_info.num_color_targets = 1;
        if (dfmt != SDL_GPU_TEXTUREFORMAT_INVALID) { gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = dfmt; }   // depth_check off, depth_write off
        pipe = SDL_CreateGPUGraphicsPipeline(dev_, &gi);
        comp_pipes_[n_comp_pipes_++] = { cfmt, dfmt, pipe };
        if (!pipe) { MD_LOG(MD_LOG_WARNING, "ground live hdr: composite pipeline create failed (%d / %d)", (int)cfmt, (int)dfmt); return false; }
    }
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    const SDL_GPUTextureSamplerBinding tb[3] = { { hdr_, linear_ }, { bloom_, linear_ }, { adapted_, linear_ } };
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 3);
    SDL_PushGPUFragmentUniformData(cmd, 0, prog_[kComposite].ub.data(), prog_[kComposite].ub_size);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    return true;
}

}   // namespace md
