#include "gbuf_check.h"
#include "gbuf_analyze.h"
#include "ground_live_driver.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gbuf_check {
namespace {
std::string s_path;
bool s_want = false;

bool Download(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* t, uint32_t w, uint32_t h, SDL_GPUTransferBuffer*& tb) {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTransferBufferCreateInfo tci{}; tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tci.size = w * h * 8;
    tb = SDL_CreateGPUTransferBuffer(dev, &tci); if (!tb) return false;
    SDL_GPUTextureRegion src{}; src.texture = t; src.w = w; src.h = h; src.d = 1;
    SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb; dst.pixels_per_row = w; dst.rows_per_layer = h;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    return true;
}
}

void Request(const char* path) { s_path = path ? path : ""; s_want = !s_path.empty(); }
bool Wants() { return s_want; }

bool Submit(md::GpuCommandBufferHandle cmd) {
    s_want = false;
    auto& pass = md::GroundLiveDriver::Get().Pass();
    SDL_GPUTexture *ta = pass.AlbedoRgb(), *tb_ = pass.AuxB();
    const uint32_t w = (uint32_t)pass.Width(), h = (uint32_t)pass.Height();
    if (!ta || !tb_ || !w || !h) { fprintf(stdout, "[gbuf_check] FAIL: the ground pass has no decoded targets (ground live off?)\n"); return false; }
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTransferBuffer *ba = nullptr, *bb = nullptr;
    if (!Download(cmd, ta, w, h, ba) || !Download(cmd, tb_, w, h, bb)) return false;
    SDL_GPUFence* fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
    md::GpuDevice::Get().NotifyCommandBufferSubmitted();
    if (fence) { SDL_WaitForGPUFences(dev, true, &fence, 1); SDL_ReleaseGPUFence(dev, fence); }
    const uint16_t* A = (const uint16_t*)SDL_MapGPUTransferBuffer(dev, ba, false);
    const uint16_t* B = (const uint16_t*)SDL_MapGPUTransferBuffer(dev, bb, false);
    char verdict[900] = "";
    if (A && B) Analyze(A, B, (size_t)w * h, verdict, sizeof verdict);
    else snprintf(verdict, sizeof verdict, "gbuf_check: FAIL readback not mapped");
    if (A) SDL_UnmapGPUTransferBuffer(dev, ba);
    if (B) SDL_UnmapGPUTransferBuffer(dev, bb);
    SDL_ReleaseGPUTransferBuffer(dev, ba); SDL_ReleaseGPUTransferBuffer(dev, bb);
    fprintf(stdout, "[gbuf_check] %s\n", verdict + 12);
    if (FILE* f = fopen(s_path.c_str(), "w")) { fprintf(f, "%s\n", verdict); fclose(f); }
    return true;
}
}
