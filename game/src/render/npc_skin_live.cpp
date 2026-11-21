#include "npc_skin_live.h"
#include "kenshi_hlsl.h"
#include "npc_skin_glue.h"
#include "npc_hair_mesh.h"
#include "spirv_combine.h"
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_static_buffer.h>
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
    Stage vs, fs, hfs;
    SDL_GPUGraphicsPipeline* pipe = nullptr; SDL_GPUGraphicsPipeline* hair_pipe = nullptr;
    struct Hair { HairSpec spec; std::string key; bool loaded = false, usable = false; GpuTexture tex; GpuStaticBuffer vbo, ibo; uint32_t n_idx = 0; };
    Hair hairs[kMaxHairGroups]; int n_hairs = 0;
    SDL_GPUSampler* diffuse_smp = nullptr;
    struct Head { bool female = false, loaded = false, usable = false; std::string key, path[3]; GpuTexture t[3]; };
    Head heads[kMaxHeadGroups]; int n_heads = 0;
    GpuTexture texf[13];   // the same units for the female body (human_female files); fem_ok = all present
    bool fem_ok = false;
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

// character.hlsl hair_fs behind the depth test against the terrain G-buffer; the three samplers and the uniforms as globals, like the body wrapper (FragmentGlue).
constexpr const char* kHairWrap = R"(sampler2D md_s0 : register(s0); sampler2D md_s1 : register(s1); sampler2D md_s2 : register(s2);
Texture2D<float4> md_gdepth : register(s3);
uniform float3 md_hcolor; uniform float4 md_hdiffuse; uniform float4 md_halpha; uniform float3 md_hthreshold; uniform float3 md_cameraPos; uniform float md_farClip; uniform float md_hspec; uniform float depthTol;
void main(float4 fragCoord : SV_Position, [[vk::location(0)]] float2 texCoord : TEXCOORD0, [[vk::location(1)]] float3 normal : TEXCOORD1, [[vk::location(2)]] float3 binormal : TEXCOORD2,
          [[vk::location(3)]] float3 tangent : TEXCOORD3, [[vk::location(4)]] float3 camDir : TEXCOORD4, [[vk::location(5)]] float4 worldPos : TEXCOORD5, out GBuffer buffer) {
    if (fragCoord.z > md_gdepth.Load(int3(int2(fragCoord.xy), 0)).r + depthTol) discard;
    ch_hair_fs(fragCoord, texCoord, normal, binormal, tangent, camDir, worldPos, md_s0, md_s1, md_s2, md_hcolor, md_hdiffuse, md_halpha, md_hthreshold, md_cameraPos, md_farClip, md_hspec, buffer);
}
)";

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
    const bool hair_prog = Build(S.hfs, std::string(kBorder) + "#define VPOS SV_Position\n#define hair_fs ch_hair_fs\n#include \"" + skin + "\"\n" + kHairWrap, deps, 'f', 4, 0, "fragment (character.hlsl hair_fs)");

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
    {   // female set: same units, human_female files; a missing file leaves the female group off (no stand-in)
        const std::string base = kd + "/data/character/skins/human_female/";
        const struct { int unit; const char* file; } files[] = { { 0, "human_female_body_diffuse.dds" }, { 1, "human_female_body_normal.dds" }, { 3, "human_female_body_mask_HI.dds" },
                                                                 { 5, "human_female_head_diffuse_HI.dds" }, { 6, "human_female_head_normal_HI.dds" }, { 7, "human_female_head_mask.dds" } };
        bool ok = true;
        for (const auto& f : files) if (!S.texf[f.unit].InitFromDDS((base + f.file).c_str())) { fprintf(stderr, "[NpcSkin] female texture missing: %s\n", f.file); ok = false; break; }
        if (ok) {
            GpuSamplerDesc sd; const uint8_t none[4] = { 0, 0, 0, 0 }, flat[4] = { 128, 128, 255, 128 };
            for (int u = 0; u < 13 && ok; ++u) { if (S.texf[u].SDLTexture()) continue; S.texf[u].InitFromMemory(u == 4 || u == 11 ? flat : none, 1, 1, sd); ok = S.texf[u].SDLTexture() != nullptr; }
        }
        S.fem_ok = ok;
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
    if (hair_prog) {   // material Hair : Skinned — cull none, no blending (alpha test in the program), same targets and depth
        gi.fragment_shader = S.hfs.sh;
        S.hair_pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi);
        if (!S.hair_pipe) fprintf(stderr, "[NpcSkin] hair pipeline: %s\n", SDL_GetError());
    }
    S.ready = true;
    fprintf(stderr, "[NpcSkin] ready\n");
    return true;
}

bool Ready() { return S.ready; }

int HeadGroupCount() { return S.n_heads; }

bool GroupFemale(int g) { return g == 1 || (g >= 2 && g - 2 < S.n_heads && S.heads[g - 2].female); }

int RegisterHead(bool female, const std::string& d, const std::string& n, const std::string& m) {
    const std::string key = std::string(female ? "f|" : "m|") + d + "|" + n + "|" + m;
    for (int i = 0; i < S.n_heads; ++i) if (S.heads[i].key == key) return 2 + i;
    if (S.n_heads >= kMaxHeadGroups) return -1;
    State::Head& h = S.heads[S.n_heads];
    h.female = female; h.key = key; h.path[0] = d; h.path[1] = n; h.path[2] = m;
    return 2 + S.n_heads++;   // textures load at the first draw (the GPU side may not be up at spawn)
}

int HairGroupCount() { return S.n_hairs; }

int RegisterHair(const HairSpec& sp) {
    char k[256]; snprintf(k, sizeof k, "%.3f,%.3f,%.3f|%g,%g,%g,%g|%g,%g,%g,%g|%g|%g", sp.color[0], sp.color[1], sp.color[2], sp.diffuse[0], sp.diffuse[1], sp.diffuse[2], sp.diffuse[3], sp.alpha[0], sp.alpha[1], sp.alpha[2], sp.alpha[3], sp.threshold, sp.specular);
    const std::string key = sp.mesh + "|" + sp.skeleton + "|" + sp.texture + "|" + k;
    for (int i = 0; i < S.n_hairs; ++i) if (S.hairs[i].key == key) return i;
    if (S.n_hairs >= kMaxHairGroups) return -1;
    State::Hair& h = S.hairs[S.n_hairs]; h.spec = sp; h.key = key;
    return S.n_hairs++;   // mesh and texture load at the first draw
}

static void LoadHair(State::Hair& h) {
    h.loaded = true;
    std::vector<uint8_t> v; std::vector<uint32_t> ix;
    if (!BuildAttachmentMesh(h.spec.mesh, h.spec.skeleton, v, ix)) { fprintf(stderr, "[NpcSkin] hair mesh does not build: %s\n", h.spec.mesh.c_str()); return; }
    if (!h.tex.InitFromDDS(h.spec.texture.c_str())) { fprintf(stderr, "[NpcSkin] hair texture missing: %s\n", h.spec.texture.c_str()); return; }
    h.vbo.Init(0x8892u, v.data(), (uint32_t)v.size()); h.ibo.Init(0x8893u, ix.data(), (uint32_t)(ix.size() * sizeof(uint32_t)));
    h.n_idx = (uint32_t)ix.size(); h.usable = h.vbo.SDLBuffer() && h.ibo.SDLBuffer();
}

static void LoadHead(State::Head& h) {
    h.loaded = true; h.usable = true;
    for (int k = 0; k < 3 && h.usable; ++k) {
        if (k == 1 && h.path[1].empty()) { GpuSamplerDesc sd; const uint8_t flat[4] = { 128, 128, 255, 128 }; h.t[1].InitFromMemory(flat, 1, 1, sd); if (!h.t[1].SDLTexture()) h.usable = false; continue; }
        if (!h.t[k].InitFromDDS(h.path[k].c_str())) { fprintf(stderr, "[NpcSkin] head texture missing: %s\n", h.path[k].c_str()); h.usable = false; }
    }
    if (!h.usable) for (auto& t : h.t) t.Shutdown();
}

void Record(const Frame& f) {
    if (!S.ready || !f.cmd || !f.vbo || !f.ibo || !f.terrain_depth || !f.view_proj || (f.n01 + f.n2 + f.fem_n01 + f.fem_n2 + f.xg_total + f.hair_total) == 0) return;
    for (int h = 0; h < S.n_hairs; ++h) if (f.hair_n[h] && !S.hairs[h].loaded) LoadHair(S.hairs[h]);
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
    SDL_GPUBuffer* st[5] = { f.storage[0], f.storage[1], f.storage[2], f.storage[3], f.storage[4] }; SDL_BindGPUVertexStorageBuffers(rp, 0, st, 5);
    SDL_GPUViewport vpt = { 0.f, 0.f, (float)f.w, (float)f.h, 0.f, 1.f }; SDL_SetGPUViewport(rp, &vpt);
    for (int grp = 0; grp < 2 + S.n_heads; ++grp) {
        const bool fem = GroupFemale(grp);
        State::Head* hd = grp >= 2 ? &S.heads[grp - 2] : nullptr;
        if (hd && !hd->loaded) LoadHead(*hd);
        if (hd && !hd->usable) hd = nullptr;   // missing file: the body set's default head
        SDL_GPUBuffer* gv = fem ? f.fem_vbo : f.vbo; SDL_GPUBuffer* gi = fem ? f.fem_ibo : f.ibo;
        uint32_t cnt[2], bas[2];
        if (grp == 0) { cnt[0] = f.n01; cnt[1] = f.n2; bas[0] = 0; bas[1] = f.t2_base; }
        else if (grp == 1) { cnt[0] = f.fem_n01; cnt[1] = f.fem_n2; bas[0] = f.fem_base01; bas[1] = f.fem_base2; }
        else { for (int t = 0; t < 2; ++t) { cnt[t] = f.xg_n[grp - 2][t]; bas[t] = f.xg_base[grp - 2][t]; } }
        if (!(cnt[0] + cnt[1]) || !gv || !gi || (fem && !S.fem_ok)) continue;
        GpuTexture* tx = fem ? S.texf : S.tex;
        SDL_GPUBufferBinding vb = { gv, 0 }; SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        SDL_GPUBufferBinding ib = { gi, 0 }; SDL_BindGPUIndexBuffer(rp, &ib, fem ? f.fem_idx_elem : f.idx_elem);
        SDL_GPUTextureSamplerBinding tb[14];
        for (int u = 0; u < 13; ++u) { const GpuTexture& t = (hd && u >= 5 && u <= 7) ? hd->t[u - 5] : tx[u]; tb[u] = { t.SDLTexture(), u == 0 ? S.diffuse_smp : t.SDLSampler() }; }
        tb[13] = { f.terrain_depth, S.tex[1].SDLSampler() };
        SDL_BindGPUFragmentSamplers(rp, 0, tb, 14);
        SDL_PushGPUFragmentUniformData(f.cmd, 0, fu.b.data(), (Uint32)fu.b.size());
        for (int tier = 0; tier < 2; ++tier) {
            const uint32_t n = cnt[tier]; if (!n) continue;
            const float base[4] = { (float)bas[tier], 0.f, 0.f, 0.f }; vu.set("base_inst", base, 4);
            SDL_PushGPUVertexUniformData(f.cmd, 0, vu.b.data(), (Uint32)vu.b.size());
            SDL_DrawGPUIndexedPrimitives(rp, fem ? f.fem_index_count : f.index_count, n, 0, 0, 0);
        }
    }
    if (S.hair_pipe && f.hair_total) {
        SDL_BindGPUGraphicsPipeline(rp, S.hair_pipe);
        SDL_BindGPUVertexStorageBuffers(rp, 0, st, 5);
        Ub hu(S.hfs);
        const float cam3[4] = { f.cam[0], f.cam[1], f.cam[2], 0.f };
        hu.set("md_cameraPos", cam3, 3); hu.set("md_farClip", &far1, 1); hu.set("depthTol", &tol, 1);
        for (int h = 0; h < S.n_hairs; ++h) {
            State::Hair& hr = S.hairs[h];
            if (!f.hair_n[h] || !hr.usable) continue;
            const float thr[3] = { hr.spec.threshold, hr.spec.threshold, hr.spec.threshold };
            hu.set("md_hcolor", hr.spec.color, 3); hu.set("md_hdiffuse", hr.spec.diffuse, 4); hu.set("md_halpha", hr.spec.alpha, 4); hu.set("md_hthreshold", thr, 3); hu.set("md_hspec", &hr.spec.specular, 1);
            SDL_GPUBufferBinding vb2 = { hr.vbo.SDLBuffer(), 0 }; SDL_BindGPUVertexBuffers(rp, 0, &vb2, 1);
            SDL_GPUBufferBinding ib2 = { hr.ibo.SDLBuffer(), 0 }; SDL_BindGPUIndexBuffer(rp, &ib2, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_GPUTextureSamplerBinding tb[4] = { { hr.tex.SDLTexture(), S.diffuse_smp }, { S.tex[4].SDLTexture(), S.tex[4].SDLSampler() }, { S.tex[2].SDLTexture(), S.tex[2].SDLSampler() }, { f.terrain_depth, S.tex[1].SDLSampler() } };
            SDL_BindGPUFragmentSamplers(rp, 0, tb, 4);
            SDL_PushGPUFragmentUniformData(f.cmd, 0, hu.b.data(), (Uint32)hu.b.size());
            const float base[4] = { (float)f.hair_base[h], 0.f, 0.f, 0.f }; vu.set("base_inst", base, 4);
            SDL_PushGPUVertexUniformData(f.cmd, 0, vu.b.data(), (Uint32)vu.b.size());
            SDL_DrawGPUIndexedPrimitives(rp, hr.n_idx, f.hair_n[h], 0, 0, 0);
        }
    }
    SDL_EndGPURenderPass(rp);
    static int logged = 0; if (logged++ % 600 == 0) fprintf(stderr, "[NpcSkin] pass drew %u + %u instances\n", f.n01, f.n2);
}

void Shutdown() {
    if (!S.dev) return;
    if (S.pipe) SDL_ReleaseGPUGraphicsPipeline(S.dev, S.pipe);
    if (S.vs.sh) SDL_ReleaseGPUShader(S.dev, S.vs.sh);
    if (S.fs.sh) SDL_ReleaseGPUShader(S.dev, S.fs.sh);
    if (S.hfs.sh) SDL_ReleaseGPUShader(S.dev, S.hfs.sh);
    if (S.hair_pipe) SDL_ReleaseGPUGraphicsPipeline(S.dev, S.hair_pipe);
    for (auto& h : S.hairs) { h.tex.Shutdown(); h.vbo.Shutdown(); h.ibo.Shutdown(); }
    if (S.diffuse_smp) SDL_ReleaseGPUSampler(S.dev, S.diffuse_smp);
    for (auto& t : S.tex) t.Shutdown();
    for (auto& t : S.texf) t.Shutdown();
    for (auto& h : S.heads) for (auto& t : h.t) t.Shutdown();
    S = State{};
}

}  // namespace md::npcskin
