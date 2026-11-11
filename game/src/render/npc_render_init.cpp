#include "town_buildings.h"
#include "town_foliage.h"
#include "npc_render_internal.h"
#include <monkey_dust/io/dds_decode.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/data_files.h>
#include <monkey_dust/render/human_body_build.h>
#include <vector>

PlayerClothSlot s_player_cloth[3];
GpuPipeline     s_cloth_game_pipeline;
PlayerHairState s_player_hair;
GpuPipeline     s_hair_game_pipeline;

// Body skin pixels: the bundled PNG when present, else the install's DXT5 (2048 -> 1024, same as the PNG copy).
static bool LoadBodyPixels(std::vector<uint8_t>& px, int& w, int& h) {
    const std::string png = md::DevFile("game/data/textures/md_human_body.png");
    if (!png.empty()) {
        int nc; unsigned char* d = stbi_load(png.c_str(), &w, &h, &nc, 4);
        if (!d) return false;
        px.assign(d, d + (size_t)w * h * 4); stbi_image_free(d);
        return true;
    }
    if (md::KenshiDir().empty()) return false;
    if (!md::DecodeDdsRgba(md::KenshiDir() + "/data/character/skins/human_male/human_male_body_diffuse.dds", w, h, px)) return false;
    md::BoxDownsample(px, w, h, 1);
    return true;
}

const char* NpcRender::ResolveBodyTexture() {
    // Only use bundled game asset — no external/system paths.
    static std::string primary_png;
    primary_png = md::DevFile("game/data/textures/md_human_body.png");
    return primary_png.empty() ? nullptr : primary_png.c_str();
}


bool ClothDrawReady() {
    return s_cloth_game_pipeline.SDLPipeline() &&
           AnimationSoA::Get().SDLBonesBuffer();
}

void InitClothPipeline() {
    GpuPipeline::Desc d;
    d.vert_path = "shaders/char_clothes_game.vert";
    d.frag_path = "shaders/char_clothes.frag";
    d.layout.count  = 5; d.layout.stride = 52;
    d.layout.attribs[0] = {0,  0, GpuAttribFmt::F3   };
    d.layout.attribs[1] = {1, 12, GpuAttribFmt::F3   };
    d.layout.attribs[2] = {2, 24, GpuAttribFmt::F2   };
    d.layout.attribs[3] = {3, 32, GpuAttribFmt::U8x4 };
    d.layout.attribs[4] = {4, 36, GpuAttribFmt::F4   };
    d.vert_storage_bufs = 1;
    d.vert_uniform_bufs = 1;
    d.frag_samplers     = 0;
    d.frag_uniform_bufs = 1;
    d.raster.depth_test  = true; d.raster.depth_write = true;
    d.raster.cull_back   = false;
    d.has_depth_target   = true;
    if (!s_cloth_game_pipeline.Create(d))
        fprintf(stderr,"[NpcCloth] pipeline failed\n");
}

// Rigid single-bone (head, idx=21) attach — hair mesh is pos+norm only, no
// joints/weights, so unlike cloth this reads exactly one FinalBonesBuf entry
// instead of 4-bone weighted skinning. Double-sided (cull_back=false), matches
// the editor's char_hair.vert/.frag reference (tools/editor/editor_char_preview_assets.cpp).
void InitHairPipeline() {
    GpuPipeline::Desc d;
    d.vert_path = "shaders/char_hair_game.vert";
    d.frag_path = "shaders/char_hair_game.frag";
    // PropVertex stride=36 (pos,norm,uv,layer) — only pos/norm declared; uv/layer unused.
    d.layout.count  = 2; d.layout.stride = 36;
    d.layout.attribs[0] = {0,  0, GpuAttribFmt::F3};
    d.layout.attribs[1] = {1, 12, GpuAttribFmt::F3};
    d.vert_storage_bufs = 1;
    d.vert_uniform_bufs = 1;
    d.frag_samplers     = 0;
    d.frag_uniform_bufs = 1;
    d.raster.depth_test  = true; d.raster.depth_write = true;
    d.raster.cull_back   = false;
    d.has_depth_target   = true;
    if (!s_hair_game_pipeline.Create(d))
        fprintf(stderr,"[NpcHair] pipeline failed\n");
}

// ── NpcRender::Shutdown ──────────────────────────────────────────────────────
// Never existed before -- NpcRender::Get() has real GPU-owning members
// (vis_buf/ind_buf/dead_mask_buf/dissolve_vis_buf SSBOs, vbuf/body_color_buf/
// ibuf GpuStaticBuffers, depth GpuDepthTexture, mesh SkinMesh) that
// ShutdownGame() never released -- only body_rdtex.Release() (a separate
// member) was ever called. Accounted for most of the remaining leak after
// SceneRender::Shutdown()/FeatureScatterSystem::Shutdown() were wired in
// (docs/SDLGPU_FENCE_OBJECT_LEAK_BUG.md §4-5): 846 -> 789 -> 594 -> 39,
// this closes nearly all of the last 39. pipeline/prepass_pipeline/
// dissolve_pipeline (GpuPipeline) deliberately excluded --
// already released by MdPipeCache_Shutdown(), same reasoning as
// SceneRender::Shutdown(). dummy_tex/dummy_samp deliberately excluded --
// owned by RdDevice, already released via RdDevice::Shutdown() at the
// ShutdownGame() call site. body_rdtex deliberately excluded -- already
// released via its own .Release() call at the same call site, before this
// function runs; releasing it again here would double-release.
// s_player_cloth[3]/s_player_hair are separate globals (this file, not
// NpcRender members) with the same gap -- SetPlayerClothSlot/SetPlayerHair
// load them but nothing ever released them at process exit either.
void NpcRender::Shutdown() {
    if (!init) return;
    TownFoliage::Get().Shutdown();
    TownBuildings::Get().Shutdown();   // joins the streaming worker before the static destructors run

    // vbuf/ibuf are non-owning copies of mesh.vbo/mesh.ibo (assigned at
    // Init(): "vbuf = mesh.vbo; ibuf = mesh.ibo;" for draw-call binding) --
    // mesh.Shutdown() is the sole owner and releases the real SDL_GPUBuffer
    // handles. Do NOT call vbuf.Shutdown()/ibuf.Shutdown(): they hold copies
    // of the same handle, so releasing both is a use-after-free/double-free
    // (confirmed via ASan on a locally-built SDL3, heap-use-after-free in
    // VULKAN_INTERNAL_ReleaseBufferContainer regardless of call order).
    mesh.Shutdown();
    vis_buf.Shutdown();
    ind_buf.Shutdown();
    dead_mask_buf.Shutdown();
    dissolve_vis_buf.Shutdown();
    body_color_buf.Shutdown();
    depth.Shutdown(); depth_alt.Shutdown();

    for (auto& cs : s_player_cloth) {
        cs.vbo.Shutdown();
        cs.ibo.Shutdown();
        cs.active = false;
    }
    s_player_hair.mesh.Shutdown();
    s_player_hair.loaded = false;

    init = false;
}

void NpcRender::Init() {
    if (init) return;
    init = true;

    // Load Kenshi human GLB (SkinVertex stride=44: pos+norm+joints+weights).
    // The body is built from the install as stored (#201 R3d, no clamps, no own morphs). MD_PURE_BODY=0 = old .glb if present (fallback, remove after 30.10).
    const char* pure = getenv("MD_PURE_BODY");
    const bool want_old = pure && pure[0] == '0';
    bool body_ok = want_old && mesh.LoadGLB("game/data/props/md_human.glb");
    if (!body_ok && !md::KenshiDir().empty()) {   // pure body / release: converted from the install at run time
        static HumanBodyData hb;
        body_ok = BuildHumanBody(md::KenshiDir(), hb) && mesh.LoadFromBody(hb);
    }
    if (body_ok) {
        if (!ozz.Init(mesh))
            fprintf(stderr, "[NPC] OzzAnimator::Init failed — falling back to legacy LBS\n");
        else
            fprintf(stderr, "[NPC] OzzAnimator ready: %d clips, %d bones\n",
                    ozz.ClipCount(), mesh.bone_count);
        vbuf     = mesh.vbo;
        ibuf     = mesh.ibo;
        index_count= mesh.index_count;
        idx_elem = mesh.indices_u16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                  : SDL_GPU_INDEXELEMENTSIZE_32BIT;
        sm_idle        = mesh.ClipIndexByName("idle_stand_normal");
        sm_idle_combat = mesh.ClipIndexByName("combatstance");
        sm_walk_lo     = mesh.ClipIndexByName("walk lower");
        sm_walk_hi   = mesh.ClipIndexByName("walk upper");
        sm_jog_lo    = mesh.ClipIndexByName("jog lower");
        sm_jog_hi    = mesh.ClipIndexByName("jog upper");
        sm_run_lo    = mesh.ClipIndexByName("run lower");
        sm_run_hi    = mesh.ClipIndexByName("run upper");
        sm_breathing = mesh.ClipIndexByName("breathing");
        sm_postures  = mesh.ClipIndexByName("postures");
        fprintf(stdout,"[Clips] idle=%d combat=%d walk=%d/%d jog=%d/%d run=%d/%d breath=%d postures=%d\n",
                sm_idle, sm_idle_combat, sm_walk_lo, sm_walk_hi,
                sm_jog_lo, sm_jog_hi, sm_run_lo, sm_run_hi, sm_breathing, sm_postures);

        // Pre-bake body texture colors per vertex (CPU nearest-sample at load time)
        // and interleave them into ONE combined vertex buffer alongside the
        // SkinVertex data (stride 52+4=56), instead of a second bound vertex
        // buffer. RenderDoc forensic capture (2026-07-21) proved the 2-VBO +
        // 4-SSBO vertex-stage binding count (6 total) intermittently corrupts
        // the second buffer's fetch on Intel ANV — surviving pixels showed the
        // exact sky clear color baked into body_color_buf reads, even though
        // both the CPU staging array and the uploaded GPU buffer contents were
        // independently verified byte-correct (PickPixel matched the corrupt
        // on-screen color; the staged/uploaded buffers had zero such values).
        // This is the SAME class of bug the body-color-via-vertex-attribute
        // workaround below was originally written for (see historical note
        // this replaces) — collapsing to a single VBO removes one binding
        // from the vertex stage (2+4 -> 1+4) rather than assuming vertex
        // attributes are immune outright.
        {
            const SkinVertex* verts = mesh.CpuVerts();
            uint32_t nv = mesh.VertCount();
            std::vector<uint8_t> bpx_v; int bw = 0, bh = 0;
            const bool have_body = LoadBodyPixels(bpx_v, bw, bh);
            if (verts && nv > 0) {
                const unsigned char* bpx = have_body ? bpx_v.data() : nullptr;
                uint8_t* combined = new uint8_t[(size_t)nv * 56];
                for (uint32_t i = 0; i < nv; ++i) {
                    memcpy(combined + (size_t)i * 56, &verts[i], 52);
                }
                if (bpx) {
                    for (uint32_t i = 0; i < nv; ++i) {
                        float u = verts[i].u - floorf(verts[i].u);
                        float v = verts[i].v - floorf(verts[i].v);
                        int tx = (int)(u * (float)bw); if (tx >= bw) tx = bw - 1;
                        int ty = (int)(v * (float)bh); if (ty >= bh) ty = bh - 1;
                        const unsigned char* p = bpx + (ty * bw + tx) * 4;
                        memcpy(combined + (size_t)i * 56 + 52, p, 4);
                    }
                    fprintf(stderr, "[NPC] body colors baked (combined VBO): %u verts from %dx%d\n",
                            nv, bw, bh);
                } else {
                    // stbi_load failed — fill grey so the combined buffer is never null
                    static const uint8_t grey[4] = {0x80,0x80,0x80,0xFF};
                    for (uint32_t i = 0; i < nv; ++i)
                        memcpy(combined + (size_t)i * 56 + 52, grey, 4);
                    fprintf(stderr, "[NPC] body tex missing — grey fallback (%u verts)\n", nv);
                }
                body_color_buf.Init(0x8892u, combined, nv * 56u);
                delete[] combined;
            }
        }
    } else {
        fprintf(stderr,"[NPC] kenshi GLB missing — NPC rendering disabled\n");
    }

    // Player body-slider proportions from player.chardef are applied in
    // InitGame (game/src/main.cpp) via CharBodyState/BodyBaseline — the same
    // pipeline SpawnFaction uses for NPCs (world_init.cpp). A face-morph-only
    // block used to live here (CharacterDef/CharDef_LoadJSON + a mismatched
    // JSON schema vs the on-disk player.chardef) but had no visual effect —
    // its one real side effect, writing mesh.morph_weights[], is only ever
    // consumed by SkinMesh::WriteMorphedVerts, which nothing in the game
    // runtime calls (editor-preview only). Removed; face-morph and skin/hair
    // tint parity with the editor's Characters tab remain open follow-ups.

    // Init Jolt Physics world
    JoltWorld::Get().Init(512);

    // Init ragdoll system (requires JoltWorld ready)
    RagdollSystem::Get().Init("game/data/props/md_human.glb");

    // Combined vertex buffer stride=56: SkinVertex (52B: pos/norm/uv/joints/
    // weights) + pre-baked body color (4B RGBA8 UNORM) appended per vertex,
    // in ONE buffer/slot — see body_color_buf bake block above for why this
    // was collapsed from a second bound vertex buffer.
    auto make_skin_layout = [](GpuPipeline::Desc& d) {
        d.layout.attribs[0] = { 0, 0,  GpuAttribFmt::F3       };  // aPos      loc=0
        d.layout.attribs[1] = { 2, 12, GpuAttribFmt::F3       };  // aNormal   loc=2
        d.layout.attribs[2] = { 5, 24, GpuAttribFmt::F2       };  // aTexCoord loc=5
        d.layout.attribs[3] = { 8, 32, GpuAttribFmt::U8x4     };  // aJoints   loc=8
        d.layout.attribs[4] = { 9, 36, GpuAttribFmt::F4       };  // aWeights  loc=9
        d.layout.attribs[5] = { 10, 52, GpuAttribFmt::U8x4_NORM }; // aBodyColor loc=10
        d.layout.count  = 6;
        d.layout.stride = 56;
    };

    // Load body texture via RdTexture — auto mipmap chain + Intel-safe sampler.
    {
        std::vector<uint8_t> px; int bw = 0, bh = 0;
        if (LoadBodyPixels(px, bw, bh)) {
            md::RdTexture::Desc bd; bd.width = (uint32_t)bw; bd.height = (uint32_t)bh; bd.gen_mipmaps = false;
            if (!body_rdtex.Upload(px.data(), bd)) fprintf(stderr, "[NPC] body texture upload failed\n");
        } else fprintf(stderr, "[NPC] body texture missing\n");
    }
    // RdDevice: detect Intel caps + create 1×1 white dummy for unbound slots.
    if (auto* dev = md::GpuDevice::Get().SDLDevice()) {
        md::RdDevice::Get().Init(dev);
        dummy_tex  = md::RdDevice::Get().DummyTex();
        dummy_samp = md::RdDevice::Get().DummySamp();
    }

    // SDL_GPU NPC graphics pipeline — forward shading pass (LEQUAL + depth write).
    // Previously used EQUAL+prepass; switched to LEQUAL for robustness on Intel/AMD
    // where depth precision prevents bit-exact match, causing black silhouettes.
    GpuPipeline::Desc d;
    d.vert_path = "shaders/animated.vert";
    d.frag_path = "shaders/npc_instanced.frag";
    d.vert_uniform_bufs = 1;
    d.vert_storage_bufs = 5;  // Transform Visible Faction Bones Skin
    d.frag_uniform_bufs = 1;
    d.frag_samplers     = 0;
    d.frag_storage_bufs = 0;
    d.has_depth_target  = true;
    d.raster.depth_compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    d.raster.depth_write      = true;
    make_skin_layout(d);
    bool npc_ok = pipeline.Create(d);
    fprintf(stderr, "[NPC-INIT] pipeline=%p ok=%d body_valid=%d mips=%u\n",
            (void*)pipeline.SDLPipeline(), (int)npc_ok,
            (int)body_rdtex.Valid(), body_rdtex.MipLevels());

    // VBfA-R9: dissolve variant — death/despawn effect with noise clip.
    // Uses alphatest-like discard (not OIT: dissolving chars never transparent).
    {
        GpuPipeline::Desc dv = d;
        dv.frag_path         = "shaders/animated_dissolve.frag";
        dv.frag_samplers     = 0;              // body via vBodySample from vertex
        dv.frag_storage_bufs = 0;
        dv.frag_uniform_bufs = 1;              // DissolveUBO
        dv.shader_features   = SF_Dissolve;
        dissolve_pipeline.Create(dv);
    }

    // Early-Z prepass pipeline.
    // vert_path uses a depth-biased copy of animated.vert (animated_prepass.slang),
    // not the same shader as the main color pass — see that file's header
    // comment for why: pairing the "same" vertex shader with a different
    // fragment shader (shadow_csm.frag here vs npc_instanced.frag in the main
    // draw) let the driver's codegen diverge just enough to occasionally fail
    // the main draw's LESS_OR_EQUAL depth test against this prepass, leaving
    // sky-colored gaps on the character silhouette (confirmed via forced solid
    // fragment-color test — the gaps persisted regardless of fragment output,
    // proving depth rejection, not a color/texture bug).
    GpuPipeline::Desc dp;
    dp.vert_path         = "shaders/animated_prepass.vert";
    dp.frag_path         = "shaders/shadow_csm.frag";
    dp.vert_uniform_bufs = 1;
    dp.vert_storage_bufs = 4;  // Transform Visible Faction Bones
    dp.depth_only        = true;
    make_skin_layout(dp);
    prepass_pipeline.Create(dp);

    // Depth texture at current window size (fixed for now).
    int w = window_get_width(), h = window_get_height();
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;
    depth.Init(w, h, false);

    // resolution_scale (#253): upscale pipeline (the scene target itself is sized per frame, before the cmd buffer).
    SceneRender::Get().upscale_pass_.Init();
    SceneRender::Get().ground_split_pass_.Init();

    // Vis/ind buffers + shadow textures — only needed in pure SDL_GPU.
    // cull.comp's pipeline (GPU-driven visibility compaction) was removed
    // 2026-08-09 -- CullAndPrepass does this on CPU now (see its own doc
    // comment); the shader file itself is also deleted (was already
    // undispatched dead weight, no call sites, unlike terrain_page_fill.comp
    // which is real disabled-but-live VT infrastructure and was left alone).
    // Godot PR #64044: vis_buf holds 2 LOD-tier regions × MAX_ANIMATED_NPC each.
    // Region 0: T01 (lod_tier 0-1, 0-80m). Region 1: T2 (lod_tier 2, 80-150m).
    vis_buf.Init(2 * MAX_ANIMATED_NPC * (int)sizeof(uint32_t));
    ind_buf.Init(40, SSBO_INDIRECT);  // 2 × GpuDrawIndexedIndirect (2 × 20B)
    // VBfA-R9: dead mask (excludes dead NPCs from main draw) + dissolve vis buffer
    dead_mask_buf.Init(TransformSoA::MAX_SLOTS * (int)sizeof(uint32_t));
    dissolve_vis_buf.Init(MAX_DISSOLVE * (int)sizeof(uint32_t));
    ShadowSystem::Get().InitSDLGPU();
    InitClothPipeline();
    InitHairPipeline();

    // Render graph step 3.3: register the real live per-frame pass order as
    // metadata (documentation-as-code + startup ordering validation) — does
    // NOT change how any of these systems allocate/own their textures.
    // Mirrors NpcRender::RenderFrame's actual sequence: UploadAndSkin ->
    // ShadowSystem::Update -> CullAndPrepass -> DrawScene -> DrawDeferredPasses
    // (sky-visibility AO). See docs/FULL_AUDIT.md's render-target inventory —
    // this is the first real (non-flare_demo) use of RenderPassGraph.
    {
        auto& rg = md::RenderPassGraph::Get();
        rg.Register("cull_prepass");
        rg.Register("draw_scene");

        rg.DeclareWrite("cull_prepass", "scene_depth");

        rg.DeclareWrite("draw_scene", "scene_color");

        // render-pass-graph-2026-09-03: same JSON override call as
        // flare_demo's own RenderPassGraph setup (tools/flare_demo/main.cpp)
        // -- must run after ALL Register() calls above so the
        // names exist to match against.
        rg.LoadFromJSON(md::DevFile("data/render_settings.json").c_str());

        rg.Validate();
    }

    // RENDER-BACKEND-STAGE-2a (docs/RENDER_BACKEND_ABSTRACTION.md §5.2):
    // backend_ створюється тут (не в game_init.cpp per §6 -- §5.2's власна
    // виправлена версія, backend_ має існувати вже для першої під-стадії),
    // бо реальний GPU-виклик живе тут, у NpcRender, не в GameState. Callback
    // реєструється ОДИН раз -- див. render_backend_types.h's BackendStageFn
    // doc-коментар для обґрунтування callback-based IoC замість прямого
    // виклику (engine/ не може залежати від game/'s NpcRender напряму).
#if defined(MD_RENDER_BACKEND_GRANITE) && defined(MD_USE_GRANITE)
    // RENDER-BACKEND-STAGE-4 (docs/archive/GRANITE_IRENDERBACKEND_INTEGRATION.md
    // §2.4): GraniteBackend constructed, but deliberately ZERO
    // Set<Stage>PassCallback registered -- DrawShadowMaps/DrawScene/
    // DrawDeferredPasses/UploadAndSkin/CullAndPrepass below are all written
    // directly against SDL_GPU (SDL_GPUCommandBuffer/SDL_GPURenderPass),
    // not ported to Granite's Vulkan::CommandBuffer/Device API -- that's a
    // separate future phase (§4 "ЩО НЕ РОБИТИ", full feature parity).
    // Every RenderXxx()/UploadTransforms()/RunGpuCulling() call in
    // npc_render_deferred.cpp still runs every frame (pure-virtual
    // IRenderBackend dispatch, doesn't care which concrete backend) --
    // each one just hits GraniteBackend's own "no callback registered"
    // warning and no-ops. The game runs, window shows only Granite's own
    // swapchain clear color, no NPCs/terrain/shadows -- deliberate, not a
    // bug (verified live, docs/RENDER_BACKEND_ABSTRACTION.md §7.1.3).
    auto granite = std::make_unique<md::render_backend::GraniteBackend>();
    granite->Init();
    backend_ = std::move(granite);
#else
    auto sdlgpu = std::make_unique<md::render_backend::SdlGpuBackend>();
    sdlgpu->Init();
    sdlgpu->SetShadowPassCallback(
        [](void* user, const md::render_backend::RenderFrameParams& params) {
            (void)user;
            ShadowSystem::Get().Update(*params.camera, LightSystem::Get().sun_dir,
                                        params.viewport_w, params.viewport_h);
        },
        this);

    // RENDER-BACKEND-STAGE-2b/2f (docs/RENDER_BACKEND_ABSTRACTION.md §5):
    // NpcRender::DrawScene -- also draws terrain/props/particles, one call
    // site covers both sub-stages (render_backend.h's own RenderGBufferPass
    // doc comment). frame_ctx is an opaque void* alias of RenderFrame()'s
    // own FrameCtx& (private nested type, engine/ can't name it); cam_x_io/
    // cam_z_io are pointers so this callback's writes are visible to
    // RenderFrame()'s locals after backend_->RenderGBufferPass() returns
    // (render_backend_types.h's RenderFrameParams doc comment explains why).
    sdlgpu->SetGBufferPassCallback(
        [](void* user, const md::render_backend::RenderFrameParams& params) {
            auto* self = static_cast<NpcRender*>(user);
            auto* fc = static_cast<NpcRender::FrameCtx*>(params.frame_ctx);
            self->DrawScene(*fc, *params.camera, params.now_s, params.player_entity,
                             *params.cam_x_io, *params.cam_z_io, params.cam_az);
        },
        this);

    // RENDER-BACKEND-STAGE-2c/2d (docs/RENDER_BACKEND_ABSTRACTION.md §5):
    // NpcRender::DrawDeferredPasses -- sky-visibility AO as ONE
    // callback, see SdlGpuBackend::SetDeferredPassCallback's doc comment for
    // why this doesn't split across the 3 named interface methods.
    sdlgpu->SetDeferredPassCallback(
        [](void* user, const md::render_backend::RenderFrameParams& params) {
            auto* self = static_cast<NpcRender*>(user);
            auto* fc = static_cast<NpcRender::FrameCtx*>(params.frame_ctx);
            self->DrawDeferredPasses(*fc, *params.camera);
        },
        this);

    // RENDER-BACKEND-STAGE-2e (docs/RENDER_BACKEND_ABSTRACTION.md §5):
    // NpcRender::UploadAndSkin -- TransformSoA upload + GPU skinning as ONE
    // callback (SdlGpuBackend::SetUploadSkinCallback's doc comment explains
    // why). cam_x/cam_z here are plain value inputs, unlike DrawScene's
    // cam_x_io/cam_z_io output pointers.
    sdlgpu->SetUploadSkinCallback(
        [](void* user, const md::render_backend::RenderFrameParams& params) {
            auto* self = static_cast<NpcRender*>(user);
            auto* fc = static_cast<NpcRender::FrameCtx*>(params.frame_ctx);
            self->UploadAndSkin(*fc, *params.camera, params.now_s, params.dt,
                                 params.logic_accum, params.player_entity,
                                 params.cam_x, params.cam_z);
        },
        this);

    // RENDER-BACKEND-STAGE-2e: NpcRender::CullAndPrepass, clean 1:1 mapping.
    sdlgpu->SetCullCallback(
        [](void* user, const md::render_backend::RenderFrameParams& params) {
            auto* self = static_cast<NpcRender*>(user);
            auto* fc = static_cast<NpcRender::FrameCtx*>(params.frame_ctx);
            self->CullAndPrepass(*fc, *params.camera);
        },
        this);
    backend_ = std::move(sdlgpu);
#endif
}


