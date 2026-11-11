#pragma once
// #119 step 2: building parts of the Kenshi zones around the camera, streamed. One HubSpike per zone; the zone files and the merged game data are read on a
// worker thread (CPU only), meshes and GPU buffers are made on the main thread one mesh per frame (Tick, before any render pass). A zone is wanted while it
// touches the square of half-width range + 100 m around the camera; an unwanted zone is dropped after UNLOAD_S seconds (Okran KEEP_CAMERA, 4 s).
// Off until md.set_town_buildings(true).
#include "hub_spike.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class TownBuildings {
public:
    static constexpr float ZONE_M = 460.8f;
    static constexpr float UNLOAD_S = 4.0f;
    static TownBuildings& Get();
    void SetEnabled(bool on, float range_m);
    bool Enabled() const { return enabled_; }
    void Tick(float cam_x, float cam_z);   // main thread, before the render passes
    void Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, const float* sun32, float cam_x, float cam_z, const float* frustum16);
    void Shutdown();
    struct Stats { int zones = 0, loading = 0, requested = 0, instances = 0, draws = 0, batches = 0; int64_t triangles = 0; int textured_batches = 0, no_texture_batches = 0; int loaded_total = 0, unloaded_total = 0; double max_step_ms = 0; };
    Stats stats() const { return stats_; }
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
    Stats stats_;
};
