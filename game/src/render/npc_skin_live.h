#pragma once
// NPC body on the install's skin program (deferred/skin.hlsl main_fs, COLOURING) written into the ground-live G-buffer.
// docs/research/NPC_INSTALL_SKIN_DESIGN_2026-11-14.md; oracle row «NPC у грі» in docs/ORACLES.md.
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <string>

namespace md::npcskin {

constexpr int kMaxHeadGroups = 8;
constexpr int kMaxHairGroups = 32;
constexpr int kHairRegionBase = 4 + 2 * kMaxHeadGroups;   // first visible-index region of the hair / beard groups (one region per group, tiers 0 and 1)

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
    // Second group: the female body (own skeleton, own textures); its two tiers start at fem_base01 / fem_base2 in Visible.
    SDL_GPUBuffer* fem_vbo = nullptr; SDL_GPUBuffer* fem_ibo = nullptr; SDL_GPUIndexElementSize fem_idx_elem = SDL_GPU_INDEXELEMENTSIZE_16BIT; uint32_t fem_index_count = 0;
    uint32_t fem_n01 = 0, fem_n2 = 0, fem_base01 = 0, fem_base2 = 0;
    // Head groups (id 2 + i): the body of the group's sex with its own head textures; tier t of group i starts at xg_base[i][t] in Visible.
    uint32_t xg_n[kMaxHeadGroups][2] = {}, xg_base[kMaxHeadGroups][2] = {}; uint32_t xg_total = 0;
    // Hair / beard groups: group h starts at hair_base[h] in Visible.
    uint32_t hair_n[kMaxHairGroups] = {}, hair_base[kMaxHairGroups] = {}; uint32_t hair_total = 0;
};

constexpr uint32_t kStride = 84;   // SkinVertex 52 + 4 unused + tangent float4 (56) + binormal float3 (72)

bool Init(SDL_GPUDevice* dev, const std::string& kenshi_dir);   // false: the NPCs stay on the old path
bool Ready();
// A head texture set (units s5..s7 of the Character material) for one sex; returns the group id (2..) or -1 (no room / file missing). Same files give the same id.
int RegisterHead(bool female, const std::string& diffuse, const std::string& normal, const std::string& mask);
bool GroupFemale(int group);
int HeadGroupCount();
// A hair / beard attachment (install `Hair` material, character.hlsl hair_fs): mesh and skeleton are install paths, `texture` the DDS; colour is the `color` uniform.
struct HairSpec {
    std::string mesh, skeleton, texture;
    float color[3] = {}, diffuse[4] = {}, alpha[4] = {}, threshold = 0.5f, specular = 1.f;
};
int RegisterHair(const HairSpec& spec);   // group id 0.. or -1 (no room); equal specs give the same id
int HairGroupCount();
void Record(const Frame& f);
void Shutdown();

}  // namespace md::npcskin
