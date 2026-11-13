// Character editor body on the install programs (#207 item 8 step 3, docs/research/CHAR_BODY_INSTALL_FS_DESIGN_2026-11-13.md).
// Ours: the glue only. deferred/character.hlsl main_fs (the body into a 3-target G-buffer) and deferred/rtt.hlsl main_fs (the character screen's light over that G-buffer) are included
// from KENSHI_DIR at run time and compiled as they are; nothing of their text is stored. Everything is in view space: the camera is the origin, so the originals' cameraPos is 0 and
// inverseView is the identity.
#include "kenshi_hlsl.h"
#include "spirv_combine.h"
#include "editor_char_body_live.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace md::charlive {
namespace {

// The originals declare their pixel position as VPOS (a plain input to the compiler); SV_Position makes it the real fragment coordinate, so the varyings start at location 0.
// Vertex glue of the body: the preview's skinning (bone matrices as columns), outputs in the original fragment's input order (locations 0..7), view space.
const char* kBodyVert = R"(uniform float4 mvpc[4];
uniform float4 mvc[4];
uniform float4 bonec[120];
struct VI {
    [[vk::location(0)]] float3 pos : POSITION; [[vk::location(1)]] float3 nrm : NORMAL; [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint4 joints : BLENDINDICES; [[vk::location(4)]] float4 weights : BLENDWEIGHT; [[vk::location(5)]] float3 tan : TANGENT; [[vk::location(6)]] float3 bin : BINORMAL;
};
struct VO {
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0; [[vk::location(1)]] float3 normal : TEXCOORD1; [[vk::location(2)]] float3 binormal : TEXCOORD2; [[vk::location(3)]] float3 tangent : TEXCOORD3;
    [[vk::location(4)]] float3 camDir : TEXCOORD4; [[vk::location(5)]] float4 worldPos : TEXCOORD5; [[vk::location(6)]] float3 wet : TEXCOORD6; [[vk::location(7)]] float4 blood : TEXCOORD7;
};
float4 col4(float4 c[4], float4 v) { return c[0] * v.x + c[1] * v.y + c[2] * v.z + c[3] * v.w; }
float3 col3(float4 c0, float4 c1, float4 c2, float3 v) { return c0.xyz * v.x + c1.xyz * v.y + c2.xyz * v.z; }
VO main(VI i) {
    float tw = i.weights.x + i.weights.y + i.weights.z + i.weights.w;
    float4 a = float4(1, 0, 0, 0), b = float4(0, 1, 0, 0), c = float4(0, 0, 1, 0), d = float4(0, 0, 0, 1);
    if (tw > 0.001) {
        float k = 1.0 / tw; a = b = c = d = float4(0, 0, 0, 0);
        for (int j = 0; j < 4; ++j) { int bi = int(i.joints[j]) * 4; float w = i.weights[j] * k; a += bonec[bi] * w; b += bonec[bi + 1] * w; c += bonec[bi + 2] * w; d += bonec[bi + 3] * w; }
    }
    float4 p = a * i.pos.x + b * i.pos.y + c * i.pos.z + d;
    float3 ax = a.xyz, bx = b.xyz, cx = c.xyz;
    float3 cof = cross(bx, cx) * i.nrm.x + cross(cx, ax) * i.nrm.y + cross(ax, bx) * i.nrm.z;
    float3 vN = normalize(cof * sign(dot(ax, cross(bx, cx))));
    VO o;
    o.pos = col4(mvpc, p);
    o.uv = i.uv;
    o.normal = normalize(col3(mvc[0], mvc[1], mvc[2], vN));
    o.tangent = col3(mvc[0], mvc[1], mvc[2], normalize(ax * i.tan.x + bx * i.tan.y + cx * i.tan.z));
    o.binormal = col3(mvc[0], mvc[1], mvc[2], normalize(ax * i.bin.x + bx * i.bin.y + cx * i.bin.z));
    float4 vp = col4(mvc, p);
    o.worldPos = float4(vp.xyz, 1.0);
    o.camDir = float3(0, 0, 0); o.wet = float3(0, 0, 0); o.blood = float4(0, 0, 0, 1);
    return o;
}
)";

// The install material samples the character textures with `tex_address_mode border` (data/materials/deferred/character.material); SDL_GPU has no border mode, so every fetch outside 0..1
// returns transparent black here (the head layer at body UVs and the body layer at head UVs add nothing, as the program expects).
const char* kBorder = "float4 md_border_tex2D(sampler2D s, float2 uv) { float4 c = tex2D(s, uv); return (any(uv < 0.0) || any(uv > 1.0)) ? float4(0.0, 0.0, 0.0, 0.0) : c; }\n#define tex2D md_border_tex2D\n";

// Light pass glue: a full-screen triangle that hands the original rtt main_fs its texCoord / ray / screen.
const char* kLightVert = R"(uniform float4 rayScale;
struct VO { float4 pos : SV_Position; [[vk::location(0)]] float2 uv : TEXCOORD0; [[vk::location(1)]] float3 ray : TEXCOORD1; [[vk::location(2)]] float2 screen : TEXCOORD2; };
VO main(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2); VO o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.uv = float2(p.x, rayScale.z > 0.0 ? 1.0 - p.y : p.y);
    o.ray = float3((p.x * 2.0 - 1.0) * rayScale.x, (p.y * 2.0 - 1.0) * rayScale.y, -1.0); o.screen = p;
    return o;
}
)";

struct Stage {
    SDL_GPUShader* sh = nullptr;
    std::map<std::string, std::pair<uint32_t, uint32_t>> mem;   // $Global member -> offset, array stride
    uint32_t size = 0;
};

struct State {
    SDL_GPUDevice* dev = nullptr;
    Stage bvs, bfs, lvs, lfs;
    SDL_GPUGraphicsPipeline *gpipe = nullptr, *lpipe = nullptr;
    SDL_GPUSampler* point = nullptr;
    SDL_GPUTexture* gb[3] = {};
    int gw = 0, gh = 0;
    bool ready = false;
};
State S;

std::vector<std::string> Deps(const std::string& dir, const std::string& orig) {
    std::vector<std::string> deps = { orig };
    for (const char* d : { "common", "deferred", "forward", "post" })
        for (const auto& e : std::filesystem::directory_iterator(dir + d))
            if (e.path().extension() == ".hlsl" && e.path().string() != orig) deps.push_back(e.path().string());
    return deps;
}

bool Build(Stage& st, const std::string& glue, const std::vector<std::string>& deps, char stage, int samplers, const char* what) {
    hlsl::Compiled c;
    if (!hlsl::Compile(glue, deps, stage, c)) { fprintf(stderr, "[CharBodyLive] %s: %s\n", what, c.log.c_str()); return false; }
    std::vector<hlsl::UniformMember> m;
    if (!hlsl::GlobalBlockMembers(c.spv, m, st.size)) { st.size = 0; }
    for (const auto& u : m) st.mem[u.name] = { u.offset, u.array_stride };
    SDL_GPUShaderCreateInfo si = {}; si.code = (const Uint8*)c.spv.data(); si.code_size = c.spv.size() * 4; si.entrypoint = "main"; si.format = SDL_GPU_SHADERFORMAT_SPIRV;
    si.stage = stage == 'v' ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT; si.num_samplers = samplers; si.num_uniform_buffers = st.size ? 1 : 0;
    st.sh = SDL_CreateGPUShader(S.dev, &si);
    if (!st.sh) { fprintf(stderr, "[CharBodyLive] %s: SDL_CreateGPUShader: %s\n", what, SDL_GetError()); return false; }
    fprintf(stderr, "[CharBodyLive] %s OK: %zu words, $Global %u bytes, %zu members (%s)\n", what, c.spv.size(), st.size, m.size(), c.from_cache ? "cache" : "compiled");
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

void EnsureTargets(int w, int h) {
    if (S.gw == w && S.gh == h && S.gb[0]) return;
    for (auto& t : S.gb) { if (t) SDL_ReleaseGPUTexture(S.dev, t); t = nullptr; }
    const SDL_GPUTextureFormat fmt[3] = { SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R32_FLOAT };
    for (int i = 0; i < 3; ++i) {
        SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fmt[i]; ti.width = w; ti.height = h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER; S.gb[i] = SDL_CreateGPUTexture(S.dev, &ti);
    }
    S.gw = w; S.gh = h;
}

}   // namespace

bool Init(SDL_GPUDevice* dev) {
    if (S.ready) return true;
    S.dev = dev;
    const std::string kd = md::KenshiDir();
    if (kd.empty()) return false;
    const std::string dir = kd + "/data/materials/", chr = dir + "deferred/character.hlsl", rtt = dir + "deferred/rtt.hlsl";
    if (!std::filesystem::exists(chr) || !std::filesystem::exists(rtt)) { fprintf(stderr, "[CharBodyLive] install programs missing\n"); return false; }
    if (!Build(S.bvs, kBodyVert, {}, 'v', 0, "body vertex")) return false;
    if (!Build(S.bfs, std::string(kBorder) + "#define VPOS SV_Position\n#define main_fs main\n#include \"" + chr + "\"\n", Deps(dir, chr), 'f', 13, "body fragment (character.hlsl)")) return false;
    if (!Build(S.lvs, kLightVert, {}, 'v', 0, "light vertex")) return false;
    if (!Build(S.lfs, "#define VPOS SV_Position\n#define main_fs main\n#include \"" + rtt + "\"\n", Deps(dir, rtt), 'f', 5, "light fragment (rtt.hlsl)")) return false;

    SDL_GPUVertexBufferDescription vb = {}; vb.slot = 0; vb.pitch = kVertexStride; vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute va[7] = {};
    const SDL_GPUVertexElementFormat vf[7] = { SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4,
                                               SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 };
    const uint32_t vo[7] = { 0, 12, 24, 32, 36, 52, 64 };
    for (int i = 0; i < 7; ++i) { va[i].location = i; va[i].buffer_slot = 0; va[i].format = vf[i]; va[i].offset = vo[i]; }
    SDL_GPUColorTargetDescription ct[3] = {};
    ct[0].format = ct[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[2].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = S.bvs.sh; gi.fragment_shader = S.bfs.sh; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gi.vertex_input_state.vertex_buffer_descriptions = &vb; gi.vertex_input_state.num_vertex_buffers = 1; gi.vertex_input_state.vertex_attributes = va; gi.vertex_input_state.num_vertex_attributes = 7;
    gi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; gi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK; gi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    gi.depth_stencil_state.enable_depth_test = true; gi.depth_stencil_state.enable_depth_write = true; gi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 3; gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    S.gpipe = SDL_CreateGPUGraphicsPipeline(dev, &gi);
    if (!S.gpipe) { fprintf(stderr, "[CharBodyLive] G-buffer pipeline: %s\n", SDL_GetError()); return false; }

    SDL_GPUColorTargetDescription lc = {}; lc.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    lc.blend_state.enable_blend = true; lc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; lc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    lc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD; lc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE; lc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    lc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD; lc.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    SDL_GPUGraphicsPipelineCreateInfo li = {}; li.vertex_shader = S.lvs.sh; li.fragment_shader = S.lfs.sh; li.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    li.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; li.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    li.target_info.color_target_descriptions = &lc; li.target_info.num_color_targets = 1; li.target_info.has_depth_stencil_target = true; li.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    S.lpipe = SDL_CreateGPUGraphicsPipeline(dev, &li);
    if (!S.lpipe) { fprintf(stderr, "[CharBodyLive] light pipeline: %s\n", SDL_GetError()); return false; }

    SDL_GPUSamplerCreateInfo sc = {}; sc.min_filter = sc.mag_filter = SDL_GPU_FILTER_NEAREST; sc.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sc.address_mode_u = sc.address_mode_v = sc.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    S.point = SDL_CreateGPUSampler(dev, &sc);
    S.ready = S.point != nullptr;
    return S.ready;
}

bool Ready() { return S.ready; }

void DrawBody(const BodyArgs& a) {
    if (!S.ready) return;
    EnsureTargets(a.w, a.h);
    // 1. G-buffer: the install body program into 3 targets, depth shared with the preview (platform, pole already in it; the hair draws after).
    {
        SDL_GPUColorTargetInfo cti[3] = {};
        for (int i = 0; i < 3; ++i) { cti[i].texture = S.gb[i]; cti[i].load_op = SDL_GPU_LOADOP_CLEAR; cti[i].store_op = SDL_GPU_STOREOP_STORE; }
        SDL_GPUDepthStencilTargetInfo dti = {}; dti.texture = a.depth; dti.load_op = SDL_GPU_LOADOP_LOAD; dti.store_op = SDL_GPU_STOREOP_STORE; dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(a.cmd, cti, 3, &dti);
        if (!rp) return;
        SDL_BindGPUGraphicsPipeline(rp, S.gpipe);
        Ub vu(S.bvs);
        for (int c = 0; c < 4; ++c) { vu.set("mvpc", a.mvp + 4 * c, 4, c); vu.set("mvc", a.mv + 4 * c, 4, c); }
        vu.set("bonec", a.bones, 4 * 4 * 30);
        SDL_PushGPUVertexUniformData(a.cmd, 0, vu.b.data(), (Uint32)vu.b.size());
        Ub fu(S.bfs);
        float one = 1.f, zero = 0.f, far_clip = 100.f;
        fu.set("skintone", a.skintone, 3); fu.set("color", a.shirt, 4); fu.set("muscleBlend", &zero, 1); fu.set("cameraPos", a.zero4, 3); fu.set("farClip", &far_clip, 1);
        const float dry = -1.0e5f; fu.set("waterHeightRel", &dry, 1);   // the preview has no water: view-space heights must never read as under it fu.set("bloodColour", a.zero4, 4); fu.set("hairColor", a.hair_colour, 3); fu.set("hairMult", a.hair_mult, 4);
        fu.set("hairAlpha", a.hair_alpha, 4); fu.set("beardAlpha", a.beard_alpha, 4); (void)one;
        SDL_PushGPUFragmentUniformData(a.cmd, 0, fu.b.data(), (Uint32)fu.b.size());
        SDL_GPUBufferBinding vbind = { a.vbo, 0 }, ibind = { a.ibo, 0 };
        SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1); SDL_BindGPUIndexBuffer(rp, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_BindGPUFragmentSamplers(rp, 0, a.tex, 13);
        SDL_DrawGPUIndexedPrimitives(rp, a.ni, 1, 0, 0, 0);
        SDL_EndGPURenderPass(rp);
    }
    // 2. Light: the install character-screen light over the G-buffer, blended onto the preview colour (empty pixels return 0,0,0,0).
    {
        SDL_GPUColorTargetInfo cti = {}; cti.texture = a.color; cti.load_op = SDL_GPU_LOADOP_LOAD; cti.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo dti = {}; dti.texture = a.depth; dti.load_op = SDL_GPU_LOADOP_LOAD; dti.store_op = SDL_GPU_STOREOP_STORE; dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(a.cmd, &cti, 1, &dti);
        if (!rp) return;
        SDL_BindGPUGraphicsPipeline(rp, S.lpipe);
        Ub vu(S.lvs);
        const float rs[4] = { a.tan_half * a.aspect, a.tan_half, a.flip_v ? 1.f : -1.f, 0.f };
        vu.set("rayScale", rs, 4);
        SDL_PushGPUVertexUniformData(a.cmd, 0, vu.b.data(), (Uint32)vu.b.size());
        Ub fu(S.lfs);
        const float vp[4] = { (float)a.w, (float)a.h, 1.f / a.w, 1.f / a.h }, ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const float ld[3] = { -3.f, 2.f, -3.f }, lc[3] = { 0.6f, 0.6f, 0.6f }, ex = 0.5f, far_clip = 100.f;
        fu.set("viewport", vp, 4); fu.set("offset", a.zero4, 4); fu.set("inverseView", ident, 16); fu.set("farClip", &far_clip, 1);
        fu.set("lightDirection", ld, 3); fu.set("lightColour", lc, 3); fu.set("exposure", &ex, 1);
        SDL_PushGPUFragmentUniformData(a.cmd, 0, fu.b.data(), (Uint32)fu.b.size());
        SDL_GPUTextureSamplerBinding tb[5] = { { S.gb[0], S.point }, { S.gb[1], S.point }, { S.gb[2], S.point }, a.irr, a.spec };
        SDL_BindGPUFragmentSamplers(rp, 0, tb, 5);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
    }
}

}   // namespace md::charlive
