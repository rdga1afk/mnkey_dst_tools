#pragma once
// Grass overlay (default; MD_GRASS_EVERY_FRAME=0 puts the blades back into the cached G-buffer): every frame, full or still, the blades are drawn after the ground with the wind of this frame.
// Same chain as a full frame, but over the blades only: the install grass program into a scratch G-buffer, decode, the original main_fs, the fog quad + exposure,
// written over a copy of the cached ground target. The ground target is restored before the next full frame.
#include "ground_live_pass.h"
#include "grass_pass.h"

namespace md::grassstill {

bool Enabled();
// ground: the cached HDR ground target (format fmt, gw x gh). gf: cmd, depth-free fields of the grass frame (view_proj, cam, w, h, terrain_depth, far_clip); targets are the overlay's.
bool Run(SDL_GPUDevice* dev, const std::string& kenshi_dir, SDL_GPUTexture* ground, SDL_GPUTextureFormat fmt, int gw, int gh, const GroundLiveFrame& f,
         const GroundLiveLightParams& lp, const GroundLiveFogParams& fp, grasspass::Frame gf);
void Restore(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* ground);   // before a full frame writes the ground target
void Shutdown();

}  // namespace md::grassstill
