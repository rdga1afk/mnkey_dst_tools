#include "monkey_dust/render/ground_slot.h"
#include "ground_live_driver.h"
#include "sky_clouds.h"
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/world/biome_def.h>
#include <stb_image.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace md {
namespace {

SDL_GPUTexture* MakeTex(SDL_GPUDevice* dev, SDL_GPUTextureFormat fm, int w, int h, const void* data, size_t bytes) {
    SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = fm; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = w; ti.height = h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(dev, &ti); if (!t) return nullptr;
    SDL_GPUTransferBufferCreateInfo bi = {}; bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = (Uint32)bytes;
    SDL_GPUTransferBuffer* up = SDL_CreateGPUTransferBuffer(dev, &bi);
    memcpy(SDL_MapGPUTransferBuffer(dev, up, false), data, bytes); SDL_UnmapGPUTransferBuffer(dev, up);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev); SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo s = { up, 0, (Uint32)w, (Uint32)h }; SDL_GPUTextureRegion d = {}; d.texture = t; d.w = w; d.h = h; d.d = 1; SDL_UploadToGPUTexture(cp, &s, &d, false);
    SDL_EndGPUCopyPass(cp); SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); SDL_ReleaseGPUTransferBuffer(dev, up);
    return t;
}

bool SameList(const PatchBiomes& a, const PatchBiomes& b) {
    if (a.count != b.count) return false;
    for (int i = 0; i < a.count; ++i) if (a.ids[i] != b.ids[i] || a.channels[i] != b.channels[i]) return false;
    return true;
}

}   // namespace

GroundLiveDriver& GroundLiveDriver::Get() { static GroundLiveDriver d; return d; }
bool GroundLiveDriver::Wanted() { static const bool w = getenv("MD_GROUND_LIVE") && getenv("MD_GROUND_LIVE")[0] == '1'; return w; }
bool GroundLiveDriver::OrigLight() { return GroundLiveOrigLightOn(); }

const GroundLiveDriver::BiomeInfo* GroundLiveDriver::Find(int raw) const {
    for (const auto& kv : biome_of_) if (kv.first == raw) return &kv.second;
    return nullptr;
}

bool GroundLiveDriver::InitData() {
    kenshi_dir_ = BiomeRegistry::ResolveKenshiDir();
    if (kenshi_dir_.empty()) { MD_LOG(MD_LOG_WARNING, "[GroundLive] no Kenshi folder"); return false; }
    if (!kbi_.Load(kenshi_dir_ + "/data/newland/land/blendinfo.dat")) { MD_LOG(MD_LOG_WARNING, "[GroundLive] blendinfo.dat missing"); return false; }
    // the biome records (merged) by their "index" value (= the raw KBI1 id), their ground pack and the registry index (legend colour)
    fcs::Install inst(kenshi_dir_); fcs::LoadOrder order; fcs::Error err; fcs::GameDataContainer gd;
    if (!inst.LoadOrderFor(order, err) || !gd.Load(order, err)) { MD_LOG(MD_LOG_WARNING, "[GroundLive] game data: %s", err.Message().c_str()); return false; }
    const BiomeRegistry& R = BiomeRegistry::Get();
    for (const fcs::GameData* g : gd.OfType(fcs::ITEM_BIOMES)) {
        fcs::Record r; r.kind = g->kind; r.name = g->name;
        for (const auto& kv : g->floats) r.floats.emplace_back(kv.first, kv.second);
        for (const auto& kv : g->ints) r.ints.emplace_back(kv.first, kv.second);
        int raw = 0; bool has = false; for (const auto& kv : r.ints) if (kv.first == "index") { raw = kv.second; has = true; }
        if (!has) continue;
        BiomeInfo bi; bi.pack = PackGroundBiome(r);
        const BiomeDef& d = R.ForColor((uint8_t)(raw >> 16), (uint8_t)(raw >> 8), (uint8_t)raw);
        bi.reg_index = d.biome_id; bi.valid = d.biome_id >= 0;
        biome_of_.push_back({ raw, bi });
    }
    if (const char* e = getenv("MD_GROUND_LIVE_GROUPS")) max_groups_ = std::max(1, std::min(31, atoi(e)));
    lists_.reserve(1024);
    return true;
}

bool GroundLiveDriver::InitGpu(SDL_GPUDevice* dev) {
    int w = 0, h = 0, nc = 0; unsigned char* bm = stbi_load((kenshi_dir_ + "/data/newland/land/blendmap.png").c_str(), &w, &h, &nc, 4);
    if (!bm) { MD_LOG(MD_LOG_WARNING, "[GroundLive] blendmap.png missing"); return false; }
    blend_ = MakeTex(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, bm, (size_t)w * h * 4); stbi_image_free(bm);
    const Uint8 white[4] = { 255, 255, 255, 255 }, zero[4] = {};
    { int dw = 0, dh = 0, dn = 0; unsigned char* dm = stbi_load((kenshi_dir_ + "/data/newland/land/overlaymaps/distant.png").c_str(), &dw, &dh, &dn, 4);   // the far program's distantColour
      if (dm) { distant_ = MakeTex(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, dw, dh, dm, (size_t)dw * dh * 4); stbi_image_free(dm); } }
    white_ = MakeTex(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, white, 4); clear_ = MakeTex(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, zero, 4);
    if (!blend_ || !white_ || !clear_) return false;
    arrays_.SetBuildGapMs(getenv("MD_GROUND_LIVE_GAP_MS") ? atoi(getenv("MD_GROUND_LIVE_GAP_MS")) : 40);
    if (const char* e = getenv("MD_GROUND_LIVE_MB")) arrays_.SetBudgetBytes((uint64_t)atoi(e) << 20);
    if (!pass_.Init(dev, kenshi_dir_)) return false;
    MD_LOG(MD_LOG_INFO, "[GroundLive] ready: %zu biomes, up to %d groups", biome_of_.size(), max_groups_);
    return true;
}

int GroundLiveDriver::ListIdOf(const PatchBiomes& pb) {
    for (size_t i = 0; i < lists_.size(); ++i) if (SameList(lists_[i].pb, pb)) return (int)i;
    PatchList pl; pl.pb = pb; lists_.push_back(pl); return (int)lists_.size() - 1;
}

int GroundLiveDriver::SlotAt(float x, float z) {
    int best = 0; float best_size = 1e30f;
    for (int i = 0; i < n_node_slot_; ++i) {
        const NodeSlot& q = node_slot_[i];
        if (x >= q.x0 && x < q.x0 + q.size && z >= q.z0 && z < q.z0 + q.size && q.size < best_size) { best = q.slot; best_size = q.size; }
    }
    if (best || !data_ok_) return best;
    // no visible node holds the point (its node is culled): the slot of the list at the point itself, else the nearest visible node's slot
    const float px = (float)((x - 0.05) - 14745.6) * 10.f, pz = (float)((z - 0.05) - 14745.6) * 10.f, qx = (float)((x + 0.05) - 14745.6) * 10.f, qz = (float)((z + 0.05) - 14745.6) * 10.f;
    const PatchBiomes pb = kbi_.Query(px, pz, qx, qz);
    if (pb.count > 0) { const int lid = ListIdOf(pb); if (lists_[(size_t)lid].slot) return lists_[(size_t)lid].slot; }
    float best_d2 = 1e30f;
    for (int i = 0; i < n_node_slot_; ++i) {
        const NodeSlot& q = node_slot_[i]; if (!q.slot) continue;
        const float dx = std::max(std::max(q.x0 - x, x - (q.x0 + q.size)), 0.f), dz = std::max(std::max(q.z0 - z, z - (q.z0 + q.size)), 0.f), d2 = dx * dx + dz * dz;
        if (d2 < best_d2) { best_d2 = d2; best = q.slot; }
    }
    return best;
}

void GroundLiveDriver::AssignSlots(TerrainLodNode* nodes, int n, const float cam[3], const TerrainLodNode* pre, int n_pre) {
    if (!data_tried_) { data_tried_ = true; data_ok_ = InitData(); }
    stats_.assign_ms = 0; n_slots_ = 0;
    if (!data_ok_) { for (int i = 0; i < n; ++i) nodes[i].list_slot = 0; return; }
    const auto t0 = std::chrono::steady_clock::now();
    for (PatchList& l : lists_) { l.dmin = 1e30f; l.dmin_pre = 1e30f; }
    static thread_local int lid_of[8192]; if (n > 8192) n = 8192;
    auto list_of = [&](const TerrainLodNode& nd) -> int {
        const uint64_t key = ((uint64_t)std::lround(nd.origin_x * 4.f) << 24) ^ ((uint64_t)std::lround(nd.origin_z * 4.f) << 6) ^ (uint64_t)nd.level;
        auto it = node_list_.find(key); int lid;
        if (it != node_list_.end()) lid = it->second;
        else {   // the original's rule on the node's box (centred 0.1 m units); the max corner is exclusive
            const float x0 = (float)((nd.origin_x + 0.01) - 14745.6) * 10.f, z0 = (float)((nd.origin_z + 0.01) - 14745.6) * 10.f, x1 = (float)((nd.origin_x + nd.size - 0.01) - 14745.6) * 10.f, z1 = (float)((nd.origin_z + nd.size - 0.01) - 14745.6) * 10.f;
            const PatchBiomes pb = kbi_.Query(x0, z0, x1, z1);
            lid = pb.count > 0 ? ListIdOf(pb) : -1; node_list_.emplace(key, lid);
            if (lid < 0 && getenv("MD_GROUND_LIVE_LOG")) MD_LOG(MD_LOG_INFO, "[GroundLive] node without a list: level %d origin (%.1f, %.1f) size %.1f", nd.level, nd.origin_x, nd.origin_z, nd.size);
        }
        return lid;
    };
    for (int i = 0; i < n; ++i) {
        const TerrainLodNode& nd = nodes[i];
        const int lid = list_of(nd);
        lid_of[i] = lid;
        if (lid < 0) continue;
        const float dx = std::max(std::max(nd.origin_x - cam[0], cam[0] - (nd.origin_x + nd.size)), 0.f), dz = std::max(std::max(nd.origin_z - cam[2], cam[2] - (nd.origin_z + nd.size)), 0.f);
        const float d2 = dx * dx + dz * dz; if (d2 < lists_[(size_t)lid].dmin) lists_[(size_t)lid].dmin = d2;
    }
    // the nodes the horizon cull dropped still decide the array tier of their list (the tier rule uses the list's nearest node): without them a list whose near nodes are hidden would take a coarser tier and the picture would differ from the picture without the cull
    if (pre && n_pre > 0) for (int i = 0; i < n_pre && i < 8192; ++i) {
        const TerrainLodNode& nd = pre[i]; const int lid = list_of(nd); if (lid < 0) continue;
        const float dx = std::max(std::max(nd.origin_x - cam[0], cam[0] - (nd.origin_x + nd.size)), 0.f), dz = std::max(std::max(nd.origin_z - cam[2], cam[2] - (nd.origin_z + nd.size)), 0.f);
        const float d2 = dx * dx + dz * dz; if (d2 < lists_[(size_t)lid].dmin_pre) lists_[(size_t)lid].dmin_pre = d2;
    }
    for (PatchList& l : lists_) if (l.dmin < l.dmin_pre) l.dmin_pre = l.dmin;
    if (GroundLiveRtState().groups > 0) max_groups_ = GroundLiveRtState().groups;   // md.set_ground_live_switch("groups", n)
    // the nearest lists get slots 1..max_groups
    static thread_local int order[1024]; int m = 0;
    for (size_t i = 0; i < lists_.size() && m < 1024; ++i) { lists_[i].slot = 0; if (lists_[i].dmin < 1e29f) order[m++] = (int)i; }
    std::sort(order, order + m, [&](int a, int b) { return lists_[(size_t)a].dmin < lists_[(size_t)b].dmin; });
    stats_.lists_seen = m; stats_.over_limit = std::max(0, m - max_groups_);
    stats_.lists_near = 0; for (int k = 0; k < m; ++k) if (lists_[(size_t)order[k]].dmin < 3000.f * 3000.f) ++stats_.lists_near;   // within the original's DistantTerrain distance (3 km)
    for (int k = 0; k < m && k < max_groups_; ++k) { lists_[(size_t)order[k]].slot = k + 1; slot_list_[k + 1] = order[k]; slot_dmin_[k + 1] = std::sqrt(lists_[(size_t)order[k]].dmin_pre); n_slots_ = k + 1; }
    stats_.no_slot_nodes = 0;
    stats_.no_slot_near = 0;
    for (int i = 0; i < n; ++i) {
        nodes[i].list_slot = lid_of[i] >= 0 ? lists_[(size_t)lid_of[i]].slot : 0;
        if (nodes[i].list_slot) continue;
        ++stats_.no_slot_nodes;
        const TerrainLodNode& nd = nodes[i];
        const float dx = std::max(std::max(nd.origin_x - cam[0], cam[0] - (nd.origin_x + nd.size)), 0.f), dz = std::max(std::max(nd.origin_z - cam[2], cam[2] - (nd.origin_z + nd.size)), 0.f);
        if (dx * dx + dz * dz < 3000.f * 3000.f) ++stats_.no_slot_near;
    }
    for (int i = 0; i < n; ++i) node_slot_[i] = { nodes[i].origin_x, nodes[i].origin_z, nodes[i].size, nodes[i].list_slot };
    n_node_slot_ = n;
    GroundSlotAtHook() = [](float x, float z) { return GroundLiveDriver::Get().SlotAt(x, z); };
    stats_.assign_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void GroundLiveDriver::Frame(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, SDL_GPUTexture* packed, SDL_GPUTexture* slot, int w, int h, const float inv_vp[16], const float cam[3], uint64_t) {
    const uint64_t frame = ++frame_counter_;
    lit_frame_ = false;
    if (!data_ok_) return;
    if (!gpu_tried_) { gpu_tried_ = true; gpu_ok_ = InitGpu(dev); }
    if (!gpu_ok_ || !depth || !packed || !slot || w < 16 || h < 16) return;
    if (!pass_.EnsureTargets(w, h)) return;
    // pixel angle from the inverse view-projection: two neighbouring pixels at the same depth
    auto unproj = [&](float nx, float ny, float out[3]) {
        const float* m = inv_vp; const float v[4] = { nx, ny, 0.5f, 1.f }; float r[4];
        for (int i = 0; i < 4; ++i) r[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
        out[0] = r[0] / r[3]; out[1] = r[1] / r[3]; out[2] = r[2] / r[3];
    };
    float p0[3], p1[3]; unproj(0.f, 0.f, p0); unproj(2.f / h, 0.f, p1);
    const float dd = std::sqrt((p0[0] - cam[0]) * (p0[0] - cam[0]) + (p0[1] - cam[1]) * (p0[1] - cam[1]) + (p0[2] - cam[2]) * (p0[2] - cam[2]));
    const float sep = std::sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1]) + (p1[2] - p0[2]) * (p1[2] - p0[2]));
    const float pix_angle = dd > 1e-3f ? sep / dd : 0.f;
    arrays_.Poll(frame);
    const int skipped0 = 0; (void)skipped0;
    stats_.skipped = stats_.waiting = stats_.groups = 0; stats_.min_margin = 1e9f; for (int& t : stats_.tiers) t = 0;
    static GroundLiveGroup groups[32]; int ng = 0;
    for (int s = 1; s <= n_slots_; ++s) {
        const PatchBiomes& pb = lists_[(size_t)slot_list_[s]].pb;
        GroundLiveGroup& g = groups[ng]; g.slot = s; g.biome_count = 0; g.far = false; g.x0 = g.x1 = g.y0 = g.y1 = 0;
        float tile = 1e9f; bool ok = true;
        const float d = slot_dmin_[s] + 1.f;
        for (int b = 0; b < pb.count; ++b) {
            const BiomeInfo* bi = Find(pb.ids[b]); if (!bi || !bi->valid) { ok = false; break; }
            for (const float* a : { bi->pack.scalesA, bi->pack.scalesB, bi->pack.scalesC }) for (int j = 0; j < 4; ++j) if (a[j] > 0.f && a[j] < tile) tile = a[j];
        }
        if (!ok) { g.far = true; ++stats_.skipped; ++ng; continue; }   // a biome without data: the original far program (never left unwritten)
        const float footprint = pix_angle * d * 4.096f * (tile < 1e8f ? tile : 1.f);   // base texels per pixel along the minor axis
        int K = 0; while (K < 6 && std::ldexp(1.f, K + 2) <= footprint / 1.25f) K += 2;
        const int tier_rec = K / 2; if (K > 0) stats_.min_margin = std::min(stats_.min_margin, footprint / 1.25f / std::ldexp(1.f, K));
        for (int b = 0; b < pb.count; ++b) {
            const BiomeInfo* bi = Find(pb.ids[b]);
            // finest-adequate rule: any resident tier at or below the wanted one serves (the coarsest of them); otherwise the build is requested and a coarser resident tier stands in
            const BiomeLayerSet* set = nullptr;
            for (int t = tier_rec; t >= 0 && !set; --t) set = arrays_.Peek(bi->reg_index, frame, t);
            if (!set) {
                arrays_.Request(bi->reg_index, tier_rec); ++stats_.waiting;
                for (int t = tier_rec + 1; t <= 3 && !set; ++t) set = arrays_.Peek(bi->reg_index, frame, t);
            }
            if (!set) { ok = false; ++stats_.skipped; break; }
            g.biome[g.biome_count].pack = bi->pack; g.biome[g.biome_count].diffuse = set->diffuse.SDLTexture(); g.biome[g.biome_count].normal = set->normal.SDLTexture(); ++g.biome_count;
        }
        if (!ok || g.biome_count < 1) { g.far = true; g.biome_count = 0; ++ng; continue; }   // no resident tier (streaming): the original far program until the arrays arrive (skipped counts these)
        ++stats_.tiers[tier_rec]; ++ng;
    }
    stats_.groups = ng; stats_.resident = arrays_.Cache().Resident(); stats_.resident_bytes = arrays_.Cache().ResidentBytes(); stats_.builds = arrays_.Cache().Builds(); stats_.evictions = arrays_.Cache().Evictions();
    static const bool log = getenv("MD_GROUND_LIVE_LOG") != nullptr;
    if (log && frame % 120 == 0) MD_LOG(MD_LOG_INFO, "[GroundLive] frame %llu: %d groups of %d lists (%d nodes without a slot), tiers %d/%d/%d/%d, min margin %.2f, resident %d (%.0f MB), builds %llu, evictions %llu, waiting %d, skipped %d, assign %.3f ms",
                                       (unsigned long long)frame, ng, stats_.lists_seen, stats_.no_slot_nodes, stats_.tiers[0], stats_.tiers[1], stats_.tiers[2], stats_.tiers[3], stats_.min_margin, stats_.resident, stats_.resident_bytes / 1048576.0,
                                       (unsigned long long)stats_.builds, (unsigned long long)stats_.evictions, stats_.waiting, stats_.skipped, stats_.assign_ms);
    if (ng < 1) return;
    GroundLiveFrame f;
    f.depth = depth; f.packed = packed; f.blend_map = blend_; f.overlay = overlay_ ? overlay_ : white_; f.colour = colour_ ? colour_ : white_; f.interior = clear_; f.distant = distant_; f.slot_tex = slot;
    memcpy(f.inv_vp, inv_vp, sizeof(f.inv_vp)); memcpy(f.cam_ws, cam, sizeof(f.cam_ws)); f.far_clip = far_clip_; f.wetness = SkyClouds::OverrideWetness();
    static const bool no_decode = getenv("MD_GROUND_LIVE_NODECODE") != nullptr;   // measurement: cost split
    { static const bool ph = getenv("MD_GROUND_LIVE_PHASE") != nullptr; pass_.SetPhase(ph ? (int)(frame & 7) : -1); }   // measurement: upper bound of the 1/8 reuse
    static GroundLiveCacheView cview;   // the rings baked in earlier frames (this frame's bake runs after the pass)
    static const bool serve = !(getenv("MD_GROUND_CACHE_SERVE") && getenv("MD_GROUND_CACHE_SERVE")[0] == '0');   // 0: bake only (measurement)
    bool use_cache = false;
    if (serve && cache_.Ready() && pass_.BakeReady()) {
        for (int r = 0; r < GroundCacheRings::kRings; ++r) {
            const CacheRect& b = cache_.Rings().Baked(r); const float t = GroundCacheRings::kTexel[r];
            cview.a[r] = cache_.RingA(r); cview.b[r] = cache_.RingB(r);
            cview.rect[r][0] = b.x0 * t; cview.rect[r][1] = b.z0 * t; cview.rect[r][2] = b.x1 * t; cview.rect[r][3] = b.z1 * t;   // empty: x1 <= x0, nothing passes
        }
        static const float dmin = getenv("MD_GROUND_CACHE_D") ? (float)std::atof(getenv("MD_GROUND_CACHE_D")) : 150.f;
        static const float min_ny = getenv("MD_GROUND_CACHE_MINNY") ? (float)std::atof(getenv("MD_GROUND_CACHE_MINNY")) : 0.7f;
        cview.d_min = dmin; cview.min_ny = min_ny; use_cache = true;
    }
    pass_.SetCacheView(use_cache ? &cview : nullptr);
    const bool ran = pass_.Run(cmd, f, groups, ng);
    if (ran && npc_hook_) { SDL_GPUTexture* t3[3] = { pass_.Target(0), pass_.Target(1), pass_.Target(2) }; npc_hook_(cmd, t3, pass_.DepthStencil(), pass_.Width(), pass_.Height(), npc_user_); }
    if (ran && !no_decode) pass_.Decode(cmd);
    if (pass_.BakeReady()) BakeCache(dev, cmd, f, frame);
    if (ran && light_) {   // the original main_fs; the camera in centred Kenshi units (0.1 m, world centre 14745.6 m) is the origin of the ambient map mapping
        GroundLiveLightParams lp = *light_; lp.world_origin_xz[0] = (cam[0] - 14745.6f) * 10.f; lp.world_origin_xz[1] = (cam[2] - 14745.6f) * 10.f;
        lit_frame_ = pass_.Light(cmd, f, lp); last_frame_ = f;
    }
    static const bool zcount = getenv("MD_GROUND_LIVE_ZCOUNT") != nullptr;   // measurement: pixels of the distance target that still hold the clear value (0) after the pass
    if (ran && zcount) {   // the copy is recorded now, read three frames later (the buffer ring), so no stall
        struct Slot { SDL_GPUTransferBuffer* tb = nullptr; SDL_GPUTransferBuffer* tp = nullptr; int n = 0; uint64_t frame = 0; int skipped = 0; bool used = false; };
        static Slot ring[3]; Slot& sl = ring[frame % 3];
        if (sl.used) {
            const float* dp = (const float*)SDL_MapGPUTransferBuffer(dev, sl.tb, false); const uint16_t* pp = (const uint16_t*)SDL_MapGPUTransferBuffer(dev, sl.tp, false); int zero = 0, ground = 0, uncovered = 0;
            for (int i = 0; i < sl.n; ++i) { const bool g = (pp[4 * i] | pp[4 * i + 1] | pp[4 * i + 2] | pp[4 * i + 3]) != 0; if (dp[i] == 0.f) { ++zero; if (g) ++uncovered; } if (g) ++ground; }
            SDL_UnmapGPUTransferBuffer(dev, sl.tb); SDL_UnmapGPUTransferBuffer(dev, sl.tp);
            MD_LOG(MD_LOG_INFO, "[GroundLive] ZCOUNT frame %llu skipped_groups %d zero_dist_pixels %d ground_pixels %d uncovered_ground_pixels %d", (unsigned long long)sl.frame, sl.skipped, zero, ground, uncovered);
        }
        const int n = w * h;
        if (n != sl.n) { if (sl.tb) SDL_ReleaseGPUTransferBuffer(dev, sl.tb); if (sl.tp) SDL_ReleaseGPUTransferBuffer(dev, sl.tp); SDL_GPUTransferBufferCreateInfo di = {}; di.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; di.size = (Uint32)n * 4; sl.tb = SDL_CreateGPUTransferBuffer(dev, &di); di.size = (Uint32)n * 8; sl.tp = SDL_CreateGPUTransferBuffer(dev, &di); sl.n = n; }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd); SDL_GPUTextureRegion r = {}; r.texture = pass_.Dist(); r.w = (Uint32)w; r.h = (Uint32)h; r.d = 1; SDL_GPUTextureTransferInfo ti = { sl.tb, 0, (Uint32)w, (Uint32)h }; SDL_DownloadFromGPUTexture(cp, &r, &ti); SDL_GPUTextureRegion rp2 = r; rp2.texture = packed; SDL_GPUTextureTransferInfo tp2 = { sl.tp, 0, (Uint32)w, (Uint32)h }; SDL_DownloadFromGPUTexture(cp, &rp2, &tp2); SDL_EndGPUCopyPass(cp);
        sl.frame = frame; sl.skipped = stats_.skipped; sl.used = true;
    }
}

void GroundLiveDriver::BakeCache(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, uint64_t frame) {
    if (!cache_tried_) { cache_tried_ = true; if (!cache_.Init(dev)) MD_LOG(MD_LOG_WARNING, "[GroundLive] cache init failed: no bake"); }
    if (!cache_.Ready() || !hmap_) return;
    cache_.DumpTick(cmd, frame);
    GroundCacheRings& rings = cache_.Rings();
    if (f.wetness != cache_wetness_) { cache_wetness_ = f.wetness; rings.Invalidate(); ++stats_.cache_invalidations; }   // §3: wetness is a global input of the program; everything is baked again, progressively
    rings.Update(f.cam_ws[0], f.cam_ws[2]);
    static const bool log = getenv("MD_GROUND_LIVE_LOG") != nullptr;
    if (log && frame % 60 == 0) MD_LOG(MD_LOG_INFO, "[GroundCache] frame %llu: strips %llu texels %llu invalidations %llu waits %llu nolist %llu, baked %.2f / %.2f / %.2f Mtexel", (unsigned long long)frame,
                                      (unsigned long long)stats_.cache_strips, (unsigned long long)stats_.cache_texels, (unsigned long long)stats_.cache_invalidations, (unsigned long long)stats_.cache_waits, (unsigned long long)stats_.cache_nolist,
                                      rings.Baked(0).Area() / 1e6, rings.Baked(1).Area() / 1e6, rings.Baked(2).Area() / 1e6);
    static const int64_t budget = getenv("MD_GROUND_CACHE_BUDGET") ? std::atoll(getenv("MD_GROUND_CACHE_BUDGET")) : 65536;   // texels per frame (§5: the 1.5 ms cap is checked by the probe)
    int r = 0; CacheRect s;
    if (!rings.NextStrip(budget, &r, &s, GroundLiveCache::kMaxShort)) return;
    const int32_t nx = s.x1 - s.x0, nz = s.z1 - s.z0;
    const bool tr = nz > nx;   // the target's long axis (u) runs along z
    const int32_t nlong = tr ? nz : nx, nshort = tr ? nx : nz, ap = GroundLiveCache::kApron;
    if (nlong > GroundLiveCache::kMaxLong || nshort > GroundLiveCache::kMaxShort) return;
    const float t = GroundCacheRings::kTexel[r];
    const int w = nlong + 2 * ap, h = nshort + 2 * ap;
    // cells: the strip split at the cells of the biome-list map (Kbi1, 29491.2 m / Dim() each), so a cell's box lies in one map cell like a terrain node's does; the list is the
    // original's rule on the box between the first and last texel centres (an enclosing box gives the same weights, see the header); equal neighbours along u are merged
    static const bool leaf = !(getenv("MD_GROUND_CACHE_LEAF") && getenv("MD_GROUND_CACHE_LEAF")[0] == '0');   // split at the map's sub-cells too (one leaf mask per box)
    const double kcell = 29491.2 / std::max(kbi_.Dim(), 1) / (leaf ? std::max(kbi_.Res(), 1) : 1);
    const int32_t su = (tr ? s.z0 : s.x0) - ap, sv = (tr ? s.x0 : s.z0) - ap;   // world texel of target pixel 0 along u / v
    auto segs = [&](int32_t start, int32_t n, int32_t* a, int max) {   // [a[k], a[k+1]) runs of target pixels in one map cell
        int m = 0; a[m++] = 0; long last = (long)std::floor((start + 0.5) * t / kcell);
        for (int32_t i = 1; i < n && m < max; ++i) { const long c = (long)std::floor((start + i + 0.5) * t / kcell); if (c != last) { a[m++] = i; last = c; } }
        a[m] = n; return m;
    };
    static int32_t ua[1024], va[64]; const int nu = segs(su, w, ua, 1023), nv = segs(sv, h, va, 63);
    static GroundLiveGroup cells[512]; int nc = 0;
    for (int jv = 0; jv < nv; ++jv) {
        int last_lid = -2;
        for (int ju = 0; ju < nu && nc < 512; ++ju) {
            const int32_t u0 = ua[ju], u1 = ua[ju + 1], v0 = va[jv], v1 = va[jv + 1];
            const double cu0 = (su + u0 + 0.5) * t, cu1 = (su + u1 - 0.5) * t, cv0 = (sv + v0 + 0.5) * t, cv1 = (sv + v1 - 0.5) * t;   // texel centres, metres
            const double bx0 = tr ? cv0 : cu0, bx1 = tr ? cv1 : cu1, bz0 = tr ? cu0 : cv0, bz1 = tr ? cu1 : cv1;
            const PatchBiomes pb = kbi_.Query((float)((bx0 - 14745.6) * 10.0), (float)((bz0 - 14745.6) * 10.0), (float)((bx1 - 14745.6) * 10.0), (float)((bz1 - 14745.6) * 10.0));
            const int lid = pb.count > 0 ? ListIdOf(pb) : -1;
            if (lid < 0) { ++stats_.cache_nolist; return; }   // no list (outside the map): the strip stays unbaked, those pixels keep the program path
            if (lid == last_lid && nc > 0) { cells[nc - 1].x1 = u1; continue; }
            GroundLiveGroup& g = cells[nc]; g = GroundLiveGroup(); g.slot = 0; g.biome_count = 0;
            g.x0 = u0; g.x1 = u1; g.y0 = v0; g.y1 = v1;
            float tile = 1e9f;
            for (int b = 0; b < pb.count; ++b) {
                const BiomeInfo* bi = Find(pb.ids[b]); if (!bi || !bi->valid) return;
                for (const float* a : { bi->pack.scalesA, bi->pack.scalesB, bi->pack.scalesC }) for (int k = 0; k < 4; ++k) if (a[k] > 0.f && a[k] < tile) tile = a[k];
            }
            const float footprint = t * 4.096f * (tile < 1e8f ? tile : 1.f);   // the same tier rule as Frame, with the texel size as the pixel footprint
            int K = 0; while (K < 6 && std::ldexp(1.f, K + 2) <= footprint / 1.25f) K += 2;
            for (int b = 0; b < pb.count; ++b) {
                const BiomeInfo* bi = Find(pb.ids[b]);
                const BiomeLayerSet* set = nullptr;   // the rule of Frame: the finest resident tier at or below the wanted one, else request it and take a coarser resident one
                for (int tt = K / 2; tt >= 0 && !set; --tt) set = arrays_.Peek(bi->reg_index, frame, tt);
                if (!set) { arrays_.Request(bi->reg_index, K / 2); for (int tt = K / 2 + 1; tt <= 3 && !set; ++tt) set = arrays_.Peek(bi->reg_index, frame, tt); }
                if (!set) { ++stats_.cache_waits; return; }   // nothing resident: the strip is retried next frame
                g.biome[g.biome_count].pack = bi->pack; g.biome[g.biome_count].diffuse = set->diffuse.SDLTexture(); g.biome[g.biome_count].normal = set->normal.SDLTexture(); ++g.biome_count;
            }
            last_lid = lid; ++nc;
        }
    }
    const float o0 = (float)((s.x0 - ap) + 0.5) * t, o1 = (float)((s.z0 - ap) + 0.5) * t;
    const float bake0[4] = { o0, o1, 0.f, 0.f };
    const float bake2[4] = { tr ? 0.f : t, tr ? t : 0.f, tr ? t : 0.f, tr ? 0.f : t };
    if (!pass_.Bake(cmd, cache_.StripTargets(), w, h, f, hmap_, bake0, hmap_p_, bake2, cells, nc)) return;
    // the decode maps fragCoord / viewport to the sampled uv: the viewport is the strip textures' full size, not the baked part
    if (!pass_.DecodeTo(cmd, cache_.StripTargets(), GroundLiveCache::kMaxLong + 2 * ap, GroundLiveCache::kMaxShort + 2 * ap, cache_.StripRgb(), cache_.StripAux())) return;
    cache_.Store(cmd, r, s, tr);
    rings.MarkBaked(r, s);
    ++stats_.cache_strips; stats_.cache_texels += (uint64_t)nx * nz;
}

}   // namespace md

