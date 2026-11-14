#pragma once
// Ground live pass in the game (MD_GROUND_LIVE=1, default off). The biome list is chosen per terrain patch, as the original does (ZoneMap::getBiomes on the patch box, common Morton
// ancestor mask): AssignSlots gives every LOD node the slot of its list, the terrain G-buffer pass writes the slot into a second R8_UINT target (engine: ground_slot.h), and Frame runs
// GroundLivePass (stencil groups from the slot target) + the original decodePixel after the pass. Every enclosing node's list gives identical weights (private leaf-vs-patch proof,
// scripts/kbi1_leaf_vs_patch.py), so the node size of our LOD does not matter. The result is not consumed by the lighting yet (measurement path).
#include "ground_live_cache.h"
#include "ground_live_pass.h"
#include <monkey_dust/render/biome_layer_arrays.h>
#include <monkey_dust/world/kbi1.h>
#include <monkey_dust/world/terrain_lod_tree.h>
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace md {

class GroundLiveDriver {
public:
    static GroundLiveDriver& Get();
    static bool Wanted();   // MD_GROUND_LIVE=1
    // NPC G-buffer pass: called after the ground Run() and before Decode() / Light() with the raw targets (set per frame by the caller, cleared by it).
    using NpcHook = void (*)(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* const target[3], SDL_GPUTexture* depth, int w, int h, void* user);
    float FarClip() const { return far_clip_; }
    void SetNpcHook(NpcHook h, void* user) { npc_hook_ = h; npc_user_ = user; }
    void Shutdown() { arrays_.Stop(); }   // before GpuDevice::Shutdown(): joins the layer-array worker, whose build in flight uses the GPU device (ASan double-free at exit, PR #164 6856689259)
    static bool OrigLight();   // MD_GROUND_LIVE_ORIGLIGHT=1 (with MD_GROUND_LIVE_LIGHT): the ground is lit by the original main_fs (GroundLivePass::Light) instead of TS_KenshiLight
    struct Stats { int skipped = 0, waiting = 0, groups = 0, lists_seen = 0, no_slot_nodes = 0, over_limit = 0, lists_near = 0, no_slot_near = 0, tiers[4] = {}, resident = 0; uint64_t resident_bytes = 0, builds = 0, evictions = 0; float min_margin = 0.f; double assign_ms = 0; uint64_t cache_strips = 0, cache_texels = 0, cache_invalidations = 0, cache_waits = 0, cache_nolist = 0; };
    // Before the nodes are uploaded: sets nodes[i].list_slot (0 = none) from the node's biome list; the nearest lists get slots 1..max_groups.
    void AssignSlots(TerrainLodNode* nodes, int n, const float cam[3], const TerrainLodNode* pre = nullptr, int n_pre = 0);   // pre = the nodes before the horizon cull: their nearest distance per list sets the array tier
    // The slot of the smallest node of the last AssignSlots that contains (x, z), metres (granite absolute); a point outside every visible node takes its own list's slot, else the nearest visible node's; 0 = none. TERRAIN-mode features write it (md::GroundSlotAt).
    int SlotAt(float x, float z);
    // After the G-buffer pass (no pass open): depth = G-buffer depth, packed = R16G16B16A16_UINT, slot = the R8_UINT second target. inv_vp column-major; cam in metres (granite absolute).
    void Frame(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, SDL_GPUTexture* packed, SDL_GPUTexture* slot, int w, int h, const float inv_vp[16], const float cam[3], uint64_t frame);
    void SetLight(const GroundLiveLightParams* p) { light_ = p; }   // null = no original light pass this frame
    // Release consumer (no translated resolve): inside the open ground pass, after Frame() of the same frame; false when nothing was lit this frame.
    bool DrawFog(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, const GroundLiveFogParams& p, SDL_GPUTextureFormat cfmt, SDL_GPUTextureFormat dfmt) { return lit_frame_ && pass_.DrawFog(rp, cmd, last_frame_, p, cfmt, dfmt); }
    void SetMaps(SDL_GPUTexture* colour, SDL_GPUTexture* overlay) { colour_ = colour; overlay_ = overlay; }   // the world colour map and the paint mask of the terrain (the original's colourMap / overlayMap); white stands in while unset
    // the world heightmap (R16_UNORM, res x res cells over extent metres) for the cache bake (MD_GROUND_CACHE)
    void SetHeightmap(SDL_GPUTexture* tex, float hmin, float hmax, float extent, int res) { hmap_ = tex; hmap_p_[0] = hmin; hmap_p_[1] = hmax; hmap_p_[2] = extent; hmap_p_[3] = (float)res; }
    GroundLiveCache& Cache() { return cache_; }
    const Stats& GetStats() const { return stats_; }
    int PendingArrays() const { return arrays_.Pending(); }   // layer-array builds queued or in flight
    GroundLivePass& Pass() { return pass_; }
private:
    bool InitData();
    bool InitGpu(SDL_GPUDevice* dev);
    struct BiomeInfo { GroundPack pack; int reg_index = -1; bool valid = false; };
    const BiomeInfo* Find(int raw) const;
    struct PatchList { PatchBiomes pb; float dmin = 1e30f, dmin_pre = 1e30f; int slot = 0; bool untinted = false; int cls = 0; int alt[6] = { -1, -1, -1, -1, -1, -1 }; };   // untinted: the same biome list drawn with white.dds / nothing.dds (an overlay tile that is not enabled); alt: its index
    int VariantOf(int lid, bool untinted, int cls);   // the list twin drawn untinted (white / nothing maps) and / or with the far colour map and layer bias of class cls
    void UpdateTiles(const float cam[3]);
    bool tile_on_[64] = {};   // ZoneMapOverlay enabled state of the 8 x 8 colour tiles (FUN_140a12c70)
    int ListIdOf(const PatchBiomes& pb);
    bool data_tried_ = false, data_ok_ = false, gpu_tried_ = false, gpu_ok_ = false;
    std::string kenshi_dir_;
    GroundLivePass pass_; Kbi1 kbi_; BiomeLayerArrays arrays_;
    std::vector<std::pair<int, BiomeInfo>> biome_of_;
    std::unordered_map<uint64_t, int> node_list_;   // node (origin, level) -> index in lists_ (-1: empty list)
    std::vector<PatchList> lists_;                  // distinct patch lists seen so far
    const GroundLiveLightParams* light_ = nullptr;
    NpcHook npc_hook_ = nullptr; void* npc_user_ = nullptr;
    GroundLiveFrame last_frame_; bool lit_frame_ = false;   // the inputs of the last Frame() that ran the light pass (DrawFog)
    SDL_GPUTexture *colour_ = nullptr, *overlay_ = nullptr, *blend_ = nullptr, *white_ = nullptr, *colour_l3_ = nullptr, *colour_l4_ = nullptr, *clear_ = nullptr, *distant_ = nullptr;
    float far_clip_ = 40000.f; int max_groups_ = 31;
    int slot_list_[32] = {}; int n_slots_ = 0;      // slot -> index in lists_ (this frame)
    float slot_dmin_[32] = {};
    struct NodeSlot { float x0, z0, size; int slot; };
    NodeSlot node_slot_[8192]; int n_node_slot_ = 0;   // the nodes of the last AssignSlots (SlotAt)
    Stats stats_; uint64_t frame_counter_ = 0;
    // live-ground cache (MD_GROUND_CACHE): bake strips of the rings after the frame's pass, within a texel budget per frame
    void BakeCache(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, uint64_t frame);
    GroundLiveCache cache_; bool cache_tried_ = false; float cache_wetness_ = -1.f;
    SDL_GPUTexture* hmap_ = nullptr; float hmap_p_[4] = {};
};

}   // namespace md
