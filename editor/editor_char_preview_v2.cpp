// Character preview v2: the body of a race read from the Kenshi install (md::ogre mesh + skeleton through md::character::SkinnedBody), posed by the idle stance and the
// body-slider bone sizes (md::character::SolveSkinMatrices), face sliders as mesh poses. Drawn with the preview's existing pipeline in DrawCharPreview.
// All data logic is in tools/editor/character/ (no ImGui there); this file only moves it to the GPU.
#include "character/appearance.h"
#include "character/editor_limits.h"
#include "character/pose_solver.h"
#include "character/race_assets.h"
#include "character/races.h"
#include "character/session.h"
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/kenshi_resources.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "editor_char_preview_sdlgpu_internal.h"
#ifdef MD_SDL_GPU

// (the internal header leaves namespace CharPreviewSDLGPU open)
GpuStaticBuffer s_v2_vbo;
GpuStaticBuffer s_v2_ibo;
int             s_v2_ni = 0;
GpuTexture      s_v2_tex;
GpuTexture      s_v2_tex_head;
float           s_v2_ws[30][16];
bool            s_v2_active = false;
float           s_v2_ground = 0.f;   // lowest skinned vertex (file units): the body is lifted so that it stands on y = 0

namespace {
using namespace md::character;

struct V2State {
    Session* session = nullptr;
    md::ogre::Mesh mesh;
    md::ogre::Skeleton skeleton;
    SkinnedBody body;
    std::vector<Vtx> base;           // vertices in file units
    std::string key;                 // race id + gender of the loaded body
    uint64_t revision = 0;
    bool dirty = true;
    bool loaded = false;
} g;

Session s_own_session;   // used by V2Load (scenarios); the editor panel binds its own with V2Bind

std::string FileOf(const std::string& race_id, const char* field) {
    const md::fcs::GameData* r = g.session->Data().Get(race_id);
    if (!r) return "";
    auto it = r->files.find(field);
    return it == r->files.end() ? "" : it->second;
}

// Face / body sliders -> pose weights: value * 0.01 on the target, a negative value on target_opposite (weight = -value * 0.01); without an opposite the signed weight stays on the target.
void PoseWeights(std::vector<float>& w) {
    w.assign(g.body.poses.size(), 0.f);
    const Config* cfg = g.session->Limits();
    if (!cfg) return;
    auto idx = [&](const std::string& n) { for (size_t i = 0; i < g.body.poses.size(); ++i) if (g.body.poses[i].name == n) return int(i); return -1; };
    for (const Category& c : cfg->categories) for (const Range& r : c.ranges) {
        if (r.target.empty() && r.target_opposite.empty()) continue;
        float v = g.session->Value(r);
        if (v < 0.f && !r.target_opposite.empty()) { int k = idx(r.target_opposite); if (k >= 0) w[k] += -v * 0.01f; }
        else { int k = idx(r.target); if (k >= 0) w[k] += v * 0.01f; }
    }
}

// Loads the body, skeleton and textures of the session's current race and gender.
bool Rebuild() {
    const Session& ss = *g.session;
    const RaceInfo& race = ss.Current();
    const bool female = ss.gender() == Gender::Female;
    std::string rel, err;
    g.mesh = md::ogre::Mesh(); g.skeleton = md::ogre::Skeleton(); g.loaded = false;
    if (!LoadBodyMesh(ss.Resources(), race, ss.gender(), g.mesh, rel, err)) { fprintf(stderr, "[CharV2] %s\n", err.c_str()); return false; }
    md::ogre::Error oe;
    if (!md::ogre::ReadSkeleton(ss.Resources().Path(g.mesh.skeleton), g.skeleton, oe)) { fprintf(stderr, "[CharV2] skeleton %s: %s\n", g.mesh.skeleton.c_str(), oe.Message().c_str()); return false; }
    if (!BuildSkinnedBody(g.mesh, g.skeleton, g.body, err)) { fprintf(stderr, "[CharV2] %s\n", err.c_str()); return false; }
    if (g.body.bone_name.size() > 30) { fprintf(stderr, "[CharV2] %zu bones: the preview supports 30\n", g.body.bone_name.size()); return false; }
    g.base.resize(g.body.vertices.size());
    for (size_t i = 0; i < g.base.size(); ++i) {
        const SkinnedVertex& sv = g.body.vertices[i]; Vtx& v = g.base[i];
        v.px = sv.pos[0]; v.py = sv.pos[1]; v.pz = sv.pos[2]; v.nx = sv.normal[0]; v.ny = sv.normal[1]; v.nz = sv.normal[2]; v.u = sv.uv[0]; v.v = sv.uv[1];
        for (int k = 0; k < 4; ++k) { v.ji[k] = sv.joint[k]; v.wt[k] = sv.weight[k]; }
    }
    // textures: the race's body texture, the first head's texture map (stored paths are stale: found by file name)
    std::string body_tex = ss.Resources().Path(FileOf(race.id, female ? "body texture female" : "body texture male"));
    const std::vector<std::string>& heads = female ? race.heads_female : race.heads_male;
    std::string head_tex;
    if (!heads.empty()) { const md::fcs::GameData* h = ss.Data().Get(heads[0]); if (h) { auto it = h->files.find("texture map"); if (it != h->files.end()) head_tex = ss.Resources().Path(it->second); } }
    GpuSamplerDesc sd; sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP; sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    const char* bp = body_tex.c_str(); const char* hp = head_tex.c_str();
    if (body_tex.empty() || !s_v2_tex.InitFromDDSArray(&bp, 1, sd)) { uint8_t fb[4] = {200, 162, 122, 255}; s_v2_tex.InitFromMemory(fb, 1, 1, sd); fprintf(stderr, "[CharV2] body texture '%s' not loaded\n", body_tex.c_str()); }
    if (head_tex.empty() || !s_v2_tex_head.InitFromDDSArray(&hp, 1, sd)) { uint8_t fb[4] = {200, 162, 122, 255}; s_v2_tex_head.InitFromMemory(fb, 1, 1, sd); fprintf(stderr, "[CharV2] head texture '%s' not loaded\n", head_tex.c_str()); }
    s_v2_vbo.Init(0x8892u, g.base.data(), uint32_t(g.base.size() * sizeof(Vtx)));
    s_v2_ibo.Init(0x8893u, g.body.indices.data(), uint32_t(g.body.indices.size() * sizeof(uint32_t)));
    s_v2_ni = int(g.body.indices.size());
    g.key = race.id + (female ? "|f" : "|m"); g.dirty = true; g.loaded = true;
    fprintf(stdout, "[CharV2] %s %s: %zu vertices, %zu bones, %zu poses, mesh %s, body texture %s, head texture %s\n", race.name.c_str(), female ? "female" : "male", g.base.size(),
            g.body.bone_name.size(), g.body.poses.size(), rel.c_str(), body_tex.c_str(), head_tex.c_str());
    return true;
}
} // namespace

bool V2Bind(md::character::Session* session) {
    g.session = session; g.loaded = false; g.key.clear(); g.revision = 0;
    if (!session || !session->Loaded()) { s_v2_active = false; return false; }
    s_v2_active = Rebuild();
    g.revision = session->revision();
    return s_v2_active;
}

bool V2Load(const char* race_name, bool female) {
    if (!s_own_session.Loaded()) {
        std::string kdir = md::KenshiDir(), err;
        if (kdir.empty()) { fprintf(stderr, "[CharV2] KENSHI_DIR not set: the old preview stays\n"); return false; }
        if (!s_own_session.Load(kdir, err)) { fprintf(stderr, "[CharV2] %s\n", err.c_str()); return false; }
    }
    if (!s_own_session.SelectSubraceByName(race_name, female)) { fprintf(stderr, "[CharV2] race '%s' not found\n", race_name); return false; }
    return V2Bind(&s_own_session);
}

void V2Off() { s_v2_active = false; }
bool V2Active() { return s_v2_active && g.loaded; }
void V2SetValue(const char* slider, float value) { if (g.session) g.session->SetValue(slider, value); }
void V2ResetValues() { if (g.session) g.session->Reset(); }

// Per frame, before the pass: skin matrices from the idle stance and the body sliders; face poses uploaded into the vertex buffer when a slider changed.
void V2Prepare(md::GpuCommandBufferHandle cmd) {
    if (!V2Active()) return;
    if (g.session->revision() != g.revision) {   // a slider or the selection changed
        g.revision = g.session->revision();
        const std::string key = g.session->Current().id + (g.session->gender() == Gender::Female ? "|f" : "|m");
        if (key != g.key && !Rebuild()) { s_v2_active = false; return; }
        g.dirty = true;
    }
    if (g.dirty) {
        BodyScaleInputs in; in.female = g.session->gender() == Gender::Female;
        for (const std::string& n : g.body.bone_name) { if (n == "L Boob") in.has_breast_bones = true; if (n == "Bip01 Jaw") in.has_jaw_bone = true; }
        std::map<std::string, float> vals(g.session->Values().values.begin(), g.session->Values().values.end());
        static std::vector<md::character::M4> skin;
        SolveSkinMatrices(g.body, g.skeleton, FindAnimation(g.skeleton, "idle_stand_normal"), 0.f, ComputeBoneScales(vals, in), skin);
        for (size_t i = 0; i < 30; ++i) {
            if (i < skin.size()) memcpy(s_v2_ws[i], skin[i].data(), 64);
            else { memset(s_v2_ws[i], 0, 64); s_v2_ws[i][0] = s_v2_ws[i][5] = s_v2_ws[i][10] = s_v2_ws[i][15] = 1.f; }
        }
        float miny = 1e9f;   // lowest vertex of the skinned body (feet), for grounding
        for (const SkinnedVertex& v : g.body.vertices) {
            float y = 0.f;
            for (int k = 0; k < 4; ++k) { if (v.weight[k] <= 0.f) continue; const md::character::M4& a = skin[v.joint[k]]; y += v.weight[k] * (a[1] * v.pos[0] + a[5] * v.pos[1] + a[9] * v.pos[2] + a[13]); }
            miny = std::min(miny, y);
        }
        s_v2_ground = miny;
        std::vector<float> w; PoseWeights(w);
        std::vector<Vtx> mv = g.base;
        for (size_t p = 0; p < w.size(); ++p) {
            if (w[p] == 0.f) continue;
            const std::vector<float>& d = g.body.poses[p].delta;
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
            SDL_GPUTransferBufferLocation src = {tr, 0}; SDL_GPUBufferRegion dst = {s_v2_vbo.SDLBuffer(), 0, sz};
            cp.UploadBuffer(src, dst, false); cp.End();
            GpuReleaseTransferBuffer(dev, tr);
        }
        g.dirty = false;
    }
}

} // namespace CharPreviewSDLGPU
#endif // MD_SDL_GPU
