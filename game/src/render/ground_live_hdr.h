#pragma once
// Ground live HDR / exposure (path 1, #207 1c): the Resolve_HDR node of the install's compositors/main.compositor run as quad passes of the ORIGINAL post/hdrfp4.hlsl,
// compiled at run time from KENSHI_DIR (glue game/src/render/ground_live_hdr.hlsl). Order and sizes are the compositor's:
//   rt_lum4 128x128 <- HDR/Downsample2x2Luminance(hdr); rt_lum3 64 <- Downsample3x3(lum4); rt_lum2 16; rt_lum1 4; rt_lum0 1 (all R32F, point)
//   rt_adaptedLum 1x1 <- AdaptLuminance(lum0, lastLum); rt_lastLum <- Copy(adaptedLum)    (adapted / last hold log(luminance))
//   rt_brightpass w/4 x h/4 <- Downsample3x3Brightpass(hdr, adaptedLum); rt_bloom <- HDR/BloomBlur (pass 1 BloomBlurV, pass 2 BloomBlurH, both read rt_brightpass)
//   final <- HDR/Composite(hdr, bloom, adaptedLum)
// Run() does everything up to rt_bloom outside any render pass; Composite() draws the final quad inside the caller's pass.
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <string>
#include <vector>

namespace md {

// post/hdr.material shared_params HDRSettings (the material's defaults; the exe drives them from its sliders)
struct GroundLiveHdrParams {
    float bloom_threshold = 6.0f;
    float bloom_magnitude = 0.0f;
    float bloom_blur_sigma = 0.8f;
    float adaptation_rate = 0.5f;   // AUTOEXP_ADAPTATION_RATE
    float exposure_key = 0.55f;
    float min_luminance = 0.8f;     // MIN_LUMINANCE (today's translated path passes KenshiSkySettings::GetMainLight(h).min_lum)
    float max_luminance = 0.96f;    // MAX_LUMINANCE (today's translated path passes KenshiSkySettings::exposure_max)
};

class GroundLiveHdr {
public:
    static GroundLiveHdr& Get();
    // The release consumer's exposure: on when the build has no translated ground resolve and the live ground is on (MD_GROUND_LIVE_HDR=0 turns it off);
    // the programs are compiled on the first call. Every frame decision (float ground target, Run, Composite) asks this.
    static bool Active();
    bool Init(SDL_GPUDevice* dev);   // reads md::KenshiDir(); false (logged) when a program does not compile
    void Shutdown();
    bool Ready() const { return ready_; }
    // Outside any render pass, after the HDR colour is final for the frame. hdr: RGBA16F w x h (alpha 1 = written ground pixel). dt: frame time, s.
    bool Run(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* hdr, int w, int h, float dt, const GroundLiveHdrParams& p);
    // Inside the caller's pass (colour format cfmt; dfmt = its depth-stencil format or SDL_GPU_TEXTUREFORMAT_INVALID): the original Composite_fp4 over the texture given to
    // the last Run; pixels with alpha < 0.5 are discarded. Pipelines are created lazily per format pair (max 4).
    bool Composite(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, SDL_GPUTextureFormat cfmt, SDL_GPUTextureFormat dfmt);
    void ResetAdaptation() { reset_ = true; }   // the next Run adapts at once (frameTime 1e4 s for that frame)
    SDL_GPUTexture* AdaptedLumTex() const { return adapted_; }   // 1x1 R32F, log(adapted luminance)

private:
    enum { kLum2x2, kDown3x3, kAdapt, kCopy, kBright, kBlurV, kBlurH, kComposite, kProgs };
    enum { kTexelSize, kBloomThreshold, kExposureKey, kBloomSigma, kFrameTime, kMinLum, kMaxLum, kBloomMag, kAdaptRate, kMdBloomMag, kMdExposureKey, kUniforms };
    struct Prog {
        SDL_GPUShader* fs = nullptr;
        SDL_GPUGraphicsPipeline* pipe = nullptr;   // null for kComposite (per format, comp_pipes_)
        uint32_t ub_size = 0;
        int off[kUniforms];
        std::vector<uint8_t> ub;   // sized at Init
    };
    struct FmtPipe { SDL_GPUTextureFormat c, d; SDL_GPUGraphicsPipeline* pipe; };
    void Set(Prog& p, int u, const float* v, int n);
    void Quad(SDL_GPUCommandBuffer* cmd, Prog& p, SDL_GPUTexture* target, const SDL_GPUTextureSamplerBinding* tb, int ntb);
    bool EnsureSized(int w, int h);

    SDL_GPUDevice* dev_ = nullptr;
    SDL_GPUShader* vs_ = nullptr;
    Prog prog_[kProgs];
    FmtPipe comp_pipes_[4] = {};
    int n_comp_pipes_ = 0;
    SDL_GPUSampler* point_ = nullptr;
    SDL_GPUSampler* linear_ = nullptr;
    SDL_GPUTexture* lum_[5] = {};   // rt_lum0 1, rt_lum1 4, rt_lum2 16, rt_lum3 64, rt_lum4 128
    SDL_GPUTexture* adapted_ = nullptr;
    SDL_GPUTexture* last_ = nullptr;
    SDL_GPUTexture* bright_ = nullptr;
    SDL_GPUTexture* bloom_ = nullptr;
    SDL_GPUTextureFormat bloom_fmt_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* hdr_ = nullptr;   // the last Run's input, read by Composite
    int bw_ = 0, bh_ = 0;
    bool reset_ = true, last_cleared_ = false, ready_ = false;
};

}   // namespace md
