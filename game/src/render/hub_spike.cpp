#include <monkey_dust/render/ground_slot.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include "hub_spike.h"
#include "kenshi_material_cull.h"
#include <monkey_dust/io/ogre_mesh.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/zone_buildings.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "hzb_pass.h"
#include <string>
#include <vector>

static bool g_hub_cull_back = true;   // Kenshi parity: Ogre default cull for materials without cull_hardware none; applies to batches built after the call
void md_hub_set_cull_back(bool on) { g_hub_cull_back = on; }

HubSpike& HubSpike::Slot(int i) { static HubSpike s[SLOTS]; return s[i < 0 ? 0 : (i >= SLOTS ? SLOTS - 1 : i)]; }

void HubSpike::Shutdown() {
    for (int i = 0; i < batch_count_; ++i) { batches_[i].r.Shutdown(); batches_[i].inst.Shutdown(); batches_[i].ntiles = 0; }
    batch_count_ = 0; textured_ = 0; no_tex_ = 0; stats_ = Stats{};
}


// Mesh units are 0.1 m; the building scale is baked into the vertices so one batch = one (mesh, scale).
static void BuildSubmeshes(const md::ogre::Mesh& m, float k, float layer, std::vector<std::vector<PropVertex>>& vs, std::vector<std::vector<uint32_t>>& is, std::vector<uint8_t>& two) {
    for (const md::ogre::SubMesh& sm : m.submeshes) {
        if (sm.operation != md::ogre::OP_TRIANGLE_LIST || sm.indices.empty()) continue;
        const md::ogre::Geometry& g = sm.use_shared ? m.shared : sm.geometry;
        std::vector<float> p, n, uv;
        if (!g.Positions(p)) continue;
        bool hn = g.Normals(n), hu = g.Uvs(0, uv);
        std::vector<PropVertex> v(g.vertex_count);
        for (uint32_t i = 0; i < g.vertex_count; ++i) {
            v[i] = { p[i*3] * k, p[i*3+1] * k, p[i*3+2] * k, hn ? n[i*3] : 0.f, hn ? n[i*3+1] : 1.f, hn ? n[i*3+2] : 0.f, hu ? uv[i*2] : 0.f, hu ? uv[i*2+1] : 0.f, layer };
        }
        std::vector<uint32_t> ix = sm.indices;
        {   // bit-identical vertices share one index (the files list every triangle corner separately): ACMR 3.0 -> about half the vertex shader runs
            std::unordered_map<std::string, uint32_t> seen; std::vector<PropVertex> w; w.reserve(v.size() / 2); std::vector<uint32_t> remap(v.size(), 0);
            for (uint32_t i = 0; i < (uint32_t)v.size(); ++i) {
                std::string key((const char*)&v[i], sizeof(PropVertex));
                auto it = seen.find(key);
                if (it == seen.end()) { it = seen.emplace(std::move(key), (uint32_t)w.size()).first; w.push_back(v[i]); }
                remap[i] = it->second;
            }
            for (uint32_t& q : ix) q = remap[q];
            v = std::move(w);
        }
        vs.push_back(std::move(v)); is.push_back(std::move(ix)); two.push_back(KenshiMaterialTwoSided(sm.material) ? 1 : 0);
    }
}

// Static per-batch instance buffer, instances sorted by square tile (side = range / 4, 25 m minimum), one Tile per run.
void HubSpike::BuildTiles(Batch& b) {
    b.ntiles = 0;
    if (b.count <= 0) return;
    const float ts = range_ > 0.f ? std::fmax(25.f, range_ * 0.25f) : 1.0e6f;
    std::vector<int> idx((size_t)b.count);
    std::vector<long long> key((size_t)b.count);
    for (int k = 0; k < b.count; ++k) { idx[k] = b.first + k; key[k] = (long long)std::floor(pos_[(b.first + k) * 3] / ts) * 1000003LL + (long long)std::floor(pos_[(b.first + k) * 3 + 2] / ts); }
    std::vector<int> ord((size_t)b.count); for (int k = 0; k < b.count; ++k) ord[k] = k;
    std::sort(ord.begin(), ord.end(), [&](int a, int c) { return key[a] < key[c]; });
    std::vector<float> data((size_t)b.count * 8);
    for (int k = 0; k < b.count; ++k) {
        int o = idx[ord[k]];
        data[k*8] = pos_[o*3]; data[k*8+1] = pos_[o*3+1]; data[k*8+2] = pos_[o*3+2]; data[k*8+3] = scale_[o];
        data[k*8+4] = quat_[o*4]; data[k*8+5] = quat_[o*4+1]; data[k*8+6] = quat_[o*4+2]; data[k*8+7] = quat_[o*4+3];
    }
    b.inst.Init(0x8892u, data.data(), (uint32_t)(data.size() * sizeof(float)));
    int start = 0;
    while (start < b.count && b.ntiles < MAX_TILES) {
        int end = start + 1;
        while (end < b.count && key[ord[end]] == key[ord[start]]) ++end;
        if (b.ntiles == MAX_TILES - 1) end = b.count;   // the last tile takes the rest
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f, smax = 0;
        for (int k = start; k < end; ++k) { x0 = std::fmin(x0, data[k*8]); x1 = std::fmax(x1, data[k*8]); y0 = std::fmin(y0, data[k*8+1]); y1 = std::fmax(y1, data[k*8+1]); z0 = std::fmin(z0, data[k*8+2]); z1 = std::fmax(z1, data[k*8+2]); smax = std::fmax(smax, data[k*8+3]); }
        Tile& t = b.tiles[b.ntiles++];
        t.first = start; t.count = end - start; t.cx = 0.5f * (x0 + x1); t.cy = 0.5f * (y0 + y1); t.cz = 0.5f * (z0 + z1);
        t.rad = 0.5f * std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0)) + b.radius * smax;
        start = end;
    }
}

bool HubSpike::Load(const char* txt_path) {
    Shutdown();
    FILE* f = fopen(txt_path, "r");
    if (!f) { fprintf(stderr, "[HubSpike] cannot open %s\n", txt_path); return false; }
    std::vector<HubPlacement> pl;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char* tab = strchr(line, '\t'); if (!tab) continue;
        *tab = 0; HubPlacement p; p.mesh = line;
        char tex[300] = "";
        int al = 0;
        int n = sscanf(tab + 1, "%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%299[^\t\n]\t%d", &p.x, &p.y, &p.z, &p.scale, &p.q[0], &p.q[1], &p.q[2], &p.q[3], tex, &al);
        if (n >= 8) { if (n >= 9 && strcmp(tex, "-") != 0) p.tex = tex; p.alpha = (n >= 10) ? al : 0; pl.push_back(p); }
    }
    fclose(f);
    return LoadPlacements(pl);
}

static double RssMb() { long a = 0, r = 0; FILE* f = fopen("/proc/self/statm", "r"); if (f) { if (fscanf(f, "%ld %ld", &a, &r) != 2) r = 0; fclose(f); } return (double)r * 4096.0 / 1048576.0; }

bool HubSpike::LoadZone(float cx_m, float cz_m, float radius_m) {
    Shutdown();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<md::world::PlacedPart> parts; std::string err;
    if (!md::world::LoadBuildingsAt(md::KenshiDir(), cx_m, cz_m, radius_m, parts, err)) { fprintf(stderr, "[HubSpike] zone read failed: %s\n", err.c_str()); return false; }
    std::vector<HubPlacement> pl; pl.reserve(parts.size());
    for (const md::world::PlacedPart& p : parts) { HubPlacement h; h.mesh = p.mesh; h.x = p.x; h.y = p.y; h.z = p.z; h.scale = p.scale; for (int k = 0; k < 4; ++k) h.q[k] = p.quat[k]; pl.push_back(std::move(h)); }
    fprintf(stdout, "[HubSpike] zone buildings: %zu parts around (%.1f, %.1f) r=%.0f m\n", pl.size(), cx_m, cz_m, radius_m);
    const auto t1 = std::chrono::steady_clock::now();
    const bool ok = LoadPlacements(pl);
    const auto t2 = std::chrono::steady_clock::now();
    fprintf(stdout, "[HubSpike] zone load: data %.0f ms, meshes+buffers %.0f ms, %d batches, RSS %.0f MB\n", std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(), batch_count_, RssMb());
    return ok;
}

bool HubSpike::LoadPlacements(const std::vector<HubPlacement>& pl) {
    Begin(pl);
    while (Step()) {}
    return batch_count_ > 0;
}

static std::string PlacementKey(const HubPlacement& p) {
    std::string k = p.mesh + '|' + p.tex + '|' + (p.alpha ? '1' : '0') + (p.terrain ? "|T" : "");
    if (p.cut || p.dsided || p.tile_x != 1.f || p.tile_y != 1.f) { char b[96]; std::snprintf(b, sizeof b, "|c%d d%d t%.3f %.3f a%.3f", p.cut ? 1 : 0, p.dsided ? 1 : 0, p.tile_x, p.tile_y, p.thr); k += b; if (p.cut) k += '|' + p.nrm; }
    return k;
}

// One GPU texture per file for the whole process. Called from the zone worker (Prepare) so the file read, texture create and upload happen
// off the render thread (3..14 ms each); Step() then only finds it. A caller that meets a load in flight waits for it.
static GpuTexture* HubTexture(const std::string& tp) {
    static std::mutex mu; static std::condition_variable cv;
    static std::unordered_map<std::string, GpuTexture*> cache; static std::unordered_set<std::string> loading;
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return loading.find(tp) == loading.end(); });
    auto it = cache.find(tp);
    if (it != cache.end()) return it->second;
    loading.insert(tp);
    lk.unlock();
    std::string load = tp; GpuTexture* t = new GpuTexture; GpuSamplerDesc sd; sd.gen_mipmap = true;
    bool ok = t->InitFromFile(load.c_str(), sd);
    if (!ok) { const size_t hi = load.rfind("_HI.dds"); if (hi != std::string::npos) { load.erase(hi, 3); ok = t->InitFromFile(load.c_str(), sd); } }   // the record names the high-quality variant, the install may only have the plain one
    if (!ok) { delete t; t = nullptr; }
    lk.lock();
    cache.emplace(tp, t); loading.erase(tp);
    lk.unlock(); cv.notify_all();
    return t;
}

static std::string PlacementTexPath(const HubPlacement& p) { const std::string dir = md::KenshiDir(); return dir + "/" + (p.tex.rfind("./", 0) == 0 ? p.tex.substr(2) : p.tex); }

static bool PrepareOne(const HubPlacement& p, const std::vector<HubPlacement>& all, HubPrepared& out) {
    const std::string dir = md::KenshiDir();
        const float vlayer = p.tex.empty() ? 0.f : (p.cut ? 2.f + std::min(0.99f, std::max(0.f, p.thr)) : (p.alpha ? 3.f : 2.f));
        std::vector<std::vector<PropVertex>>& vs = out.vs; std::vector<std::vector<uint32_t>>& is = out.is;
        float bound_m = 10.f;
        if (p.mesh == "@grass") {   // two crossed quads, 10 units wide / high in mesh units (x0.1 -> 1 m)
            std::vector<PropVertex> v; std::vector<uint32_t> ix;
            for (int q = 0; q < 2; ++q) {
                float ax = q ? 0.f : 5.f, az = q ? 5.f : 0.f; uint32_t o = (uint32_t)v.size();
                v.push_back({ -ax, 0.f, -az, 0.f, 1.f, 0.f, 0.f, 1.f, vlayer }); v.push_back({ ax, 0.f, az, 0.f, 1.f, 0.f, 1.f, 1.f, vlayer });
                v.push_back({ ax, 10.f, az, 0.f, 1.f, 0.f, 1.f, 0.f, vlayer }); v.push_back({ -ax, 10.f, -az, 0.f, 1.f, 0.f, 0.f, 0.f, vlayer });
                for (uint32_t k : { 0u, 1u, 2u, 0u, 2u, 3u }) ix.push_back(o + k);
            }
            for (PropVertex& pv : v) { pv.x *= 0.1f; pv.y *= 0.1f; pv.z *= 0.1f; }
            vs.push_back(std::move(v)); is.push_back(std::move(ix)); out.two_sided.push_back(1); bound_m = 0.8f;
        } else {
        md::ogre::Mesh m; md::ogre::Error err;
        std::string path = dir + "/" + (p.mesh.rfind("./", 0) == 0 ? p.mesh.substr(2) : p.mesh);
        if (!md::ogre::ReadMesh(path, m, err)) { fprintf(stderr, "[HubSpike] mesh read failed: %s\n", path.c_str()); return false; }
        BuildSubmeshes(m, 0.1f, vlayer, vs, is, out.two_sided);
        if (p.tile_x != 1.f || p.tile_y != 1.f) for (auto& sub : vs) for (PropVertex& v : sub) { v.u *= p.tile_x; v.v *= p.tile_y; }   // spec `tile X/Y` (exe: uv * tiling in the vertex shader)
        if (p.dsided) for (size_t q = 0; q < out.two_sided.size(); ++q) out.two_sided[q] = 1;
        {   // oracle line: size in metres at scale 1 (positions x 0.1), counts, and the scale range of its placements
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }; uint64_t nv = 0, nt = 0;
            for (size_t q = 0; q < vs.size(); ++q) { nv += vs[q].size(); nt += is[q].size() / 3; for (const PropVertex& v : vs[q]) { const float c[3] = { v.x, v.y, v.z }; for (int a = 0; a < 3; ++a) { if (c[a] < lo[a]) lo[a] = c[a]; if (c[a] > hi[a]) hi[a] = c[a]; } } }
            float smin = 1e30f, smax = 0; int cnt = 0;
            for (const HubPlacement& q : all) if (q.mesh == p.mesh) { if (q.scale < smin) smin = q.scale; if (q.scale > smax) smax = q.scale; ++cnt; }
            fprintf(stdout, "[HubSpike] mesh %s: submeshes=%zu verts=%llu tris=%llu aabb_m=%.2fx%.2fx%.2f file_bounds_m=%.2fx%.2fx%.2f scale=%.3f..%.3f x%d -> max_extent_m=%.1f\n", p.mesh.c_str(), vs.size(), (unsigned long long)nv, (unsigned long long)nt,
                    hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], m.has_bounds ? (m.bounds_max[0] - m.bounds_min[0]) * 0.1f : -1.f, m.has_bounds ? (m.bounds_max[1] - m.bounds_min[1]) * 0.1f : -1.f, m.has_bounds ? (m.bounds_max[2] - m.bounds_min[2]) * 0.1f : -1.f, smin, smax, cnt, std::fmax(hi[0] - lo[0], std::fmax(hi[1] - lo[1], hi[2] - lo[2])) * smax);
        }
        bound_m = m.has_bounds ? m.bounds_radius * 0.1f : 10.f;
        }
    out.bound_m = bound_m;
    return true;
}

void HubSpike::Prepare(std::vector<HubPlacement>& pl, std::vector<HubPrepared>& out) {
    std::vector<std::string> done;
    for (HubPlacement& p : pl) p.key = PlacementKey(p);
    for (const HubPlacement& p : pl) {
        const std::string& key = p.key;
        if (std::find(done.begin(), done.end(), key) != done.end()) continue;
        done.push_back(key);
        HubPrepared h; h.key = key;
        if (PrepareOne(p, pl, h)) {
            if (!p.tex.empty()) HubTexture(PlacementTexPath(p));
            if (p.cut && !p.nrm.empty()) HubTexture(md::KenshiDir() + "/" + (p.nrm.rfind("./", 0) == 0 ? p.nrm.substr(2) : p.nrm));   // the cut-out normal map too: Step() loaded it on the render thread (24-57 ms per mesh in the town / foliage walks, #207 hitch trace)
            out.push_back(std::move(h));
        }
    }
}

void HubSpike::Begin(std::vector<HubPlacement> pl, std::vector<HubPrepared> prepared) {
    Shutdown();
    pl_ = std::move(pl); prepared_ = std::move(prepared); next_ = 0; done_.clear(); inst_ = 0; loading_ = !pl_.empty();
}

// One distinct (mesh, texture) per call: reads the mesh, builds its batches and GPU buffers. Returns true while more remain.
bool HubSpike::Step() {
    if (!loading_) return false;
    const std::string dir = md::KenshiDir();
    while (next_ < pl_.size()) {
        const size_t i = next_++;
        bool seen = false;
        if (pl_[i].key.empty()) pl_[i].key = PlacementKey(pl_[i]);   // the Load() path has no Prepare
        const std::string& key = pl_[i].key;
        for (const std::string& k : done_) if (k == key) { seen = true; break; }
        if (seen) continue;
        done_.push_back(key);
        HubPrepared tmp; const HubPrepared* P = nullptr;
        for (const HubPrepared& pr : prepared_) if (pr.key == key) { P = &pr; break; }
        if (!P) { tmp.key = key; if (!PrepareOne(pl_[i], pl_, tmp)) continue; P = &tmp; }
        const std::vector<std::vector<PropVertex>>& vs = P->vs; const std::vector<std::vector<uint32_t>>& is = P->is; const float bound_m = P->bound_m;
        for (size_t s = 0; s < vs.size() && batch_count_ < MAX_BATCHES; ++s) {
            Batch& b = batches_[batch_count_];
            b.terrain = pl_[i].terrain;
            b.r.SetCullBack(g_hub_cull_back && s < P->two_sided.size() && !P->two_sided[s]);
            if (pl_[i].cut) {   // TRANSPARENCY spec: the normal map's alpha is the cut-out mask; the renderer makes its pipeline for it before InitFromArrays
                const std::string& np = pl_[i].nrm; const std::string npath = dir + "/" + (np.rfind("./", 0) == 0 ? np.substr(2) : np);
                if (GpuTexture* nt = HubTexture(npath)) b.r.SetNormalAlphaCut(nt); else fprintf(stderr, "[HubSpike] normal map load failed: %s\n", npath.c_str());
            }
            if (!b.r.InitFromArrays(vs[s].data(), (uint32_t)vs[s].size(), is[s].data(), (uint32_t)is[s].size())) continue;
            if (pl_[i].tex.empty()) ++no_tex_;
            if (!pl_[i].tex.empty()) { std::string tp = PlacementTexPath(pl_[i]); {   // one GPU texture per file for the whole process (a zone reload or another batch of the same mesh must not read the 2048^2 file again: 17 ms each)
                GpuTexture* ht = HubTexture(tp); bool tex_ok = ht != nullptr;
                if (tex_ok) { b.r.SetSharedTexture(ht); ++textured_; }
                if (!tex_ok) fprintf(stderr, "[HubSpike] texture load failed: %s\n", tp.c_str()); } }
            b.tris = (uint32_t)is[s].size() / 3;
            b.radius = bound_m;
            b.first = inst_; b.count = 0;
            for (const HubPlacement& q : pl_) {
                if (q.mesh != pl_[i].mesh || q.tex != pl_[i].tex || q.alpha != pl_[i].alpha || inst_ >= MAX_INSTANCES) continue;
                if ((q.key.empty() ? PlacementKey(q) : q.key) != key) continue;   // same spec variant (cut-out, double-sided, tiling)
                pos_[inst_*3] = q.x; pos_[inst_*3+1] = q.y; pos_[inst_*3+2] = q.z; scale_[inst_] = q.scale;
                { float n = std::sqrt(q.q[0]*q.q[0] + q.q[1]*q.q[1] + q.q[2]*q.q[2] + q.q[3]*q.q[3]); if (n < 1e-6f) n = 1.f; float w = q.q[0] / n; quat_[inst_*4] = q.q[1] / n; quat_[inst_*4+1] = q.q[2] / n; quat_[inst_*4+2] = q.q[3] / n; quat_[inst_*4+3] = std::fabs(w) > 1e-4f ? w : 1e-4f; }   // prop.vert reads w == 0 as "no quaternion"
                ++inst_; ++b.count;
            }
            BuildTiles(b);
            ++batch_count_;
        }
            return next_ < pl_.size();
    }
    loading_ = false;
    stats_.batches = batch_count_;
    fprintf(stdout, "[HubSpike] %zu placements, %d batches, %d part instances\n", pl_.size(), batch_count_, inst_);
    return false;
}

// sphere (absolute centre, radius) hidden behind the farthest depth of the older frame's HZB: test with that frame's view-projection
static bool HzbOccluded(const HzbPass::View& v, float x, float y, float z, float r) {
    const float* m = v.vp;   // column-major
    const double cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= r + 0.5) return false;   // behind / too close to the near plane: keep
    const double cx = (m[0] * x + m[4] * y + m[8] * z + m[12]) / cw, cy = (m[1] * x + m[5] * y + m[9] * z + m[13]) / cw;
    const double fx = std::sqrt((double)m[0] * m[0] + (double)m[4] * m[4] + (double)m[8] * m[8]), fy = std::sqrt((double)m[1] * m[1] + (double)m[5] * m[5] + (double)m[9] * m[9]);
    const double rx = r * fx / (cw - r), ry = r * fy / (cw - r);
    int u0 = (int)std::floor((cx - rx) * 0.5 * HzbPass::W + 0.5 * HzbPass::W) - 1, u1 = (int)std::ceil((cx + rx) * 0.5 * HzbPass::W + 0.5 * HzbPass::W) + 1;
    int v0 = (int)std::floor((0.5 - (cy + ry) * 0.5) * HzbPass::H) - 1, v1 = (int)std::ceil((0.5 - (cy - ry) * 0.5) * HzbPass::H) + 1;
    if (u1 < 0 || v1 < 0 || u0 >= HzbPass::W || v0 >= HzbPass::H) return false;   // off screen in that frame: no information
    if (u0 < 0 || v0 < 0 || u1 >= HzbPass::W || v1 >= HzbPass::H) return false;   // partly off screen then: keep
    if ((u1 - u0) * (v1 - v0) > 3000) return false;
    float far_max = 0.f;
    for (int j = v0; j <= v1; ++j) for (int i = u0; i <= u1; ++i) far_max = std::max(far_max, v.far_m[j * HzbPass::W + i]);
    return far_max < (float)(cw - r) - 1.0f;
}

void HubSpike::Draw(
    SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
    const float* vp16, const float* sun32, float cam_x, float cam_z, const float* fr, float cam_y) {
    static float vp_[PropRenderer::MAX_PROPS * 3], vq_[PropRenderer::MAX_PROPS * 4], vs_[PropRenderer::MAX_PROPS];
    Stats s; s.batches = batch_count_;
    HzbPass::View hz; if (occlusion_) { hz = HzbPass::Get().Latest(); if (hz.valid && (std::fabs(hz.cam[0] - cam_x) > 3.f || std::fabs(hz.cam[2] - cam_z) > 3.f || (cam_y < 1.0e29f && std::fabs(hz.cam[1] - cam_y) > 3.f))) hz.valid = false; }   // the camera moved on: no culling from that map
    float pn[4][4];
    if (fr) for (int p = 0; p < 4; ++p) {
        float l = std::sqrt(fr[p*4] * fr[p*4] + fr[p*4+1] * fr[p*4+1] + fr[p*4+2] * fr[p*4+2]); if (l < 1e-6f) l = 1.f;
        for (int k = 0; k < 4; ++k) pn[p][k] = fr[p*4+k] / l;
    }
    if (instanced_) {   // one DrawIndexed per run of consecutive visible tiles of a mesh (static instance buffers, tile culling by range + frustum)
        static GpuPipeline pipe, pipe_flat, pipe_depth, pipe_depth_alpha;
        static bool tried = false;
        if (!tried) {
            tried = true;
            GpuPipeline::Desc pd;
            pd.vert_path = "shaders/hub_inst.vert"; pd.frag_path = "shaders/prop.frag";
            pd.layout.count = 4; pd.layout.stride = 36;
            pd.layout.attribs[0] = { 0, 0, GpuAttribFmt::F3 }; pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };
            pd.layout.attribs[2] = { 2, 24, GpuAttribFmt::F2 }; pd.layout.attribs[3] = { 3, 32, GpuAttribFmt::F1 };
            pd.layout.inst_count = 2; pd.layout.inst_stride = 32;
            pd.layout.inst_attribs[0] = { 4, 0, GpuAttribFmt::F4 }; pd.layout.inst_attribs[1] = { 5, 16, GpuAttribFmt::F4 };
            pd.raster.depth_test = true; pd.raster.depth_write = true; pd.raster.cull_back = false;
            pd.has_depth_target = true;
            pd.vert_uniform_bufs = 1; pd.frag_uniform_bufs = 1; pd.frag_samplers = 3;
            if (!pipe.Create(pd)) fprintf(stderr, "[HubSpike] instanced pipeline create failed\n");
            pd.frag_path = "shaders/hub_flat.frag"; pd.frag_uniform_bufs = 0; pd.frag_samplers = 0;
            if (!pipe_flat.Create(pd)) fprintf(stderr, "[HubSpike] flat pipeline create failed\n");
            pd.raster.color_write = false;
            if (!pipe_depth.Create(pd)) fprintf(stderr, "[HubSpike] depth pipeline create failed\n");
            pd.frag_path = "shaders/hub_alpha.frag"; pd.frag_samplers = 3;
            if (!pipe_depth_alpha.Create(pd)) fprintf(stderr, "[HubSpike] depth+alpha pipeline create failed\n");
        }
        for (int i = 0; i < batch_count_; ++i) {
            Batch& b = batches_[i];
            if (b.terrain && terrain_in_gbuf_) continue;
            bool begun = false;
            int run_first = -1, run_count = 0;
            auto emit = [&]() { if (run_count > 0) { b.r.DrawInstancedRange(rp, cmd, (uint32_t)run_first, (uint32_t)run_count); s.instances += run_count; ++s.draws; s.triangles += (int64_t)b.tris * run_count; } run_first = -1; run_count = 0; };
            for (int t = 0; t < b.ntiles; ++t) {
                const Tile& tl = b.tiles[t];
                bool vis = true;
                if (range_ > 0.f) { float dx = tl.cx - cam_x, dz = tl.cz - cam_z, lim = range_ + tl.rad; if (dx * dx + dz * dz > lim * lim) vis = false; }
                if (vis && fr) for (int p = 0; p < 4; ++p) if (pn[p][0] * tl.cx + pn[p][1] * tl.cy + pn[p][2] * tl.cz + pn[p][3] < -tl.rad) { vis = false; break; }
                if (vis && hz.valid && HzbOccluded(hz, tl.cx, tl.cy, tl.cz, tl.rad)) { vis = false; s.occluded += tl.count; }
                if (!vis) { emit(); continue; }
                if (!begun) { b.r.BeginInstanced(rp, cmd, variant_ == 1 ? pipe_flat : variant_ == 2 ? pipe_depth : variant_ == 3 ? pipe_depth_alpha : pipe, b.inst, vp16, sun32, variant_ == 0 || variant_ == 3, variant_ == 0); begun = true; }
                if (run_count > 0 && run_first + run_count == tl.first) run_count += tl.count; else { emit(); run_first = tl.first; run_count = tl.count; }
            }
            emit();
        }
        stats_ = s;
        return;
    }
    for (int i = 0; i < batch_count_; ++i) {
        Batch& b = batches_[i];
        if (b.terrain && terrain_in_gbuf_) continue;
        int n = 0;
        auto flush = [&]() {
            if (n == 0) return;
            b.r.DrawRaw(
                rp, cmd,
                vp_, n, vp16, sun32, 1.0f, 0.f, 0.f, nullptr, vq_, vs_);
            s.instances += n; s.draws += n; s.triangles += (int64_t)b.tris * n; n = 0;
        };
        for (int k = 0; k < b.count; ++k) {
            int o = b.first + k;
            float x = pos_[o*3], y = pos_[o*3+1], z = pos_[o*3+2], r = b.radius * scale_[o];
            if (range_ > 0.f) { float dx = x - cam_x, dz = z - cam_z, lim = range_ + r; if (dx * dx + dz * dz > lim * lim) continue; }
            bool out = false;
            if (fr) for (int p = 0; p < 4; ++p) if (pn[p][0] * x + pn[p][1] * y + pn[p][2] * z + pn[p][3] < -r) { out = true; break; }
            if (out) continue;
            if (hz.valid && HzbOccluded(hz, x, y, z, r)) { ++s.occluded; continue; }
            memcpy(vp_ + n * 3, pos_ + o * 3, 12); memcpy(vq_ + n * 4, quat_ + o * 4, 16); vs_[n] = scale_[o];
            if (++n == PropRenderer::MAX_PROPS) flush();
        }
        flush();
    }
    stats_ = s;
    if (occlusion_) { const float sh = (s.instances + s.occluded) > 0 ? (float)s.occluded / (float)(s.instances + s.occluded) : 0.f; if (sh > occ_share_) occ_share_ = sh; }
}

// TERRAIN-mode batches into the terrain G-buffer (feature_gbuf.vert + terrain_gbuffer_mini.frag, as FeatureScatterSystem::DrawGbuf): range and frustum culled per instance.
void HubSpike::DrawGbuf(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, float cam_x, float cam_z, const float* fr) {
    static GpuPipeline pipe; static bool tried = false;
    if (!tried) {
        tried = true;
        GpuPipeline::Desc pd;
        pd.vert_path = "shaders/feature_gbuf.vert"; pd.frag_path = "shaders/terrain_gbuffer_mini.frag";
        pd.layout.count = 4; pd.layout.stride = 36;
        pd.layout.attribs[0] = { 0, 0, GpuAttribFmt::F3 }; pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };
        pd.layout.attribs[2] = { 2, 24, GpuAttribFmt::F2 }; pd.layout.attribs[3] = { 3, 32, GpuAttribFmt::F1 };
        pd.raster.depth_test = true; pd.raster.depth_write = true; pd.raster.cull_back = false;
        pd.has_depth_target = true;
        pd.vert_uniform_bufs = 1; pd.frag_uniform_bufs = 0; pd.frag_samplers = 0; pd.frag_storage_bufs = 0;
        pd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT;
        if (md::GroundSlotTargetOn()) { pd.color_format2 = SDL_GPU_TEXTUREFORMAT_R8_UINT; pd.frag_path = "shaders/terrain_gbuffer_mini_slot0.frag"; pd.color2_keep = true; }   // the slot of the ground under the feature stays: the live pass shades it with the original terrain program (the install draws TERRAIN-mode features with terrainfp4.hlsl mapfeature_fs, deferred/mapfeature.material)
        if (!pipe.Create(pd)) fprintf(stderr, "[HubSpike] G-buffer pipeline create failed\n");
    }
    float pn[4][4];
    if (fr) for (int p = 0; p < 4; ++p) {
        float l = std::sqrt(fr[p*4] * fr[p*4] + fr[p*4+1] * fr[p*4+1] + fr[p*4+2] * fr[p*4+2]); if (l < 1e-6f) l = 1.f;
        for (int k = 0; k < 4; ++k) pn[p][k] = fr[p*4+k] / l;
    }
    static float gp[PropRenderer::MAX_PROPS * 3], gq[PropRenderer::MAX_PROPS * 4], gs[PropRenderer::MAX_PROPS];
    for (int i = 0; i < batch_count_; ++i) {
        Batch& b = batches_[i];
        if (!b.terrain) continue;
        int n = 0;
        auto flush = [&]() { if (n > 0) b.r.DrawGbuf(rp, cmd, pipe, gp, gq, n, vp16, gs); n = 0; };
        for (int k = 0; k < b.count; ++k) {
            const int o = b.first + k;
            const float x = pos_[o*3], y = pos_[o*3+1], z = pos_[o*3+2], r = b.radius * scale_[o];
            if (range_ > 0.f) { const float dx = x - cam_x, dz = z - cam_z, lim = range_ + r; if (dx * dx + dz * dz > lim * lim) continue; }
            bool out = false;
            if (fr) for (int p = 0; p < 4; ++p) if (pn[p][0] * x + pn[p][1] * y + pn[p][2] * z + pn[p][3] < -r) { out = true; break; }
            if (out) continue;
            memcpy(gp + n * 3, pos_ + o * 3, 12); memcpy(gq + n * 4, quat_ + o * 4, 16); gs[n] = scale_[o];
            if (++n == PropRenderer::MAX_PROPS) flush();
        }
        flush();
    }
}
