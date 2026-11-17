#include "gbuf_check.h"
#include "gbuf_analyze.h"
#include "ground_live_driver.h"
#include "grass_still.h"
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

bool DownloadR32(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* t, uint32_t w, uint32_t h, SDL_GPUTransferBuffer*& tb) {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTransferBufferCreateInfo tci{}; tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tci.size = w * h * 4;
    tb = SDL_CreateGPUTransferBuffer(dev, &tci); if (!tb) return false;
    SDL_GPUTextureRegion src{}; src.texture = t; src.w = w; src.h = h; src.d = 1;
    SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb; dst.pixels_per_row = w; dst.rows_per_layer = h;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    return true;
}

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
    SDL_GPUTransferBuffer *ba = nullptr, *bb = nullptr, *bc = nullptr;
    SDL_GPUTexture* td = pass.Raw(2);
    if (!Download(cmd, ta, w, h, ba) || !Download(cmd, tb_, w, h, bb) || (td && !DownloadR32(cmd, td, w, h, bc))) return false;
    md::grassstill::DebugTargets gt; const bool have_grass = md::grassstill::GetDebugTargets(gt);
    SDL_GPUTransferBuffer *ga = nullptr, *gb = nullptr, *gd = nullptr;
    if (have_grass && !(Download(cmd, gt.albedo, gt.w, gt.h, ga) && Download(cmd, gt.aux, gt.w, gt.h, gb) && (!gt.dist || DownloadR32(cmd, gt.dist, gt.w, gt.h, gd)))) return false;
    SDL_GPUFence* fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
    md::GpuDevice::Get().NotifyCommandBufferSubmitted();
    if (fence) { SDL_WaitForGPUFences(dev, true, &fence, 1); SDL_ReleaseGPUFence(dev, fence); }
    const uint16_t* A = (const uint16_t*)SDL_MapGPUTransferBuffer(dev, ba, false);
    const uint16_t* B = (const uint16_t*)SDL_MapGPUTransferBuffer(dev, bb, false);
    const float* D = bc ? (const float*)SDL_MapGPUTransferBuffer(dev, bc, false) : nullptr;
    char verdict[2600] = "";
    if (A && B) Analyze(A, B, (size_t)w * h, verdict, sizeof verdict);
    else snprintf(verdict, sizeof verdict, "gbuf_check: FAIL readback not mapped");
    if (D) { char dt[200]; const bool dok = AnalyzeDistance(D, (size_t)w * h, dt, sizeof dt); const size_t l = strlen(verdict); snprintf(verdict + l, sizeof verdict - l, " | %s", dt); if (!dok && strstr(verdict, "gbuf_check: PASS")) memcpy(verdict + 12, "FAIL", 4); }
    if (have_grass) {   // A2 on the grass pixels (the overlay's own G-buffer): flagged pixels > 20 = FAIL
        const uint16_t* GA = (const uint16_t*)SDL_MapGPUTransferBuffer(dev, ga, false); const uint16_t* GB = (const uint16_t*)SDL_MapGPUTransferBuffer(dev, gb, false);
        const float* GD = gd ? (const float*)SDL_MapGPUTransferBuffer(dev, gd, false) : nullptr;
        char gtxt[1600] = ""; size_t flagged = 0;
        if (GA && GB) flagged = AnalyzeGrass(GA, GB, GD, gt.w, gt.h, gtxt, sizeof gtxt); else snprintf(gtxt, sizeof gtxt, "grass_overlay: FAIL readback not mapped");
        const size_t l = strlen(verdict); snprintf(verdict + l, sizeof verdict - l, " | %.*s", (int)(sizeof verdict - l - 4), gtxt);
        if (flagged > 20 && strstr(verdict, "gbuf_check: PASS")) memcpy(strstr(verdict, "gbuf_check: PASS"), "gbuf_check: FAIL", 16);
        if (GA) SDL_UnmapGPUTransferBuffer(dev, ga);
        if (GB) SDL_UnmapGPUTransferBuffer(dev, gb);
        if (GD) SDL_UnmapGPUTransferBuffer(dev, gd);
        SDL_ReleaseGPUTransferBuffer(dev, ga); SDL_ReleaseGPUTransferBuffer(dev, gb); if (gd) SDL_ReleaseGPUTransferBuffer(dev, gd);
    }
    if (A) SDL_UnmapGPUTransferBuffer(dev, ba);
    if (B) SDL_UnmapGPUTransferBuffer(dev, bb);
    if (D) SDL_UnmapGPUTransferBuffer(dev, bc);
    SDL_ReleaseGPUTransferBuffer(dev, ba); SDL_ReleaseGPUTransferBuffer(dev, bb); if (bc) SDL_ReleaseGPUTransferBuffer(dev, bc);
    fprintf(stdout, "[gbuf_check] %s\n", verdict + 12);
    if (FILE* f = fopen(s_path.c_str(), "w")) { fprintf(f, "%s\n", verdict); fclose(f); }
    return true;
}
}
