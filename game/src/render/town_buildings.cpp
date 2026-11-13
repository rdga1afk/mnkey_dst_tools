#include "town_buildings.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/zone_buildings.h>
#include <monkey_dust/render/prop_renderer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

bool g_town_building_cutout = true;   // D-009 2c: TRANSPARENCY specs cut out by the normal map alpha (off: solid, for before / after frames)
bool g_town_building_textures = true;   // D-009: bind the part's MATERIAL_SPEC diffuse instead of the shared rock texture (J/frame within noise at the cap, 02.11)

TownBuildings& TownBuildings::Get() { static TownBuildings t; return t; }

void TownBuildings::WorkerMain() {
    // merged game data: read once (≈ 0.5 s), then one zone per request
    md::fcs::Install inst(md::KenshiDir());
    md::fcs::LoadOrder order; md::fcs::Error e;
    md::fcs::GameDataContainer data;
    data.replace_reference_lists = true;
    bool ok = inst.LoadOrderFor(order, e) && data.Load(order, e);
    if (!ok) fprintf(stderr, "[TownBuildings] game data failed: %s\n", e.Message().c_str());
    for (;;) {
        std::pair<int, int> rq;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !requests_.empty(); });
            if (stop_) return;
            rq = requests_.front(); requests_.erase(requests_.begin());
        }
        Result r{ rq.first, rq.second, {} };
        std::vector<md::world::PlacedPart> parts; std::string err;
        if (ok && md::world::LoadZoneBuildings(md::KenshiDir(), data, rq.first, rq.second, parts, err)) {
            r.parts.reserve(parts.size());
            for (const md::world::PlacedPart& p : parts) { HubPlacement h; h.mesh = p.mesh; h.x = p.x; h.y = p.y; h.z = p.z; h.scale = p.scale; if (g_town_building_textures) { h.tex = p.tex; h.nrm = p.nrm; h.cut = g_town_building_cutout && !p.tex.empty() && !p.nrm.empty() && (p.material_type == 1 || p.material_type == 2); h.dsided = p.material_type == 2; h.thr = float(p.alpha_threshold) / 255.f; h.tile_x = p.tile[0]; h.tile_y = p.tile[1]; } for (int k = 0; k < 4; ++k) h.q[k] = p.quat[k]; r.parts.push_back(std::move(h)); }
            HubSpike::Prepare(r.parts, r.prepared);
        } else if (!err.empty()) fprintf(stderr, "[TownBuildings] zone %d.%d: %s\n", rq.first, rq.second, err.c_str());
        std::lock_guard<std::mutex> lk(mu_);
        results_.push_back(std::move(r));
    }
}

void TownBuildings::SetEnabled(bool on, float range_m) {
    range_ = range_m;
    if (on == enabled_) return;
    if (on) {
        {   // warm-up on the toggle, not in the first streaming frame: the shared staging buffer, the white texture and the prop buffers' first allocation
            PropVertex v[3] = { { 0, 0, 0, 0, 1, 0, 0, 0, 0 }, { 1, 0, 0, 0, 1, 0, 1, 0, 0 }, { 0, 0, 1, 0, 1, 0, 0, 1, 0 } };
            const uint32_t ix[3] = { 0, 1, 2 };
            GpuStaticBuffer::BeginUploadBatch();
            PropRenderer warm; warm.InitFromArrays(v, 3, ix, 3);
            GpuStaticBuffer::EndUploadBatch();
            warm.Shutdown();
        }
        stop_ = false;
        worker_ = std::thread([this] { WorkerMain(); });
        enabled_ = true;
    } else Shutdown();
}

void TownBuildings::Shutdown() {
    if (worker_.joinable()) { { std::lock_guard<std::mutex> lk(mu_); stop_ = true; } cv_.notify_all(); worker_.join(); }
    for (auto& z : zones_) if (z->hs) z->hs->Shutdown();
    zones_.clear(); requests_.clear(); results_.clear();
    enabled_ = false; stats_ = Stats{};
}

void TownBuildings::Tick(float cam_x, float cam_z) {
    if (!enabled_) return;
    HubSpike::FrameTick();
    const auto t0 = std::chrono::steady_clock::now();
    const double now_s = std::chrono::duration<double>(t0.time_since_epoch()).count();
    const float R = range_ + 100.f;
    const int x0 = std::max(0, (int)std::floor((cam_x - R) / ZONE_M)), x1 = std::min(63, (int)std::floor((cam_x + R) / ZONE_M));
    const int z0 = std::max(0, (int)std::floor((cam_z - R) / ZONE_M)), z1 = std::min(63, (int)std::floor((cam_z + R) / ZONE_M));
    auto find = [&](int zx, int zz) -> Zone* { for (auto& z : zones_) if (z->zx == zx && z->zz == zz) return z.get(); return nullptr; };
    for (int zx = x0; zx <= x1; ++zx) for (int zz = z0; zz <= z1; ++zz) {
        Zone* z = find(zx, zz);
        if (!z) {
            zones_.push_back(std::make_unique<Zone>()); z = zones_.back().get(); z->zx = zx; z->zz = zz;
            { std::lock_guard<std::mutex> lk(mu_); requests_.emplace_back(zx, zz); }
            cv_.notify_one();
        }
        z->last_wanted_s = now_s;
    }
    {   // results of the worker
        std::lock_guard<std::mutex> lk(mu_);
        for (Result& r : results_) if (Zone* z = find(r.zx, r.zz)) { z->parts = std::move(r.parts); z->prepared = std::move(r.prepared); z->state = Zone::Ready; }
        results_.clear();
    }
    // one zone loads at a time; meshes are made until BUDGET_MS is used (at least one), all GPU uploads of the frame in one batch
    {
        const double BUDGET_MS = 3.0;
        GpuStaticBuffer::BeginUploadBatch();
        for (;;) {
            Zone* cur = nullptr;
            for (auto& z : zones_) if (z->state == Zone::Loading) { cur = z.get(); break; }
            if (!cur)
                for (auto& z : zones_) if (z->state == Zone::Ready) {
                    z->hs = std::make_unique<HubSpike>(); z->hs->SetRange(range_);
                    if (z->parts.empty()) { z->state = Zone::Live; continue; }
                    z->hs->Begin(std::move(z->parts), std::move(z->prepared)); z->state = Zone::Loading; cur = z.get(); break;
                }
            if (!cur) break;
            if (!cur->hs->Step()) { cur->state = Zone::Live; cur->parts.clear(); cur->parts.shrink_to_fit(); ++stats_.loaded_total; }
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= BUDGET_MS) break;
        }
        GpuStaticBuffer::EndUploadBatch();
    }
    for (size_t i = 0; i < zones_.size();) {   // unload zones not wanted for UNLOAD_S
        Zone& z = *zones_[i];
        if (now_s - z.last_wanted_s > UNLOAD_S && z.state != Zone::Requested && z.state != Zone::Loading) {
            if (z.hs) z.hs->Shutdown();
            zones_.erase(zones_.begin() + i); ++stats_.unloaded_total;
        } else ++i;
    }
    for (auto& z : zones_) if (z->hs) z->hs->SetRange(range_);
    stats_.zones = (int)zones_.size();
    stats_.loading = 0; stats_.requested = 0;
    for (auto& z : zones_) { if (z->state == Zone::Loading || z->state == Zone::Ready) ++stats_.loading; if (z->state == Zone::Requested) ++stats_.requested; }
    stats_.max_step_ms = std::max(stats_.max_step_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void TownBuildings::Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, const float* sun32, float cam_x, float cam_z, const float* frustum16) {
    if (!enabled_) return;
    stats_.instances = stats_.draws = stats_.batches = stats_.textured_batches = stats_.no_texture_batches = 0; stats_.triangles = 0;
    for (auto& z : zones_) if (z->hs && z->hs->Active()) {
        z->hs->Draw(rp, cmd, vp16, sun32, cam_x, cam_z, frustum16);
        const HubSpike::Stats s = z->hs->stats();
        stats_.instances += s.instances; stats_.draws += s.draws; stats_.batches += s.batches; stats_.triangles += s.triangles; stats_.textured_batches += z->hs->textured_batches(); stats_.no_texture_batches += z->hs->no_texture_batches();
    }
}
