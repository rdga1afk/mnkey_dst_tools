#pragma once
#include <monkey_dust/render/gpu_pipeline.h>
#include "upscale_pass.h"
#include "ground_split_pass.h"
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/render/terrain_shading_projected.h>
#include <monkey_dust/render/terrain_skyvis_ao.h>
#include <monkey_dust/render/terrain_quadtree_renderer.h>
#include <monkey_dust/world/terrain_quadtree.h>
#include <monkey_dust/render/prop_renderer.h>
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/physics/jolt_world.h>
#include <SDL3/SDL_gpu.h>
#include <cstdint>

static constexpr int SCENE_TNKN = 9; // 9×9 Kenshi zone window (4500m visibility)

// ── Per-frame UBO structs ────────────────────────────────────────────────────

struct alignas(16) SkyUBO {
    float cam_right[4];
    float cam_up[4];
    float cam_fwd[4];
    float sun_dir[4];
    float horizon_col[4];
    float fov_tan;
    float aspect;
    float _pad[2];
};
static_assert(sizeof(SkyUBO) == 96, "SkyUBO size");

struct alignas(16) SolidVertUBO { float viewProj[16]; float model[16]; };
struct alignas(16) SolidFragUBO { float viewProj[16]; float model[16]; float solidColor[4]; };

// ── SceneRender ──────────────────────────────────────────────────────────────
// Owns terrain, props, sky/UI pipelines, and camera recording state.
// Populated by Init(); fields accessed by main.cpp and npc_render.cpp.
struct SceneRender {
    static SceneRender& Get() { static SceneRender s; return s; }

    static constexpr int TNKN           = 9;
    static constexpr int CAM_BTN_SZ     = 72;
    static constexpr int CAM_BTN_MARGIN = 14;

    // Zone streaming state.
    // tnoff_x/z are an ARBITRARY FLOATING LOCAL ORIGIN for the loaded TNKN×TNKN
    // chunk window -- NOT a real Kenshi world-meters position (task #158i-5/9,
    // coordinate audit). Chunk world position = tnoff_x + coord.x*CHUNK_SIZE +
    // CHUNK_SIZE/2 (terrain_gen.cpp), decoupled from zone_ox/oz (which only
    // selects WHICH real Kenshi zone's .r32 data streams into that window —
    // see Init()'s WorldRegistry lookup below). Any code that needs a world
    // position for TerrainQuery/physics/spawn must derive it from THIS origin
    // (see HandleTerrainStreaming's cx_w/cz_w, main.cpp), never from
    // zone_ox*CHUNK_SIZE or any other "absolute Kenshi metres" assumption —
    // that mismatch (editor uses true absolute metres since EDITOR_TNKN=64
    // loads the whole world) previously spawned the player ~10km outside the
    // loaded window (task #158, floating-in-sky bug). The literal 500.f below
    // is a legacy constant (Kenshi's raw zone size) kept only because nothing
    // depends on tnoff_x's specific value, only its self-consistent use.
    float tnoff_x = -(TNKN / 2) * 500.f;
    float tnoff_z = -(TNKN / 2) * 500.f;
    int   zone_ox = 25;
    int   zone_oz = 27;
    int   base_cx = 0;
    int   base_cz = 0;

    // Terrain data
    TerrainChunk    terrain_chunks[TNKN][TNKN];
    JPH::BodyID     terrain_jolt_ids[TNKN][TNKN];
    TerrainRenderer terrain;


    // Terrain geometry: Ogre-quadtree (geomorph+skirts) rewrite (plan:
    // /home/rdga1/.claude/plans/serene-pondering-teapot.md) -- THE game's
    // terrain renderer. Replaced GEOCLIPMAP (toroidal clipmap cache) and,
    // before that, a vertex-buffer-less Geomipmapping patch-grid -- both
    // measured/dual-run against this system and removed after live A/B
    // confirmed parity (this cutover additionally fixed a real geomorph
    // banding bug the patch-grid/clipmap systems never had to deal with,
    // see terrain_quadtree.cpp's morph-formula doc comment). ONE static
    // world-wide heightmap (no per-window rebuild, no per-level cache) --
    // TerrainQuadtree::SelectVisible recomputes visible nodes purely from
    // camera+frustum every frame (see that class's own doc comment for why
    // that's safe: nothing to page, the whole world is one fully-resident
    // VTF texture). Entirely in ABSOLUTE Kenshi-world space
    // (TerrainWorldHeightmap's native [0,extent) convention), NOT
    // tnoff_x/z's floating local window -- see UpdateGraniteTerrain's doc
    // comment in scene_render.cpp for the local-to-absolute camera
    // conversion this requires. Field/function names below keep the
    // "granite"/"Granite" prefix from the original migration deliberately
    // (matches the world-space-camera-conversion machinery they name) even
    // though the geometry system itself has changed twice since.
    TerrainWorldHeightmap terrain_granite_hmap;
    bool  terrain_granite_ready   = false;

    // TERRAIN_CA_REBUILD_PROMPT.md Phase 4 -- Variant A (screen-space
    // decoupled shading). Originally coexisted with a baked geometry path
    // and inline forward shading (Variant B) behind runtime flags
    // (MD_TERRAIN_PATH/MD_TERRAIN_SHADING); both alternates measured
    // against this one and were removed 2026-08-02 after VTF+Variant A
    // was locked in as the sole terrain path (user decision, not the
    // faster Variant B measured in Phase 4 -- see terrain_research/perf/
    // PROGRESS.md for the full history and why).
    TerrainShadingProjected terrain_shading_projected_;

    // КРОК 7 (docs/KROK7_SKYVIS_AO_DESIGN.md): toroidal heightfield-
    // horizon-sample sky-visibility AO. Reads terrain_shading_projected_'s
    // own gbuf_color_/gbuf_depth_ (Variant A screen-space G-buffer, always
    // populated regardless of geometry path -- see TerrainShadingProjected's
    // own doc comment) + terrain_granite_hmap for probe update sampling.
    TerrainSkyVisAO terrain_skyvis_ao_;

    // БОРГ-TERRAIN-2 (2026-09-13): TerrainVtPageCache (terrain_vt_cache)
    // and terrain_vt_ready removed. Confirmed fully inert since
    // 2026-08-09 (shading resolve never read the VT atlas/indirection --
    // see terrain_shading_screenspace.frag's git history) before this
    // removal; deleting it also let DrawShadingResolve drop its `vt`
    // parameter and 2 fragment samplers + 1 SSBO the pipeline no longer
    // needs.

    // Ogre-quadtree (geomorph+skirts) terrain geometry -- THE sole draw
    // path (see terrain_granite_hmap's own doc comment above for the full
    // migration history). No persistent tree state and no per-frame
    // LOD-update step: TerrainQuadtree::SelectVisible recomputes visible
    // nodes purely from camera+frustum at draw time (see terrain_quadtree.h's
    // own doc comment for why this is safe -- the whole world is one
    // fully-resident VTF texture, nothing to page/cache). Output contract
    // identical to every prior terrain renderer this project has had --
    // unmodified terrain_gbuffer_mini.frag / unmodified
    // terrain_shading_projected_ resolve pass.
    TerrainQuadtree         terrain_quadtree_;
    TerrainQuadtreeRenderer terrain_quadtree_renderer_;
    bool terrain_quadtree_ready_ = false;

    // #400 (2026-08-23): real per-frame SelectVisible() node count, set
    // right after the call site (npc_render_frame_prep.cpp) -- lets
    // md.granite_terrain_stats() report an ACTUAL draw-call count instead
    // of the pre-#400 stub (always 1 per depth level).
    int terrain_quadtree_last_visible_count_ = 0;


    // terrain-perf-measure (2026-08-12): falsification probe for the
    // resolution-scaling GPU-timing test -- re-runs the resolve pass this
    // many times per frame (default 1, real content unaffected visually
    // since later draws just overwrite the same pixels) so a fixed-
    // resolution "multiply the work" test can distinguish real linear GPU
    // cost growth from a period-locked measurement floor. md.set_terrain_
    // resolve_repeat(n). NOT for shipping -- always 1 outside this test.
    // 0 is a distinct ablation value (2026-08-13): skips the resolve draw
    // entirely rather than falling back to 1 -- gpu_ms(0) vs gpu_ms(1) gives
    // the pass's direct frame contribution. N>=2 measures the pass's cost
    // in a serialized same-target chain, which can't overlap with other GPU
    // work the way one real invocation does in a normal frame -- treat N>=2
    // slopes as an upper bound on the pass's true isolated cost, not the
    // real number; 0-vs-1 is the number that actually matters for sizing a
    // fix like half-res resolve (#396).
    int terrain_resolve_repeat_debug = 1;


    // resolution_scale (#253, 2026-10-04): internal 3D render resolution as a fraction of the window. Default 1.0 = native (#114, 13.10; the goal is
    // native 1080p; AUTO stays selectable with md.set_resolution_scale(0), the player-facing upscaling is #156). 0 = AUTO:
    // 1.0 up to a 1366x768 window (the default window size), above that 720/height clamped to 0.5..1 -- so switching
    // 1366x768 -> 1920x1080 renders the scene at 0.667 (1280x720) and upscales it. 0.5..1.0 = fixed override
    // (md.set_resolution_scale(s); md.set_resolution_scale(0) back to auto), 1.0 = off. The scene (terrain G-buffer +
    // resolve, props, particles/rings in the scene pass) renders into upscale_pass_'s target and is upscaled
    // (bilinear + upscale_sharpness) to the swapchain; 2D overlays + editor ImGui are drawn afterwards at full
    // resolution (DrawFullResUi). v1 limits: only the forward path with no deferred G-buffer (the
    // Intel HD 520 default tier); otherwise it silently stays at 1.0.
    float resolution_scale  = 1.0f;
    float upscale_sharpness = 0.3f;
    UpscalePass upscale_pass_;
    // #163 5b-2: the ground resolve in its own pass (md.set_opt_ground_split); ground_split_active_ is decided once per frame in DrawScene and read by DrawTerrainAndProps.
    GroundSplitPass ground_split_pass_;
    bool ground_split_active_ = false;
    uint32_t frame_counter_pvt_ = 0;   // #207 PVT: frames planned so far
    bool lum_adapt_active_ = false;   // #140 L4 (md.set_kenshi_lighting(4)): this frame's ground target is before the exposure, GroundSplitPass::Adapt + CompositeExposed
    // #163 5b-2b ground reuse (md.set_opt_ground_reuse): the camera of the previous ground pass, the parameters of this frame's resolve, and the frame counter that rotates the live blocks.
    TerrainShadingProjected::ReuseParams gr_params_;
    float gr_prev_proj_[16] = {};
    double gr_prev_rot_[9] = {}, gr_prev_eye_[3] = {};
    bool  still_skip_ = false; uint64_t still_key_ = 0; uint32_t still_n_ = 0;   // #207 H14 (MD_STILL_REUSE): this frame skips the G-buffer + the ground passes (same view key as the last full frame)
    bool  depth_alias_ = false;   // #207 H13.3 measurement (MD_DEPTH_NOCOPY): the scene depth is the G-buffer depth this frame, no copy
    bool  gr_have_ = false, gr_wanted_ = false, gr_reset_ = false;   // gr_reset_: md.ground_reuse_reset() / a player teleport: the next frame shades every block live
    unsigned gr_frame_ = 0;
    uint64_t gr_geom_hash_ = 0, gr_prev_geom_hash_ = 0;   // hash of this / the last frame's selected LOD nodes + fine window: a changed terrain under a still camera makes the history stale

    // TEMP DEBUG TOOL (2026-09-04, CLAUDE_CONSTITUTION.md sec 7.7): draws
    // the real terrain mesh (main grid + skirts + stitched-edge variant, on
    // top of the normally-shaded scene) via TerrainQuadtreeRenderer::
    // DrawNodeWireframe. md.set_terrain_wireframe(on). NOT for shipping.
    bool terrain_wireframe_debug_ = false;

    // Computes this frame's absolute-space camera position + frustum planes
    // for the Granite terrain draw. MUST be called before DrawScene's
    // passes begin -- see RenderFrame's call site. No-op if
    // !terrain_granite_ready.
    void UpdateGraniteTerrain(md::GpuCommandBufferHandle cmd, const MdCamera& cam, float aspect);
    // Absolute-space camera position UpdateGraniteTerrain computed this
    // frame -- DrawTerrainAndProps's draw calls reuse this (and
    // GraniteAbsCam() below for the matching view-proj) so both stay
    // consistent within the same frame.
    float granite_abs_cam_[3] = {};
    // 4 side planes (MdCamera::FrustumPlanes convention, no far plane),
    // computed once per frame in UpdateGraniteTerrain -- minimal-variant
    // Part B (per-tile culling): DrawTerrainAndProps's
    // TerrainClipmapRenderer::DrawLevel calls reuse this, for the same
    // reason granite_abs_cam_ above is cached per-frame instead of
    // recomputed at each call site.
    float terrain_frustum_planes_[16] = {};
    MdCamera GraniteAbsCam(const MdCamera& cam) const;


    // Sky / UI / water pipelines
    GpuPipeline sky_pipeline;
    GpuPipeline sky_kenshi_pipeline;   // #173: SkyX scattering (shaders/sky_kenshi.frag), md.set_kenshi_sky(1)
    GpuPipeline overlay_pipeline;
    GpuPipeline water_pipeline;
    GpuPipeline water_kenshi_pipeline;   // #173: water.hlsl of Kenshi (shaders/water_kenshi.frag), md.set_kenshi_water(1)
    struct alignas(16) WaterKenshiFragUBO { float sun_dir[4], cam_pos[4], sun_col[4], amb[4]; };   // sun_dir.xyz = toward the sun, sun_col.w = time, amb.w = invOpacity

    // G-3: real per-pixel water/land mask (task #158i-4/9). CPU samples
    // TerrainQuery::GetHeight on a grid covering the same [-world_sz,+world_sz]
    // square as the water quad, uploads it as a fragment SSBO each frame;
    // water.slang bilinear-samples it and fades out over dry terrain instead
    // of drawing the whole flat plane unconditionally.
    //
    // task water-mask-false-positive-bleed (2026-07-31): kWaterMaskN=32 over
    // the old kWaterWorldSz=3000m meant ~193m between grid samples -- any
    // real but small underwater pocket (anything below WATER_LEVEL+3m=1.0m
    // elevation, e.g. a few-metre canyon-floor puddle) that happened to land
    // near just one grid sample got bilinear-smeared by water.frag's
    // SampleWaterMask() across the whole ~193m neighbouring cell, painting
    // translucent blue water colour over completely dry, flat ground tens to
    // a hundred-plus metres from the real wet spot (user screenshots
    // 2026-07-31 18:21: thin blue line/triangle slivers on dry canyon-floor
    // ground). kWaterWorldSz shrunk 3000m->2200m to match what's actually
    // loaded/relevant (SCENE_TNKN=9 chunks * 500m = 4500m window, i.e. 2250m
    // half-extent -- 3000m was already oversized past real terrain data
    // before this fix) and kWaterMaskN doubled 32->64 (1024->4096 GetHeight
    // samples/frame, each a single cheap heightmap array lookup -- negligible
    // next to Init()'s own cost). Together: ~193m cells -> ~70m cells, a
    // ~2.8x cut in false-positive bleed radius. Both constants MUST stay
    // driven from here, not restated as separate literals at the mask-fill
    // (npc_render.cpp) and draw (water.vert's world_sz uniform) call sites --
    // that duplication is exactly how the mismatch between TNKN's real 2250m
    // window and the old 3000m literal went unnoticed for as long as it did.
    static constexpr int   kWaterMaskN   = 64;    // grid side length (16384 floats, 64KB)
    static constexpr float kWaterWorldSz = 2200.f; // half-size of water quad + mask grid, metres
    SSBO water_mask_ssbo;

    struct alignas(16) WaterVertUBO {
        float vp[16];
        float cam_x, cam_z, world_sz, time;
        float water_y, pad0, pad1, pad2;
    };
    struct alignas(16) WaterFragUBO {
        float sun_dir[4];
        float cam_pos[4];
        float time, opacity, pad0, pad1;
    };


    // Player solid sphere
    bool            scene_init      = false;

    // Camera recording
    GpuColorTexture cam_idle_tex;
    GpuColorTexture cam_rec_tex;
    GpuSampler      cam_sampler;
    bool            recording    = false;


    // Terrain sculpt (set by input / editor)
    bool  terrain_edit = false;
    float brush_r      = 30.f;
    float brush_str    = 3.f;

    void Init();
    void Shutdown();
    void StartRecording();
    void StopRecording();
    void WaitRecordingChild();

private:
    SceneRender() = default;

#ifndef _WIN32
#endif

    static void GenCamIcon(uint8_t* p, bool rec);
    static bool MakeCamTex(md::GpuDeviceHandle dev, const uint8_t* pixels, GpuColorTexture& out);
};
