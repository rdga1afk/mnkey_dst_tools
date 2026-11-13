#pragma once
// Live-ground cache, hours 2-3 (docs/research/LIVE_GROUND_CACHE_DESIGN_2026-11-13.md §5): the GPU side of the rings. A strip of ring texels is baked by the original program
// (GroundLivePass::Bake, GW_BAKE glue) into strip targets with a 2-texel apron, decoded by the original decodePixel (GroundLivePass::DecodeTo), and stored into the ring textures
// A (albedo + gloss) and B (normal as stored + 1) at the toroidal address (world texel mod 2048). MD_GROUND_CACHE=1, default off; nothing reads the rings yet (hour 4: serving).
#include "ground_live_cache_rings.h"
#include <SDL3/SDL_gpu.h>
#include <cstdint>

namespace md {

class GroundLiveCache {
public:
    static constexpr int kApron = 2, kMaxLong = GroundCacheRings::kSize, kMaxShort = 128;   // strip target: (long + 2 apron) x (short + 2 apron)
    bool Init(SDL_GPUDevice* dev);   // false + log: the rings stay unbaked, every pixel takes the program path
    void Shutdown();
    bool Ready() const { return ready_; }
    GroundCacheRings& Rings() { return rings_; }
    SDL_GPUTexture* const* StripTargets() const { return strip_; }   // RGBA8, RGBA8, R32F: the Bake targets
    SDL_GPUTexture* StripRgb() const { return dec_[0]; }   // RGBA16F: DecodeTo outputs
    SDL_GPUTexture* StripAux() const { return dec_[1]; }
    // The decoded strip (world texel rect s of ring r, baked transposed when it is longer along z) into the ring textures, split at the toroidal wrap.
    void Store(SDL_GPUCommandBuffer* cmd, int r, const CacheRect& s, bool transposed);
    SDL_GPUTexture* RingA(int r) const { return ring_[r][0]; }
    // diagnostic (MD_GROUND_CACHE_DUMP=<frame>): records the download of the three A rings at that frame, writes temp_/ground_cache_ring<r>.ppm three frames later
    void DumpTick(SDL_GPUCommandBuffer* cmd, uint64_t frame);
    SDL_GPUTexture* RingB(int r) const { return ring_[r][1]; }
private:
    SDL_GPUDevice* dev_ = nullptr;
    GroundCacheRings rings_;
    SDL_GPUTexture* ring_[GroundCacheRings::kRings][2] = {};
    SDL_GPUTexture* strip_[3] = {};
    SDL_GPUTexture* dec_[2] = {};
    SDL_GPUShader *vs_ = nullptr, *fs_ = nullptr;
    SDL_GPUGraphicsPipeline* pipe_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUTransferBuffer* dump_[GroundCacheRings::kRings] = {}; uint64_t dump_frame_ = 0;
    bool ready_ = false;
};

}   // namespace md
