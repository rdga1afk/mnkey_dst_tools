#pragma once
// Bug video (coordinator / owner 19.10): the camera button of the game starts and stops a lossless recording of the final frame into temp_/bugcap/<time>/:
//   video.mkv (ffmpeg, H.264 -qp 0, every frame that reached the encoder), frames.csv (one row per video frame: number, time, dt, camera pose, speed, resolution, ground-reuse phase),
//   settings.txt (the settings at the start), replay.lua (the same camera path with the same settings), dropped.txt (frames the encoder could not take; none when it kept up).
// The frame is read back from the GPU without waiting (3 transfer buffers, fences checked on the next frames); the encoder runs in its own threads, the game does not wait for it.
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>
struct MdCamera;
namespace bugcap {
bool Start();                       // false when ffmpeg or the directory is not available
void Stop();                        // flushes the encoder, writes replay.lua; safe when not recording
bool Active();
// Called by the render loop at the point where the frame's command buffer is submitted, instead of the plain submit. Returns true when the command buffer was submitted here.
bool SubmitFrame(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle swapchain, uint32_t w, uint32_t h, SDL_GPUTextureFormat fmt, const MdCamera& cam, float dt, int render_w, int render_h);
}
