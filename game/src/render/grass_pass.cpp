#include "grass_pass.h"
#include "grass_glue.h"
#include "kenshi_hlsl.h"
#include "spirv_combine.h"
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

namespace md::grasspass {
namespace {

struct Stage { SDL_GPUShader* sh = nullptr; std::map<std::string, std::pair<uint32_t, uint32_t>> mem; uint32_t size = 0; };

struct State {
    SDL_GPUDevice* dev = nullptr;
    std::string kd;
    Stage vs, fs;
    SDL_GPUGraphicsPipeline* pipe = nullptr;
    SDL_GPUSampler* smp = nullptr;
    GpuTexture none;                       // interiorClip: (0, 0, 0, 0) = no interior clip
    SDL_GPUBuffer *vbo = nullptr, *ibo = nullptr;
    Mesh mesh;
    std::vector<std::unique_ptr<GpuTexture>> tex;   // one per run
    float phase = 0.f; uint64_t last_ms = 0;
    bool ready = false;
};
State S;

bool Build(Stage& st, const std::string& glue, const std::vector<std::string>& deps, char stage, int samplers, const char* what) {
    hlsl::Compiled c;
    if (!hlsl::Compile(glue, deps, stage, c)) { fprintf(stderr, "[Grass] %s: %s\n", what, c.log.c_str()); return false; }
    if (!c.valid) { fprintf(stderr, "[Grass] %s: SPIR-V validation failed, program skipped: %s\n", what, c.valid_log.c_str()); return false; }
    std::vector<hlsl::UniformMember> m;
    if (!hlsl::GlobalBlockMembers(c.spv, m, st.size)) st.size = 0;
    for (const auto& u : m) st.mem[u.name] = { u.offset, u.array_stride };
    SDL_GPUShaderCreateInfo si = {}; si.code = (const Uint8*)c.spv.data(); si.code_size = c.spv.size() * 4; si.entrypoint = "main"; si.format = SDL_GPU_SHADERFORMAT_SPIRV;
    si.stage = stage == 'v' ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT; si.num_samplers = samplers; si.num_uniform_buffers = st.size ? 1 : 0;
    st.sh = SDL_CreateGPUShader(S.dev, &si);
    if (!st.sh) { fprintf(stderr, "[Grass] %s: SDL_CreateGPUShader: %s\n", what, SDL_GetError()); return false; }
    fprintf(stderr, "[Grass] %s OK: %zu words, $Global %u bytes, %zu members (%s)\n", what, c.spv.size(), st.size, m.size(), c.from_cache ? "cache" : "compiled");
    return true;
}

struct Ub {
    const Stage& st; std::vector<uint8_t> b;
    explicit Ub(const Stage& s) : st(s), b(s.size + 16, 0) {}
    void set(const char* name, const float* v, int n, int idx = 0) {
        auto it = st.mem.find(name); if (it == st.mem.end()) return;
        const uint32_t off = it->second.first + (it->second.second ? it->second.second : 16u) * idx;
        if (off + 4 * n <= b.size()) memcpy(&b[off], v, 4 * n);
    }
};

SDL_GPUBuffer* Upload(const void* data, uint32_t bytes, SDL_GPUBufferUsageFlags usage) {
    SDL_GPUBufferCreateInfo bi = {}; bi.usage = usage; bi.size = bytes;
    SDL_GPUBuffer* b = SDL_CreateGPUBuffer(S.dev, &bi);
    SDL_GPUTransferBufferCreateInfo ti = {}; ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = bytes;
    SDL_GPUTransferBuffer* t = SDL_CreateGPUTransferBuffer(S.dev, &ti);
    if (!b || !t) { if (b) SDL_ReleaseGPUBuffer(S.dev, b); if (t) SDL_ReleaseGPUTransferBuffer(S.dev, t); return nullptr; }
    void* p = SDL_MapGPUTransferBuffer(S.dev, t, false); memcpy(p, data, bytes); SDL_UnmapGPUTransferBuffer(S.dev, t);
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(S.dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src = { t, 0 }; SDL_GPUBufferRegion dst = { b, 0, bytes };
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cb);
    SDL_ReleaseGPUTransferBuffer(S.dev, t);
    return b;
}

}  // namespace

void Build(const std::vector<md::world::Blade>& blades, const md::world::Foliage& f, Mesh& out) {
    out = Mesh{};
    std::vector<size_t> order(blades.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return blades[a].grass < blades[b].grass; });
    out.verts.reserve(blades.size() * 8); out.idx.reserve(blades.size() * 12);
    int cur = -1;
    for (size_t k : order) {
        const md::world::Blade& b = blades[k];
        const md::world::Grass& g = f.grass[b.grass];
        if (b.grass != cur) {
            if (cur >= 0) out.runs.back().index_count = (uint32_t)out.idx.size() - out.runs.back().first_index;
            Run r; r.grass = b.grass; r.first_index = (uint32_t)out.idx.size(); out.runs.push_back(r);
            std::string s = g.sprite; for (char& ch : s) if (ch == '\\') ch = '/';
            out.sprites.push_back(std::move(s)); cur = b.grass;
        }
        const float cx = (float)(((double)b.x + 147456.0) / 10.0), cz = (float)(((double)b.z + 147456.0) / 10.0), cy = b.y / 10.f;
        const float hw = ((g.min_width * 9.f + (g.max_width * 17.f - g.min_width * 9.f) * b.u) * 0.5f) / 10.f;   // half of the width, in metres (GRASS_EXE_2026-11-15.md)
        const float hh = (g.min_height * 9.f + (g.max_height * 17.f - g.min_height * 9.f) * b.u) / 10.f;
        const float a = b.yaw * 0.01745329252f, ca = std::cos(a) * hw, sa = std::sin(a) * hw;
        const uint8_t col[4] = { (uint8_t)(b.colour & 255), (uint8_t)(b.colour >> 8 & 255), (uint8_t)(b.colour >> 16 & 255), (uint8_t)(b.colour >> 24) };   // layer colour map at the blade centre (GRASS_EXE_2026-11-15.md)
        auto quad = [&](float dx, float dz) {
            const uint32_t v0 = (uint32_t)out.verts.size();
            const Vertex v[4] = { { cx - dx, cy, cz - dz, { col[0], col[1], col[2], col[3] }, 0.f, 1.f }, { cx + dx, cy, cz + dz, { col[0], col[1], col[2], col[3] }, 1.f, 1.f },
                                  { cx - dx, cy + hh, cz - dz, { col[0], col[1], col[2], col[3] }, 0.f, 0.f }, { cx + dx, cy + hh, cz + dz, { col[0], col[1], col[2], col[3] }, 1.f, 0.f } };
            out.verts.insert(out.verts.end(), v, v + 4);
            const uint32_t i6[6] = { v0, v0 + 1, v0 + 2, v0 + 1, v0 + 3, v0 + 2 };
            out.idx.insert(out.idx.end(), i6, i6 + 6);
        };
        quad(ca, sa);
        if (g.cross_quads) quad(-sa, ca);
    }
    if (cur >= 0) out.runs.back().index_count = (uint32_t)out.idx.size() - out.runs.back().first_index;
    out.blades = blades.size();
}

bool Init(SDL_GPUDevice* dev, const std::string& kd) {
    if (S.ready) return true;
    if (!dev || kd.empty()) return false;
    S.dev = dev; S.kd = kd;
    const std::string dir = kd + "/data/materials/deferred/", src = dir + "foliage.hlsl";
    if (!std::filesystem::exists(src)) return false;
    std::vector<std::string> deps = { src };
    for (const char* d : { "common", "deferred", "forward", "post" }) {
        const std::string p = kd + "/data/materials/" + d;
        if (!std::filesystem::exists(p)) continue;
        for (const auto& e : std::filesystem::directory_iterator(p)) if (e.path().extension() == ".hlsl" && e.path().string() != src) deps.push_back(e.path().string());
    }
    if (!Build(S.vs, VertexGlue(src), deps, 'v', 0, "vertex (foliage.hlsl grass_vs)")) return false;
    if (!Build(S.fs, FragmentGlue(src), deps, 'f', 4, "fragment (foliage.hlsl grass_fs)")) return false;
    { GpuSamplerDesc sd; const uint8_t zero[4] = { 0, 0, 0, 0 }; S.none.InitFromMemory(zero, 1, 1, sd); if (!S.none.SDLTexture()) return false; }
    SDL_GPUSamplerCreateInfo sci = {}; sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_LINEAR; sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT; sci.max_lod = 1000.f;
    S.smp = SDL_CreateGPUSampler(dev, &sci);
    if (!S.smp) return false;
    SDL_GPUVertexBufferDescription vb = {}; vb.slot = 0; vb.pitch = sizeof(Vertex); vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute va[3] = {};
    va[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 }; va[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 12 }; va[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 16 };
    SDL_GPUColorTargetDescription ct[3] = {};
    ct[0].format = ct[1].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ct[2].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUGraphicsPipelineCreateInfo gi = {}; gi.vertex_shader = S.vs.sh; gi.fragment_shader = S.fs.sh; gi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gi.vertex_input_state.vertex_buffer_descriptions = &vb; gi.vertex_input_state.num_vertex_buffers = 1; gi.vertex_input_state.vertex_attributes = va; gi.vertex_input_state.num_vertex_attributes = 3;
    gi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; gi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    gi.depth_stencil_state.enable_depth_test = true; gi.depth_stencil_state.enable_depth_write = true; gi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    gi.target_info.color_target_descriptions = ct; gi.target_info.num_color_targets = 3; gi.target_info.has_depth_stencil_target = true; gi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    S.pipe = SDL_CreateGPUGraphicsPipeline(dev, &gi);
    if (!S.pipe) { fprintf(stderr, "[Grass] pipeline: %s\n", SDL_GetError()); return false; }
    S.ready = true;
    fprintf(stderr, "[Grass] ready\n");
    return true;
}

bool Ready() { return S.ready; }
size_t Blades() { return S.mesh.blades; }

void SetMesh(Mesh&& m) {
    if (!S.ready) return;
    const uint64_t t0 = SDL_GetTicksNS();
    if (S.vbo) { SDL_ReleaseGPUBuffer(S.dev, S.vbo); S.vbo = nullptr; }
    if (S.ibo) { SDL_ReleaseGPUBuffer(S.dev, S.ibo); S.ibo = nullptr; }
    for (auto& t : S.tex) if (t) t->Shutdown();
    S.tex.clear();
    S.mesh = std::move(m);
    if (S.mesh.idx.empty()) return;
    S.vbo = Upload(S.mesh.verts.data(), (uint32_t)(S.mesh.verts.size() * sizeof(Vertex)), SDL_GPU_BUFFERUSAGE_VERTEX);
    S.ibo = Upload(S.mesh.idx.data(), (uint32_t)(S.mesh.idx.size() * 4), SDL_GPU_BUFFERUSAGE_INDEX);
    for (const std::string& sp : S.mesh.sprites) {
        auto t = std::make_unique<GpuTexture>();
        if (!t->InitFromFile((S.kd + "/" + (sp.rfind("./", 0) == 0 ? sp.substr(2) : sp)).c_str())) fprintf(stderr, "[Grass] texture missing: %s\n", sp.c_str());
        S.tex.push_back(std::move(t));
    }
    fprintf(stderr, "[Grass] mesh: %zu blades, %zu verts, %zu indices, %zu runs, SetMesh %.1f ms\n", S.mesh.blades, S.mesh.verts.size(), S.mesh.idx.size(), S.mesh.runs.size(), (double)(SDL_GetTicksNS() - t0) / 1e6);
}

void Record(const Frame& f) {
    if (!S.ready || !S.vbo || !S.ibo || !f.cmd || !f.terrain_depth || !f.view_proj) return;
    SDL_GPUColorTargetInfo cti[3] = {};
    for (int i = 0; i < 3; ++i) { cti[i].texture = f.target[i]; cti[i].load_op = SDL_GPU_LOADOP_LOAD; cti[i].store_op = SDL_GPU_STOREOP_STORE; }
    SDL_GPUDepthStencilTargetInfo dti = {}; dti.texture = f.depth; dti.load_op = SDL_GPU_LOADOP_CLEAR; dti.clear_depth = 1.f; dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE; dti.cycle = false;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(f.cmd, cti, 3, &dti);
    if (!rp) return;
    // wind phase += dt (s) x speed 0.3 x pi, wrapped at 2 pi; time = phase, frequency = 2.0 (the distribution), direction = wind (1, 0, 0) x sway length (wind factor x 3).
    const uint64_t now = SDL_GetTicks(); const float dt = S.last_ms ? (float)(now - S.last_ms) / 1000.f : 0.f; S.last_ms = now;
    S.phase += dt * 0.3f * 3.14159265f; if (S.phase > 3.14159265f * 2.f) S.phase -= 3.14159265f * 2.f;
    Ub vu(S.vs), fu(S.fs);
    for (int c = 0; c < 4; ++c) vu.set("md_vp", f.view_proj + 4 * c, 4, c);
    const float time = S.phase, freq = 2.0f * 10.f, dir[3] = { 3.0f / 10.f, 0.f, 0.f }, fade = 1000.f / 10.f, gh = 1.0f / 10.f;   // units -> metres
    vu.set("md_time", &time, 1); vu.set("md_frequency", &freq, 1); vu.set("md_direction", dir, 3); vu.set("md_camPos", f.cam, 3); vu.set("md_fadeRange", &fade, 1); vu.set("md_grassHeight", &gh, 1);
    const float thr = 0.6f, far1 = f.far_clip, vpt4[4] = { (float)f.w, (float)f.h, 1.f / (float)f.w, 1.f / (float)f.h }, wh = -1e5f, wet = 0.f, tol = 0.f;
    fu.set("md_threshold", &thr, 1); fu.set("md_farClip", &far1, 1); fu.set("md_cameraPos", f.cam, 3); fu.set("md_viewport", vpt4, 4); fu.set("md_waterHeightRel", &wh, 1); fu.set("md_wetness", &wet, 1); fu.set("md_depthTol", &tol, 1);
    SDL_BindGPUGraphicsPipeline(rp, S.pipe);
    SDL_GPUBufferBinding vb = { S.vbo, 0 }; SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    SDL_GPUBufferBinding ib = { S.ibo, 0 }; SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_PushGPUVertexUniformData(f.cmd, 0, vu.b.data(), (Uint32)vu.b.size());
    SDL_PushGPUFragmentUniformData(f.cmd, 0, fu.b.data(), (Uint32)fu.b.size());
    SDL_GPUViewport vp = { 0.f, 0.f, (float)f.w, (float)f.h, 0.f, 1.f }; SDL_SetGPUViewport(rp, &vp);
    int drawn = 0;
    for (size_t i = 0; i < S.mesh.runs.size() && i < S.tex.size(); ++i) {
        const Run& r = S.mesh.runs[i];
        if (!r.index_count || !S.tex[i]->SDLTexture()) continue;
        SDL_GPUTextureSamplerBinding tb[4] = { { S.none.SDLTexture(), S.none.SDLSampler() }, { S.tex[i]->SDLTexture(), S.smp }, { S.tex[i]->SDLTexture(), S.smp }, { f.terrain_depth, S.none.SDLSampler() } };
        SDL_BindGPUFragmentSamplers(rp, 0, tb, 4);
        SDL_DrawGPUIndexedPrimitives(rp, r.index_count, 1, r.first_index, 0, 0); ++drawn;
    }
    SDL_EndGPURenderPass(rp);
    static int logged = 0; if (logged++ % 600 == 0) fprintf(stderr, "[Grass] pass drew %d of %zu runs, %zu blades\n", drawn, S.mesh.runs.size(), S.mesh.blades);
}

void Shutdown() {
    if (!S.dev) return;
    if (S.vbo) SDL_ReleaseGPUBuffer(S.dev, S.vbo);
    if (S.ibo) SDL_ReleaseGPUBuffer(S.dev, S.ibo);
    if (S.pipe) SDL_ReleaseGPUGraphicsPipeline(S.dev, S.pipe);
    if (S.vs.sh) SDL_ReleaseGPUShader(S.dev, S.vs.sh);
    if (S.fs.sh) SDL_ReleaseGPUShader(S.dev, S.fs.sh);
    if (S.smp) SDL_ReleaseGPUSampler(S.dev, S.smp);
    for (auto& t : S.tex) if (t) t->Shutdown();
    S.none.Shutdown();
    S = State{};
}

}  // namespace md::grasspass
