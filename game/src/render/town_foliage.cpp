#include "town_foliage.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/foliage_scatter.h>
#include <monkey_dust/render/prop_renderer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

TownFoliage& TownFoliage::Get() { static TownFoliage t; return t; }

void TownFoliage::WorkerMain() {
    // merged game data (later file wins a reference value: the rule of the exe, not the buildings' replace rule), the foliage tables and the towns: read once, then one zone per request
    md::fcs::Install inst(md::KenshiDir());
    md::fcs::LoadOrder order; md::fcs::Error e;
    md::fcs::GameDataContainer data;
    bool ok = inst.LoadOrderFor(order, e) && data.Load(order, e);
    if (!ok) fprintf(stderr, "[TownFoliage] game data failed: %s\n", e.Message().c_str());
    md::world::Foliage foliage; std::vector<md::world::Town> towns; std::string terr;
    if (ok) { foliage = md::world::LoadFoliage(data); if (!md::world::LoadTowns(md::KenshiDir(), data, towns, terr)) { fprintf(stderr, "[TownFoliage] towns: %s\n", terr.c_str()); ok = false; } }
    for (;;) {
        std::pair<int, int> rq;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !requests_.empty() || grass_req_; });
            if (stop_) return;
            if (requests_.empty()) {   // the grass circle: not before the zones
                const float gx = grass_req_x_, gz = grass_req_z_, range = grass_range_, dens = grass_density_; grass_req_ = false;
                lk.unlock();
                GrassResult gr{ gx, gz, {}, {}, 0 };
                if (ok) {
                    const auto g0 = std::chrono::steady_clock::now();
                    std::vector<md::world::Blade> blades; std::string gerr;
                    if (md::world::ScatterGrass(md::KenshiDir(), foliage, gx * 10.f - 147456.f, gz * 10.f - 147456.f, range * 10.f, dens, (uint32_t)(int)(gx * 0.5f) * 2654435761u ^ (uint32_t)(int)(gz * 0.5f), MAX_BLADES, blades, gerr)) {
                        gr.parts.reserve(blades.size());
                        for (const md::world::Blade& b : blades) {
                            const md::world::Grass& g = foliage.grass[b.grass];
                            HubPlacement h; h.mesh = "@grass"; h.tex = g.sprite; for (char& ch : h.tex) if (ch == '\\') ch = '/';
                            h.alpha = 1; h.x = (float)(((double)b.x + 147456.0) / 10.0); h.y = b.y / 10.0f; h.z = (float)(((double)b.z + 147456.0) / 10.0); h.scale = b.size;
                            const float ya = b.yaw * 0.01745329252f; h.q[0] = std::cos(ya * 0.5f); h.q[1] = 0.f; h.q[2] = std::sin(ya * 0.5f); h.q[3] = 0.f;
                            gr.parts.push_back(std::move(h));
                        }
                        HubSpike::Prepare(gr.parts, gr.prepared);
                    } else if (!gerr.empty()) fprintf(stderr, "[TownFoliage] grass: %s\n", gerr.c_str());
                    gr.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count();
                }
                lk.lock(); grass_results_.push_back(std::move(gr));
                continue;
            }
            rq = requests_.front(); requests_.erase(requests_.begin());
        }
        Result r{ rq.first, rq.second, {} };
        std::vector<md::world::Placed> placed; std::string err;
        if (ok && md::world::ScatterZone(md::KenshiDir(), foliage, rq.first, rq.second, 1, towns, placed, err)) {
            r.parts.reserve(placed.size() * 2);
            auto fix = [](std::string s) { for (char& ch : s) if (ch == '\\') ch = '/'; return s; };
            for (const md::world::Placed& p : placed) {
                const md::world::FoliageMesh& m = foliage.meshes[p.mesh];
                const float ya = p.yaw * 0.01745329252f;
                for (int leaves = 0; leaves < 2; ++leaves) {
                    if (leaves && m.leaves_mesh.empty()) break;
                    HubPlacement h; h.mesh = fix(leaves ? m.leaves_mesh : m.mesh); h.tex = fix(leaves ? m.leaves_texture : m.texture); h.alpha = leaves || m.material_type == 4;
                    if (m.material_type == 2 && !leaves) { h.terrain = true; h.tex.clear(); }   // TERRAIN mode (MapFeatureMode 2): shaded with the biome ground layers by the resolve, no textures of its own
                    h.x = (float)(((double)p.x + 147456.0) / 10.0); h.y = p.y / 10.0f; h.z = (float)(((double)p.z + 147456.0) / 10.0); h.scale = p.scale;
                    h.q[0] = std::cos(ya * 0.5f); h.q[1] = 0.f; h.q[2] = std::sin(ya * 0.5f); h.q[3] = 0.f;
                    r.parts.push_back(std::move(h));
                }
            }
            HubSpike::Prepare(r.parts, r.prepared);
        } else if (!err.empty()) fprintf(stderr, "[TownFoliage] zone %d.%d: %s\n", rq.first, rq.second, err.c_str());
        std::lock_guard<std::mutex> lk(mu_);
        results_.push_back(std::move(r));
    }
}

void TownFoliage::SetGrass(bool on, float range_m, float density) {
    std::lock_guard<std::mutex> lk(mu_);
    grass_on_ = on; grass_range_ = range_m; grass_density_ = density;
    if (!on) { grass_req_ = false; grass_shown_ = false; grass_loading_ = false; if (grass_shown_hs_) { grass_shown_hs_->Shutdown(); grass_shown_hs_.reset(); } if (grass_next_hs_) { grass_next_hs_->Shutdown(); grass_next_hs_.reset(); } stats_.grass_blades = 0; }
    else grass_shown_ = false;
}

void TownFoliage::SetEnabled(bool on, float range_m) {
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

void TownFoliage::Shutdown() {
    if (worker_.joinable()) { { std::lock_guard<std::mutex> lk(mu_); stop_ = true; } cv_.notify_all(); worker_.join(); }
    for (auto& z : zones_) if (z->hs) z->hs->Shutdown();
    zones_.clear(); requests_.clear(); results_.clear(); grass_results_.clear(); grass_req_ = grass_req_pending_ = grass_shown_ = grass_loading_ = false;
    if (grass_shown_hs_) { grass_shown_hs_->Shutdown(); grass_shown_hs_.reset(); } if (grass_next_hs_) { grass_next_hs_->Shutdown(); grass_next_hs_.reset(); }
    enabled_ = false; stats_ = Stats{};
}

void TownFoliage::Tick(float cam_x, float cam_z) {
    if (!enabled_) return;
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
    if (grass_on_) {   // the grass circle: request a new one when the camera has left the shown one, load the result next to it, swap when it is done
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!grass_loading_ && !grass_results_.empty()) {
                GrassResult& r = grass_results_.back();
                grass_next_hs_ = std::make_unique<HubSpike>(); grass_next_hs_->SetRange(grass_range_);
                grass_next_cx_ = r.cx; grass_next_cz_ = r.cz; stats_.grass_gen_ms = r.ms;
                grass_next_hs_->Begin(std::move(r.parts), std::move(r.prepared)); grass_loading_ = true;
                grass_results_.clear(); grass_req_pending_ = false;
            } else if (grass_loading_) grass_results_.clear();
            const float dx = cam_x - grass_cx_, dz = cam_z - grass_cz_;
            if (!grass_req_pending_ && !grass_loading_ && (!grass_shown_ || dx * dx + dz * dz > REGEN_M * REGEN_M)) {
                grass_req_x_ = cam_x; grass_req_z_ = cam_z; grass_req_ = true; grass_req_pending_ = true; cv_.notify_one();
            }
        }
        if (grass_loading_) {
            GpuStaticBuffer::BeginUploadBatch();
            if (!grass_next_hs_->Step()) {
                grass_loading_ = false;
                if (grass_shown_hs_) grass_shown_hs_->Shutdown();
                grass_shown_hs_ = std::move(grass_next_hs_); grass_cx_ = grass_next_cx_; grass_cz_ = grass_next_cz_; grass_shown_ = true;
            }
            GpuStaticBuffer::EndUploadBatch();
        }
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

void TownFoliage::Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, const float* sun32, float cam_x, float cam_z, const float* frustum16) {
    if (!enabled_) return;
    stats_.instances = stats_.draws = stats_.batches = 0; stats_.triangles = 0;
    if (grass_shown_hs_ && grass_shown_hs_->Active()) {
        grass_shown_hs_->Draw(rp, cmd, vp16, sun32, cam_x, cam_z, frustum16);
        const HubSpike::Stats s = grass_shown_hs_->stats();
        stats_.instances += s.instances; stats_.draws += s.draws; stats_.triangles += s.triangles; stats_.grass_blades = s.instances;
    }
    for (auto& z : zones_) if (z->hs && z->hs->Active()) {
        z->hs->Draw(rp, cmd, vp16, sun32, cam_x, cam_z, frustum16);
        const HubSpike::Stats s = z->hs->stats();
        stats_.instances += s.instances; stats_.draws += s.draws; stats_.batches += s.batches; stats_.triangles += s.triangles;
    }
}

void TownFoliage::DrawGbuf(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, float cam_x, float cam_z, const float* frustum16) {
    if (!enabled_) return;
    for (auto& z : zones_) if (z->hs && z->hs->Active()) z->hs->DrawGbuf(rp, cmd, vp16, cam_x, cam_z, frustum16);
}
