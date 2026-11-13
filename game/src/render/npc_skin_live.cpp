#include "npc_skin_live.h"
#include "kenshi_hlsl.h"
#include "npc_skin_glue.h"
#include "spirv_combine.h"
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace md::npcskin {
namespace {

struct Stage { SDL_GPUShader* sh = nullptr; std::map<std::string, std::pair<uint32_t, uint32_t>> mem; uint32_t size = 0; };

struct State {
    SDL_GPUDevice* dev = nullptr;
    Stage vs, fs;
    SDL_GPUGraphicsPipeline* pipe = nullptr;
    SDL_GPUSampler* diffuse_smp = nullptr;
    GpuTexture tex[13];   // the Character material's units s0..s12: install files or the material's defaults
    SDL_GPUSampler* diffuse_smp_head = nullptr;
    bool ready = false;
};
State S;

bool Build(Stage& st, const std::string& glue, const std::vector<std::string>& deps, char stage, int samplers, int storage, const char* what) {
    hlsl::Compiled c;
    if (!hlsl::Compile(glue, deps, stage, c)) { fprintf(stderr, "[NpcSkin] %s: %s\n", what, c.log.c_str()); return false; }
    if (!c.valid) { fprintf(stderr, "[NpcSkin] %s: SPIR-V validation failed, program skipped: %s\n", what, c.valid_log.c_str()); return false; }
    std::vector<hlsl::UniformMember> m;
    if (!hlsl::GlobalBlockMembers(c.spv, m, st.size)) st.size = 0;
    for (const auto& u : m) st.mem[u.name] = { u.offset, u.array_stride };
    SDL_GPUShaderCreateInfo si = {}; si.code = (const Uint8*)c.spv.data(); si.code_size = c.spv.size() * 4; si.entrypoint = "main"; si.format = SDL_GPU_SHADERFORMAT_SPIRV;
    si.stage = stage == 'v' ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT; si.num_samplers = samplers; si.num_storage_buffers = storage; si.num_uniform_buffers = st.size ? 1 : 0;
    st.sh = SDL_CreateGPUShader(S.dev, &si);
    if (!st.sh) { fprintf(stderr, "[NpcSkin] %s: SDL_CreateGPUShader: %s\n", what, SDL_GetError()); return false; }
    fprintf(stderr, "[NpcSkin] %s OK: %zu words, $Global %u bytes, %zu members (%s)\n", what, c.spv.size(), st.size, m.size(), c.from_cache ? "cache" : "compiled");
    return true;
}

struct Ub {
    const Stage& st; std::vector<uint8_t> b;
    explicit Ub(const Stage& s) : st(s), b(s.size + 16, 0) {}
    void set(const char* name, const float* v, int n, int idx = 0) {
        auto it = st.mem.find(name); if (it == st.mem.end()) return;
        const uint32_t off = it->second.first + (it->second.second ? it->second.second : 16u) * idx;   // a float4 array has stride 16 even when the module reports none
        if (off + 4 * n <= b.size()) memcpy(&b[off], v, 4 * n);
    }
};

}  // namespace

bool Init(SDL_GPUDevice* dev, const std::string& kd) {
    if (S.ready) return true;
    if (!dev || kd.empty()) return false;
    S.dev = dev;
    const std::string dir = kd + "/data/materials/deferred/", skin = dir + "character.hlsl";
    if (!std::filesystem::exists(skin)) return false;
    std::vector<std::string> deps = { skin };
    for (const char* d : { "common", "deferred", "forward", "post" }) {
        const std::string p = kd + "/data/materials/" + d;
        if (!std::filesystem::exists(p)) continue;
        for (const auto& e : std::filesystem::directory_iterator(p)) if (e.path().extension() == ".hlsl" && e.path().string() != skin) deps.push_back(e.path().string());
    }
    if (!Build(S.vs, kVert, {}, 'v', 0, 5, "vertex")) return false;
    if (!Build(S.fs, FragmentGlue(skin), deps, 'f', 14, 0, "fragment (character.hlsl main_fs)")) return false;

    {   // the install textures of the male body and head; the units the NPC has no file for get the material's defaults (nothing / flat / white)
        const std::string base = kd + "/data/character/skins/human_male/";
        const struct { int unit; const char* file; } files[] = { { 0, "human_male_body_diffuse.dds" }, { 1, "human_male_body_normal.dds" }, { 3, "human_male_body_mask_HI.dds" },
                                                                 { 5, "human_male_head_diffuse_HI.dds" }, { 6, "human_male_head_normal_HI.dds" }, { 7, "human_male_head_mask.dds" } };
        for (const auto& f : files) if (!S.tex[f.unit].InitFromDDS((base + f.file).c_str())) { fprintf(stderr, "[NpcSkin] install texture missing: %s\n", f.file); return false; }
        GpuSamplerDesc sd; const uint8_t none[4] = { 0, 0, 0, 0 }, flat[4] = { 128, 128, 255, 128 };
        for (int u = 0; u < 13; ++u) {
            if (S.tex[u].SDLTexture()) continue;
            S.tex[u].InitFromMemory(u == 4 || u == 11 ? flat : none, 1, 1, sd);
            if (!S.tex[u].SDLTexture()) return false;
        }
    }
    SDL_GPUSamplerCreateInfo sci = {}; sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_LINEAR; sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT; sci.mip_lod_bias = kDiffuseMipBias; sci.max_lod = 1000.f;   // skin.material: diffuseMap trilinear, mipmap_bias -2
    S.diffuse_smp = SDL_CreateGPUSampler(dev, &sci);
    if (!S.diffuse_smp) return false;

    SDL_GPUVertexBufferDescription vb = {}; vb.slot = 0; vb.pitch = kStride; vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute va[7] = {};
    const struct { uint32_t loc; SDL_GPUVertexElementFormat f; uint32_t off; } at[7] = {
        { 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 }, { 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12 }, { 5, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24 }, { 8, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, 32 },
        { 9, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 36 }, { 11, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 56 }, { 12, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 72 } };
    for (int i = 0; i < 7; ++i) { va[i].location = at[i].loc; va[i].buffer_slot = 0; va[i].format = at[i].f; va[i].offset = at[i].off; }
    SDL_GPUColorTargetDescription ct[3] = {};
    ct[0].format = ct[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[2].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = S.vs.sh; gi.fragment_shader = S.fs.sh; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gi.vertex_input_state.vertex_buffer_descriptions = &vb; gi.vertex_input_state.num_vertex_buffers = 1; gi.vertex_input_state.vertex_attributes = va; gi.vertex_input_state.num_vertex_attributes = 7;
    gi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; gi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    gi.depth_stencil_state.enable_depth_test = true; gi.depth_stencil_state.enable_depth_write = true; gi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 3; gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    S.pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi);
    if (!S.pipe) { fprintf(stderr, "[NpcSkin] pipeline: %s\n", SDL_GetError()); return false; }
    S.ready = true;
    fprintf(stderr, "[NpcSkin] ready\n");
    return true;
}

bool Ready() { return S.ready; }

void Record(const Frame& f) {
    if (!S.ready || !f.cmd || !f.vbo || !f.ibo || !f.terrain_depth || !f.view_proj || (f.n01 + f.n2) == 0) return;
    SDL_GPUColorTargetInfo cti[3] = {};
    for (int i = 0; i < 3; ++i) { cti[i].texture = f.target[i]; cti[i].load_op = SDL_GPU_LOADOP_LOAD; cti[i].store_op = SDL_GPU_STOREOP_STORE; }
    SDL_GPUDepthStencilTargetInfo dti = {}; dti.texture = f.depth; dti.load_op = SDL_GPU_LOADOP_CLEAR; dti.clear_depth = 1.f; dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE; dti.cycle = false;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(f.cmd, cti, 3, &dti);
    if (!rp) return;
    Ub vu(S.vs), fu(S.fs);
    for (int c = 0; c < 4; ++c) vu.set("vp", f.view_proj + 4 * c, 4, c);
    const float cam4[4] = { f.cam[0], f.cam[1], f.cam[2], 0.f }, far1 = f.far_clip, wh = -1e5f, zero1 = 0.f, zero4[4] = {};
    fu.set("md_skintone", zero4, 3);   /* body *= 1 - skintone * mask: 0 = the skin as stored; the per-NPC skin tone is not wired (UNKNOWN source) */ fu.set("md_color", zero4, 4); fu.set("md_muscleBlend", &zero1, 1); fu.set("md_cameraPos", cam4, 3); fu.set("md_farClip", &far1, 1); fu.set("md_waterHeightRel", &wh, 1);
    fu.set("md_bloodColour", zero4, 4); fu.set("md_hairColor", zero4, 3); fu.set("md_hairMult", zero4, 4); fu.set("md_hairAlpha", zero4, 4); fu.set("md_beardAlpha", zero4, 4);
    static const float tol = getenv("MD_NPC_SKIN_NODEPTH") ? 1e9f : 0.f; fu.set("depthTol", &tol, 1);   // diagnostic: the depth test against the terrain G-buffer off
    SDL_BindGPUGraphicsPipeline(rp, S.pipe);
    SDL_GPUBufferBinding vb = { f.vbo, 0 }; SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    SDL_GPUBufferBinding ib = { f.ibo, 0 }; SDL_BindGPUIndexBuffer(rp, &ib, f.idx_elem);
    SDL_GPUBuffer* st[5] = { f.storage[0], f.storage[1], f.storage[2], f.storage[3], f.storage[4] }; SDL_BindGPUVertexStorageBuffers(rp, 0, st, 5);
    SDL_GPUTextureSamplerBinding tb[14];
    for (int u = 0; u < 13; ++u) tb[u] = { S.tex[u].SDLTexture(), u == 0 ? S.diffuse_smp : S.tex[u].SDLSampler() };
    tb[13] = { f.terrain_depth, S.tex[1].SDLSampler() };
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 14);
    SDL_PushGPUFragmentUniformData(f.cmd, 0, fu.b.data(), (Uint32)fu.b.size());
    SDL_GPUViewport vpt = { 0.f, 0.f, (float)f.w, (float)f.h, 0.f, 1.f }; SDL_SetGPUViewport(rp, &vpt);
    for (int tier = 0; tier < 2; ++tier) {
        const uint32_t n = tier ? f.n2 : f.n01; if (!n) continue;
        const float base[4] = { (float)(tier ? f.t2_base : 0u), 0.f, 0.f, 0.f }; vu.set("base_inst", base, 4);
        SDL_PushGPUVertexUniformData(f.cmd, 0, vu.b.data(), (Uint32)vu.b.size());
        SDL_DrawGPUIndexedPrimitives(rp, f.index_count, n, 0, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
    static int logged = 0; if (logged++ % 600 == 0) fprintf(stderr, "[NpcSkin] pass drew %u + %u instances\n", f.n01, f.n2);
}

void Shutdown() {
    if (!S.dev) return;
    if (S.pipe) SDL_ReleaseGPUGraphicsPipeline(S.dev, S.pipe);
    if (S.vs.sh) SDL_ReleaseGPUShader(S.dev, S.vs.sh);
    if (S.fs.sh) SDL_ReleaseGPUShader(S.dev, S.fs.sh);
    if (S.diffuse_smp) SDL_ReleaseGPUSampler(S.dev, S.diffuse_smp);
    for (auto& t : S.tex) t.Shutdown();
    S = State{};
}

}  // namespace md::npcskin
