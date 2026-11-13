#pragma once
// Character editor body on the install programs (see editor_char_body_live.cpp). Off unless MD_CHAR_LIVE=1.
#include <SDL3/SDL_gpu.h>

namespace md::charlive {
constexpr uint32_t kVertexStride = 76;   // the preview's vertex: pos, normal, uv, joints (u8 x4), weights, tangent, binormal

struct HairDraw {                 // hair / beard: deferred/character.hlsl hair_fs into the same G-buffer
    SDL_GPUBuffer *vbo = nullptr, *ibo = nullptr; uint32_t ni = 0;
    SDL_GPUTextureSamplerBinding tex, none;   // s0 = the hair record's texture, s1 / s2 unused by the program
    float colour[3] = {}, diffuse[4] = {}, alpha[4] = {}, threshold = 0.f, specular = 0.f;
};

struct PlatformDraw {             // deferred/objects.hlsl main_ps (DX11, COLOURING, DUAL_TEXTURE) into the same G-buffer
    SDL_GPUBuffer *vbo = nullptr, *ibo = nullptr; uint32_t ni = 0;   // stride kPlatformStride: position, normal, uv, colour, tangent (xyz, w = handedness)
    SDL_GPUTextureSamplerBinding tex[6] = {};   // s0 base, s1 normal, s2 metal, s3 base 2, s4 normal 2, s5 metal 2
    const float *mvp = nullptr, *mv = nullptr;
};
constexpr uint32_t kPlatformStride = 64;

struct BodyArgs {
    SDL_GPUCommandBuffer* cmd = nullptr;
    SDL_GPUTexture *color = nullptr, *depth = nullptr;
    int w = 0, h = 0;
    float aspect = 1.f, tan_half = 0.4f;
    bool flip_v = true;                   // the light pass samples the G-buffer with v = 1 - ndc
    const float *mvp = nullptr, *mv = nullptr, *bones = nullptr;   // column-major 4x4, 30 bone matrices
    SDL_GPUBuffer *vbo = nullptr, *ibo = nullptr; uint32_t ni = 0;
    SDL_GPUTextureSamplerBinding tex[13] = {};   // character.hlsl s0..s12
    SDL_GPUTextureSamplerBinding irr = {}, spec = {};
    HairDraw hair[2]; int num_hair = 0;
    PlatformDraw platform; bool has_platform = false;
    float skintone[3] = {}, shirt[4] = {1, 1, 1, 1}, hair_colour[3] = {}, hair_mult[4] = {}, hair_alpha[4] = {}, beard_alpha[4] = {}, zero4[4] = {};
};

bool Init(SDL_GPUDevice* dev);
bool Ready();
bool PlatformReady();   // objects.hlsl is in the install and compiled
void DrawBody(const BodyArgs& a);   // no render pass may be open
}
