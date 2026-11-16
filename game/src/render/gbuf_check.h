#pragma once
// A2 (coordinator 6906254512): G-buffer sanity of the live ground. md.gbuffer_check(path) asks for one check on the next rendered frame: the decoded albedo (RGBA16F, rgb + gloss) and the aux target
// (world normal as stored, rgb 0..1, + distance in alpha, 0 = not written) are read back and tested on the pixels the ground pass wrote: no NaN / Inf, no negative albedo or distance, |normal| ~ 1,
// and albedo is not the normal (the grass_fs bug class: one output written into the other target). The verdict goes to <path> and to the log ([gbuf_check] PASS / FAIL ...).
#include <cstddef>
#include <cstdint>
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>
namespace gbuf_check {
void Request(const char* path);
bool Wants();
// Reads the targets back with its own submit (fence wait); true when the command buffer was submitted here.
bool Submit(md::GpuCommandBufferHandle cmd);
}
