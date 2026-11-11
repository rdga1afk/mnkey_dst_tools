#include "town_buildings.h"
#include "town_foliage.h"
#include "npc_render_internal.h"
#include "../bugcap.h"
#include <cmath>
#include "../world/char_def.h"

int   g_up_sharp = 0;            // #207 md.set_upscale_sharpness / Settings «Sharpening while moving» (owner 31.10): 0 Quality (5 taps + adaptive, default), 1 Fast (3 taps), 2 Off (1 tap); only the upscale of a lowered scene
float g_speed_override = -1.f;   // md.set_speed(v): C15 and the ring switch take the camera speed (m/s, turn rate 0) from the scenario instead of measuring it (< 0 = measure); a scenario renders a variable number of frames per pose, so a measured speed is not the one it plans
bool  g_dynres_on = true;   // #182 md.set_dyn_res (default on, owner 17.10): the scene renders at g_dynres_scale only while the camera moves, native when it stands; only with the native scene (no fixed scale)
float g_dynres_scale = 0.75f, g_dynres_cur = 1.f, g_dynres_v_move = 2.f, g_dynres_w_move = 10.f, g_dynres_v_stop = 0.5f, g_dynres_w_stop = 3.f;
int   g_dynres_down = 5, g_dynres_up = 2, g_dynres_cnt = 0;
static bool DynResActive() { return g_dynres_on && SceneRender::Get().resolution_scale >= 0.999f; }
float NpcRender::EffectiveResolutionScale() {
    if (DynResActive()) return g_dynres_cur;
    const float s = SceneRender::Get().resolution_scale;
    if (s > 0.f) return s;
    const int h = window_get_height();
    if (h <= 768) return 1.0f; // default window (1366x768) = native
    const float a = 720.0f / (float)h;
    return a < 0.5f ? 0.5f : (a > 1.0f ? 1.0f : a);
}

bool  g_opt_sgsr1 = false;   // #198 S1 md.set_sgsr1: Snapdragon GSR 1 instead of the bilinear + sharpen upscale (only when the scene is below native), default off
bool  g_opt_fxaa = false;   // #180 md.set_fxaa: FXAA 3.11 with Kenshi's parameters as the last pass (the scene goes through the intermediate target)
float g_frame_dt_s = 1.f / 60.f;   // RenderFrame's dt, read by the L4 auto-exposure
float g_adapt_override_mean = -1.f, g_adapt_override_dt = -1.f; // md.adapt_lum_debug: >= 0 replaces the measured mean luminance (oracle)
extern int g_kenshi_lighting_mode;
int  g_opt_ground_reuse_period = 16;   // #191 stage 6 (owner 19.10: default 16): the live block is shaded once per 4 / 8 / 16 frames (the rest is taken from the history); F3 Settings, md.set_ground_reuse_period
int  g_opt_ground_reuse_cr = 3;   // 0 nearest texel, 1 Catmull-Rom, 2 Catmull-Rom + only a pixel shaded live in the previous frame is reused;    // #163 reopened md.set_opt_ground_reuse_cr: the history of the ground reuse is read with Catmull-Rom taps instead of the nearest texel
float g_ru_chain_pushed = -1.f;   // #207: last ru_chain.x given to the resolve (logged on change; md.ground_reuse_stats().chain_pushed)
int  g_opt_ground_reuse_chain = [] { const char* e = getenv("MD_GROUND_CHAIN"); const int c = e ? atoi(e) : 5; return c == 3 ? 3 : (c == 0 ? 0 : 5); }();   // MD_GROUND_CHAIN=0|3|5 picks it for a run (gates, measurements)   // #207 (owner 27.10): 0 standard (a reused pixel is written with alpha 0.75), 3 / 5 chain: the alpha decays by 0.04 / 0.02 per reuse, so a pixel is reused up to 3 / 5 times in a row
bool g_opt_ground_reuse = true;     // #163 5b-2b md.set_opt_ground_reuse: with the split, 3 of 4 blocks of the ground take the previous frame's colour (default on, owner decision 15.10: variant 3, docs/DECISIONS.md)
int  g_opt_ground_split = 1;        // #163 5b-2a md.set_opt_ground_split: 1 = the ground resolve in its own pass + a copy into the main pass; 2 = diagnostic: no copy (the ground is missing) to time the passes apart; 0 = off
bool g_opt_force_upscale = false;    // #168 md.set_opt_force_upscale: the scene in its own target + UI on the swapchain even at scale 1.0 (the path the extrapolation needs)

// #168: the view basis of a camera in double (rows: right, up, -forward, as glm::lookAt builds them) and its eye.
static void ReprojViewBasis(const MdCamera& c, double rot[9], double eye[3]) {
    double f[3] = { (double)c.target.x - c.pos.x, (double)c.target.y - c.pos.y, (double)c.target.z - c.pos.z };
    double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); for (double& x : f) x /= fl;
    double up[3] = { c.up.x, c.up.y, c.up.z };
    double s[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
    double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]); for (double& x : s) x /= sl;
    double u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    for (int k = 0; k < 3; ++k) { rot[k] = s[k]; rot[3 + k] = u[k]; rot[6 + k] = -f[k]; }
    eye[0] = c.pos.x; eye[1] = c.pos.y; eye[2] = c.pos.z;
}

bool NpcRender::UpscaleActive() {
    const auto& sr = SceneRender::Get();
    const bool forced = g_opt_force_upscale || g_opt_fxaa;
    if ((EffectiveResolutionScale() >= 0.999f && !forced) || !sr.upscale_pass_.Ready()) return false;
    return true;
}

void NpcRender::RenderSize(int& w, int& h) {
    w = window_get_width();
    h = window_get_height();
    if (UpscaleActive() && w > 0 && h > 0) {
        const float sc = EffectiveResolutionScale();
        w = (int)(w * sc + 0.5f); if (w < 2) w = 2;
        h = (int)(h * sc + 0.5f); if (h < 2) h = 2;
    }
}

md::GpuTextureHandle NpcRender::AcquireRealSwapchain(FrameCtx& fc, uint32_t* out_w, uint32_t* out_h) {
    if (!fc.sc_acquired) {
        // render-audit-2026: this can block waiting for a swapchain image
        // (compositor/present-mode dependent) -- previously uninstrumented.
        FS_BEGIN("AcquireSC");
        fc.sc_tex = md::GpuDevice::Get().AcquireSwapchainTexture(fc.cmd, &fc.sc_w, &fc.sc_h);
        FS_END("AcquireSC");
        fc.sc_acquired = true;
    }
    if (out_w) *out_w = fc.sc_w;
    if (out_h) *out_h = fc.sc_h;
    return fc.sc_tex;
}

md::GpuTextureHandle NpcRender::AcquireSwapchainCached(FrameCtx& fc, uint32_t* out_w, uint32_t* out_h) {
    md::GpuTextureHandle real = AcquireRealSwapchain(fc, out_w, out_h);
    if (real && UpscaleActive()) {
        // resolution_scale: every consumer of "the swapchain" (scene pass, SSAO/sky-vis apply) works on the internal-
        // resolution scene target; the real swapchain is only written by the final upscale blit.
        const UpscalePass& up = SceneRender::Get().upscale_pass_;
        int rw = 0, rh = 0;
        RenderSize(rw, rh);
        if (up.HasTarget(rw, rh)) {
            if (out_w) *out_w = (uint32_t)up.W();
            if (out_h) *out_h = (uint32_t)up.H();
            return up.Tex();
        }
    }
    return real;
}

void NpcRender::DrawFullResUi(FrameCtx& fc, float now_s, MdEntity player_entity, float& cam_x, float& cam_z, float cam_az)
{
    uint32_t sw = 0, sh = 0;
    md::GpuTextureHandle real_sc = AcquireRealSwapchain(fc, &sw, &sh);
    if (!real_sc) return;
    GpuPushDebugGroup(fc.cmd, "FullResUi");
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd          = fc.cmd;
    cpd.color_tex[0] = real_sc;
    cpd.depth_tex    = nullptr;
    cpd.load_color   = true; // draw on top of the upscaled scene
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* rp = cb.SDLPass();
    if (rp) {
        if (md::RenderPassGraph::Get().IsEnabled("draw_hud"))
            DrawScreenOverlays(fc, now_s, player_entity, cam_x, cam_z, cam_az, rp);
#if defined(MONKEY_DUST_EDITOR)
        ImDrawData* dd = ImGui::GetDrawData();
        if (dd && dd->CmdListsCount > 0)
            ImGui_ImplSDLGPU3_RenderDrawData(dd, fc.cmd, rp);
#endif
        SDL_EndGPURenderPass(rp);
    }
    GpuPopDebugGroup(fc.cmd);
}

void NpcRender::DrawDeferredPasses(FrameCtx& fc, const MdCamera& cam)
{

// КРОК 7 (docs/KROK7_SKYVIS_AO_DESIGN.md): sky-visibility AO apply --
// applies a multiply-blend onto the swapchain. Only meaningful in
// the screen-space terrain shading path (see SceneRender::terrain_skyvis_
// ao_'s own doc comment) -- SceneRender::Get() member accessed the same
// way.
if (SceneRender::Get().terrain_skyvis_ao_.IsEnabled() &&
    md::RenderPassGraph::Get().IsEnabled("skyvis_ao")) {
    GpuPushDebugGroup(fc.cmd, "SkyVisAO Apply");
    md::GpuProfiler::Get().BeginPass("SkyVisAO Apply");
    GpuTexture& gcol = SceneRender::Get().terrain_shading_projected_.GBufferColor();
    GpuDepthTexture& gdep = SceneRender::Get().terrain_shading_projected_.GBufferDepth();
    // S7: the colour G-buffer is not written, the sky-vis apply reconstructs world XZ from depth instead.
    // #142 phase 1b: always (the 8 B G-buffer no longer holds the world position; world XZ comes from the depth)
    SceneRender::Get().terrain_skyvis_ao_.SetS7Recon(SceneRender::Get().terrain_shading_projected_.S7InvVp());
    {
        uint32_t sc_w = 0, sc_h = 0;
        md::GpuTextureHandle sc_tex = AcquireSwapchainCached(fc, &sc_w, &sc_h);
        if (sc_tex)
            SceneRender::Get().terrain_skyvis_ao_.ApplyPass(
                fc.cmd, sc_tex, (int)sc_w, (int)sc_h,
                gcol.SDLTexture(), gcol.SDLSampler(),
                gdep.SDLTexture(), gdep.SDLSampler());
    }
    md::GpuProfiler::Get().EndPass();
    GpuPopDebugGroup(fc.cmd);
}
}

void NpcRender::RenderFrame(const MdCamera& cam, float now_s, float dt,
                            MdEntity player_entity, float& cam_x, float& cam_z,
                            float cam_az,
                            float logic_accum) {
    g_frame_dt_s = dt;
    if (DynResActive()) {   // #182: linear speed (m/s) and turn rate (deg/s) of the camera, two thresholds (move / stand), N frames down and M frames up
        static Vec3 pp_{}, pt_{}; static bool have_ = false;
        float v = 0.f, w = 0.f;
        const float dtv = dt;
        if (have_ && dtv > 1.0e-4f) {
            const float dx = cam.pos.x - pp_.x, dy = cam.pos.y - pp_.y, dz = cam.pos.z - pp_.z;
            v = sqrtf(dx * dx + dy * dy + dz * dz) / dtv;
            float a[3] = { pt_.x - pp_.x, pt_.y - pp_.y, pt_.z - pp_.z }, b[3] = { cam.target.x - cam.pos.x, cam.target.y - cam.pos.y, cam.target.z - cam.pos.z };
            const float la = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]), lb = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
            if (la > 1.0e-6f && lb > 1.0e-6f) { float c = (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) / (la * lb); c = c > 1.f ? 1.f : (c < -1.f ? -1.f : c); w = acosf(c) * 57.29578f / dtv; }
        }
        pp_ = cam.pos; pt_ = cam.target; have_ = true;
        if (g_speed_override >= 0.f) { v = g_speed_override; w = 0.f; }
        const bool low = g_dynres_cur < 0.999f;
        const bool want_low = low ? (v > g_dynres_v_stop || w > g_dynres_w_stop) : (v > g_dynres_v_move || w > g_dynres_w_move);
        if (want_low == low) g_dynres_cnt = 0;
        else if (++g_dynres_cnt >= (want_low ? g_dynres_down : g_dynres_up)) { g_dynres_cur = want_low ? g_dynres_scale : 1.f; g_dynres_cnt = 0; }
    } else g_dynres_cur = 1.f;
            {
                FS_BEGIN("FenceWait");
                md::GpuDevice::Get().BeginFrame();
                FS_END("FenceWait");
            }
            // render-audit-2026: this whole region (AdvanceFrameSlot +
            // both resize checks + AcquireCommandBuffer) ran with NO FS_
            // timer at all -- the [FRAMESPIKE] dump (docs/RENDER_AUDIT_2026.md
            // §11.1/§11.2) found frames with RenderTotal up to 65.8ms where
            // FenceWait AND every other named section summed to <5ms,
            // meaning the time was passing somewhere in an untimed gap.
            // This is the only untimed gap between FenceWait and UploadSkin.
            FS_BEGIN("AdvanceSlot");
            md::GpuDevice::Get().AdvanceFrameSlot();
            FS_END("AdvanceSlot");

            if (init && index_count > 0 &&
                pipeline.SDLPipeline() &&
                vbuf.SDLBuffer() && ibuf.SDLBuffer() &&
                vis_buf.SDLBuffer() && ind_buf.SDLBuffer() &&
                body_color_buf.SDLBuffer() &&
                TransformSoA::Get().SDLTransformBuffer() &&
                TransformSoA::Get().SDLFactionBuffer() &&
                AnimationSoA::Get().SDLBonesBuffer()) {
                // Recreate depth texture if window was resized (e.g. moved to another monitor).
                FS_BEGIN("ResizeChecks");
                {
                    int cur_w = 0, cur_h = 0;
                    RenderSize(cur_w, cur_h); // resolution_scale: internal resolution when the upscale is active
                    if (cur_w > 0 && cur_h > 0 &&
                        (cur_w != depth.Width() || cur_h != depth.Height())) {
                        if (depth_alt.SDLTexture() && depth_alt.Width() == cur_w && depth_alt.Height() == cur_h) std::swap(depth, depth_alt);   // #194: the previous size is kept, nothing is created or destroyed
                        else {
                            md::GpuDevice::Get().WarnIfActive("depth texture resize");
                            // CPU/GPU pipelining (SubmitFrame): the previous frame may still be executing -- drain before
                            // destroying/recreating the persistent frame targets (depth here; G-buffer/upscale target below).
                            md::GpuDevice::Get().WaitForIdle();
                            depth_alt.Shutdown();
                            std::swap(depth, depth_alt);
                            depth.Init(cur_w, cur_h, false);
                        }
                    }
                }
                {   // #194: the first frame with the dynamic resolution on creates the targets of both sizes (once, here), so no later switch creates or destroys anything
                    static bool warmed = false;
                    if (!warmed && DynResActive()) {
                        warmed = true;
                        auto& sr_w = SceneRender::Get();
                        const SDL_GPUTextureFormat sfmt = SDL_GetGPUSwapchainTextureFormat(md::GpuDevice::Get().SDLDevice(), _wnd::ptr());
                        const int nw = window_get_width(), nh = window_get_height();
                        int cw = 0, ch = 0; RenderSize(cw, ch);
                        const int sizes[2][2] = { { (int)(nw * g_dynres_scale + 0.5f), (int)(nh * g_dynres_scale + 0.5f) }, { cw, ch } };
                        md::GpuDevice::Get().WaitForIdle();
                        for (int k = 0; k < 2; ++k) {
                            const int w = sizes[k][0], h = sizes[k][1];
                            if (w < 2 || h < 2) continue;
                            if (w != depth.Width() || h != depth.Height()) {
                                if (depth_alt.SDLTexture() && depth_alt.Width() == w && depth_alt.Height() == h) std::swap(depth, depth_alt);
                                else { depth_alt.Shutdown(); std::swap(depth, depth_alt); depth.Init(w, h, false); }
                            }
                            sr_w.upscale_pass_.Ensure(w, h, sfmt);
                            if (g_opt_ground_split > 0) sr_w.ground_split_pass_.Ensure(w, h, (g_kenshi_lighting_mode >= 4 && sr_w.ground_split_pass_.AdaptReady()) ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : sfmt, g_opt_ground_reuse && g_opt_ground_split == 1);
                            sr_w.terrain_shading_projected_.EnsureHistory(g_opt_ground_reuse && g_opt_ground_split == 1, w, h);
                            sr_w.terrain_shading_projected_.EnsureSize(md::GpuDevice::Get().SDLDevice(), w, h);
                        }
                        sr_w.gr_reset_ = true; sr_w.gr_have_ = false;
                    }
                }
                // Same reasoning as the shared `depth` resize just above --
                // MUST run before AcquireCommandBuffer, never after. This
                // call used to live inside CullAndPrepass (after the
                // command buffer for this frame was already active),
                // destroying/recreating gbuf_color_/gbuf_depth_ mid-frame
                // -- diagnosed live 2026-08-02 as a real SIGSEGV inside the
                // Intel Vulkan driver (crash surfaced one call later, in
                // DrawShadingResolve, consistent with a use-after-free on
                // the just-destroyed G-buffer texture that a still-forming
                // or still-in-flight command buffer referenced). Moving it
                // here (no active command buffer yet) fixed it -- confirmed
                // by disabling the call entirely as a control, which also
                // stopped the crash, isolating EnsureSize's mid-frame
                // timing (not its resize logic itself) as the cause.
                {
                    int cur_w = 0, cur_h = 0;
                    RenderSize(cur_w, cur_h);
                    if (UpscaleActive())
                        SceneRender::Get().upscale_pass_.Ensure(cur_w, cur_h,
                            SDL_GetGPUSwapchainTextureFormat(md::GpuDevice::Get().SDLDevice(), _wnd::ptr()));
                    {
                        auto& sr_g = SceneRender::Get();
                        sr_g.gr_wanted_ = g_opt_ground_reuse && g_opt_ground_split == 1;
                        if (g_opt_ground_split > 0)
                            if (sr_g.ground_split_pass_.Ensure(cur_w, cur_h, (g_kenshi_lighting_mode >= 4 && sr_g.ground_split_pass_.AdaptReady()) ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : SDL_GetGPUSwapchainTextureFormat(md::GpuDevice::Get().SDLDevice(), _wnd::ptr()), sr_g.gr_wanted_)) { sr_g.gr_have_ = false; sr_g.ground_split_pass_.ResetAdaptation(); }
                        sr_g.terrain_shading_projected_.EnsureHistory(sr_g.gr_wanted_, cur_w, cur_h);
                        if (sr_g.gr_wanted_) sr_g.terrain_shading_projected_.BeginFrame(); else sr_g.gr_have_ = false;
                    }
                    md::GpuDevice::Get().WarnIfActive("terrain_shading_projected_.EnsureSize");
                    SceneRender::Get().terrain_shading_projected_.EnsureSize(
                        md::GpuDevice::Get().SDLDevice(), cur_w, cur_h);
                }
                FS_END("ResizeChecks");
                FS_BEGIN("AcquireCmdBuf");
                md::GpuCommandBufferHandle r_cmd = md::GpuDevice::Get().AcquireCommandBuffer();
                FS_END("AcquireCmdBuf");
                if (r_cmd) {
                    FrameCtx fc{ r_cmd, 0, 0 };
                    {
                    // Debug groups (2026-08-07): SDL_GPU/Vulkan draws had no
                    // human-readable names in RenderDoc captures (only
                    // numeric "Shader Module N" labels, since these SPIR-V
                    // builds carry no debug info) -- made it impossible to
                    // tell which of the ~18-20ms/frame FenceWait was waiting
                    // on actually came from. Boundaries match the FS_BEGIN/
                    // FS_END sections below exactly, so RenderDoc's per-
                    // group GPU Duration lines up with FrameStats' own CPU-
                    // side section names.
                    // RENDER-BACKEND-STAGE-2e: call site перенесено на
                    // backend_->UploadTransforms(), яка викликає
                    // зареєстрований callback (одна UploadAndSkin -- upload
                    // ТА skinning разом, див. SetUploadSkinCallback doc-
                    // коментар).
                    GpuPushDebugGroup(fc.cmd, "UploadSkin");
                    FS_BEGIN("UploadSkin");
                    {
                        md::render_backend::RenderFrameParams rbp;
                        rbp.camera        = &cam;
                        rbp.now_s         = now_s;
                        rbp.dt            = dt;
                        rbp.logic_accum   = logic_accum;
                        rbp.player_entity = player_entity;
                        rbp.cam_x         = cam_x;
                        rbp.cam_z         = cam_z;
                        rbp.cmd           = fc.cmd;
                        rbp.frame_ctx     = &fc;
                        backend_->SetFrameParams(rbp);
                        backend_->UploadTransforms();
                    }
                    FS_END("UploadSkin");
                    GpuPopDebugGroup(fc.cmd);

                    // Granite terrain migration, Phase 6: computes visible
                    // patches + uploads their per-tier instance batches
                    // (SceneRender::UpdateGraniteTerrain's doc comment) --
                    // MUST run here, before any render pass in this frame
                    // opens (it issues its own SDL_GPU copy passes on
                    // fc.cmd). No-op unless SceneRender::terrain_granite_
                    // enabled was toggled on (md.set_granite_terrain).
                    //
                    // render-audit-2026 (2026-08-29): this section was
                    // misread as "terrain cost" in perf captures for months
                    // -- UpdateGraniteTerrain early-returns
                    // (!terrain_granite_ready) in the DEFAULT build config
                    // , leaving only
                    // camera/frustum math, no GPU work (`(void)cmd;`). The
                    // REAL default-path terrain CPU cost is the
                    // SelectVisible+DrawBatched/DrawNode block inside
                    // CullAndPrepass (npc_render_frame_prep.cpp, new
                    // "TerrainPrepCPU" FS section) -- renamed here so a
                    // near-permanent 0.0ms reading isn't mistaken for "no
                    // terrain cost" again.
                    {
                        float g_asp = (float)window_get_width() / (float)window_get_height();
                        GpuPushDebugGroup(fc.cmd, "TerrainBake_Disabled");
                        FS_BEGIN("TerrainBake_Disabled");
                        SceneRender::Get().UpdateGraniteTerrain(fc.cmd, cam, g_asp);
                        FS_END("TerrainBake_Disabled");
                        GpuPopDebugGroup(fc.cmd);
                    }

                    // RENDER-BACKEND-STAGE-2a (docs/RENDER_BACKEND_ABSTRACTION.md
                    // §5): call site перенесено на backend_->RenderShadowPass(),
                    // яка викликає зареєстрований callback (NpcRender::Init())
                    // -- та сама логіка (ShadowSystem::Update + DrawShadowMaps),
                    // тільки точка виклику інша ("adapter, не rewrite", §2.1).
                    GpuPushDebugGroup(fc.cmd, "Shadows");
                    FS_BEGIN("Shadows");
                    {
                        md::render_backend::RenderFrameParams rbp;
                        rbp.camera       = &cam;
                        { int vw_, vh_; RenderSize(vw_, vh_); rbp.viewport_w = vw_; rbp.viewport_h = vh_; }
                        rbp.cmd          = fc.cmd;
                        rbp.active_count = fc.ac;
                        backend_->SetFrameParams(rbp);
                        backend_->RenderShadowPass();
                    }
                    FS_END("Shadows");
                    GpuPopDebugGroup(fc.cmd);

                    // RENDER-BACKEND-STAGE-2e: call site перенесено на
                    // backend_->RunGpuCulling().
                    // RenderDoc label corrected 2026-09-20 (RENDER-AUDIT-4):
                    // this scope's real GPU cost is NOT culling (that moved
                    // to CPU 2026-08-09, cull.comp is undispatched) -- it's
                    // dominated by the Terrain GBuffer (Variant A) batched
                    // draw + NPC early-Z depth prepass, both hoisted inside
                    // CullAndPrepass() for depth-seeding order (see that
                    // function's own doc comments). "Cull" alone as a
                    // RenderDoc marker was actively misleading a per-debug-
                    // group GPU profile into blaming culling for ~30% of
                    // frame GPU time that's really terrain rendering. The
                    // FS_BEGIN/END("Cull") CPU timer name is untouched --
                    // it's a different, still-accurate measurement (command
                    // recording time for this whole scope) already cited by
                    // name throughout docs/RENDER_AUDIT_2026.md.
                    GpuPushDebugGroup(fc.cmd, "CullAndPrepass");
                    FS_BEGIN("Cull");
                    {
                        md::render_backend::RenderFrameParams rbp;
                        rbp.camera    = &cam;
                        rbp.cmd       = fc.cmd;
                        rbp.frame_ctx = &fc;
                        backend_->SetFrameParams(rbp);
                        backend_->RunGpuCulling();
                    }
                    FS_END("Cull");
                    GpuPopDebugGroup(fc.cmd);

                    // RENDER-BACKEND-STAGE-2b/2f: call site перенесено на
                    // backend_->RenderGBufferPass(), яка викликає
                    // зареєстрований callback (NpcRender::Init()) -- та сама
                    // DrawScene, тільки точка виклику інша.
                    // #163 5b-2b: the reprojection of this frame's ground resolve (previous ground pass camera -> this one), in double.
                    {
                        auto& sr_g = SceneRender::Get();
                        if (sr_g.gr_wanted_) {
                            int gw = 0, gh = 0;
                            RenderSize(gw, gh);
                            Mat4 proj = cam.ProjMatrix((float)gw / (float)gh);
                            double rn[9], en[3];
                            ReprojViewBasis(cam, rn, en);
                            auto& P = sr_g.gr_params_;
                            memcpy(P.p_new, mat4_ptr(proj), 64);
                            P.params[0] = P.params[1] = P.params[2] = P.params[3] = 0.f;
                            if (sr_g.gr_have_) {
                                const double* ro = sr_g.gr_prev_rot_; const double* eo = sr_g.gr_prev_eye_;
                                double Td[16] = {};
                                const double de[3] = { en[0] - eo[0], en[1] - eo[1], en[2] - eo[2] };
                                for (int i = 0; i < 3; ++i) {
                                    for (int j = 0; j < 3; ++j) Td[j * 4 + i] = ro[i * 3] * rn[j * 3] + ro[i * 3 + 1] * rn[j * 3 + 1] + ro[i * 3 + 2] * rn[j * 3 + 2];
                                    Td[12 + i] = ro[i * 3] * de[0] + ro[i * 3 + 1] * de[1] + ro[i * 3 + 2] * de[2];
                                }
                                Td[15] = 1.0;
                                bool ident = true;
                                for (int k = 0; k < 16; ++k) if (std::fabs(Td[k] - ((k % 5) == 0 ? 1.0 : 0.0)) > 1.0e-9) ident = false;
                                for (int k = 0; k < 16; ++k) P.t[k] = (float)Td[k];
                                memcpy(P.p_old, sr_g.gr_prev_proj_, 64);
                                // a camera jump (teleport, map click, camera switch) makes the history invalid for one frame: all blocks are shaded live
                                const double move2 = de[0] * de[0] + de[1] * de[1] + de[2] * de[2];
                                const double cos_ang = (Td[0] + Td[5] + Td[10] - 1.0) * 0.5;
                                bool proj_changed = false;
                                for (int k = 0; k < 16; ++k) if (std::fabs(P.p_new[k] - P.p_old[k]) > 1.0e-6f * (1.f + std::fabs(P.p_old[k]))) proj_changed = true;
                                const bool still = move2 < 0.0025 && cos_ang > 0.99999;   // under a still camera a changed terrain (LOD nodes, fine window) makes the history stale; in motion the staleness is ignored
                                const bool geom_changed = sr_g.gr_geom_hash_ != sr_g.gr_prev_geom_hash_;
                                const bool jump = move2 > 100.0 || cos_ang < 0.8660254 /* 30 degrees */ || proj_changed || sr_g.gr_reset_ || (still && geom_changed);
                                sr_g.gr_reset_ = false;
                                P.params[0] = jump ? 0.f : 1.f;
                                P.params[1] = ident ? 1.f : 0.f; P.params[2] = (float)((sr_g.gr_frame_ & (uint32_t)(g_opt_ground_reuse_period - 1)) + 16 * (g_opt_ground_reuse_period == 16 ? 2 : g_opt_ground_reuse_period == 8 ? 1 : 0)); P.params[3] = (float)g_opt_ground_reuse_cr; P.chain[0] = g_opt_ground_reuse_chain == 5 ? 0.02f : (g_opt_ground_reuse_chain == 3 ? 0.04f : 0.f); P.chain[1] = P.chain[2] = P.chain[3] = 0.f; if (P.chain[0] != g_ru_chain_pushed) { g_ru_chain_pushed = P.chain[0]; fprintf(stderr, "[chain] ru_chain.x pushed to the resolve = %.2f (chain %d)\n", g_ru_chain_pushed, g_opt_ground_reuse_chain); }
                                if (sr_g.ground_split_pass_.CrossSize() && sr_g.terrain_shading_projected_.CrossSize()) { P.params[1] = 0.f; P.params[3] = 1.f; }   // #194: the size changed: the history of the old size is reprojected and read with Catmull-Rom, every ground pixel of it is accepted
                            }
                            sr_g.gr_prev_geom_hash_ = sr_g.gr_geom_hash_;
                            memcpy(sr_g.gr_prev_proj_, mat4_ptr(proj), 64);
                            memcpy(sr_g.gr_prev_rot_, rn, sizeof(rn));
                            memcpy(sr_g.gr_prev_eye_, en, sizeof(en));
                            sr_g.gr_have_ = true;   // DrawScene clears it when the ground pass does not run
                            ++sr_g.gr_frame_;
                        }
                    }
                    GpuPushDebugGroup(fc.cmd, "DrawScene");
                    FS_BEGIN("DrawScene");
                    {
                        md::render_backend::RenderFrameParams rbp;
                        rbp.camera        = &cam;
                        rbp.now_s         = now_s;
                        { int vw_, vh_; RenderSize(vw_, vh_); rbp.viewport_w = vw_; rbp.viewport_h = vh_; }
                        rbp.cmd           = fc.cmd;
                        rbp.active_count  = fc.ac;
                        rbp.player_entity = player_entity;
                        rbp.cam_az        = cam_az;
                        rbp.cam_x_io      = &cam_x;
                        rbp.cam_z_io      = &cam_z;
                        rbp.frame_ctx     = &fc;
                        backend_->SetFrameParams(rbp);
                        backend_->RenderGBufferPass();
                    }
                    FS_END("DrawScene");
                    GpuPopDebugGroup(fc.cmd);

                    // RENDER-BACKEND-STAGE-2c/2d: call site перенесено на
                    // backend_->RenderDeferredLighting(), яка викликає
                    // зареєстрований callback (одна DrawDeferredPasses,
                    // ambient+SSAO разом -- див.
                    // SdlGpuBackend::SetDeferredPassCallback doc-коментар).
                    GpuPushDebugGroup(fc.cmd, "Deferred");
                    FS_BEGIN("Deferred");
                    {
                        md::render_backend::RenderFrameParams rbp;
                        rbp.camera    = &cam;
                        rbp.cmd       = fc.cmd;
                        rbp.frame_ctx = &fc;
                        backend_->SetFrameParams(rbp);
                        backend_->RenderDeferredLighting();
                    }
                    FS_END("Deferred");
                    GpuPopDebugGroup(fc.cmd);

                    }
                    // resolution_scale (#253): the scene (and its post passes) went into the internal-resolution target;
                    // upscale it onto the real swapchain now, before the screenshot capture / submit below.
                    if (UpscaleActive()) {
                        uint32_t up_w = 0, up_h = 0;
                        md::GpuTextureHandle real_sc = AcquireRealSwapchain(fc, &up_w, &up_h);
                        auto& sr_up = SceneRender::Get();
                        int rw_ = 0, rh_ = 0;
                        RenderSize(rw_, rh_);
                        if (real_sc && sr_up.upscale_pass_.HasTarget(rw_, rh_)) {
                            GpuPushDebugGroup(fc.cmd, "Upscale");
                            bool upscaled = false;
                            if (g_opt_fxaa && EffectiveResolutionScale() >= 0.999f) { GpuPushDebugGroup(fc.cmd, "FXAA"); sr_up.upscale_pass_.ApplyFxaa(fc.cmd, real_sc); GpuPopDebugGroup(fc.cmd); upscaled = true; }
                            if (!upscaled && g_opt_sgsr1 && sr_up.upscale_pass_.Sgsr1Ready() && EffectiveResolutionScale() < 0.999f) { GpuPushDebugGroup(fc.cmd, "SGSR1"); sr_up.upscale_pass_.ApplySgsr1(fc.cmd, real_sc); GpuPopDebugGroup(fc.cmd); upscaled = true; }   // #198 S1
                            if (!upscaled) sr_up.upscale_pass_.Apply(fc.cmd, real_sc, EffectiveResolutionScale() >= 0.999f ? 0.f : sr_up.upscale_sharpness, g_up_sharp); // scale 1.0 (the forced path): a plain copy
                            GpuPopDebugGroup(fc.cmd);
                            DrawFullResUi(fc, now_s, player_entity, cam_x, cam_z, cam_az);
                        }
                    }

#ifdef MONKEY_DUST_EDITOR
                    char _shot_path[256];
                    if (EditorScreenshot_ConsumePending(_shot_path, sizeof(_shot_path))) {
                        uint32_t _sc_w = 0, _sc_h = 0;
                        md::GpuTextureHandle _sc_tex = AcquireRealSwapchain(fc, &_sc_w, &_sc_h);
                        SDL_GPUTextureFormat _sc_fmt = SDL_GetGPUSwapchainTextureFormat(
                            md::GpuDevice::Get().SDLDevice(), _wnd::ptr());
                        if (_sc_tex) {
                            EditorScreenshot_CaptureAndSubmit(md::GpuDevice::Get().SDLDevice(),
                                r_cmd, _sc_tex, _sc_w, _sc_h, _sc_fmt, _shot_path);
                        } else {
                            EditorScreenshot_RequestPending(_shot_path);   // no swapchain image this frame (frames in flight 1): ask again for the next frame instead of dropping the capture
                            FS_BEGIN("Submit");
                            md::GpuDevice::Get().SubmitFrame(r_cmd);
                            FS_END("Submit");
                        }
                    } else
#endif
                    if (bugcap::Active()) {
                        // bug video (the camera button): the frame is copied back without waiting; the submit happens inside
                        uint32_t _bw = 0, _bh = 0;
                        md::GpuTextureHandle _btex = AcquireRealSwapchain(fc, &_bw, &_bh);
                        int _rw = 0, _rh = 0; RenderSize(_rw, _rh);
                        if (!bugcap::SubmitFrame(r_cmd, _btex, _bw, _bh, SDL_GetGPUSwapchainTextureFormat(md::GpuDevice::Get().SDLDevice(), _wnd::ptr()), cam, dt, _rw, _rh))
                            md::GpuDevice::Get().SubmitFrame(r_cmd);
                    } else
                    {
                        // render-audit-2026: CPU-side cost of submitting this
                        // frame's command buffer to the driver -- previously
                        // uninstrumented, named as a candidate for the
                        // unattributed 16-111ms RenderTotal spikes
                        // (docs/RENDER_AUDIT_2026.md §11.1) alongside
                        // AcquireSC above.
                        FS_BEGIN("Submit");
                        md::GpuDevice::Get().SubmitFrame(r_cmd);
                        FS_END("Submit");
                    }
                }
            }
}

// DrawAnimDebugWindow() moved to npc_render_debug.cpp
// (split to keep this TU focused: debug overlay changes only rebuild that small file)

// Load .clothbin into GPU VBO/IBO. path=nullptr clears the slot.
// .clothbin: magic(4)+nv(4)+ni(4)+flags(4)+nv*SkinVertex(52)+ni*u16
void NpcRender::SetPlayerClothSlot(int slot, const char* path, const float color[3]) {
    if (slot < 0 || slot >= 3) return;
    auto& s = s_player_cloth[slot];
    s.vbo.Shutdown(); s.ibo.Shutdown(); s.ni = 0; s.active = false;
    if (color) { s.color[0]=color[0]; s.color[1]=color[1]; s.color[2]=color[2]; }
    if (!path) return;

    FILE* f = fopen(path, "rb"); if (!f) { fprintf(stderr,"[NpcCloth] missing: %s\n",path); return; }
    uint32_t hdr[4]; fread(hdr,4,4,f);
    if (hdr[0] != 0x544F4C43u) { fclose(f); fprintf(stderr,"[NpcCloth] bad magic\n"); return; }
    uint32_t nv=hdr[1], ni=hdr[2];
    static char vtmp[20000*52];
    if (nv > 20000) { fclose(f); return; }
    fread(vtmp, 52, nv, f);
    s.vbo.Shutdown(); s.vbo.Init(0x8892u, vtmp, (uint32_t)(nv*52));
    static uint16_t itmp[65536];
    if (ni > 65536) { fclose(f); return; }
    fread(itmp, 2, ni, f); fclose(f);
    s.ibo.Shutdown(); s.ibo.Init(0x8893u, itmp, (uint32_t)(ni*2));
    s.ni = (int)ni; s.active = true;
    fprintf(stdout,"[NpcCloth] slot%d: %s (%uv %ut)\n", slot, path, nv, ni/3);
}

void NpcRender::SetClothColor(int slot, const float color[3]) {
    if (slot < 0 || slot >= 3 || !color) return;
    s_player_cloth[slot].color[0]=color[0];
    s_player_cloth[slot].color[1]=color[1];
    s_player_cloth[slot].color[2]=color[2];
}

// Loads game/data/hair/<kHairStyles[style]>.glb via the shared PropMesh
// loader (same GLB path FeatureScatter uses) instead of hand-parsing cgltf.
void NpcRender::SetPlayerHair(int style, const float color[3]) {
    if (style < 0 || style >= CHARDEF_HAIR_STYLE_N) style = 0;
    char path[256];
    snprintf(path, sizeof(path), "game/data/hair/%s.glb", kHairStyles[style]);
    s_player_hair.mesh.Shutdown();
    s_player_hair.loaded = s_player_hair.mesh.LoadGLB(path, 0.0f);
    if (!s_player_hair.loaded) {   // no converted .glb (the release): the install's own mesh, geometry of the .glb (UV 0 as in it)
        std::vector<PropVertex> v; std::vector<uint32_t> ix;
        if (LoadOgreStaticMesh(std::string("data/character/hair/meshes/") + kHairStyles[style] + ".mesh", style == 29 ? 1.0f : 0.1f, 0.0f, v, ix, false))
            s_player_hair.loaded = s_player_hair.mesh.LoadFromArrays(v.data(), (uint32_t)v.size(), ix.data(), (uint32_t)ix.size());
    }
    if (!s_player_hair.loaded)
        fprintf(stderr, "[NpcHair] missing: %s\n", path);
    if (color) {
        s_player_hair.color[0]=color[0];
        s_player_hair.color[1]=color[1];
        s_player_hair.color[2]=color[2];
    }
}


// F3 Settings tab bridge (tools/editor/editor_core.cpp declares these extern; tools/ does not include game headers).
extern int g_terrain_lod_patch; // npc_render_frame_prep.cpp
float md_settings_get_resolution_scale() { return SceneRender::Get().resolution_scale; } // raw: 0 = AUTO
void md_settings_render_size(int* in_w, int* in_h, int* out_w, int* out_h) {
    *out_w = window_get_width(); *out_h = window_get_height();
    NpcRender::RenderSize(*in_w, *in_h);
}
void md_settings_set_frames_in_flight(int n) { SDL_SetGPUAllowedFramesInFlight(md::GpuDevice::Get().SDLDevice(), (Uint32)(n < 1 ? 1 : (n > 3 ? 3 : n))); }   // F3 Settings (#152)
extern bool g_ground_pvt, g_ground_hist_fast;   // engine/src/render/terrain_shading_projected.cpp
void md_settings_set_ground_hist_fast(int mode) { g_ground_hist_fast = mode != 0; }   // F3 Settings (#207, owner 01.11)
void md_settings_set_ground_pvt(int on) { g_ground_pvt = on != 0; }                    // F3 Settings (#207, owner 01.11)
void md_settings_set_up_sharp(int mode) { g_up_sharp = mode < 0 || mode > 2 ? 0 : mode; }   // F3 Settings (#207, owner 31.10)
void md_settings_set_fxaa(bool on) { g_opt_fxaa = on; }   // F3 Settings (#180)
void md_settings_set_dyn_res(float scale) { g_dynres_on = scale > 0.f; if (scale > 0.f) g_dynres_scale = scale < 0.5f ? 0.5f : (scale > 1.f ? 1.f : scale); }   // F3 Settings (#182): 0 = off
int  md_settings_get_ground_reuse_chain() { return g_opt_ground_reuse_chain; }   // F3 Settings (#207): 0 / 3 / 5
void md_settings_set_ground_reuse_chain(int c) { g_opt_ground_reuse_chain = (c == 5) ? 5 : (c == 3) ? 3 : 0; SceneRender::Get().gr_reset_ = true; }
int  md_settings_get_ground_reuse_period() { return g_opt_ground_reuse_period; }   // F3 Settings (#191): 4 / 8 / 16
void md_settings_set_ground_reuse_period(int p) { g_opt_ground_reuse_period = (p == 4) ? 4 : (p == 8) ? 8 : 16; SceneRender::Get().gr_reset_ = true; }   // takes effect at once, the history restarts
bool md_settings_get_ground_reuse() { return g_opt_ground_split > 0 && g_opt_ground_reuse; }   // F3 Settings: the ground shading reuse (#163) needs the split of the passes
void md_settings_set_ground_reuse(bool on) { g_opt_ground_split = on ? 1 : 0; g_opt_ground_reuse = on; g_opt_ground_reuse_cr = on ? 3 : 0; }
extern float g_terrain_lod_near_px, g_terrain_lod_far_px, g_terrain_lod_near_dist, g_terrain_lod_far_dist;
void md_settings_set_terrain_lod(float detail, float distant, float threshold) {   // F3 Settings (#175): Kenshi's "terrain detail" / "terrain distant" (T = (1 - v) * 30 + 5 px) and "terrain threshold" (blend starts at the value in m, ends at twice that)
    g_terrain_lod_near_px = (1.f - detail) * 30.f + 5.f; g_terrain_lod_far_px = (1.f - distant) * 30.f + 5.f;
    g_terrain_lod_near_dist = threshold; g_terrain_lod_far_dist = 2.f * threshold;
}
extern int g_terrain_ground_aniso_level;
int  md_settings_get_ground_reuse_cr() { return g_opt_ground_reuse_cr; }   // #193 presets: 3 = history rejected at fractional motion > 0.25 px (default), 1 = never rejected (0.5 px)
void md_settings_set_ground_reuse_cr(int cr) { g_opt_ground_reuse_cr = cr; }
// #193 "Ground quality" (owner 22.10, aniso1 removed 22.10 #194): n 0 dyn075_a1 = footprint mip + resolution 0.75 while moving (default); 1 dyn075 = aniso x16 + resolution 0.75 while moving;
// 2 reuse05 = aniso x16, resolution fixed, history never rejected. False for n out of range.
bool md_settings_ground_preset(int n, int* aniso, float* dyn_scale, int* cr) {
    static const int   kA[3] = { 1, 16, 16 }; static const float kD[3] = { 0.75f, 0.75f, 0.f }; static const int kC[3] = { 3, 3, 1 };
    if (n < 0 || n > 2) return false;
    *aniso = kA[n]; *dyn_scale = kD[n]; *cr = kC[n]; return true;
}
bool md_settings_set_ground_preset(int n) {
    int a, c; float d; if (!md_settings_ground_preset(n, &a, &d, &c)) return false;
    g_terrain_ground_aniso_level = a; g_dynres_on = d > 0.f; if (d > 0.f) g_dynres_scale = d; g_opt_ground_reuse_cr = c;
    fprintf(stderr, "[GroundPreset] %d: aniso x%d, dyn_res %.2f, reuse cr %d\n", n, a, d, c); return true;
}
void md_settings_set_ground_aniso(int level) { g_terrain_ground_aniso_level = level < 1 ? 1 : (level > 16 ? 16 : level); }   // F3 Settings (#180): 1 = mip from the pixel footprint, 4 / 16 = + anisotropy
void md_settings_set_view_distance(float v) { md::g_view_distance_m = v < 1500.f ? 1500.f : (v > 12000.f ? 12000.f : v); }   // F3 Settings (#175): Kenshi's "view distance" 1500...12000
void md_settings_set_town_foliage(bool on, float range_m) { TownFoliage::Get().SetEnabled(on, range_m); }   // F3 Settings (#120): MEDIUM plants and rocks of the zones around the camera, Kenshi range 100 m x setting
void md_settings_set_town_grass(bool on, float range_m, float density) { TownFoliage::Get().SetGrass(on, range_m, density); }   // F3 Settings (#120): CLOSE grass blades round the camera (needs Plants and rocks on)
void md_settings_set_town_buildings(bool on, float range_m) { TownBuildings::Get().SetEnabled(on, range_m); }   // F3 Settings (#119): buildings of the zones around the camera, Kenshi `objects view range` in metres
void md_settings_set_fps_cap(int fps) { window_set_fps_cap(fps); }   // F3 Settings (#191, #207): -1 auto (default) / 30 / 40 / 60 / 0 = unlimited
void md_settings_set_vsync(bool on) { window_set_vsync(on ? 60 : 0); }   // F3 Settings (#175): Kenshi's VSync (default No = the engine's mailbox)
void md_settings_set_window_size(int w, int h) { window_set_size(w, h); }   // F3 Settings (#175)
int  md_settings_get_terrain_chunk() { return g_terrain_lod_patch; }
void md_settings_set_terrain_chunk(int cells) { g_terrain_lod_patch = (cells == 64) ? 64 : 16; }

