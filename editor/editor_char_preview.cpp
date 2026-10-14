#include "character/appearance.h"
#include "character/pose_solver.h"
#include "character/race_assets.h"
#include "character/session.h"
#include "editor_char_preview.h"
#ifdef MD_SDL_GPU
#include "imgui.h"
#include "stb_image.h"
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/backend/sdl_gpu_backend.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace CharPreviewSDLGPU {
namespace {
using md::character::M4;
using md::character::Mul;
using namespace md::character;

// vertex layout of shaders/char_preview.vert: pos(12) normal(12) uv(8) joints u8x4(4) weights f4(16)
struct Vtx { float px, py, pz, nx, ny, nz, u, v; uint8_t ji[4]; float wt[4]; };
struct VU { float mvp[16]; };

Session* g_session = nullptr;
std::unique_ptr<md::render_backend::SdlGpuBackend> g_backend;
GpuPipeline    g_body_pipe, g_bg_pipe, g_scene_pipe;
GpuStaticBuffer g_vbo, g_ibo, g_scene_vbo, g_scene_ibo;
GpuTexture     g_tex, g_tex_head, g_bg_sand, g_bg_dune;
GpuColorTexture g_color, g_depth;
int g_ni = 0, g_scene_ni = 0, g_rtt_w = 0, g_rtt_h = 0;
bool g_init = false, g_active = false;

md::ogre::Mesh g_mesh;
md::ogre::Skeleton g_skeleton;
SkinnedBody g_body;
std::vector<Vtx> g_base;
std::string g_key;
std::string g_error;                       // why the body is not shown (a missing file, with its name), "" when it is
uint64_t g_revision = 0;
bool g_dirty = true;
float g_ws[30][16];
float g_tint[4] = {1.f, 1.f, 1.f, 0.f};
float g_ground = 0.f;                      // lowest skinned vertex (file units)

float g_yaw = 0.f, g_pit = -0.06f, g_dist = 3.5f, g_lookat_y = 0.9f;

M4 RotY(float a) { M4 r = Identity(); r[0] = cosf(a); r[2] = sinf(a); r[8] = -sinf(a); r[10] = cosf(a); return r; }
M4 RotX(float a) { M4 r = Identity(); r[5] = cosf(a); r[6] = -sinf(a); r[9] = sinf(a); r[10] = cosf(a); return r; }
M4 Translate(float x, float y, float z) { M4 r = Identity(); r[12] = x; r[13] = y; r[14] = z; return r; }
M4 Persp(float fov, float asp, float n, float f) {
    M4 r{}; float t = 1.f / tanf(fov * 0.5f);
    r[0] = t / asp; r[5] = t; r[10] = (f + n) / (n - f); r[11] = -1.f; r[14] = (2.f * f * n) / (n - f);
    return r;
}
M4 InvRigid(const M4& m) {
    M4 o = Identity();
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o[c * 4 + r] = m[r * 4 + c];
    for (int k = 0; k < 3; ++k) o[12 + k] = -(o[k] * m[12] + o[4 + k] * m[13] + o[8 + k] * m[14]);
    return o;
}

void EnsureRtt(int w, int h) {
    if (w == g_rtt_w && h == g_rtt_h && g_color.SDLTexture()) return;
    g_color.Shutdown(); g_depth.Shutdown();
    g_rtt_w = w; g_rtt_h = h;
    g_color.Init(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    g_depth.Init(w, h, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
}

bool CreatePipelines() {
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/char_preview.vert"; pd.frag_path = "shaders/char_body.frag";
    pd.layout.count = 5; pd.layout.stride = sizeof(Vtx);
    pd.layout.attribs[0] = {0, 0, GpuAttribFmt::F3}; pd.layout.attribs[1] = {1, 12, GpuAttribFmt::F3}; pd.layout.attribs[2] = {2, 24, GpuAttribFmt::F2};
    pd.layout.attribs[3] = {3, 32, GpuAttribFmt::U8x4}; pd.layout.attribs[4] = {4, 36, GpuAttribFmt::F4};
    pd.raster.depth_test = true; pd.raster.depth_write = true; pd.raster.cull_back = true;
    pd.vert_uniform_bufs = 2; pd.vert_samplers = 0; pd.frag_samplers = 2; pd.frag_uniform_bufs = 1;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; pd.has_depth_target = true;
    if (!g_body_pipe.Create(pd)) { fprintf(stderr, "[CharPreview] body pipeline create failed\n"); return false; }

    GpuPipeline::Desc bg;   // sky + ground plane by ray intersection, fullscreen triangle
    bg.vert_path = "shaders/char_bg.vert"; bg.frag_path = "shaders/char_bg.frag";
    bg.layout.count = 0; bg.layout.stride = 0;
    bg.raster.depth_test = false; bg.raster.depth_write = false; bg.raster.cull_back = false;
    bg.frag_samplers = 2; bg.frag_uniform_bufs = 1;
    bg.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; bg.has_depth_target = true;
    if (!g_bg_pipe.Create(bg)) fprintf(stderr, "[CharPreview] background pipeline create failed\n");
    GpuSamplerDesc rep; rep.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP; rep.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    rep.wrap_s = rep.wrap_t = GpuSamplerDesc::Wrap::REPEAT;
    auto load_bg = [&](GpuTexture& tex, const char* path) {
        stbi_set_flip_vertically_on_load(0);
        int w, h, c; unsigned char* d = stbi_load(path, &w, &h, &c, 4);
        if (d) { tex.InitFromMemory(d, w, h, rep); stbi_image_free(d); }
        else { uint8_t fb[4] = {160, 130, 80, 255}; tex.InitFromMemory(fb, 1, 1, rep); fprintf(stderr, "[CharPreview] ground texture missing: %s\n", path); }
    };
    load_bg(g_bg_sand, "game/data/textures/terrain/desert_sand.jpg");
    load_bg(g_bg_dune, "game/data/textures/terrain/desert_dune.jpg");

    GpuPipeline::Desc sp;   // measuring pole
    sp.vert_path = "shaders/char_scene.vert"; sp.frag_path = "shaders/char_scene.frag";
    sp.layout.count = 2; sp.layout.stride = 24;
    sp.layout.attribs[0] = {0, 0, GpuAttribFmt::F3}; sp.layout.attribs[1] = {1, 12, GpuAttribFmt::F3};
    sp.raster.depth_test = true; sp.raster.depth_write = true; sp.raster.cull_back = false;
    sp.vert_uniform_bufs = 1;
    sp.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; sp.has_depth_target = true;
    if (!g_scene_pipe.Create(sp)) fprintf(stderr, "[CharPreview] pole pipeline create failed\n");

    struct SV { float x, y, z, r, g, b; };
    static SV verts[512]; static uint32_t idx[1024];
    int vi = 0, ii = 0;
    auto quad = [&](SV a, SV b, SV c, SV d) {
        uint32_t base = uint32_t(vi); verts[vi++] = a; verts[vi++] = b; verts[vi++] = c; verts[vi++] = d;
        idx[ii++] = base; idx[ii++] = base + 1; idx[ii++] = base + 2; idx[ii++] = base; idx[ii++] = base + 2; idx[ii++] = base + 3;
    };
    const float x0 = 0.630f, x1 = 0.658f, z0 = -0.014f, z1 = 0.014f, y1 = 2.10f;
    quad({x0, 0, z1, .30f, .22f, .15f}, {x1, 0, z1, .30f, .22f, .15f}, {x1, y1, z1, .30f, .22f, .15f}, {x0, y1, z1, .30f, .22f, .15f});
    quad({x1, 0, z0, .22f, .16f, .10f}, {x0, 0, z0, .22f, .16f, .10f}, {x0, y1, z0, .22f, .16f, .10f}, {x1, y1, z0, .22f, .16f, .10f});
    quad({x1, 0, z0, .26f, .19f, .12f}, {x1, 0, z1, .26f, .19f, .12f}, {x1, y1, z1, .26f, .19f, .12f}, {x1, y1, z0, .26f, .19f, .12f});
    for (int t = 1; t <= 20; ++t) {
        float ty = t * 0.10f, th = 0.007f, tw = (t % 10 == 0) ? 0.11f : (t % 5 == 0 ? 0.07f : 0.04f);
        quad({x1, ty + th, z0, .55f, .45f, .32f}, {x1 + tw, ty + th, z0, .55f, .45f, .32f}, {x1 + tw, ty + th, z1, .55f, .45f, .32f}, {x1, ty + th, z1, .55f, .45f, .32f});
        quad({x1, ty - th, z1, .44f, .36f, .26f}, {x1 + tw, ty - th, z1, .44f, .36f, .26f}, {x1 + tw, ty + th, z1, .55f, .45f, .32f}, {x1, ty + th, z1, .55f, .45f, .32f});
    }
    g_scene_ni = ii;
    g_scene_vbo.Init(0x8892u, verts, uint32_t(vi * sizeof(SV)));
    g_scene_ibo.Init(0x8893u, idx, uint32_t(ii * sizeof(uint32_t)));

    return true;
}

std::string FileOf(const std::string& race_id, const char* field) {
    const md::fcs::GameData* r = g_session->Data().Get(race_id);
    if (!r) return "";
    auto it = r->files.find(field);
    return it == r->files.end() ? "" : it->second;
}

// Loads the body, skeleton and textures of the session's current race and gender.
bool Rebuild() {
    const Session& ss = *g_session;
    const RaceInfo& race = ss.Current();
    const bool female = ss.gender() == Gender::Female;
    std::string rel, err;
    g_mesh = md::ogre::Mesh(); g_skeleton = md::ogre::Skeleton(); g_key.clear();
    if (!LoadBodyMesh(ss.Resources(), race, ss.gender(), g_mesh, rel, err)) { g_error = err; fprintf(stderr, "[CharPreview] %s\n", err.c_str()); return false; }
    md::ogre::Error oe;
    if (!md::ogre::ReadSkeleton(ss.Resources().Path(g_mesh.skeleton), g_skeleton, oe)) { g_error = "skeleton " + g_mesh.skeleton + ": " + oe.Message(); fprintf(stderr, "[CharPreview] %s\n", g_error.c_str()); return false; }
    if (!BuildSkinnedBody(g_mesh, g_skeleton, g_body, err)) { g_error = err; fprintf(stderr, "[CharPreview] %s\n", err.c_str()); return false; }
    if (g_body.bone_name.size() > 30) { fprintf(stderr, "[CharPreview] %zu bones: the preview supports 30\n", g_body.bone_name.size()); return false; }
    g_base.resize(g_body.vertices.size());
    for (size_t i = 0; i < g_base.size(); ++i) {
        const SkinnedVertex& sv = g_body.vertices[i]; Vtx& v = g_base[i];
        v.px = sv.pos[0]; v.py = sv.pos[1]; v.pz = sv.pos[2]; v.nx = sv.normal[0]; v.ny = sv.normal[1]; v.nz = sv.normal[2]; v.u = sv.uv[0]; v.v = sv.uv[1];
        for (int k = 0; k < 4; ++k) { v.ji[k] = sv.joint[k]; v.wt[k] = sv.weight[k]; }
    }
    // body texture from the race, head texture = `texture map` of the first head (stored paths are stale: found by file name)
    std::string body_tex = ss.Resources().Path(FileOf(race.id, female ? "body texture female" : "body texture male"));
    const std::vector<std::string>& heads = female ? race.heads_female : race.heads_male;
    std::string head_tex;
    if (!heads.empty()) { const md::fcs::GameData* h = ss.Data().Get(heads[0]); if (h) { auto it = h->files.find("texture map"); if (it != h->files.end()) head_tex = ss.Resources().Path(it->second); } }
    GpuSamplerDesc sd; sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP; sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    const char* bp = body_tex.c_str(); const char* hp = head_tex.c_str();
    if (body_tex.empty() || !g_tex.InitFromDDSArray(&bp, 1, sd)) { uint8_t fb[4] = {200, 162, 122, 255}; g_tex.InitFromMemory(fb, 1, 1, sd); g_error = "body texture not found: " + FileOf(race.id, female ? "body texture female" : "body texture male"); fprintf(stderr, "[CharPreview] %s\n", g_error.c_str()); }
    if (head_tex.empty() || !g_tex_head.InitFromDDSArray(&hp, 1, sd)) { uint8_t fb[4] = {200, 162, 122, 255}; g_tex_head.InitFromMemory(fb, 1, 1, sd); g_error = "head texture not found: " + head_tex; fprintf(stderr, "[CharPreview] %s\n", g_error.c_str()); }
    g_vbo.Init(0x8892u, g_base.data(), uint32_t(g_base.size() * sizeof(Vtx)));
    g_ibo.Init(0x8893u, g_body.indices.data(), uint32_t(g_body.indices.size() * sizeof(uint32_t)));
    g_ni = int(g_body.indices.size());
    g_error.clear();
    g_key = race.id + (female ? "|f" : "|m"); g_dirty = true;
    fprintf(stdout, "[CharPreview] %s %s: %zu vertices, %zu bones, %zu poses, mesh %s\n", race.name.c_str(), female ? "female" : "male", g_base.size(), g_body.bone_name.size(), g_body.poses.size(), rel.c_str());
    return true;
}

// Face sliders -> pose weights: value * 0.01 on the target, a negative value on target_opposite (weight = -value * 0.01).
void PoseWeights(std::vector<float>& w) {
    w.assign(g_body.poses.size(), 0.f);
    const Config* cfg = g_session->Limits();
    if (!cfg) return;
    auto idx = [&](const std::string& n) { for (size_t i = 0; i < g_body.poses.size(); ++i) if (g_body.poses[i].name == n) return int(i); return -1; };
    for (const Category& c : cfg->categories) for (const Range& r : c.ranges) {
        if (r.target.empty() && r.target_opposite.empty()) continue;
        float v = g_session->Value(r);
        if (v < 0.f && !r.target_opposite.empty()) { int k = idx(r.target_opposite); if (k >= 0) w[k] += -v * 0.01f; }
        else { int k = idx(r.target); if (k >= 0) w[k] += v * 0.01f; }
    }
}

// Skin matrices from the idle stance and the body sliders; face poses uploaded into the vertex buffer. Runs inside the frame's command buffer.
void Prepare(md::GpuCommandBufferHandle cmd) {
    if (g_session->revision() != g_revision) {
        g_revision = g_session->revision();
        const std::string key = g_session->Current().id + (g_session->gender() == Gender::Female ? "|f" : "|m");
        if (key != g_key && !Rebuild()) { g_active = false; return; }
        g_dirty = true;
    }
    if (!g_dirty) return;
    g_tint[0] = g_tint[1] = g_tint[2] = 1.f;   // a race without a Skin Tone range: the texture as it is
    if (const Config* cfg = g_session->Limits())
        for (const Category& c : cfg->categories) for (const ColourRange& cr : c.colour_ranges)
            if (cr.name == "Skin Tone") {
                auto it = g_session->Values().values.find(cr.name);
                auto col = ColourAt(cr, int(it == g_session->Values().values.end() ? float(cr.has_mid ? cr.mid : 0) : it->second));
                for (int k = 0; k < 3; ++k) g_tint[k] = col[k] / 255.f;
            }
    BodyScaleInputs in; in.female = g_session->gender() == Gender::Female;
    for (const std::string& n : g_body.bone_name) { if (n == "L Boob") in.has_breast_bones = true; if (n == "Bip01 Jaw") in.has_jaw_bone = true; }
    std::map<std::string, float> vals(g_session->Values().values.begin(), g_session->Values().values.end());
    static std::vector<M4> skin;
    SolveSkinMatrices(g_body, g_skeleton, FindAnimation(g_skeleton, "idle_stand_normal"), 0.f, ComputeBoneScales(vals, in), skin);
    float miny = 1e9f;
    for (const SkinnedVertex& v : g_body.vertices) {
        float y = 0.f;
        for (int k = 0; k < 4; ++k) { if (v.weight[k] <= 0.f) continue; const M4& a = skin[v.joint[k]]; y += v.weight[k] * (a[1] * v.pos[0] + a[5] * v.pos[1] + a[9] * v.pos[2] + a[13]); }
        miny = std::min(miny, y);
    }
    g_ground = miny;
    for (size_t i = 0; i < 30; ++i) {
        if (i < skin.size()) memcpy(g_ws[i], skin[i].data(), 64);
        else { memset(g_ws[i], 0, 64); g_ws[i][0] = g_ws[i][5] = g_ws[i][10] = g_ws[i][15] = 1.f; }
    }
    std::vector<float> w; PoseWeights(w);
    std::vector<Vtx> mv = g_base;
    for (size_t p = 0; p < w.size(); ++p) {
        if (w[p] == 0.f) continue;
        const std::vector<float>& d = g_body.poses[p].delta;
        for (size_t v = 0; v < mv.size(); ++v) { mv[v].px += w[p] * d[v * 3]; mv[v].py += w[p] * d[v * 3 + 1]; mv[v].pz += w[p] * d[v * 3 + 2]; }
    }
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    uint32_t sz = uint32_t(mv.size() * sizeof(Vtx));
    SDL_GPUTransferBufferCreateInfo tb = {}; tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size = sz;
    SDL_GPUTransferBuffer* tr = GpuCreateTransferBuffer(dev, &tb);
    if (tr) {
        void* mp = GpuMapTransfer(tr, false);
        if (mp) { memcpy(mp, mv.data(), sz); GpuUnmapTransfer(tr); }
        GpuCopyPass cp; cp.Begin(cmd);
        SDL_GPUTransferBufferLocation src = {tr, 0}; SDL_GPUBufferRegion dst = {g_vbo.SDLBuffer(), 0, sz};
        cp.UploadBuffer(src, dst, false); cp.End();
        GpuReleaseTransferBuffer(dev, tr);
    }
    g_dirty = false;
}

void Draw(md::GpuCommandBufferHandle cmd) {
    Prepare(cmd);
    if (!g_active) return;
    // file units are 0.1 m; the body is lifted so that its lowest vertex is on y = 0, where the ground and the pole stand
    M4 view = Mul(Translate(0.f, -g_lookat_y, -g_dist), Mul(RotX(g_pit), RotY(g_yaw)));
    float asp = float(g_rtt_w) / float(g_rtt_h);
    M4 proj = Persp(0.78f, asp, 0.05f, 10.f);
    M4 scale = Identity(); scale[0] = scale[5] = scale[10] = 0.1f;
    M4 mvp = Mul(proj, view);
    M4 body_mvp = Mul(proj, Mul(view, Mul(Translate(0.f, -g_ground * 0.1f, 0.f), scale)));

    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd; cpd.color_tex[0] = g_color.SDLTexture(); cpd.depth_tex = g_depth.SDLTexture();
    cpd.clear_color[0] = 0.f; cpd.clear_color[1] = 0.f; cpd.clear_color[2] = 0.f; cpd.clear_color[3] = 1.f;
    cpd.clear_depth = 1.f; cpd.load_color = false; cpd.load_depth = false;
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* rp = cb.SDLPass();
    if (!rp) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);

    if (g_bg_pipe.SDLPipeline() && g_bg_sand.SDLTexture() && g_bg_dune.SDLTexture()) {
        pv.BindPipeline(&g_bg_pipe);
        M4 iv = InvRigid(view);
        float th = tanf(0.39f);
        struct { float right[4], up[4], fwd[4], eye[4]; } u;
        u.right[0] = iv[0]; u.right[1] = iv[1]; u.right[2] = iv[2]; u.right[3] = th * asp;
        u.up[0] = iv[4]; u.up[1] = iv[5]; u.up[2] = iv[6]; u.up[3] = th;
        u.fwd[0] = -iv[8]; u.fwd[1] = -iv[9]; u.fwd[2] = -iv[10]; u.fwd[3] = 0.f;   // ground at world y = 0
        u.eye[0] = iv[12]; u.eye[1] = iv[13]; u.eye[2] = iv[14]; u.eye[3] = 0.f;
        GpuPushFragmentUniforms(cmd, 0, &u, sizeof(u));
        SDL_GPUTextureSamplerBinding t[2] = {{g_bg_sand.SDLTexture(), g_bg_sand.SDLSampler()}, {g_bg_dune.SDLTexture(), g_bg_dune.SDLSampler()}};
        pv.BindFragmentSamplers(0, t, 2);
        pv.Draw(3, 1, 0, 0);
    }
    if (g_scene_pipe.SDLPipeline() && g_scene_vbo.SDLBuffer() && g_scene_ni > 0) {
        pv.BindPipeline(&g_scene_pipe);
        pv.BindVertexBuffer(&g_scene_vbo);
        pv.BindIndexBuffer(&g_scene_ibo, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        VU svu; memcpy(svu.mvp, mvp.data(), 64);
        pv.PushVertexUniforms(0, &svu, sizeof(svu));
        pv.DrawIndexed(uint32_t(g_scene_ni), 1, 0, 0, 0);
    }
    if (g_body_pipe.SDLPipeline() && g_vbo.SDLBuffer() && g_ibo.SDLBuffer() && g_tex.SDLTexture() && g_tex_head.SDLTexture()) {
        pv.BindPipeline(&g_body_pipe);
        pv.BindVertexBuffer(&g_vbo);
        pv.BindIndexBuffer(&g_ibo, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        VU vu; memcpy(vu.mvp, body_mvp.data(), 64);
        pv.PushVertexUniforms(0, &vu, sizeof(vu));
        pv.PushVertexUniforms(1, g_ws, sizeof(g_ws));
        pv.PushFragmentUniforms(0, g_tint, sizeof(g_tint));
        SDL_GPUTextureSamplerBinding t[2] = {{g_tex.SDLTexture(), g_tex.SDLSampler()}, {g_tex_head.SDLTexture(), g_tex_head.SDLSampler()}};
        pv.BindFragmentSamplers(0, t, 2);
        pv.DrawIndexed(uint32_t(g_ni), 1, 0, 0, 0);
    }
    cb.EndPass();
}
} // namespace

bool Bind(Session* session) {
    g_session = session; g_active = false; g_key.clear(); g_revision = 0; g_error.clear();
    if (!session || !session->Loaded()) { g_error = "Kenshi not found: set KENSHI_DIR"; fprintf(stderr, "[CharPreview] %s\n", g_error.c_str()); return false; }
    if (!g_init) {
        if (!CreatePipelines()) return false;
        g_backend = std::make_unique<md::render_backend::SdlGpuBackend>();
        g_backend->Init();
        g_backend->SetGBufferPassCallback([](void*, const md::render_backend::RenderFrameParams& p) { Draw(p.cmd); }, nullptr);
        g_init = true;
    }
    g_active = Rebuild();
    g_revision = session->revision();
    return g_active;
}

bool Active() { return g_active && g_session; }

void RenderFrame(md::GpuCommandBufferHandle cmd) {
    if (!Active() || !g_color.SDLTexture() || g_rtt_w < 4 || g_rtt_h < 4) return;
    md::render_backend::RenderFrameParams rbp; rbp.cmd = cmd;
    g_backend->SetFrameParams(rbp);
    g_backend->RenderGBufferPass();
}

void ReloadPipelines() {
    g_body_pipe.Reload(); g_bg_pipe.Reload(); g_scene_pipe.Reload();
    fprintf(stdout, "[CharPreview] Pipelines reloaded\n");
}

void SetCameraForTab(int tab) {
    if (tab == 0) { g_dist = 3.5f; g_pit = -0.06f; g_lookat_y = 0.9f; }
    else { g_dist = 0.55f; g_pit = 0.f; g_lookat_y = 1.62f; }
}
void SetYaw(float radians) { g_yaw = radians; }

void DumpState(FILE* f) {
    fprintf(f, "[CharPreview]\n  active=%d  ni=%d  rtt=%dx%d\n  yaw=%.4f  pit=%.4f  dist=%.4f\n\n", g_active ? 1 : 0, g_ni, g_rtt_w, g_rtt_h, g_yaw, g_pit, g_dist);
}

void DrawInImGui(float W, float H) {
    int iw = int(W), ih = int(H);
    if (iw < 4 || ih < 4) return;
    EnsureRtt(iw, ih);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cpv", {W, H}, ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    if (hov && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f)) {
        g_yaw -= io.MouseDelta.x * 0.007f;
        g_pit = fmaxf(-1.4f, fminf(1.4f, g_pit - io.MouseDelta.y * 0.005f));
    }
    if (hov && io.MouseWheel != 0.f) g_dist = fmaxf(0.5f, fminf(6.f, g_dist - io.MouseWheel * 0.18f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_active && g_color.SDLTexture()) dl->AddImage((ImTextureID)g_color.SDLTexture(), origin, {origin.x + W, origin.y + H});
    else {
        dl->AddRectFilled(origin, {origin.x + W, origin.y + H}, IM_COL32(20, 20, 28, 255));
        const std::string msg = g_error.empty() ? std::string("Kenshi not found: set KENSHI_DIR") : g_error;
        ImGui::SetCursorScreenPos({origin.x + 12, origin.y + 40});
        ImGui::PushTextWrapPos(origin.x + W - 12);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 180, 120, 255));
        ImGui::TextUnformatted(msg.c_str());
        ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    }
    ImGui::SetCursorScreenPos({origin.x + 4, origin.y + H - 20});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 180, 160));
    ImGui::TextUnformatted("RMB=rotate  Scroll=zoom");
    ImGui::PopStyleColor();
}

} // namespace CharPreviewSDLGPU
#endif // MD_SDL_GPU
