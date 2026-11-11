#pragma once
// Water live pass, GPU side (path 1; default OFF, env MD_WATER_LIVE=1): draws the sea plane with the ORIGINAL waterVP + waterFP (near variant + BLEND) of the player's install
// (compiled by CompileWaterLive) instead of shaders/water_kenshi.frag. Inputs the engine does not have yet are bound as neutral 1x1 textures / zeros (listed in water_live_gpu.cpp);
// no GPU check has been done on this code (written on CPU, 11.11): the night job compares its frame with the translated path.
#include "water_live_pass.h"
#include <SDL3/SDL_gpu.h>

namespace md {

struct WaterLiveFrame {
    float vp[16] = {};                 // view-projection, glm column-major memory order
    float cam[3] = {};                 // camera, local (scene) coordinates, metres
    float abs_x = 0.f, abs_z = 0.f;    // absolute world position of local (0,0): zone_ox*CHUNK_SIZE - tnoff_x, zone_oz*CHUNK_SIZE - tnoff_z
    float to_sun[3] = { 0.f, 1.f, 0.f };   // unit vector TOWARD the sun
    float sun_colour[3] = { 1.f, 1.f, 1.f }, ambient[3] = { 0.2f, 0.2f, 0.2f };
    float time_s = 0.f, water_y = 1.495f, half_size = 40000.f;
    SDL_GPUTexture* normal = nullptr; SDL_GPUSampler* normal_smp = nullptr;    // water.png (repeat)
    SDL_GPUTexture* flow = nullptr;   SDL_GPUSampler* flow_smp = nullptr;      // flowmap.png (clamp, whole world)
    SDL_GPUTexture* colour = nullptr; SDL_GPUSampler* colour_smp = nullptr;    // watercolourmap.png (clamp, whole world)
};

class WaterLivePass {
public:
    bool Init(SDL_GPUDevice* dev, SDL_Window* win, const std::string& kenshi_dir);   // false + log when the original does not compile or a pipeline fails
    void Shutdown();
    bool Ready() const { return pipe_ != nullptr; }
    // inside the scene render pass (colour + depth bound, depth test on, no depth write): one draw of the two-triangle plane
    void Draw(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, const WaterLiveFrame& f);
    // Before the scene render pass: the screen-space map the original's `depthmap` (s3) reads, length(worldPos - camera) / farClip of the scene depth, R32F (the deferred depth of the original,
    // writeDepth in gbuffer.hlsl). vp = view-projection (glm column-major memory); inverted here. Draw() binds it as s3 when it was built this frame.
    void BuildDepthMap(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene_depth, uint32_t w, uint32_t h, const float vp[16], const float cam[3]);
private:
    bool InitDepthMap();
    hlsl::Compiled dvs_c_, dfs_c_; std::vector<hlsl::UniformMember> dmem_; uint32_t dsz_ = 0;
    SDL_GPUShader* dvs_ = nullptr; SDL_GPUShader* dfs_ = nullptr; SDL_GPUGraphicsPipeline* dpipe_ = nullptr;
    SDL_GPUTexture* dtex_ = nullptr; uint32_t dw_ = 0, dh_ = 0; SDL_GPUSampler* nsmp_ = nullptr; bool dvalid_ = false;
    SDL_GPUDevice* dev_ = nullptr;
    WaterLiveShaders sh_;
    SDL_GPUShader* vs_ = nullptr; SDL_GPUShader* fs_ = nullptr;
    SDL_GPUGraphicsPipeline* pipe_ = nullptr;
    SDL_GPUBuffer* vb_ = nullptr;
    SDL_GPUTexture* neutral_[12] = {}; SDL_GPUSampler* smp_ = nullptr;
    std::vector<unsigned char> vub_, fub_;
};

}   // namespace md
