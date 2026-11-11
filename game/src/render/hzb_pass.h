#pragma once
// #119 occlusion culling input (Dagor-style, docs/research/DAGOR_ENGINE_2026-10.md section 1): the scene depth of a finished frame reduced on the GPU to a 320 x 180
// map of the farthest eye depth per 6 x 6 px block, copied to the CPU and read kLatency frames later (no stall). The culling tests a bounding sphere against it.
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_depth_texture.h>
#include <monkey_dust/render/gpu_device.h>
#include <SDL3/SDL_gpu.h>
#include <cstdint>

class HzbPass {
public:
    static constexpr int W = 320, H = 180, kRing = 4, kLatency = 3, kMaxAge = 12;
    static HzbPass& Get();
    bool Init();
    void Shutdown();
    // after the scene pass ended: reduce `depth` and queue the copy into the ring slot of this frame; vp16 = the view-projection the depth was rendered with (absolute camera)
    void Record(md::GpuCommandBufferHandle cmd, const GpuDepthTexture& depth, const float* vp16, const float* cam3, float near_m, float far_m);
    // once per rendered frame (also when nothing is recorded): the frame counter the age of a map is measured in; returns true when this frame should record (every frame while the culling pays, else a probe every 8th)
    bool Tick(float culled_share);
    struct View { const float* far_m = nullptr; float vp[16] = {}; float cam[3] = {}; bool valid = false; };
    // the newest map recorded at least kLatency frames ago (complete by then for up to 3 frames in flight) and at most kMaxAge frames ago; valid = false otherwise
    View Latest();
    bool Ready() const { return ready_; }
private:
    GpuPipeline pipe_;
    SDL_GPUSampler* samp_ = nullptr;
    SDL_GPUTexture* tex_ = nullptr;
    SDL_GPUTransferBuffer* tb_[kRing] = {};
    float vp_[kRing][16] = {};
    uint64_t frame_ = 0, cpu_frame_ = ~0ull;
    float cpu_[W * H] = {};
    uint64_t rec_[kRing] = {};      // absolute frame + 1 of the record in the slot (0 = none)
    float cam_[kRing][3] = {}; uint64_t rec_idx_[kRing] = {};
    uint64_t abs_ = 0; int cur_slot_ = 0;
    bool ready_ = false;
};
