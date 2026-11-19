#pragma once
// DE-358: Intel CMAA 2 (Apache-2.0, THIRD_PARTY.md) as the native-resolution anti-aliasing pass, an alternative to FXAA (md.set_aa("cmaa2")).
// The shader is tools/shaders/cmaa2/CMAA2.hlsl compiled at run time (md::hlsl::Compile); the scene colour is copied into an RGBA8 storage image, the compute passes
// write the blended pixels into a copy of it (the original works in place), and the result is drawn to the swapchain.
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/size_slots.h>

class Cmaa2Pass {
public:
    bool Init();                          // game: compute pipelines + the copy / blit pipelines on the game device
    bool InitCompute(SDL_GPUDevice* dev);  // compute pipelines only (the synthetic test runs without a window)
    void Shutdown();
    bool Ready() const { return ready_; }
    // `scene` is a sampled colour target of w x h; the anti-aliased image goes onto `out_tex` (the swapchain). Returns false when nothing was drawn.
    bool Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle scene, int w, int h, md::GpuTextureHandle out_tex);

    // The four compute passes on the RGBA8 image InTex() (filled by the caller); the result is in OutTex(). The input image and `w x h` must stay the same between calls or targets are re-created.
    bool Dispatch(md::GpuCommandBufferHandle cmd, int w, int h);
    SDL_GPUTexture* InTex() const { return in_; }
    SDL_GPUTexture* OutTex() const { return out_; }
    SDL_GPUTexture* EdgesTex() const { return edges_; }
    bool EnsureTargets(int w, int h);

private:
    void DestroyTargets();
    struct Set { SDL_GPUTexture *in = nullptr, *out = nullptr, *edges = nullptr, *heads = nullptr; SDL_GPUBuffer *cand = nullptr, *loc = nullptr, *item = nullptr, *ctrl = nullptr, *indirect = nullptr; SDL_GPUTransferBuffer* ones = nullptr; int w = 0, h = 0; bool zeroed = false; bool Empty() const { return !in; } };
    Set Pack() const { return Set{ in_, out_, edges_, heads_, cand_, loc_, item_, ctrl_, indirect_, ones_, w_, h_, ctrl_zeroed_ }; }
    void Unpack(const Set& s) { in_ = s.in; out_ = s.out; edges_ = s.edges; heads_ = s.heads; cand_ = s.cand; loc_ = s.loc; item_ = s.item; ctrl_ = s.ctrl; indirect_ = s.indirect; ones_ = s.ones; w_ = s.w; h_ = s.h; ctrl_zeroed_ = s.zeroed; }
    void ReleaseSet(Set& s);
    Set alt_[4]; int alt_rr_ = 0;   // DE-362: the target sets of the other dynres levels
    SDL_GPUComputePipeline* pipe_[4] = {};   // edges, dispatch args, process candidates, deferred apply
    GpuPipeline copy_pipeline_, blit_pipeline_;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUDevice* dev_ = nullptr;
    SDL_GPUTransferBuffer* ones_ = nullptr;   // 0xFFFFFFFF words: the list heads are reset to «empty» every frame
    SDL_GPUTexture *in_ = nullptr, *out_ = nullptr, *edges_ = nullptr, *heads_ = nullptr;
    SDL_GPUBuffer *cand_ = nullptr, *loc_ = nullptr, *item_ = nullptr, *ctrl_ = nullptr, *indirect_ = nullptr;
    int w_ = 0, h_ = 0;
    bool ready_ = false, ctrl_zeroed_ = false;
};
