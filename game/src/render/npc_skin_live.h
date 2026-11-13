#pragma once
// NPC body on the install's skin program (deferred/skin.hlsl main_fs, COLOURING) written into the ground-live G-buffer.
// docs/research/NPC_INSTALL_SKIN_DESIGN_2026-11-14.md; oracle row «NPC у грі» in docs/ORACLES.md.
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <string>

namespace md::npcskin {

struct Frame {
    SDL_GPUCommandBuffer* cmd = nullptr;
    SDL_GPUTexture* target[3] = {};      // the ground-live G-buffer (kept: LOADOP_LOAD)
    SDL_GPUTexture* depth = nullptr;     // D32_FLOAT_S8_UINT of the same size, cleared here
    SDL_GPUTexture* terrain_depth = nullptr;   // the terrain G-buffer's NDC depth: a fragment behind it is discarded
    int w = 0, h = 0;
    const float* view_proj = nullptr;    // 16 floats, column-major (MdCamera::ViewProjMatrix)
    float cam[3] = {};                   // the camera in the space of the NPC positions
    float far_clip = 0.f;
    SDL_GPUBuffer* vbo = nullptr;        // stride kStride
    SDL_GPUBuffer* ibo = nullptr; SDL_GPUIndexElementSize idx_elem = SDL_GPU_INDEXELEMENTSIZE_16BIT; uint32_t index_count = 0;
    SDL_GPUBuffer* storage[5] = {};      // Transform, Visible, Faction, Bones, Skin
    uint32_t n01 = 0, n2 = 0, t2_base = 0;   // instances of the two tiers; t2_base = first index of tier 2 in Visible
};

constexpr uint32_t kStride = 84;   // SkinVertex 52 + 4 unused + tangent float4 (56) + binormal float3 (72)

bool Init(SDL_GPUDevice* dev, const std::string& kenshi_dir);   // false: the NPCs stay on the old path
bool Ready();
void Record(const Frame& f);
void Shutdown();

}  // namespace md::npcskin
