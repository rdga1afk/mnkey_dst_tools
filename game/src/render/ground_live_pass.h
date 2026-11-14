#pragma once
// Ground live pass (path 1, ground design A): the ORIGINAL terrain vertex + fragment programs of the player's install run per pixel on our packed terrain G-buffer and write the
// original's three G-buffer targets (buf0 albedo in YCoCg checkerboard, buf1 world normal, buf2 distance / farClip). Compiled at run time from KENSHI_DIR
// (game/src/render/ground_live_glue.hlsl wraps the original files). One draw per biome list (1..4 biomes); the caller supplies the per-biome layer arrays. Default off.
#include <monkey_dust/world/ground_pack.h>
#include <SDL3/SDL_gpu.h>
#include <string>
#include <vector>

namespace md {

struct GroundLiveBiome {
    GroundPack pack;
    SDL_GPUTexture* diffuse = nullptr;   // 2D array, layers: base, slope, cliff, grass, dirt, road
    SDL_GPUTexture* normal = nullptr;
};

struct GroundLiveGroup {                  // the pixels of one biome list
    int slot = 1;                         // value of this group in the slot target (1..31)
    int biome_count = 1;                  // 1..4, main biome first
    bool no_tint = false;                 // the colour / overlay maps of the tile are not enabled: white.dds / nothing.dds (ZoneMapOverlay::enable, FUN_140a11d70)
    bool layer_bias = false;              // B2: the layers of this far class are sampled with a positive lod bias (the old path's layer LOD at distance)
    int cls = 0;                          // NOW-0 B variants: 1 / 2 = far classes (colour map two / three mips coarser, layer lod bias); 0 = as the install program runs
    bool far = false;                     // no layer set resident yet: drawn with the original far program (distant.png) instead of being skipped
    GroundLiveBiome biome[4];
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // scissor (pixels); x1 <= x0 = the whole target
};

struct GroundLiveFrame {
    SDL_GPUTexture* depth = nullptr;      // terrain G-buffer depth (float, ndc z), sampled with Load
    SDL_GPUTexture* packed = nullptr;     // R16G16B16A16_UINT, terrain_gbuf_pack.glsl
    SDL_GPUTexture* blend_map = nullptr;  // read with >= 2 biomes
    SDL_GPUTexture* overlay = nullptr;
    SDL_GPUTexture* colour = nullptr;
    SDL_GPUTexture* colour_l3 = nullptr, *colour_l4 = nullptr;   // the colour map from its mip 3 / 4 (NOW-0 B1)
    SDL_GPUTexture* white = nullptr;      // white.dds stand-in (colour map of a tile that is not enabled)
    SDL_GPUTexture* black = nullptr;      // nothing.dds stand-in (overlay map of a tile that is not enabled)
    SDL_GPUTexture* interior = nullptr;   // "no interior" 1x1
    SDL_GPUTexture* distant = nullptr;    // data/newland/land/overlaymaps/distant.png (the far program's distantColour)
    float inv_vp[16] = {};                // gw_invVP in column-major memory order (glm layout)
    float cam_ws[3] = {};                 // camera position in metres, world corner origin
    float cam_dir[3] = { 0.f, 0.f, 1.f };    // unit view direction
    float far_clip = 40000.f,   // units (0.1 m)
    wetness = 0.f, water_height_rel = -1.0e8f;
    float biome_rect[4] = { -147456.f, -147456.f, 147456.f, 147456.f }, overlay_rect[4] = { -147456.f, -147456.f, 147456.f, 147456.f };
    SDL_GPUTexture* slot_tex = nullptr;   // R8_UINT, the biome-list slot of each pixel (second target of the terrain G-buffer pass; 0 = none)
};

// The inputs of the original main_fs that come from the weather / engine (docs/research/MAIN_LIGHT_INPUTS_2026-11-10.md).
struct GroundLiveCacheView {               // the baked rings for the fetch draw (MD_GROUND_CACHE); null textures = no fetch
    SDL_GPUTexture* a[3] = {}; SDL_GPUTexture* b[3] = {};
    float rect[3][4] = {};                // baked rectangle of each ring, metres (x0, z0, x1, z1)
    float d_min = 150.f;                  // D: nearer pixels keep the program
    float min_ny = 0.7f;                  // steeper pixels keep the program
};

struct GroundLiveLightParams {
    float sun_dir[3] = { 0.f, 1.f, 0.f }, sun_colour[4] = { 1.f, 1.f, 1.f, 1.f }, env_w = 1.f, ambient_w = 1.f;   // sunDirection, sunColour (w = strength), envColour.w, ambientParams.w
    float world_origin_xz[2] = { 0.f, 0.f }, world_extent = 294912.f;   // camera position (Kenshi units, centred on the world centre) and the extent of the ambient map: exe sets worldSize = (1/(147456 - -147456), same, 0.5 + size * camera.x, 0.5 + size * camera.z)
    SDL_GPUTexture* ambient_map = nullptr; SDL_GPUSampler* ambient_smp = nullptr;
    SDL_GPUTexture* irr_cube = nullptr; SDL_GPUSampler* irr_smp = nullptr;
    SDL_GPUTexture* spec_cube = nullptr; SDL_GPUSampler* spec_smp = nullptr;
};

// The release consumer (no translated resolve, docs/research/GROUND_BASE_PATH1_DESIGN_2026-11-12.md): the original fog quad (post/fog.hlsl atmosphere_fog_fs) over Lit(), then the
// fixed exposure of the exe (0.55 / lerp(MIN, MAX, saturate(5 sun.y)), UpdateKLight), written into the ground target of the ground pass. Values in Kenshi units (0.1 m), as the KLight UBO holds them.
struct GroundLiveFogParams {
    float sun_real[3] = { 0.f, 1.f, 0.f };          // sunDirectionReal
    float fog_near = 0.f, fog_far = 0.f;            // pFogParams.y / .z
    float skydome_radius = 1.f;                     // uSkydomeRadius
    float horizon[4] = { 0.f, 0.f, 0.f, 0.f };      // horizonClouds
    float exposure = 1.f;
};
class GroundLivePass {
public:
    bool Init(SDL_GPUDevice* dev, const std::string& kenshi_dir);   // false + log when the original files do not compile
    void Shutdown();
    bool Ready() const { return ready_; }
    bool EnsureTargets(int w, int h);                                // RGBA8, RGBA8, R32F
    SDL_GPUTexture* Albedo() const { return t_[0]; }
    SDL_GPUTexture* Normal() const { return t_[1]; }
    SDL_GPUTexture* Dist() const { return t_[2]; }
    SDL_GPUTexture* AuxB() const { return aux_; }   // Decode(): the world normal as stored (rgb, 0..1) + the distance (a, 0 = not written), RGBA16F
    SDL_GPUTexture* AlbedoRgb() const { return rgb_; }   // Decode(): decodePixel of the albedo target + gloss in alpha, RGBA16F
    bool Decode(SDL_GPUCommandBuffer* cmd);                // after Run()
    bool DecodeTo(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* const src[3], int w, int h, SDL_GPUTexture* out_rgb, SDL_GPUTexture* out_aux);   // the same decode over other targets (the cache bake)
    // Live-ground cache (MD_GROUND_CACHE): the program over a strip of ring texels into dst (RGBA8, RGBA8, R32F, w x h); one draw per cell (scissor x0..y1 in dst pixels),
    // the surface from the world heightmap (GW_BAKE in ground_live_glue.hlsl: bake0 origin, bake1 height range / extent / resolution, bake2 axes)
    bool Bake(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* const dst[3], int w, int h, const GroundLiveFrame& f, SDL_GPUTexture* hmap, const float bake0[4], const float bake1[4], const float bake2[4],
              const GroundLiveGroup* cells, int n);
    bool BakeReady() const { return bake_ok_; }
    void SetCacheView(const GroundLiveCacheView* v) { cache_view_ = v; }   // the next Run() serves the rings (null: off)
    // After Run(): the ORIGINAL main_fs (deferred.hlsl, no shadow) over the three raw targets; Lit() = rgb light (HDR, before fog / exposure), alpha 1 where the pass wrote a pixel (0 = clipped / not written).
    bool Light(SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, const GroundLiveLightParams& p);
    SDL_GPUTexture* Lit() const { return lit_; }
    bool LightReady() const { return light_.pipe != nullptr; }
    // After Light(), inside the open ground pass (colour target of format `cfmt`, depth attachment of format `dfmt`): the fog quad + exposure over the lit pixels; the others are not written.
    bool DrawFog(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, const GroundLiveFogParams& p, SDL_GPUTextureFormat cfmt, SDL_GPUTextureFormat dfmt);
    bool FogReady() const { return fog_.fs != nullptr; }
    // Clears the targets, fills the stencil from the slot texture (one draw per bit of the largest slot), then draws each group with the stencil EQUAL test.
    bool Run(SDL_GPUCommandBuffer* cmd, const GroundLiveFrame& f, const GroundLiveGroup* groups, int n);
    SDL_GPUTexture* Target(int i) const { return t_[i]; }   // the three raw G-buffer targets and their depth-stencil, for the NPC pass between Run() and Decode()
    SDL_GPUTexture* DepthStencil() const { return ds_; }
    int Width() const { return tw_; }
    int Height() const { return th_; }
    void SetPhase(int p) { phase_ = p; }   // measurement: shade one quad in eight (-1: all)
private:
    struct Variant {
        SDL_GPUShader* fs = nullptr; SDL_GPUGraphicsPipeline* pipe = nullptr;
        std::vector<unsigned char> ub; std::vector<std::pair<std::string, std::pair<unsigned, unsigned>>> mem;   // name -> (offset, array stride)
        unsigned ub_size = 0;
    };
    SDL_GPUDevice* dev_ = nullptr; SDL_GPUShader* vs_ = nullptr; Variant var_[4]; Variant far_;
    SDL_GPUSampler* nearest_ = nullptr; SDL_GPUSampler* linear_ = nullptr; SDL_GPUSampler* biased_[2] = {};
    SDL_GPUTexture* t_[3] = {}; SDL_GPUTexture* ds_ = nullptr; Variant dec_, bits_[5], light_; SDL_GPUShader* light_vs_ = nullptr; Variant light_vsv_; SDL_GPUTexture* lit_ = nullptr; SDL_GPUTexture* rgb_ = nullptr; SDL_GPUTexture* aux_ = nullptr; SDL_GPUShader* cls_fs_ = nullptr; int tw_ = 0, th_ = 0, phase_ = -1; bool ready_ = false;
    Variant bake_[4]; bool bake_ok_ = false; Variant fetch_; SDL_GPUSampler* repeat_ = nullptr; const GroundLiveCacheView* cache_view_ = nullptr;
    static void SetIn(Variant& v, const std::string& name, const float* val, int count, int idx = 0);
    void FillGroup(Variant& v, const GroundLiveFrame& f, const GroundLiveGroup& grp, int tw, int th, SDL_GPUTextureSamplerBinding* tb);
    Variant fog_; struct FogPipe { SDL_GPUTextureFormat c = SDL_GPU_TEXTUREFORMAT_INVALID, d = SDL_GPU_TEXTUREFORMAT_INVALID; SDL_GPUGraphicsPipeline* pipe = nullptr; } fog_pipes_[4]; int n_fog_pipes_ = 0;   // one pipeline per (colour, depth) format of the ground pass
};

}   // namespace md
