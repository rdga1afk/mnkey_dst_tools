#pragma once
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_depth_texture.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/ssbo.h>
#include <monkey_dust/render/skin_mesh.h>
#include <monkey_dust/render/ozz_animator.h>
#include <monkey_dust/render/rd_texture.h>
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/render/animation_soa.h>
#include <monkey_dust/render/backend/sdl_gpu_backend.h>
#include <monkey_dust/render/backend/granite_backend.h>
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <memory>

// LightUniforms for npc_instanced.frag (set=3, binding=0).
struct alignas(16) NpcFragUBO {
    float sunDir[3];       float _p0;
    float sunColor[3];     float _p1;
    float ambientColor[3]; float _p2;
    float cameraPos[3];    float _p3;
    float fogColor[3];     float _p4;
    float fogNear, fogFar; float _p5[2];
    float lightViewProj[3][16];
    float cascadeSplits[4];
    float probe[6][4];          // 6-sample directional light probe (±XYZ)
    float snow_level;           // biome snow level (0=desert, 1=snow/ice)
    float _sp[3];
    float shadow_dist_scalar;   // 1/fade_dist (default: 1/120)
    float shadow_height_scalar;
    float shadow_height_offset;
    float _shadow_pad;
};

// ── NpcRender ────────────────────────────────────────────────────────────────
// Owns all NPC GPU state: pipelines, SSBOs, skin mesh, animation clip indices.
// Init() = former InitNpcSDL(). RenderFrame() = the SDL_GPU NPC render block.
struct NpcRender {
    static NpcRender& Get() { static NpcRender s; return s; }

    static constexpr int MAX_DISSOLVE = 32;

    // RENDER-BACKEND-STAGE-2a/4 (docs/RENDER_BACKEND_ABSTRACTION.md §5.2,
    // docs/archive/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.4): створюється й
    // callback реєструється в NpcRender::Init() (npc_render_init.cpp) --
    // реальний GPU-виклик живе тут, не в GameState/game_init.cpp. Base
    // IRenderBackend* since Крок 4 -- MD_RENDER_BACKEND_GRANITE selects
    // GraniteBackend instead of SdlGpuBackend here (zero Set<Stage>
    // PassCallback registered for it, content not ported, see
    // npc_render_init.cpp's own doc comment). SetXCallback()'s (SdlGpuBackend/
    // GraniteBackend-specific, NOT part of IRenderBackend -- each backend
    // has its own registration mechanism) are only called against a LOCAL
    // concrete pointer at construction time in npc_render_init.cpp, before
    // being stored here as the base type -- every other call site
    // (UploadTransforms/RunGpuCulling/RenderShadowPass/RenderGBufferPass/
    // RenderDeferredLighting/SetFrameParams) is a pure-virtual IRenderBackend
    // method, dispatches correctly regardless of which concrete backend.
    std::unique_ptr<md::render_backend::IRenderBackend> backend_;

    // Pipelines
    GpuPipeline        pipeline;
    GpuPipeline        prepass_pipeline;
    GpuPipeline        dissolve_pipeline;

    // #399 (2026-08-23): A/B ablation for the NPC Early-Z depth prepass
    // (CullAndPrepass's own "Depth Prepass" GPU pass, npc_render_frame_
    // prep.cpp) -- with Variant A active, terrain depth is already real
    // (copied from the terrain G-buffer pass just before this one runs),
    // so this prepass's only remaining job is NPC-vs-NPC/NPC-vs-terrain
    // Early-Z; disabling it skips straight to DrawScene's main pass
    // depth-testing NPCs against that same shared `depth` for the first
    // time there instead. md.set_npc_prepass_disabled(on). NOT for
    // shipping -- same ablation-toggle pattern as terrain_shade_constant_
    // debug (scene_render.h).
    bool npc_prepass_debug_disable = false;

    // Visibility / indirect / dissolve SSBOs
    SSBO               vis_buf;
    SSBO               ind_buf;
    SSBO               dead_mask_buf;
    SSBO               dissolve_vis_buf;
    uint32_t           dissolve_slots[MAX_DISSOLVE] = {};
    float              dissolve_prog[MAX_DISSOLVE]  = {};
    int                n_dissolving = 0;
    // CPU-cull tier-split draw counts (2026-08-09, direct-draw conversion —
    // set once per frame at the end of CullAndPrepass, read by 3 draw sites:
    // the depth prepass inside CullAndPrepass itself, DrawScene's main pass,
    // and DrawShadowMaps' EVSM moment pass. DrawShadowMaps runs BEFORE
    // CullAndPrepass in RenderFrame's encode order (see its own doc comment,
    // "uses previous frame's ind_buf for draw counts") -- these members
    // preserve that exact one-frame-stale property, same as the GPU buffer
    // they replace.
    uint32_t           t01_count_ = 0, t2_count_ = 0;

    // Vertex/index geometry
    GpuStaticBuffer    vbuf;
    GpuStaticBuffer    body_color_buf;
    GpuStaticBuffer    skin_vbo;   // install skin program stream (npc_skin_live.h): SkinVertex + tangent + binormal of the install mesh
    bool               skin_live_ = false, skin_drawn_ = false;   // skin_drawn_: the install skin pass drew this frame, the forward NPC pass is skipped
    void RecordSkinPass(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* const target[3], SDL_GPUTexture* depth, int w, int h, const MdCamera& cam, SDL_GPUTexture* terrain_depth);
    GpuStaticBuffer    ibuf;
    GpuDepthTexture    depth, depth_alt;   // #194: depth_alt keeps the other size of the dynamic resolution
    uint32_t           index_count = 0;
    SDL_GPUIndexElementSize idx_elem = SDL_GPU_INDEXELEMENTSIZE_16BIT;

    // Mesh + textures
    SkinMesh           mesh;
    OzzAnimator        ozz;    // animation backend, Init'd after mesh.LoadGLB()
    md::RdTexture      body_rdtex;
    md::GpuTextureHandle    dummy_tex  = nullptr;
    SDL_GPUSampler*    dummy_samp = nullptr;

    bool               init = false;

    // Animation clip indices (resolved by name from GLB)
    int sm_idle        = -1;
    int sm_idle_combat = -1;
    int sm_walk_lo     = -1;
    int sm_walk_hi     = -1;
    int sm_jog_lo      = -1;
    int sm_jog_hi      = -1;
    int sm_run_lo      = -1;
    int sm_run_hi      = -1;
    int sm_breathing   = -1;
    int sm_postures    = -1;  // Kenshi Posture slider pose clip (see CharBodyState::posture)
    bool player_sprinting = false;

    // ── Player clothing ───────────────────────────────────────────────────────
    // SetPlayerClothSlot: loads clothbin into GPU buffers for the given slot.
    // path=nullptr clears the slot. Called from EditorCharactersPanel.
    static void SetPlayerClothSlot(int slot, const char* clothbin_path, const float color[3]);
    static void SetClothColor(int slot, const float color[3]);
    bool        cloth_visible = true;  // toggled by CLOTHES button or in-game UI

    // ── Player hair ───────────────────────────────────────────────────────────
    // SetPlayerHair: loads game/data/hair/<kHairStyles[style]>.glb via PropMesh.
    static void SetPlayerHair(int style, const float color[3]);

    // ── Animation debug overlay (F12 toggle, MONKEY_DUST_EDITOR only) ────────
    bool anim_debug = false;

    // Player anim state captured each frame for the overlay
    struct AnimDbgState {
        float bt         = 0.f;   // blend factor 0=idle, 1=full walk/jog
        float tgt_bt     = 0.f;   // target blend
        float move_speed = 0.f;   // nav.move_speed
        float walk_ph    = 0.f;   // phase accumulator
        float anim_speed = 0.f;   // 1.0=walk, 1.7=jog
        bool  is_moving  = false;
        bool  use_jog    = false;
        bool  is_sprint  = false;
        int   lod_tier   = 0;
        float s_rx_off   = 0.f;   // render pos offset from tr.x
        float desired_vx = 0.f;
        float desired_vz = 0.f;
    } player_anim_dbg;

    void DrawAnimDebugWindow();

    void Init();
    void Shutdown();
    void RenderFrame(const MdCamera& cam, float now_s, float dt,
                     MdEntity player_entity, float& cam_x, float& cam_z,
                     float cam_az,
                     float logic_accum = 0.f);  // seconds since last logic tick fired

private:
    NpcRender() = default;
    static const char* ResolveBodyTexture();

    struct FrameCtx {
        md::GpuCommandBufferHandle cmd;
        int ac;       // active NPC count (capped at MAX_DRAW_CALLS_TOTAL)
        int p_count;  // particle count from PrepareSDLGPU
        // Swapchain textures may only be acquired ONCE per command buffer
        // (task #158f: a second SDL_AcquireGPUSwapchainTexture call on the
        // same cmd deadlocks the submit's DRM fence wait). All swapchain
        // reads for this frame — SSAO composite AND any
        // ad-hoc screenshot capture — must go through AcquireSwapchainCached.
        md::GpuTextureHandle sc_tex = nullptr;
        uint32_t sc_w = 0, sc_h = 0;
        bool sc_acquired = false;
    };
    md::GpuTextureHandle AcquireSwapchainCached(FrameCtx& fc, uint32_t* out_w, uint32_t* out_h);
    // resolution_scale (#253): AcquireSwapchainCached returns the internal-resolution scene target while the upscale is
    // active; this returns the REAL swapchain (final upscale blit, screenshot capture).
    md::GpuTextureHandle AcquireRealSwapchain(FrameCtx& fc, uint32_t* out_w, uint32_t* out_h);
    static float EffectiveResolutionScale(); // auto (resolution_scale==0) or the fixed override
    static bool UpscaleActive();
    static void RenderSize(int& w, int& h);
    friend void md_settings_render_size(int*, int*, int*, int*); // F3 Settings tab (npc_render_deferred.cpp)
    void UploadAndSkin(FrameCtx& fc, const MdCamera& cam,
                       float now_s, float dt, float logic_accum,
                       MdEntity player_entity, float cam_x, float cam_z);
    void CullAndPrepass(FrameCtx& fc, const MdCamera& cam);
    void NpcDepthPrepass(FrameCtx& fc, const MdCamera& cam, bool alias);   // #207 H13.3
    void DrawScene(FrameCtx& fc, const MdCamera& cam, float now_s,
                   MdEntity player_entity,
                   float& cam_x, float& cam_z,
                   float cam_az);
    // DrawScene sub-stages (extracted 2026-07-22 — DrawScene was a single
    // 689-line/complexity-158 function, the highest-risk hotspot in the
    // repo; split into per-concern methods, same GPU state/ordering as
    // before, no behavior change). All draw into the SAME already-open
    // render pass `rp` — order matters (sky first, depth_test=false; see
    // each method's own comments for why).
    void DrawSky(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp);
    void DrawSkyHdr(const MdCamera& cam, SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd, bool stencil_depth);
    void DrawNpcForwardAndDissolve(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp);
    void DrawTerrainAndProps(FrameCtx& fc, const MdCamera& cam, float now_s, SDL_GPURenderPass* rp);
    // #163 5b-2: the ground resolve draw (all its arguments from the camera and the light), into `rp` -- the main pass, or the ground pass opened before it.
    void DrawGroundResolve(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp);
    void DrawWaterPlane(FrameCtx& fc, const MdCamera& cam, float now_s, bool water_any, SDL_GPURenderPass* rp);
    void DrawHudOverlays(FrameCtx& fc, const MdCamera& cam, float now_s,
                         MdEntity player_entity, float& cam_x, float& cam_z,
                         float cam_az, SDL_GPURenderPass* rp);
    void DrawScreenOverlays(FrameCtx& fc, float now_s, MdEntity player_entity,
                            float& cam_x, float& cam_z, float cam_az, SDL_GPURenderPass* rp);
    // resolution_scale (#253): 2D overlays + editor ImGui on the real swapchain at full resolution, after the upscale.
    void DrawFullResUi(FrameCtx& fc, float now_s, MdEntity player_entity, float& cam_x, float& cam_z, float cam_az);
    void DrawPlayerCloth(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp);
    void DrawPlayerHair(FrameCtx& fc, const MdCamera& cam, SDL_GPURenderPass* rp);
    void DrawDeferredPasses(FrameCtx& fc, const MdCamera& cam);
};
