#pragma once
// Grass blades on the install's grass program (deferred/foliage.hlsl grass_vs / grass_fs) written into the ground-live G-buffer.
// Placement: md::world::ScatterGrassPages (docs/research/GRASS_EXE_2026-11-15.md). Switch: MD_GRASS=1.
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/world/foliage_scatter.h>
#include <cstdint>
#include <string>
#include <vector>

namespace md::grasspass {

struct Vertex { float x, y, z; uint8_t rgba[4]; float u, v; };   // metres in the renderer space; v = 0 on the top edge (the program sways those)

struct Run { int grass = 0; uint32_t first_index = 0, index_count = 0; };

struct Mesh {   // built on a worker, uploaded by SetMesh on the render thread
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    std::vector<Run> runs;       // one per GRASS record, in `sprites` order
    std::vector<std::string> sprites;   // install-relative file of each run's GRASS record
    size_t blades = 0;
};

// Two crossed quads (or one when the record says so) per blade, sorted by record. Blade x, z, y are Kenshi units (origin shifted by -147456).
void Build(const std::vector<md::world::Blade>& blades, const md::world::Foliage& f, Mesh& out);

struct Frame {
    SDL_GPUCommandBuffer* cmd = nullptr;
    SDL_GPUTexture* target[3] = {};
    SDL_GPUTexture* depth = nullptr;            // D32_FLOAT_S8_UINT, cleared here
    SDL_GPUTexture* terrain_depth = nullptr;    // a fragment behind the terrain G-buffer depth is discarded
    int w = 0, h = 0;
    const float* view_proj = nullptr;           // 16 floats, column-major
    float cam[3] = {};
    float far_clip = 0.f;
    float dt = 0.f;                             // frame time, s: advances the wind phase (RenderFrame's dt, fixed in scenarios)
    bool clear = false;                         // scratch targets (the every-frame overlay): cleared here instead of loaded
};

bool Init(SDL_GPUDevice* dev, const std::string& kenshi_dir);
bool Ready();
void SetMesh(Mesh&& m);   // render thread
void Record(const Frame& f);
size_t Blades();
void Shutdown();

}  // namespace md::grasspass
