#pragma once
#include <cstdlib>
#include <atomic>
#include "grass_pass.h"
#include <memory>
// #120 step 2: MEDIUM foliage (plants, rocks; Kenshi range 100 m x setting) of the Kenshi zones around the camera, streamed like TownBuildings (same structure; one HubSpike per zone).
// The scatter of a zone (engine/world/foliage_scatter) and the merged game data are made on a worker thread (CPU only), meshes and GPU buffers are made on the main thread one mesh per frame (Tick, before any render pass). A zone is wanted while it
// touches the square of half-width range + 100 m around the camera; an unwanted zone is dropped after UNLOAD_S seconds (Okran KEEP_CAMERA, 4 s).
// Off until md.set_town_foliage(true).
#include "hub_spike.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class TownFoliage {
public:
    static constexpr float ZONE_M = 460.8f;
    static constexpr float UNLOAD_S = 4.0f;
    static TownFoliage& Get();
    void SetEnabled(bool on, float range_m);
    bool Enabled() const { return enabled_; }
    // CLOSE grass blades (Kenshi range 50 m): a circle round the camera, regenerated on the worker when the camera has moved REGEN_M; the new set loads next to the shown one and swaps in.
    void SetGrass(bool on, float range_m, float density);
    void SetGrassExe(bool on) { grass_exe_ = on; grass_shown_ = false; }   // the exe's pager + install program (MD_GRASS=1 sets it at start; md.set_grass)
    void Tick(float cam_x, float cam_z);   // main thread, before the render passes
    void Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, const float* sun32, float cam_x, float cam_z, const float* frustum16);
    void DrawGbuf(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, float cam_x, float cam_z, const float* frustum16);   // TERRAIN-mode rocks into the terrain G-buffer (#176 path)
    void Shutdown();
    static constexpr float REGEN_M = 12.f;
    static constexpr size_t MAX_BLADES = 20000;
    static constexpr size_t MAX_BLADES_EXE = 400000;   // headroom over the exe pager count (hub, range 1: ~9200)
    struct Stats { int zones = 0, loading = 0, requested = 0, instances = 0, draws = 0, batches = 0; int64_t triangles = 0; int loaded_total = 0, unloaded_total = 0; int occluded = 0; int grass_blades = 0; double grass_gen_ms = 0; double max_step_ms = 0; };
    Stats stats() const { return stats_; }
    bool AnyLive() const { for (auto& z : zones_) if (z->hs && z->hs->Active()) return true; return false; }   // a zone with drawn batches: the depth map is worth recording
private:
    struct Zone { int zx = 0, zz = 0; enum State { Requested, Ready, Loading, Live } state = Requested; double last_wanted_s = 0; std::vector<HubPlacement> parts; std::vector<HubPrepared> prepared; std::unique_ptr<HubSpike> hs; };
    void WorkerMain();
    bool enabled_ = false; float range_ = 300.f;
    std::vector<std::unique_ptr<Zone>> zones_;
    // worker
    std::thread worker_; std::mutex mu_; std::condition_variable cv_; bool stop_ = false;
    std::vector<std::pair<int, int>> requests_;
    struct Result { int zx, zz; std::vector<HubPlacement> parts; std::vector<HubPrepared> prepared; };
    std::vector<Result> results_;
    std::atomic<bool> grass_exe_{ std::getenv("MD_GRASS") != nullptr && std::getenv("MD_GRASS")[0] == '1' };
    bool grass_on_ = false; float grass_range_ = 50.f, grass_density_ = 1.f;
    bool grass_mesh_stale_ = false;   // the install-program grass pass holds a mesh from this object; cleared when the grass is switched off
    float grass_cx_ = 0, grass_cz_ = 0; bool grass_shown_ = false;     // centre (m) of the shown circle
    bool grass_req_pending_ = false; float grass_req_x_ = 0, grass_req_z_ = 0; bool grass_req_ = false;   // request for the worker (guarded by mu_)
    struct GrassResult { float cx, cz; std::vector<HubPlacement> parts; std::vector<HubPrepared> prepared; double ms; std::shared_ptr<md::grasspass::Mesh> exe; };   // exe: MD_GRASS, the blades of the exe's pager for the install grass program
    std::vector<GrassResult> grass_results_;
    std::unique_ptr<HubSpike> grass_shown_hs_, grass_next_hs_; float grass_next_cx_ = 0, grass_next_cz_ = 0; bool grass_loading_ = false;
    Stats stats_;
};
