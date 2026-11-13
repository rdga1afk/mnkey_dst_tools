#pragma once
// #119 spike: placed Kenshi meshes (buildings of one zone, foliage layers) drawn through PropRenderer: albedo = PropTexShared rock layer + sun/ambient, no shadows,
// one draw per instance (no instancing). Four independent slots so each class can be switched and counted alone. Off until md.set_hub_spike(slot, path).
// Mesh name "@grass" = two crossed 1 m quads (grass stand-in, #119). Input per line: mesh_path x y z scale qw qx qy qz (tab-separated, x/y/z = our absolute metres); private/md_hub_buildings.py writes it, tests/scenarios/hub_spike.lua converts the foliage stand-ins.
#include <monkey_dust/render/prop_renderer.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <cstdint>
#include <string>
#include <vector>
#include <SDL3/SDL_gpu.h>

struct HubPlacement { std::string mesh, tex, nrm; int alpha = 0; bool cut = false, dsided = false; float thr = 0.5f, tile_x = 1.f, tile_y = 1.f;   /* cut: TRANSPARENCY material (normal-map alpha cut-out at thr), dsided: DOUBLESIDED, tile: UV scale of the spec */ float x, y, z, scale, q[4]; bool terrain = false; std::string key; };   // terrain: a TERRAIN-mode mesh (Kenshi material type 2), drawn into the terrain G-buffer by DrawGbuf and shaded as ground by the resolve (#176)

// CPU-side result of reading one mesh (vertices / indices per submesh, bounding radius in metres): can be made on any thread.
struct HubPrepared { std::string key; std::vector<std::vector<PropVertex>> vs; std::vector<std::vector<uint32_t>> is; std::vector<uint8_t> two_sided; float bound_m = 10.f; };   // two_sided: per submesh, cull_hardware none in its Kenshi material

class HubSpike {
public:
    static constexpr int SLOTS = 4;
    static constexpr int MAX_BATCHES = 128;     // distinct (mesh, submesh)
    static constexpr int MAX_INSTANCES = 20480;  // part instances in one slot
    static HubSpike& Slot(int i);
    bool Load(const char* txt_path);
    static void FrameTick();   // render thread, once per frame: MD_HUB_TEX_PER_FRAME=n (default off) lets the zone worker start at most n new texture uploads per rendered frame
    static void Prepare(std::vector<HubPlacement>& pl, std::vector<HubPrepared>& out);   // thread-safe: reads the meshes and loads their textures (SDL_GPU resource creation and upload are thread-safe); fills HubPlacement::key so Step() does not build the string again
    void Begin(std::vector<HubPlacement> pl, std::vector<HubPrepared> prepared = {});   // incremental load: Begin, then Step() once per frame until it returns false
    bool Step();
    bool Loading() const { return loading_; }
    bool LoadZone(float cx_m, float cz_m, float radius_m);   // buildings from the Kenshi zone files (md::world::LoadBuildingsAt), same result as the txt

    void Shutdown();
    void SetRange(float m) { range_ = m; }   // draw distance from the camera in the xz plane, 0 = unlimited
    bool Active() const { return batch_count_ > 0; }
    void Draw(
        SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
        const float* vp16, const float* sun32, float cam_x, float cam_z, const float* frustum16, float cam_y = 1.0e30f);   // cam_y: the map is dropped when the camera height changed by > 3 m (the default skips that test); frustum16 = MdCamera::FrustumPlanes (4 side planes) or nullptr
    static void SetInstanced(bool on) { instanced_ = on; }   // one DrawIndexed per run of visible tiles of a mesh instead of one per instance (default off)
    // instanced-path fragment variants (cost decomposition): 0 full, 1 constant colour, 2 depth only (colour mask 0, no discard), 3 depth only with the texture alpha cut-out
    static void SetVariant(int v) { variant_ = v; }
    static bool Instanced() { return instanced_; }
    static void SetTerrainInGbuf(bool on) { terrain_in_gbuf_ = on; }   // set by the frame each frame: while on, Draw skips the terrain batches (DrawGbuf draws them)
    void DrawGbuf(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, float cam_x, float cam_z, const float* frustum16);
    static void SetOcclusion(bool on) { occlusion_ = on; }   // cull spheres hidden behind the depth of a frame kLatency frames ago (HzbPass), default on (owner 17.10, #119)
    static bool Occlusion() { return occlusion_; }
    static bool AnyActive() { for (int i = 0; i < SLOTS; ++i) if (Slot(i).Active()) return true; return false; }
    struct Stats { int instances = 0, draws = 0, batches = 0; int64_t triangles = 0; int occluded = 0; };
    Stats stats() const { return stats_; }
    int textured_batches() const { return textured_; }
    int no_texture_batches() const { return no_tex_; }   // batches whose placement names no texture (a part without a material spec): the rock fallback by design   // batches that got a texture bound (defect sweep: a placement without one is drawn flat)
private:
    int textured_ = 0, no_tex_ = 0;
    bool LoadPlacements(const std::vector<HubPlacement>& pl);
    std::vector<HubPlacement> pl_; std::vector<HubPrepared> prepared_; size_t next_ = 0; std::vector<std::string> done_; int inst_ = 0; bool loading_ = false;
    struct Tile { int first = 0, count = 0; float cx = 0, cy = 0, cz = 0, rad = 0; };   // run of instances of one batch in the instance buffer
    static constexpr int MAX_TILES = 64;
    struct Batch { PropRenderer r; bool terrain = false; uint32_t tris = 0; int first = 0, count = 0; float radius = 0; GpuStaticBuffer inst; Tile tiles[MAX_TILES]; int ntiles = 0; };   // radius: bounding sphere at scale 1, metres
    float range_ = 0;
    static inline bool instanced_ = false, occlusion_ = true, terrain_in_gbuf_ = false;
public:
    static inline float occ_share_ = 0.f;   // largest culled share of a slot this frame (reset by the HZB tick), decides whether the HZB is recorded every frame
private:
    static inline int variant_ = 0;
    void BuildTiles(Batch& b);
    Batch batches_[MAX_BATCHES];
    int batch_count_ = 0;
    float pos_[MAX_INSTANCES * 3], quat_[MAX_INSTANCES * 4], scale_[MAX_INSTANCES];
    Stats stats_;
};
