#include "ground_live_pass.h"
#include "kenshi_hlsl.h"
#include "spirv_combine.h"
#include <monkey_dust/io/data_files.h>
#include <monkey_dust/platform/md_log.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace md {
namespace {

const char* kVert = R"(float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2.0 - 1.0, 0.0, 1.0); })";
constexpr int kSamplers = 14;   // s0..s4 maps, s5 blend map, s6..s11 biome 1..3 arrays, s12 depth, s13 packed G-buffer

std::string ReadAll(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

}   // namespace

bool GroundLivePass::Init(SDL_GPUDevice* dev, const std::string& kd) {
    dev_ = dev;
    const std::string dir = kd + "/data/materials/", vsf = dir + "deferred/terrain.hlsl", fsf = dir + "deferred/terrainfp4.hlsl";
    std::string glue = ReadAll(DataPath("game/src/render/ground_live_glue.hlsl"));
    if (glue.empty()) { MD_LOG(MD_LOG_WARNING, "ground live: glue missing"); return false; }
    auto rep = [&](const char* k, const std::string& v) { glue.replace(glue.find(k), strlen(k), v); };
    rep("@WET@", dir + "common/wet.hlsl");
    rep("@VS@", vsf); rep("@FS@", fsf); rep("@GBUF@", std::filesystem::absolute(DataPath("game/src/render/ground_live_gbuf.hlsl")).string());
    std::vector<std::string> deps = { vsf, fsf };
    for (const char* d : { "common", "deferred", "forward", "post" })
        for (const auto& e : std::filesystem::directory_iterator(dir + d))
            if (e.path().extension() == ".hlsl" && e.path().string() != vsf && e.path().string() != fsf) deps.push_back(e.path().string());
    hlsl::Compiled vc;
    if (!hlsl::Compile(kVert, {}, 'v', vc)) { MD_LOG(MD_LOG_WARNING, "ground live: vertex: %s", vc.log.c_str()); return false; }
    SDL_GPUShaderCreateInfo sv = {}; sv.code = (const Uint8*)vc.spv.data(); sv.code_size = vc.spv.size() * 4; sv.entrypoint = "main"; sv.format = SDL_GPU_SHADERFORMAT_SPIRV; sv.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_ = SDL_CreateGPUShader(dev, &sv); if (!vs_) return false;
    for (int n = 0; n < 4; ++n) {
        std::string defs = "#define GW_ENGINE\n#define DX11\n";
        if (getenv("MD_GROUND_LIVE_NOWET")) defs += "#define GW_NOWET\n";   // diagnostic: the dry makeWet of the original switched off (to account for the difference against the port)
        for (int b = 1; b <= n; ++b) defs += "#define BLEND" + std::to_string(b) + " " + std::to_string(b) + "\n";
        hlsl::Compiled fc;
        if (!hlsl::Compile(defs + glue, deps, 'f', fc)) { MD_LOG(MD_LOG_WARNING, "ground live: fragment (%d): %s", n + 1, fc.log.c_str()); return false; }
        std::vector<hlsl::UniformMember> mem; uint32_t sz = 0;
        if (!hlsl::GlobalBlockMembers(fc.spv, mem, sz)) return false;
        Variant& v = var_[n]; v.ub_size = sz; v.ub.assign(sz + 16, 0);
        for (const auto& u : mem) v.mem.push_back({ u.name, { u.offset, u.array_stride } });
        SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)fc.spv.data(); sf.code_size = fc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = kSamplers; sf.num_uniform_buffers = 1;
        v.fs = SDL_CreateGPUShader(dev, &sf); if (!v.fs) return false;
        SDL_GPUColorTargetDescription ct[3] = {};
        ct[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[2].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = v.fs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 3;
        gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
        gi.depth_stencil_state.enable_stencil_test = true; gi.depth_stencil_state.compare_mask = 0xff; gi.depth_stencil_state.write_mask = 0;
        gi.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_EQUAL; gi.depth_stencil_state.back_stencil_state = gi.depth_stencil_state.front_stencil_state;
        v.pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi); if (!v.pipe) return false;
    }
    bake_ok_ = false;
    if (getenv("MD_GROUND_CACHE") && getenv("MD_GROUND_CACHE")[0] != '0') {   // the live-ground cache bake (GW_BAKE): the same program over ring texels, a scissor per cell instead of the stencil
        bake_ok_ = true;
        for (int n = 0; n < 4 && bake_ok_; ++n) {
            std::string defs = "#define GW_BAKE\n#define DX11\n";
            for (int b = 1; b <= n; ++b) defs += "#define BLEND" + std::to_string(b) + " " + std::to_string(b) + "\n";
            hlsl::Compiled fc;
            if (!hlsl::Compile(defs + glue, deps, 'f', fc)) { MD_LOG(MD_LOG_WARNING, "ground live: bake fragment (%d): %s", n + 1, fc.log.c_str()); bake_ok_ = false; break; }
            std::vector<hlsl::UniformMember> mem; uint32_t sz = 0;
            if (!hlsl::GlobalBlockMembers(fc.spv, mem, sz)) { bake_ok_ = false; break; }
            Variant& v = bake_[n]; v.ub_size = sz; v.ub.assign(sz + 16, 0);
            for (const auto& u : mem) v.mem.push_back({ u.name, { u.offset, u.array_stride } });
            SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)fc.spv.data(); sf.code_size = fc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = kSamplers; sf.num_uniform_buffers = 1;
            v.fs = SDL_CreateGPUShader(dev, &sf);
            SDL_GPUColorTargetDescription ct[3] = {};
            ct[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[2].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = v.fs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 3;
            v.pipe = v.fs ? SDL_CreateGPUGraphicsPipeline(dev, &gi) : nullptr;
            if (!v.pipe) bake_ok_ = false;
        }
        MD_LOG(bake_ok_ ? MD_LOG_INFO : MD_LOG_WARNING, "ground live: cache bake variants %s", bake_ok_ ? "ready" : "failed");
    }
    {   // classification: five bit-plane draws fill the stencil with the slot (write mask = one bit each)
        std::string bg = ReadAll(DataPath("game/src/render/ground_live_bits.hlsl"));
        hlsl::Compiled bc;
        if (!hlsl::Compile(bg, {}, 'f', bc)) { MD_LOG(MD_LOG_WARNING, "ground live: bits: %s", bc.log.c_str()); return false; }
        std::vector<hlsl::UniformMember> mem; uint32_t sz = 0;
        if (!hlsl::GlobalBlockMembers(bc.spv, mem, sz)) return false;
        SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)bc.spv.data(); sf.code_size = bc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = 1; sf.num_uniform_buffers = 1;
        SDL_GPUShader* bfs = SDL_CreateGPUShader(dev, &sf); if (!bfs) return false;
        for (int k = 0; k < 5; ++k) {
            Variant& v = bits_[k]; v.fs = k == 0 ? bfs : nullptr; v.ub_size = sz; v.ub.assign(sz + 16, 0); for (const auto& u : mem) v.mem.push_back({ u.name, { u.offset, u.array_stride } });
            SDL_GPUColorTargetDescription ct[3] = {};
            ct[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[2].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            for (auto& c : ct) { c.blend_state.color_write_mask = 0; c.blend_state.enable_color_write_mask = true; }
            SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = bfs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 3;
            gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
            gi.depth_stencil_state.enable_stencil_test = true; gi.depth_stencil_state.compare_mask = 0xff; gi.depth_stencil_state.write_mask = (Uint8)(1u << k);
            gi.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS; gi.depth_stencil_state.front_stencil_state.pass_op = SDL_GPU_STENCILOP_REPLACE; gi.depth_stencil_state.back_stencil_state = gi.depth_stencil_state.front_stencil_state;
            v.pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi); if (!v.pipe) return false;
        }
    }
    {   // decode: the original decodePixel
        std::string dg = ReadAll(DataPath("game/src/render/ground_live_decode.hlsl")); const std::string gb = dir + "deferred/gbuffer.hlsl"; dg.replace(dg.find("@ORIG@"), 6, gb);
        hlsl::Compiled dc;
        if (!hlsl::Compile(dg, { gb }, 'f', dc)) { MD_LOG(MD_LOG_WARNING, "ground live: decode: %s", dc.log.c_str()); return false; }
        std::vector<hlsl::UniformMember> mem; uint32_t sz = 0;
        if (!hlsl::GlobalBlockMembers(dc.spv, mem, sz)) return false;
        dec_.ub_size = sz; dec_.ub.assign(sz + 16, 0); for (const auto& u : mem) dec_.mem.push_back({ u.name, { u.offset, u.array_stride } });
        SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)dc.spv.data(); sf.code_size = dc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = 3; sf.num_uniform_buffers = 1;
        dec_.fs = SDL_CreateGPUShader(dev, &sf); if (!dec_.fs) return false;
        SDL_GPUColorTargetDescription ct[2] = {}; ct[0].format = ct[1].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = vs_; gi.fragment_shader = dec_.fs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 2;
        dec_.pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi); if (!dec_.pipe) return false;
    }
    {   // main light: the original main_fs (deferred/deferred.hlsl, no shadow variant) over the raw targets; the legacy semantics are renamed before the include, the original text is untouched
        const std::string orig = dir + "deferred/deferred.hlsl";
        std::string lv = ReadAll(DataPath("game/src/render/ground_live_light_vs.hlsl"));
        std::string lf = "#define VPOS SV_Position\n#define COLOR SV_Target0\n#define main_fs main\n#include \"" + orig + "\"\n";
        std::vector<std::string> ldeps = { orig };
        for (const char* d : { "common", "deferred", "forward", "post" })
            for (const auto& e : std::filesystem::directory_iterator(dir + d))
                if (e.path().extension() == ".hlsl" && e.path().string() != orig) ldeps.push_back(e.path().string());
        hlsl::Compiled lvc, lfc;
        if (lv.empty() || !hlsl::Compile(lv, {}, 'v', lvc)) { MD_LOG(MD_LOG_WARNING, "ground live: light vertex: %s", lvc.log.c_str()); return false; }
        if (!hlsl::Compile(lf, ldeps, 'f', lfc)) { MD_LOG(MD_LOG_WARNING, "ground live: light: %s", lfc.log.c_str()); return false; }
        std::vector<hlsl::UniformMember> vm, fm; uint32_t vsz = 0, fsz = 0;
        if (!hlsl::GlobalBlockMembers(lvc.spv, vm, vsz) || !hlsl::GlobalBlockMembers(lfc.spv, fm, fsz)) return false;
        for (const auto& u : fm) if (u.offset + 64 > fsz) fsz = u.offset + 64;   // a float4x4 last in the block: GlobalBlockMembers sizes by its first row
        light_vsv_.ub_size = vsz + 16; light_vsv_.ub.assign(vsz + 80, 0); for (const auto& u : vm) light_vsv_.mem.push_back({ u.name, { u.offset, u.array_stride } });
        light_.ub_size = fsz; light_.ub.assign(fsz + 16, 0); for (const auto& u : fm) light_.mem.push_back({ u.name, { u.offset, u.array_stride } });
        SDL_GPUShaderCreateInfo lsv = sv; lsv.code = (const Uint8*)lvc.spv.data(); lsv.code_size = lvc.spv.size() * 4; lsv.num_uniform_buffers = 1;
        light_vs_ = SDL_CreateGPUShader(dev, &lsv); if (!light_vs_) return false;
        SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)lfc.spv.data(); sf.code_size = lfc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = 6; sf.num_uniform_buffers = 1;
        light_.fs = SDL_CreateGPUShader(dev, &sf); if (!light_.fs) return false;
        SDL_GPUColorTargetDescription ct = {}; ct.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = light_vs_; gi.fragment_shader = light_.fs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        gi.target_info.color_target_descriptions = &ct; gi.target_info.num_color_targets = 1;
        light_.pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi); if (!light_.pipe) return false;
    }
    {   // release consumer: the original fog quad (post/fog.hlsl atmosphere_fog_fs, which includes atmospherefog.hlsl) over the lit colour; non-fatal (a build with the translated resolve does not draw it)
        const std::string orig = dir + "post/fog.hlsl";
        std::string ff = ReadAll(DataPath("game/src/render/ground_live_fog.hlsl"));
        if (ff.empty()) MD_LOG(MD_LOG_WARNING, "ground live: fog consumer glue missing");
        else {
            ff.replace(ff.find("@FOG@"), 5, orig);
            std::vector<std::string> fdeps = { orig, dir + "post/atmospherefog.hlsl" };
            hlsl::Compiled fc; std::vector<hlsl::UniformMember> fm; uint32_t fsz = 0;
            if (!hlsl::Compile(ff, fdeps, 'f', fc)) MD_LOG(MD_LOG_WARNING, "ground live: fog consumer: %s", fc.log.c_str());
            else if (!hlsl::GlobalBlockMembers(fc.spv, fm, fsz)) MD_LOG(MD_LOG_WARNING, "ground live: fog consumer: no $Global block");
            else {
                fog_.ub_size = fsz; fog_.ub.assign(fsz + 16, 0); for (const auto& u : fm) fog_.mem.push_back({ u.name, { u.offset, u.array_stride } });
                SDL_GPUShaderCreateInfo sf = sv; sf.code = (const Uint8*)fc.spv.data(); sf.code_size = fc.spv.size() * 4; sf.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; sf.num_samplers = 2; sf.num_uniform_buffers = 1;
                fog_.fs = SDL_CreateGPUShader(dev, &sf); if (!fog_.fs) MD_LOG(MD_LOG_WARNING, "ground live: fog consumer: shader create failed");
                else MD_LOG(MD_LOG_INFO, "ground live: fog consumer ready (%zu words, %u uniform bytes)", fc.spv.size(), fsz);
            }
        }
    }
    SDL_GPUSamplerCreateInfo si = {}; si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = SDL_CreateGPUSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR; si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT; si.max_anisotropy = 1; si.max_lod = 16;
    linear_ = SDL_CreateGPUSampler(dev, &si);
    ready_ = nearest_ && linear_;
    return ready_;
}

void GroundLivePass::Shutdown() {
    for (int i = 0; i < n_fog_pipes_; ++i) if (fog_pipes_[i].pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, fog_pipes_[i].pipe);
    n_fog_pipes_ = 0; if (fog_.fs) { SDL_ReleaseGPUShader(dev_, fog_.fs); fog_.fs = nullptr; }
    if (!dev_) return;
    for (auto& v : var_) { if (v.pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, v.pipe); if (v.fs) SDL_ReleaseGPUShader(dev_, v.fs); v = Variant(); }
    for (auto& v : bake_) { if (v.pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, v.pipe); if (v.fs) SDL_ReleaseGPUShader(dev_, v.fs); v = Variant(); }
    bake_ok_ = false;
    for (int k = 0; k < 5; ++k) { if (bits_[k].pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, bits_[k].pipe); }
    if (bits_[0].fs) SDL_ReleaseGPUShader(dev_, bits_[0].fs);
    for (auto& b : bits_) b = Variant();

    if (dec_.pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, dec_.pipe);
    if (dec_.fs) SDL_ReleaseGPUShader(dev_, dec_.fs);
    dec_ = Variant();
    if (light_.pipe) SDL_ReleaseGPUGraphicsPipeline(dev_, light_.pipe);
    if (light_.fs) SDL_ReleaseGPUShader(dev_, light_.fs);
    if (light_vs_) SDL_ReleaseGPUShader(dev_, light_vs_);
    light_ = Variant(); light_vsv_ = Variant(); light_vs_ = nullptr;
    if (lit_) SDL_ReleaseGPUTexture(dev_, lit_);
    lit_ = nullptr;
    if (rgb_) SDL_ReleaseGPUTexture(dev_, rgb_);
    if (aux_) SDL_ReleaseGPUTexture(dev_, aux_);
    rgb_ = aux_ = nullptr;
    if (ds_) SDL_ReleaseGPUTexture(dev_, ds_);
    ds_ = nullptr;
    if (vs_) SDL_ReleaseGPUShader(dev_, vs_);
    if (nearest_) SDL_ReleaseGPUSampler(dev_, nearest_);
    if (linear_) SDL_ReleaseGPUSampler(dev_, linear_);
    for (auto& t : t_) if (t) SDL_ReleaseGPUTexture(dev_, t);
    vs_ = nullptr; nearest_ = linear_ = nullptr; t_[0] = t_[1] = t_[2] = nullptr; ready_ = false; dev_ = nullptr;
}

bool GroundLivePass::EnsureTargets(int w, int h) {
    if (w == tw_ && h == th_ && t_[0]) return true;
    for (auto& t : t_) if (t) { SDL_ReleaseGPUTexture(dev_, t); t = nullptr; }
    const SDL_GPUTextureFormat fm[3] = { SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R32_FLOAT };
    for (int i = 0; i < 3; ++i) {
        SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fm[i]; ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = w; ti.height = h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        t_[i] = SDL_CreateGPUTexture(dev_, &ti); if (!t_[i]) return false;
    }
    if (rgb_) SDL_ReleaseGPUTexture(dev_, rgb_);
    if (aux_) SDL_ReleaseGPUTexture(dev_, aux_);
    { SDL_GPUTextureCreateInfo ri = {}; ri.type = SDL_GPU_TEXTURETYPE_2D; ri.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; ri.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
      ri.width = w; ri.height = h; ri.layer_count_or_depth = 1; ri.num_levels = 1; rgb_ = SDL_CreateGPUTexture(dev_, &ri); if (!rgb_) return false; aux_ = SDL_CreateGPUTexture(dev_, &ri); if (!aux_) return false; }
    if (lit_) SDL_ReleaseGPUTexture(dev_, lit_);
    { SDL_GPUTextureCreateInfo li = {}; li.type = SDL_GPU_TEXTURETYPE_2D; li.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; li.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
      li.width = w; li.height = h; li.layer_count_or_depth = 1; li.num_levels = 1; lit_ = SDL_CreateGPUTexture(dev_, &li); if (!lit_) return false; }
    if (ds_) SDL_ReleaseGPUTexture(dev_, ds_);
    SDL_GPUTextureCreateInfo di = {}; di.type = SDL_GPU_TEXTURETYPE_2D; di.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT; di.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    di.width = w; di.height = h; di.layer_count_or_depth = 1; di.num_levels = 1; ds_ = SDL_CreateGPUTexture(dev_, &di); if (!ds_) return false;
    tw_ = w; th_ = h; return true;
}

void GroundLivePass::SetIn(Variant& v, const std::string& name, const float* val, int count, int idx) {
    for (const auto& m : v.mem) if (m.first == name) { memcpy(&v.ub[m.second.first + (m.second.second ? m.second.second * idx : 0)], val, 4 * count); return; }
}

// the uniforms and textures of one biome-list group (the program's inputs except the G-buffer position source)
void GroundLivePass::FillGroup(Variant& v, const GroundLiveFrame& f, const GroundLiveGroup& grp, int tw, int th, SDL_GPUTextureSamplerBinding* tb) {
    auto set = [&](const std::string& nm, const float* val, int count, int idx = 0) { SetIn(v, nm, val, count, idx); };
    const float viewport[4] = { (float)tw, (float)th, 1.f / tw, 1.f / th };
    set("viewport", viewport, 4); set("farClip", &f.far_clip, 1); set("waterHeightRel", &f.water_height_rel, 1); set("wetness", &f.wetness, 1);
    const float cam_u[3] = { (f.cam_ws[0] - 14745.6f) * 10.f, f.cam_ws[1] * 10.f, (f.cam_ws[2] - 14745.6f) * 10.f };   // the original works in centred 0.1 m units
    set("cameraPos", cam_u, 3);
    float bright[4] = { 1, 1, 1, 1 };
    for (int b = 0; b < grp.biome_count; ++b) {
        const std::string s = b ? std::to_string(b) : ""; const GroundPack& p = grp.biome[b].pack;
        set("scalesA" + s, p.scalesA, 4); set("scalesB" + s, p.scalesB, 4); set("scalesC" + s, p.scalesC, 4);
        set("slopeMin" + s, p.slopeMin, 4); set("slopeMax" + s, p.slopeMax, 4); set("slopeBlend" + s, p.slopeBlend, 4);
        set("overlayMult" + s, p.overlayMult, 4); set("textureFade" + s, p.textureFade, 4);
        const float a2[8] = { p.absorbance[0], p.absorbance[1], p.absorbance[2], p.absorbance[3], p.absorbance[4], p.absorbance[5], 0, 0 };
        set("absorbance" + s, a2, 4, 0); set("absorbance" + s, a2 + 4, 4, 1);
        bright[b] = p.brightnessFix;
    }
    set("brightnessFix", bright, 4);
    float d[8] = {}; for (int b = 0; b < grp.biome_count; ++b) { d[b * 2] = grp.biome[b].pack.distortFreq; d[b * 2 + 1] = grp.biome[b].pack.distortAmp; }
    const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }, origin[4] = { 14745.6f, 14745.6f, 0.f, 10.f };
    set("gw_wvp", ident, 16); set("gw_world", ident, 16); set("gw_distortion0", d, 4); set("gw_distortion1", d + 4, 4);
    set("gw_biomeData", f.biome_rect, 4); set("gw_overlayData", f.overlay_rect, 4); set("gw_cameraPos", cam_u, 3); set("gw_cameraDir", f.cam_dir, 3);
    set("gw_invVP", f.inv_vp, 16); const float cam4[4] = { f.cam_ws[0], f.cam_ws[1], f.cam_ws[2], 0.f }; set("gw_camWs", cam4, 4); set("gw_originM", origin, 4);
    SDL_GPUTexture* tex[kSamplers];
    for (auto& t : tex) t = f.interior;
    tex[0] = grp.biome[0].diffuse; tex[1] = grp.biome[0].normal; tex[2] = f.overlay; tex[3] = f.colour; tex[4] = f.interior;
    if (grp.biome_count > 1) { tex[5] = f.blend_map; for (int k = 1; k < grp.biome_count; ++k) { tex[4 + 2 * k] = grp.biome[k].diffuse; tex[5 + 2 * k] = grp.biome[k].normal; } }
    tex[12] = f.depth; tex[13] = f.packed;
    for (int i = 0; i < kSamplers; ++i) tb[i] = { tex[i], (i >= 12 || i == 5) ? nearest_ : linear_ };
}

bool GroundLivePass::Run(SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, const GroundLiveGroup* groups, int n) {
    if (!ready_ || !t_[0] || n < 1 || n > 250) return false;
    auto setIn = [&](Variant& v, const std::string& name, const float* val, int count, int idx = 0) {
        for (const auto& m : v.mem) if (m.first == name) { memcpy(&v.ub[m.second.first + (m.second.second ? m.second.second * idx : 0)], val, 4 * count); return; }
    };
    if (n > 31) n = 31;
    SDL_GPUColorTargetInfo cti[3] = {};
    for (int i = 0; i < 3; ++i) { cti[i].texture = t_[i]; cti[i].load_op = SDL_GPU_LOADOP_CLEAR; cti[i].store_op = SDL_GPU_STOREOP_STORE; }
    SDL_GPUDepthStencilTargetInfo dti = {}; dti.texture = ds_; dti.load_op = SDL_GPU_LOADOP_DONT_CARE; dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_CLEAR; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE; dti.clear_stencil = 0; dti.cycle = false;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, cti, 3, &dti);
    auto scissor = [&](const GroundLiveGroup& g) {
        SDL_Rect r = { 0, 0, tw_, th_ }; if (g.x1 > g.x0 && g.y1 > g.y0) r = { g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0 };
        SDL_SetGPUScissor(rp, &r);
    };
    {   // classification step 2: bit planes of the slot into the stencil
        SDL_Rect full = { 0, 0, tw_, th_ }; SDL_SetGPUScissor(rp, &full);
        int max_slot = 1; for (int g = 0; g < n; ++g) max_slot = std::max(max_slot, groups[g].slot);
        int bitsNeeded = 1; while ((1 << bitsNeeded) <= max_slot) ++bitsNeeded;
        SDL_GPUTextureSamplerBinding sb = { f.slot_tex, nearest_ };
        static const bool id_only = getenv("MD_GROUND_LIVE_IDONLY") != nullptr;   // measurement: cost split
        for (int k = 0; k < bitsNeeded && !id_only; ++k) {
            Variant& v = bits_[k]; const float bit[4] = { (float)k, phase_ >= 0 ? (float)(phase_ & 7) : -1.f, 0.f, 0.f }; setIn(v, "gb_bit", bit, 4);
            SDL_BindGPUGraphicsPipeline(rp, v.pipe); SDL_BindGPUFragmentSamplers(rp, 0, &sb, 1); SDL_SetGPUStencilReference(rp, 0xff);
            SDL_PushGPUFragmentUniformData(cmd, 0, v.ub.data(), v.ub_size);
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }
    }
    static const bool class_only = getenv("MD_GROUND_LIVE_CLASSONLY") != nullptr || getenv("MD_GROUND_LIVE_IDONLY") != nullptr;   // measurement: cost split
    for (int gi = 0; gi < n && !class_only; ++gi) {
        const GroundLiveGroup& grp = groups[gi];
        if (grp.biome_count < 1 || grp.biome_count > 4) continue;
        Variant& v = var_[grp.biome_count - 1];
        SDL_GPUTextureSamplerBinding tb[kSamplers];
        FillGroup(v, f, grp, tw_, th_, tb);
        SDL_BindGPUGraphicsPipeline(rp, v.pipe);
        SDL_BindGPUFragmentSamplers(rp, 0, tb, kSamplers);
        scissor(grp); SDL_SetGPUStencilReference(rp, (Uint8)grp.slot);
        SDL_PushGPUFragmentUniformData(cmd, 0, v.ub.data(), v.ub_size);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
    return true;
}

bool GroundLivePass::Decode(SDL_GPUCommandBuffer* cmd) {
    return ready_ && rgb_ && DecodeTo(cmd, t_, tw_, th_, rgb_, aux_);
}

bool GroundLivePass::DecodeTo(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* const src[3], int w, int h, SDL_GPUTexture* out_rgb, SDL_GPUTexture* out_aux) {
    const float viewport[4] = { (float)w, (float)h, 1.f / w, 1.f / h };
    for (const auto& m : dec_.mem) if (m.first == "viewport") memcpy(&dec_.ub[m.second.first], viewport, 16);
    SDL_GPUColorTargetInfo cti[2] = {}; cti[0].texture = out_rgb; cti[1].texture = out_aux; for (auto& c : cti) { c.load_op = SDL_GPU_LOADOP_DONT_CARE; c.store_op = SDL_GPU_STOREOP_STORE; }
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, cti, 2, nullptr);
    if (!rp) return false;
    SDL_BindGPUGraphicsPipeline(rp, dec_.pipe);
    SDL_GPUTextureSamplerBinding tb[3] = { { src[0], nearest_ }, { src[1], nearest_ }, { src[2], nearest_ } }; SDL_BindGPUFragmentSamplers(rp, 0, tb, 3);
    SDL_PushGPUFragmentUniformData(cmd, 0, dec_.ub.data(), dec_.ub_size);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    return true;
}

bool GroundLivePass::Bake(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* const dst[3], int w, int h, const GroundLiveFrame& f, SDL_GPUTexture* hmap, const float bake0[4], const float bake1[4], const float bake2[4],
                          const GroundLiveGroup* cells, int n) {
    if (!ready_ || !bake_ok_ || !hmap || n < 1) return false;
    SDL_GPUColorTargetInfo cti[3] = {};
    for (int i = 0; i < 3; ++i) { cti[i].texture = dst[i]; cti[i].load_op = SDL_GPU_LOADOP_CLEAR; cti[i].store_op = SDL_GPU_STOREOP_STORE; }
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, cti, 3, nullptr);
    if (!rp) return false;
    for (int c = 0; c < n; ++c) {
        const GroundLiveGroup& grp = cells[c];
        if (grp.biome_count < 1 || grp.biome_count > 4 || grp.x1 <= grp.x0 || grp.y1 <= grp.y0) continue;
        Variant& v = bake_[grp.biome_count - 1];
        SDL_GPUTextureSamplerBinding tb[kSamplers];
        FillGroup(v, f, grp, w, h, tb);
        SetIn(v, "gw_bake0", bake0, 4); SetIn(v, "gw_bake1", bake1, 4); SetIn(v, "gw_bake2", bake2, 4);
        tb[12] = { hmap, nearest_ }; tb[13] = { f.interior, nearest_ };
        SDL_BindGPUGraphicsPipeline(rp, v.pipe);
        SDL_BindGPUFragmentSamplers(rp, 0, tb, kSamplers);
        const SDL_Rect r = { grp.x0, grp.y0, grp.x1 - grp.x0, grp.y1 - grp.y0 }; SDL_SetGPUScissor(rp, &r);
        SDL_PushGPUFragmentUniformData(cmd, 0, v.ub.data(), v.ub_size);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
    return true;
}

bool GroundLivePass::Light(SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, const GroundLiveLightParams& p) {
    if (!ready_ || !lit_ || !light_.pipe || !p.ambient_map || !p.irr_cube || !p.spec_cube) return false;
    auto set = [&](Variant& v, const char* name, const float* val, int count) { for (const auto& m : v.mem) if (m.first == name) { memcpy(&v.ub[m.second.first], val, 4 * count); return; } };
    set(light_vsv_, "vl_invVP", f.inv_vp, 16); const float cam[4] = { f.cam_ws[0], f.cam_ws[1], f.cam_ws[2], 0.f }; set(light_vsv_, "vl_cam", cam, 4);   // the ray is only a direction: the unit of cam does not matter
    const float sun_colour[4] = { p.sun_colour[0], p.sun_colour[1], p.sun_colour[2], p.sun_colour[3] }, env[4] = { 1.f, 1.f, 1.f, p.env_w }, fog[4] = { f.far_clip, 0.f, 0.f, 0.f };
    const float world_size[4] = { 1.f / p.world_extent, 1.f / p.world_extent, 0.5f, 0.5f }, world_off[3] = { p.world_origin_xz[0], 0.f, p.world_origin_xz[1] };
    const float viewport[4] = { (float)tw_, (float)th_, 1.f / tw_, 1.f / th_ }, zero4[4] = {}, amb[4] = { 1.f, 1.f, 1.f, p.ambient_w };
    const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };   // inverseView (identity is its own transpose: the column-major memory order does not matter) and proj (only the depth output reads it; the pass has no depth target)
    set(light_, "sunDirection", p.sun_dir, 3); set(light_, "sunColour__w", sun_colour, 4); set(light_, "pFogParams", fog, 4); set(light_, "envColour__w", env, 4);
    set(light_, "worldSize", world_size, 4); set(light_, "worldOffset", world_off, 3); set(light_, "viewport", viewport, 4); set(light_, "offset", zero4, 4);
    set(light_, "inverseView", ident, 16); set(light_, "ambientParams", amb, 4); set(light_, "proj", ident, 16);
    SDL_GPUColorTargetInfo cti = {}; cti.texture = lit_; cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;   // clear (0,0,0,0): alpha 0 = not lit
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(rp, light_.pipe);
    SDL_GPUTextureSamplerBinding tb[6] = { { t_[0], nearest_ }, { t_[1], nearest_ }, { t_[2], nearest_ }, { p.ambient_map, p.ambient_smp ? p.ambient_smp : linear_ }, { p.irr_cube, p.irr_smp ? p.irr_smp : linear_ }, { p.spec_cube, p.spec_smp ? p.spec_smp : linear_ } };
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 6);
    SDL_PushGPUVertexUniformData(cmd, 0, light_vsv_.ub.data(), light_vsv_.ub_size);
    SDL_PushGPUFragmentUniformData(cmd, 0, light_.ub.data(), light_.ub_size);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    return true;
}

bool GroundLivePass::DrawFog(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, const GroundLiveFogParams& p, SDL_GPUTextureFormat cfmt, SDL_GPUTextureFormat dfmt) {
    if (!ready_ || !lit_ || !fog_.fs || !light_vs_) return false;
    SDL_GPUGraphicsPipeline* pipe = nullptr;
    for (int i = 0; i < n_fog_pipes_ && !pipe; ++i) if (fog_pipes_[i].c == cfmt && fog_pipes_[i].d == dfmt) pipe = fog_pipes_[i].pipe;
    if (!pipe) {
        if (n_fog_pipes_ >= 4) return false;
        SDL_GPUColorTargetDescription ct = {}; ct.format = cfmt;
        SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = light_vs_; gi.fragment_shader = fog_.fs; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        gi.target_info.color_target_descriptions = &ct; gi.target_info.num_color_targets = 1;
        gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = dfmt;   // the ground pass carries the scene depth; no test, no write
        pipe = SDL_CreateGPUGraphicsPipeline(dev_, &gi);
        fog_pipes_[n_fog_pipes_++] = { cfmt, dfmt, pipe };
        if (!pipe) { MD_LOG(MD_LOG_WARNING, "ground live: fog consumer pipeline create failed (%d / %d)", (int)cfmt, (int)dfmt); return false; }
    }
    auto set = [&](Variant& v, const char* name, const float* val, int count) { for (const auto& m : v.mem) if (m.first == name) { memcpy(&v.ub[m.second.first], val, 4 * count); return; } };
    set(light_vsv_, "vl_invVP", f.inv_vp, 16); const float cam[4] = { f.cam_ws[0], f.cam_ws[1], f.cam_ws[2], 0.f }; set(light_vsv_, "vl_cam", cam, 4);
    // the SkyX constants the exe gives the fog material (F040804; the same numbers as SkyPanorama's SkyUniforms): inner 9.77501, outer 10.2963, height 0.01, Kr 0.0022, Km 0.000675, ESun 30, exposure 1.4
    const float inner = 9.77501f, outer = 10.2963f, thick = outer - inner, PI = 3.14159265f, kr = 0.0022f, km = 0.000675f;
    const float cam_pos[3] = { 0.f, inner + thick * 0.01f, 0.f }, inv_wl[3] = { 1.f / powf(0.57f, 4.f), 1.f / powf(0.48f, 4.f), 1.f / powf(0.44f, 4.f) };
    const float v_inner = inner, v_expo = 1.4f, v_kresun = kr * 30.f, v_kr4pi = kr * 4.f * PI, v_km4pi = km * 4.f * PI, v_scale = 1.f / thick, v_sdepth = thick * 0.5f, v_sosd = v_scale / v_sdepth, v_dome = p.skydome_radius;
    const float fogp[4] = { 0.f, p.fog_near, p.fog_far, 0.f }, far_clip = f.far_clip;
    set(fog_, "md_sunDirectionReal", p.sun_real, 3); set(fog_, "md_pFogParams", fogp, 4); set(fog_, "md_uCameraPos", cam_pos, 3); set(fog_, "md_uInvWaveLength", inv_wl, 3);
    set(fog_, "md_uInnerRadius", &v_inner, 1); set(fog_, "md_uExposure", &v_expo, 1); set(fog_, "md_uKrESun", &v_kresun, 1); set(fog_, "md_uKr4PI", &v_kr4pi, 1); set(fog_, "md_uKm4PI", &v_km4pi, 1);
    set(fog_, "md_uScale", &v_scale, 1); set(fog_, "md_uScaleDepth", &v_sdepth, 1); set(fog_, "md_uScaleOverScaleDepth", &v_sosd, 1); set(fog_, "md_uSkydomeRadius", &v_dome, 1); set(fog_, "md_horizonClouds", p.horizon, 4);
    set(fog_, "md_farClip", &far_clip, 1); set(fog_, "md_exposure", &p.exposure, 1);
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_GPUTextureSamplerBinding tb[2] = { { t_[2], nearest_ }, { lit_, nearest_ } };   // s0 = the original's gBuf2 (distance / farClip), s1 = the lit colour
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 2);
    SDL_PushGPUVertexUniformData(cmd, 0, light_vsv_.ub.data(), light_vsv_.ub_size);
    SDL_PushGPUFragmentUniformData(cmd, 0, fog_.ub.data(), fog_.ub_size);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    return true;
}

}   // namespace md
