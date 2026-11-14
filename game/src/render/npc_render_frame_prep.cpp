#include <chrono>
#include "ground_live_driver.h"
#include "horizon_cull.h"
#include <monkey_dust/render/ground_slot.h>
#include <algorithm>
#include "town_buildings.h"
#include "sky_panorama.h"
#include <monkey_dust/render/kenshi_sky.h>
#include "town_foliage.h"
#include <monkey_dust/nav/nav_world.h>
#include "npc_render_internal.h"
float g_dump_pr[16] = {}, g_dump_cam[3] = {};
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_depth_texture.h>
#include <monkey_dust/render/gpu_pass_view.h>

// Kenshi step C: cliff normal-map channel flips (terrainfp4.hlsl:131-134), default ON, md.set_terrain_cliff_normal_flip(false) = old; carried as +100 on |pix_scale|.
bool g_terrain_cliff_normal_flip = true;
// Kenshi step D: nTS.xy *= N.y^4 on the detail normal (not in terrainfp4.hlsl); md.set_terrain_detail_n_atten(false) removes it. Carried as +200 on |pix_scale|.
// #154 phase 3 (md.set_terrain_lod / md.set_terrain_lod_params): Kenshi-style terrain LOD (default off)
bool  g_terrain_lod = true;
bool  g_terrain_lod_error = false; // LOD is on but unavailable (no KENSHI_DIR / fullmap.tif): the scenario driver stops on it
bool  g_opt_skip_flat_on_cliff = true; // #155 step 1: skip the flat layer chain where cliff_w >= 1 (pixel-identical), md.set_opt_skip_flat_on_cliff
int   g_terrain_lod_patch = 16;    // patch size in quads (16 or 64), md.set_terrain_lod_patch
extern int g_opt_ground_split, g_kenshi_lighting_mode;   // #207 H13.3
int   g_terrain_lod_levels[11] = {}; // nodes per level of the last LOD selection (stats)
extern float g_dynres_cur;
float g_terrain_lod_near_px = 5.f, g_terrain_lod_far_px = 35.f, g_terrain_lod_near_dist = 750.f, g_terrain_lod_far_dist = 1500.f;
bool g_terrain_detail_n_atten = true; // owner decision 2026-10-10: back ON (SLOPE_STREAKS_FIX step 1; bisection: N.y^4 damps slope streaks). md.set_terrain_detail_n_atten(false) = as terrainfp4.hlsl:265-268
// Divergence #7 prototype: ground-layer anisotropy level (engine global, 0 = off); any level >= 1 turns on the textureGrad path, carried as +400 on |pix_scale|.
extern int g_terrain_ground_aniso_level;
// Diagnostic: false removes min(light, 1) in TS_ApplyLighting (md.set_terrain_light_clamp); carried as +800 on |pix_scale|.
bool g_terrain_light_clamp = true;
// Zone-seam fix: per-layer ground mip + zone-blended cliff distortion (default ON, it is a bug fix); false = the old home-zone mip, carried as +3200 on |pix_scale|.
bool g_terrain_per_layer_mip = true;
void NpcRender::UploadAndSkin(FrameCtx& fc, const MdCamera& cam,
                              float now_s, float dt, float logic_accum,
                              MdEntity player_entity, float cam_x, float cam_z)
{
md::GpuProfiler::Get().BeginPass("Upload+Skinning");
fc.ac = TransformSoA::Get().active_count;
// VBfA lines 9042-9043: hard cap on draw calls per stream.
// 2 streams × 200 = 400 max. Excess NPCs skip rendering this frame.
if (fc.ac > MAX_DRAW_CALLS_TOTAL)
    fc.ac = MAX_DRAW_CALLS_TOTAL;
// Render-rate NPC position smoothing (60 FPS vs 10 TPS logic).
// Interpolates between consecutive logic-tick positions using logic_accum.
// No extrapolation: render stays inside [prev_tick, cur_tick] range.
// Avoids drift/snap that dead-reckoning accumulates during ORCA turns.
if (fc.ac > 0) {
    static float s_rx  [MAX_ANIMATED_NPC] = {};
    static float s_rz  [MAX_ANIMATED_NPC] = {};
    static float s_px  [MAX_ANIMATED_NPC] = {}; // prev-tick logic X
    static float s_pz  [MAX_ANIMATED_NPC] = {}; // prev-tick logic Z
    static float s_cx  [MAX_ANIMATED_NPC] = {}; // cur-tick  logic X
    static float s_cz  [MAX_ANIMATED_NPC] = {}; // cur-tick  logic Z
    // alpha: 0 = just after tick, 1 = just before next tick
    const float alpha = fminf(logic_accum / 0.1f, 1.f);
    // new_tick: true on the first render frame after each logic tick
    // (alpha resets from ~1 back to ~0). Only update s_px/s_cx then.
    static float s_prev_alpha = 1.f;
    const bool new_tick = (alpha < s_prev_alpha);
    s_prev_alpha = alpha;
    auto& tsoa = TransformSoA::Get();
    static auto q_p3_npc_render_1 = MdRegistry::Get().Raw().query().all<WorldTransform>().all<NavAgent>();
    MdEach(q_p3_npc_render_1, [&](const WorldTransform& tr, const NavAgent& nav) {
            int s = (int)tr.slot;
            if (s < 0 || s >= fc.ac) return;
            const bool has_vel = (nav.desired_vel_x != 0.f ||
                                  nav.desired_vel_z != 0.f);
            if (has_vel) {
                if (new_tick) {
                    if (tr.x != s_cx[s] || tr.z != s_cz[s]) {
                        float jx = tr.x - s_cx[s], jz = tr.z - s_cz[s];
                        if (jx*jx + jz*jz > 4.f) {
                            // Teleport/init: snap immediately
                            s_px[s] = s_cx[s] = tr.x;
                            s_pz[s] = s_cz[s] = tr.z;
                        } else {
                            s_px[s] = s_cx[s]; s_pz[s] = s_cz[s];
                            s_cx[s] = tr.x;    s_cz[s] = tr.z;
                        }
                    } else {
                        // BT skipped — position frozen this tick.
                        // Advance s_px → s_cx so alpha=0 renders at
                        // s_cx (not behind it). Eliminates the
                        // backwards snap caused by AIBudget throttling.
                        s_px[s] = s_cx[s]; s_pz[s] = s_cz[s];
                    }
                }
                s_rx[s] = s_px[s] + (s_cx[s] - s_px[s]) * alpha;
                s_rz[s] = s_pz[s] + (s_cz[s] - s_pz[s]) * alpha;
            } else {
                // Stopped / player: snap and reset interpolation state
                s_rx[s] = s_px[s] = s_cx[s] = tr.x;
                s_rz[s] = s_pz[s] = s_cz[s] = tr.z;
            }
            tsoa.px[s] = s_rx[s];
            tsoa.pz[s] = s_rz[s];
            // Y: mirror whichever X/Z path this entity just took above.
            // has_vel (AI/ORCA-driven) entities interpolate X/Z between the
            // two most recent tick-boundary positions, so lerping Y between
            // the SAME two tick-boundary heights (s_py/s_cy) stays consistent
            // with them — no visible desync since both lag together.
            // The snap branch (has_vel==false — the player's WASD movement
            // sets is_moving/move_speed, never desired_vel_x/z, so it ALWAYS
            // lands here) instead snaps X/Z straight to the CURRENT tr.x/tr.z
            // every render frame (60Hz, no lag) — but tr.y only advances once
            // per 10Hz logic tick (TickTerrainGrounding). Continuing to lerp
            // Y here left it up to 100ms stale relative to the already-fresh
            // X/Z, most visible as the legs/pelvis visually sinking into an
            // uphill slope while moving, correcting the instant the entity
            // next stopped and the branch's own snap converged — task
            // terrain-patches, confirmed by process of elimination after two
            // other fix attempts (render-side player Y sync in main.cpp, and
            // the camera's own height clamps) turned out not to be it.
            static float s_py[MAX_ANIMATED_NPC] = {};
            static float s_cy[MAX_ANIMATED_NPC] = {};
            if (has_vel) {
                if (new_tick) { s_py[s] = s_cy[s]; s_cy[s] = tr.y; }
                tsoa.py[s] = s_py[s] + (s_cy[s] - s_py[s]) * alpha;
            } else {
                float fresh_y;
                tsoa.py[s] = TerrainQuery::Get().GetHeight(tr.x, tr.z, fresh_y)
                             ? fresh_y + 0.02f : tr.y;
                s_py[s] = s_cy[s] = tsoa.py[s];
            }
        });
    tsoa.UploadSDLGPU(fc.cmd);
}
// Animation frame counter: T1 NPCs eval at 30 FPS (every other render frame).
// T0 (<30m) always eval at full rate. s_all_bones is static — skipped slots
// retain previous-frame matrices automatically (no stale-clear needed).
static uint32_t s_anim_fc = 0;
++s_anim_fc;

// CPU LBS (ozz pattern): AnimatorComponent drives sampling, GPU reads final matrices.
if (mesh.loaded && AnimationSoA::Get().SDLBonesBuffer()) {
    static constexpr float ANIM_T0_SQ = 30.f  * 30.f;
    static constexpr float ANIM_T1_SQ = 80.f  * 80.f;
    // L2-style ActorCR: T2/T3 scale with actor_anim_t2_m
    const float ANIM_T2_SQ = RenderQualityConfig::Get().actor_anim_t2_m
                           * RenderQualityConfig::Get().actor_anim_t2_m;
    const float ANIM_T3_SQ = ANIM_T2_SQ * 4.f; // T3 = 2× T2

    // Scratch buffers for CPU LBS (T0/T1 inline eval).
    static float s_all_bones[MAX_ANIMATED_NPC * MAX_SKIN_BONES * 16];
    static float s_ik_worlds[MAX_SKIN_BONES * 16];
    static float s_model_xyz[MAX_SKIN_BONES * 3];
    static float s_pose_tmp [MAX_SKIN_BONES * 16];
    static const bool s_lo[MAX_SKIN_BONES] = {
        false, true,true,true,true,true,true,true,true,true,true,true,
    };

    // T2 async eval (80-150m): ozz Blend (upper+lower) or Sample (idle).
    // Each job owns its row — thread-safe via ozz thread_local scratch.
    struct AnimT2Job {
        OzzAnimator* ozz;
        int clip_hi, clip_lo, clip_idle;
        float phase, blend_t;
        float* row;
        const bool* lo_mask;
    };
    static AnimT2Job s_t2_jobs[MAX_ANIMATED_NPC];
    static int s_t2_count = 0;
    s_t2_count = 0;
    static auto eval_t2 = [](void* p) {
        auto* j = static_cast<AnimT2Job*>(p);
        if (j->blend_t >= 0.4f && j->clip_hi >= 0 && j->clip_lo >= 0) {
            j->ozz->Blend(j->clip_hi, j->phase,
                          j->clip_lo, j->phase,
                          j->lo_mask, j->row);
        } else {
            j->ozz->Sample(j->clip_idle >= 0 ? j->clip_idle : 0,
                           j->phase, j->row);
        }
    };

    int max_slot = -1;

    // NumWorkers()==0 means JobSystem::Init() was never called (e.g. test
    // binaries with no game bootstrap, or worker_threads=0 in a future
    // config) -- Submit()/Flush() on an uninitialized JobSystem hangs
    // forever, since inflight_ is incremented but no worker thread exists
    // to decrement it. Same pattern as sense_system.h's eval_sense_job.
    const bool jobsys_ok = JobSystem::Get().NumWorkers() > 0;

    static auto q_p3_npc_render_2 = MdRegistry::Get().Raw().query().all<WorldTransform>().all<NavAgent>().all<AnimatorComponent&>();
    MdEach(q_p3_npc_render_2, [&](MdEntity e, const WorldTransform& tr,
                  const NavAgent& nav, AnimatorComponent& anim) {
            const auto* body = MdRegistry::Get().Handle(e).try_get_mut<CharBodyState>();
            const float (*bone_sc)[3] = body ? body->bone_scales : nullptr;
            const float (*pos_sc)[3]  = body ? body->pos_scales  : nullptr;
            // Posture slider pose delta (see OzzAnimator::ApplyPostureDelta doc
            // comment) — Kenshi maps slider value to a time position on the
            // "postures" clip via t = clip_duration * slider * 0.01. No-op
            // (identity delta) when posture == neutral (35) or clip missing.
            int   posture_clip = -1;
            float posture_t_n = 0.f, posture_t_c = 0.f;
            if (sm_postures >= 0 && body) {
                const float dur = ozz.ClipDuration(sm_postures);
                posture_clip = sm_postures;
                posture_t_n  = dur * 0.35f;
                posture_t_c  = dur * (body->posture * 0.01f);
            }
            int slot = (int)tr.slot;
            if (slot < 0 || slot >= MAX_ANIMATED_NPC) return;
            if (slot > max_slot) max_slot = slot;
            float* row = s_all_bones + slot * MAX_SKIN_BONES * 16;

            const float _dx  = tr.x - cam.pos.x;
            const float _dz  = tr.z - cam.pos.z;
            const float _dsq = _dx*_dx + _dz*_dz;

            if (_dsq >= ANIM_T3_SQ) {
                anim.lod_tier = 4;
                AnimationSoA::Get().GetState(slot).lod_tier = 4;
                return;
            }
            if (_dsq >= ANIM_T2_SQ) {
                anim.lod_tier = 3;
                AnimationSoA::Get().GetState(slot).lod_tier = 3;
                return;
            }

            // Player sprint override.
            const bool is_player = (e == player_entity);
            if (is_player && nav.is_moving && player_sprinting) {
                anim.clip_lo    = (int8_t)sm_jog_lo;
                anim.clip_hi    = (int8_t)sm_jog_hi;
                anim.anim_speed = 1.7f;
                anim.tgt_blend  = 1.0f;
            }

            const float anim_speed = anim.anim_speed > 0.f ? anim.anim_speed : 1.f;
            const float tgt_bt     = anim.tgt_blend;
            const int   lo_clip    = anim.clip_lo;
            const int   hi_clip    = anim.clip_hi;
            const int   ci         = anim.clip_idle >= 0 ? anim.clip_idle : sm_idle;
            // Idle phase: absolute time (consistent across all tiers).
            const float phase_idle = (now_s + slot * 0.37f) * anim_speed;

            // Phase advances BEFORE any early-return so T2 and T1/T0
            // always use the same anim.phase — prevents snap on LOD change.
            if (anim.blend_t > 0.01f && nav.is_moving)
                anim.phase += dt * anim_speed;

            // T2 (80-150m): async full-blend eval (both clips for correct full-body walk).
            if (_dsq >= ANIM_T1_SQ) {
                anim.lod_tier = 2;
                AnimationSoA::Get().GetState(slot).lod_tier = 2;
                if (s_t2_count < MAX_ANIMATED_NPC) {
                    anim.blend_t += (tgt_bt - anim.blend_t) * fminf(dt * 4.f, 1.f);
                    AnimT2Job& j = s_t2_jobs[s_t2_count];
                    j.ozz        = &ozz;
                    j.clip_hi    = hi_clip;
                    j.clip_lo    = lo_clip;
                    j.clip_idle  = ci;
                    // T2 idle: use phase_idle (now_s-based) so idle anim is smooth.
                    // T2 walk: use anim.phase (accumulated at render rate).
                    j.phase      = (anim.blend_t < 0.4f) ? phase_idle : anim.phase;
                    j.blend_t    = anim.blend_t;
                    j.row        = row;
                    j.lo_mask    = s_lo;
                    if (jobsys_ok) JobSystem::Get().Submit(eval_t2, &s_t2_jobs[s_t2_count]);
                    else eval_t2(&s_t2_jobs[s_t2_count]);
                    ++s_t2_count;
                }
                return;
            }

            // T0/T1 (<80m): full inline eval driven by AnimatorComponent.
            // T1 (30-80m): eval every other frame (≈30 FPS bones, 60 FPS position).
            // T0 (<30m):   eval every frame (60 FPS) — closest NPCs need it.
            const bool is_t0 = (_dsq < ANIM_T0_SQ);
            const uint8_t tier = is_t0 ? 0u : 1u;
            anim.lod_tier = tier;
            AnimationSoA::Get().GetState(slot).lod_tier = tier;
            if (!is_t0 && (s_anim_fc & 1u)) return;  // T1: skip odd frames
            const bool ik_ok = is_t0;

            const float blend_rate = (tgt_bt > anim.blend_t) ? 8.f : 20.f;
            anim.blend_t += (tgt_bt - anim.blend_t) * fminf(dt * blend_rate, 1.f);
            // phase already advanced above (before T2 check) — no duplicate here.
            const float bt      = anim.blend_t;
            const float move_ph = anim.phase;

            if (is_player && anim_debug) {
                player_anim_dbg.bt         = bt;
                player_anim_dbg.tgt_bt     = tgt_bt;
                player_anim_dbg.move_speed = nav.move_speed;
                player_anim_dbg.walk_ph    = move_ph;
                player_anim_dbg.anim_speed = anim_speed;
                player_anim_dbg.is_moving  = nav.is_moving;
                player_anim_dbg.use_jog    = (anim_speed > 1.5f);
                player_anim_dbg.is_sprint  = (is_player && player_sprinting);
                player_anim_dbg.desired_vx = nav.desired_vel_x;
                player_anim_dbg.desired_vz = nav.desired_vel_z;
            }

            if (bt >= 0.99f) {
                ozz.Blend(hi_clip, move_ph, lo_clip, move_ph, s_lo, row,
                          bone_sc, pos_sc, posture_clip, posture_t_n, posture_t_c);
            } else if (bt <= 0.01f) {
                ozz.Sample(ci >= 0 ? ci : 0, phase_idle, row,
                           ik_ok ? s_model_xyz : nullptr, bone_sc, pos_sc,
                           posture_clip, posture_t_n, posture_t_c);
            } else {
                ozz.Sample(ci >= 0 ? ci : 0, phase_idle, s_pose_tmp,
                           nullptr, bone_sc, pos_sc,
                           posture_clip, posture_t_n, posture_t_c);
                ozz.Blend(hi_clip, move_ph, lo_clip, move_ph, s_lo, row,
                          bone_sc, pos_sc, posture_clip, posture_t_n, posture_t_c);
                const int nm = MAX_SKIN_BONES * 16;
                for (int i = 0; i < nm; ++i)
                    row[i] = s_pose_tmp[i] + (row[i] - s_pose_tmp[i]) * bt;
            }

            // Breathing overlay (additive) — spine only via ozz::BlendAdditive.
            const int br_clip = anim.clip_breathing;
            if (br_clip >= 0) {
                const uint32_t ehash  = (uint32_t)(e.ToIntegral() * 2654435761u) >> 24;
                const float breath_ph = now_s * 0.38f + ehash * (6.28f / 256.f);
                const float breath_w  = ik_ok ? 0.02f : 0.01f;
                ozz.BlendAdditive(row, br_clip, breath_ph, breath_w);
            }

            // T0: foot IK disabled — s_ik_worlds not yet wired for ozz path.
            // TODO: OzzAnimator::Sample needs to output world matrices for IK.
            if (false && ik_ok && bt <= 0.01f) {
                float lf_y = TerrainQuery::Get().GetHeight(
                    tr.x + s_model_xyz[FootIK::L_FOOT*3+0],
                    tr.z + s_model_xyz[FootIK::L_FOOT*3+2])
                    + s_model_xyz[FootIK::L_FOOT*3+1];
                float rf_y = TerrainQuery::Get().GetHeight(
                    tr.x + s_model_xyz[FootIK::R_FOOT*3+0],
                    tr.z + s_model_xyz[FootIK::R_FOOT*3+2])
                    + s_model_xyz[FootIK::R_FOOT*3+1];
                static float s_hw[16], s_kw[16], s_aw[16];
                if (FootIK::SolveLeg(s_model_xyz, s_ik_worlds,
                        FootIK::L_THIGH, FootIK::L_CALF, FootIK::L_FOOT,
                        tr.y, lf_y, s_hw, s_kw, s_aw)) {
                    mesh.PatchBoneIK(FootIK::L_THIGH, s_hw, row);
                    mesh.PatchBoneIK(FootIK::L_CALF,  s_kw, row);
                    mesh.PatchBoneIK(FootIK::L_FOOT,  s_aw, row);
                }
                if (FootIK::SolveLeg(s_model_xyz, s_ik_worlds,
                        FootIK::R_THIGH, FootIK::R_CALF, FootIK::R_FOOT,
                        tr.y, rf_y, s_hw, s_kw, s_aw)) {
                    mesh.PatchBoneIK(FootIK::R_THIGH, s_hw, row);
                    mesh.PatchBoneIK(FootIK::R_CALF,  s_kw, row);
                    mesh.PatchBoneIK(FootIK::R_FOOT,  s_aw, row);
                }
            }
        });

    if (jobsys_ok) JobSystem::Get().Flush();
    if (max_slot >= 0) {
        const int upload_bytes = (max_slot + 1) * MAX_SKIN_BONES * 16 * (int)sizeof(float);
        AnimationSoA::Get().UploadBonesInCmd(fc.cmd, s_all_bones, upload_bytes);
    }
}
                    md::GpuProfiler::Get().EndPass(); // Upload+Skinning
fc.p_count = ParticleRenderer::Get().PrepareSDLGPU(fc.cmd, cam.pos);
}

void NpcRender::CullAndPrepass(FrameCtx& fc, const MdCamera& cam)
{
// render-pass-graph-2026-09-03: t01_count_/t2_count_/vis_buf are class
// members, not locals -- returning here leaves them at last frame's
// values (stale visibility/counts, not uninitialized), same accepted
// "opt-out, previous state persists" behavior as evsm_shadow's own gate
// (npc_render_init.cpp). Debug/profiling toggle to isolate this CPU
// cull loop's cost; downstream draw sites still read whatever t01_count_/
// t2_count_ were last set to.
if (!md::RenderPassGraph::Get().IsEnabled("cull_prepass")) return;
// NPC cull — 2 LOD-tier draw commands. CPU, not GPU compute, since 2026-08-09
// (user directive, part 2/3 of the "shrink Filament-blocker surface" work
// alongside OIT removal and ZoneGroundLayers SSBO->texture -- see
// docs/analysis/FILAMENT_MIGRATION_ANALYSIS.md). MAX_ANIMATED_NPC=500
// (animation_soa.h) keeps this a trivial linear scan on CPU -- same order
// of magnitude of the old CPU occlusion pass.
// As of the same date, the 3 draw sites that used to read GPU-computed
// indirect-draw args from `ind_buf` (this file's depth prepass below,
// npc_render_draw_scene.cpp's main pass, npc_render_init.cpp's EVSM shadow
// moment pass) now issue direct SDL_DrawGPUIndexedPrimitives calls with
// t01_count_/t2_count_ read straight from these CPU members -- indirect
// draw for NPCs is gone (`ind_buf` itself, and cull.comp, are left in the
// tree unused/undispatched, not deleted -- smaller, easier-to-revert diff).
// shadow_system.h's SEPARATE shadow_cull.comp/shadow_ind_buf_ (CSM cascade
// culling, not this system) is untouched -- explicitly out of scope.
if (fc.ac > 0 && index_count > 0) {
    // Hoisted out of the block below (was locally `static` there) -- the
    // CPU cull loop after it needs to read this same array too, now that
    // both live in this function instead of dead-mask-build (CPU) feeding
    // a separate GPU compute dispatch that read it independently.
    static uint32_t dead_mask[TransformSoA::MAX_SLOTS];
    // dead mask + dissolve slot list: no combat in the world, all slots alive
    {
        memset(dead_mask, 0, (size_t)fc.ac * sizeof(uint32_t));
        n_dissolving = 0;
        dead_mask_buf.UploadInCmd(fc.cmd, dead_mask,
            fc.ac * (int)sizeof(uint32_t));
        if (n_dissolving > 0)
            dissolve_vis_buf.UploadInCmd(fc.cmd, dissolve_slots,
                n_dissolving * (int)sizeof(uint32_t));
    }
    // CPU cull -- exact port of cull.comp's per-slot test (frustum + far_sq
    // + dead_mask + tier split), replacing the compute dispatch that used
    // to do this on GPU. near_sq was uploaded but never actually tested in
    // cull.comp's own main() -- not replicated here either, faithful port.
    float cam_pos[3]; cam.CamPosToArr(cam_pos);
    const float far_sq = RenderQualityConfig::Get().actor_cr_m * RenderQualityConfig::Get().actor_cr_m;
    float frustum[16];
    float r_asp = (float)window_get_width() / (float)window_get_height();
    cam.FrustumPlanes(r_asp, frustum);

    static uint32_t visible_indices[2 * MAX_ANIMATED_NPC];
    t01_count_ = 0; t2_count_ = 0;
    TransformSoA& tsoa = TransformSoA::Get();
    const int scan_count = fc.ac < MAX_ANIMATED_NPC ? fc.ac : MAX_ANIMATED_NPC;
    for (int idx = 0; idx < scan_count; ++idx) {
        if (dead_mask[idx] != 0u) continue;
        float px = tsoa.px[idx], py = tsoa.py[idx], pz = tsoa.pz[idx];
        if (px >= 1e8f) continue;
        float dx = px - cam_pos[0], dy = py - cam_pos[1], dz = pz - cam_pos[2];
        if (dx * dx + dy * dy + dz * dz >= far_sq) continue;
        bool culled = false;
        for (int p = 0; p < 4; ++p) {
            float d = frustum[p * 4 + 0] * px + frustum[p * 4 + 1] * py
                    + frustum[p * 4 + 2] * pz + frustum[p * 4 + 3];
            if (d < -1.0f) { culled = true; break; }
        }
        if (culled) continue;
        uint32_t lod_tier = AnimationSoA::Get().GetState(idx).lod_tier;
        if (lod_tier <= 1u) {
            if (t01_count_ < (uint32_t)MAX_ANIMATED_NPC)
                visible_indices[t01_count_++] = (uint32_t)idx;
        } else {
            if (t2_count_ < (uint32_t)MAX_ANIMATED_NPC)
                visible_indices[(uint32_t)MAX_ANIMATED_NPC + t2_count_++] = (uint32_t)idx;
        }
    }
    vis_buf.UploadInCmd(fc.cmd, visible_indices, (int)sizeof(visible_indices));
}

// ImGui: upload vertex/index data via copy pass — MUST be before any render pass.
#if defined(MONKEY_DUST_EDITOR)
{
    ImDrawData* _dd = ImGui::GetDrawData();
    if (_dd && _dd->CmdListsCount > 0)
        ImGui_ImplSDLGPU3_PrepareDrawData(_dd, fc.cmd);
}
#endif
auto& sr_prepass = SceneRender::Get();
bool npc_prepass_active = fc.ac > 0 && prepass_pipeline.SDLPipeline()
                          && !npc_prepass_debug_disable;

// TERRAIN_CA_REBUILD_PROMPT.md Phase 4 -- Variant A G-buffer pass. Its own
// separate render pass (dedicated color+depth, NOT the shared `depth` used
// below -- see TerrainShadingProjected's doc comment for why an isolated
// depth target was chosen). Runs BEFORE the Early-Z prepass now (2026-08-10):
// this pass already computes real per-pixel terrain depth into gbuf_depth_,
// so instead of ALSO submitting the full CDLOD geometry a second time via
// DrawBatchDepthOnly (the old approach -- terrain drawn twice per frame,
// see git history), that depth is copied into the shared `depth` texture
// right after this pass, and the Early-Z prepass below only ever draws NPCs.
if (sr_prepass.terrain_granite_ready) {
    // render-audit-2026 (2026-08-29): CPU wall-clock timer, deliberately
    // separate from GpuProfiler::BeginPass below -- GpuProfiler measures
    // real GPU execution time via timestamp query, which is blind to a
    // CPU-side stall while RECORDING these commands (e.g. a blocking
    // driver call during SelectVisible/DrawBatched/DrawNode). This FS_
    // section is the first CPU-side measurement of this exact block --
    // previously only visible in aggregate inside the outer "Cull" FS
    // section. Investigating sporadic 30-110ms single-frame RenderTotal
    // spikes (docs/RENDER_AUDIT_2026.md §2.2/§2.2.1) that persisted after
    // ruling out GPU clock scaling (gt_min_freq_mhz locked to 1000, spikes
    // unchanged) and lazy pipeline/large-texture creation (zero such log
    // lines during the spike capture window).
    FS_BEGIN("TerrainPrepCPU");
    md::GpuProfiler::Get().BeginPass("Terrain GBuffer (Variant A)");
    float r_asp4 = (float)window_get_width() / (float)window_get_height();
    MdCamera g_abs_cam4 = sr_prepass.GraniteAbsCam(cam);
    Mat4 g_vp4 = g_abs_cam4.ViewProjMatrix(r_asp4);
    TownBuildings::Get().Tick(g_abs_cam4.pos.x, g_abs_cam4.pos.z);
    TownFoliage::Get().Tick(g_abs_cam4.pos.x, g_abs_cam4.pos.z);
    md::nav::NavWorld::Get().Tick(g_abs_cam4.pos.x, g_abs_cam4.pos.z);
    { extern int g_kenshi_sky; if (g_kenshi_sky) { const md::KenshiSkySettings ks; const md::KsVec3 real = ks.SunDirection(LightSystem::Get().current_hours); const float s3[3] = { real.x, real.y, real.z }; const md::KsVec3 zn = ks.ZenithLight(LightSystem::Get().current_hours); const auto ml = ks.GetMainLight(LightSystem::Get().current_hours); const float z3[3] = { zn.x, zn.y, zn.z }, c3[3] = { ml.colour.x, ml.colour.y, ml.colour.z }; SkyPanorama::Get().Update(fc.cmd, s3, z3, c3); } }   // #173: SkyX panorama, redrawn when due, before the scene pass
    {   // md.dump_depth: the projection * rotation of this frame (camera at the origin) and the absolute camera position, for the shimmer metric's reprojection
        MdCamera c0 = g_abs_cam4;
        c0.target = { c0.target.x - c0.pos.x, c0.target.y - c0.pos.y, c0.target.z - c0.pos.z };
        c0.pos = { 0.f, 0.f, 0.f };
        memcpy(g_dump_pr, mat4_ptr(c0.ViewProjMatrix(r_asp4)), 64);
        g_dump_cam[0] = g_abs_cam4.pos.x; g_dump_cam[1] = g_abs_cam4.pos.y; g_dump_cam[2] = g_abs_cam4.pos.z;
    }

    SDL_GPURenderPass* gbuf_pass = nullptr;
    GpuRenderPass s7_rp;     // S7 depth-only G-buffer pass (terrain_s7 toggle)
    bool s7_active = false;

    // Ogre-quadtree (geomorph+skirts) terrain: SelectVisible recomputes
        // visible nodes fresh every frame (no persistent tree state, see
        // terrain_quadtree_'s own doc comment in scene_render.h). Pure CPU
        // logic -- run BEFORE BeginGBufferPass so the node-data upload below
        // (a copy pass) can happen while no render pass is open yet (SDL_GPU
        // disallows nesting a copy pass inside an active render pass). static,
        // not a stack array -- kMaxNodesPublic(16384) * sizeof(VisibleNode) is
        // far too large for a per-frame stack frame (game/src/CLAUDE.md's
        // "fixed arrays, no heap in hot-path" rule means fixed STORAGE, not
        // necessarily stack -- same static-local convention the flecs
        // collect->apply pattern already uses elsewhere in this codebase).
        static TerrainQuadtree::VisibleNode visible_nodes[TerrainQuadtree::kMaxNodesPublic];
        // #154: the LOD tree replaces the flat zone tiling while md.set_terrain_lod is on and its tables (background build) are ready
        static md::TerrainLodNode lod_nodes[md::TerrainLodTree::kMaxNodes];
        sr_prepass.terrain_quadtree_renderer_.SetLodPatch(g_terrain_lod_patch);
        const bool lod_on = g_terrain_lod && sr_prepass.terrain_quadtree_renderer_.RequestLod(sr_prepass.terrain_granite_hmap);
        g_terrain_lod_error = g_terrain_lod && sr_prepass.terrain_quadtree_renderer_.LodUnavailable();
        int qt_count;
        sr_prepass.still_skip_ = false;
        if (lod_on) {
            md::TerrainLodParams lp;
            lp.near_px = g_terrain_lod_near_px; lp.far_px = g_terrain_lod_far_px;
            lp.near_dist = g_terrain_lod_near_dist; lp.far_dist = g_terrain_lod_far_dist;
            lp.max_render_distance = md::ViewFarClipM();
            lp.viewport_px = (float)sr_prepass.terrain_shading_projected_.GBufferColor().Height() / fmaxf(g_dynres_cur, 0.1f);   // the height at dynres 1, so the LOD does not change when dynres switches
            if (md::GroundLiveDriver::Wanted()) { static const int minl = getenv("MD_GROUND_LIVE_MINL") ? atoi(getenv("MD_GROUND_LIVE_MINL")) : 5; lp.min_level = minl; }   // level 5 = 921.6 m = one blendinfo cell: no node crosses a cell, so every node has an exact biome list (coordinator 6758553091)
            const auto sel_t0 = std::chrono::steady_clock::now();
            qt_count = sr_prepass.terrain_quadtree_renderer_.SelectLod(sr_prepass.granite_abs_cam_, sr_prepass.terrain_frustum_planes_, lp,
                                                                         lod_nodes, md::TerrainLodTree::kMaxNodes);
            const double sel_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sel_t0).count();
            static md::TerrainLodNode pre_cull_nodes[8192]; int n_pre_cull = 0;   // the selection before the horizon cull: it keeps deciding the array tier of the ground lists (AssignSlots)
            if (md::GroundLiveDriver::Wanted() && md::HorizonCullEnabled()) { n_pre_cull = std::min(qt_count, 8192); memcpy(pre_cull_nodes, lod_nodes, (size_t)n_pre_cull * sizeof(md::TerrainLodNode)); }
            qt_count = md::HorizonCullFilter(sr_prepass.granite_abs_cam_, lod_nodes, qt_count);   // #207 horizon map: drops nodes hidden behind terrain; off by default (md.set_horizon_cull)
            {   // #207 H13.4: the selected nodes are drawn nearest first (early-Z rejects more of the far terrain); default on (owner Gates-approved 30.10), MD_LOD_SORT=0 restores the old order
                static const bool lod_sort = !(getenv("MD_LOD_SORT") && atoi(getenv("MD_LOD_SORT")) == 0);
                if (lod_sort) {
                    const float cx = sr_prepass.granite_abs_cam_[0], cz = sr_prepass.granite_abs_cam_[2];
                    static const float bucket = getenv("MD_LOD_SORT_BUCKET") ? (float)atof(getenv("MD_LOD_SORT_BUCKET")) : 0.f;   // 0 = exact distance; > 0 = stable sort by distance bucket of that many metres
                    auto key = [cx, cz](const md::TerrainLodNode& p) { const float mx = p.origin_x + 0.5f * p.size - cx, mz = p.origin_z + 0.5f * p.size - cz; return sqrtf(mx * mx + mz * mz); };
                    std::stable_sort(lod_nodes, lod_nodes + qt_count, [&](const md::TerrainLodNode& p, const md::TerrainLodNode& q) {
                        const float pd = key(p), qd = key(q);
                        return bucket > 0.f ? floorf(pd / bucket) < floorf(qd / bucket) : pd < qd; });
                }
            }
            if (md::GroundLiveDriver::Wanted()) md::GroundLiveDriver::Get().AssignSlots(lod_nodes, qt_count, sr_prepass.granite_abs_cam_, pre_cull_nodes, n_pre_cull);   // before the nodes are uploaded
            for (int& c : g_terrain_lod_levels) c = 0;
            for (int i = 0; i < qt_count; ++i) ++g_terrain_lod_levels[lod_nodes[i].level];
            if (md::GroundLiveDriver::Wanted() && getenv("MD_GROUND_LIVE_COUNT")) {   // measurement: share of LOD nodes whose AABB corners lie in different blendinfo cells (921.6 m), where the original's list is empty
                static int fr = 0; if (++fr % 120 == 0) {
                    int cross = 0, lvl_n[16] = {}, lvl_c[16] = {}; double area = 0, area_c = 0, sa = 0, sa_c = 0; const float ccx = sr_prepass.granite_abs_cam_[0], ccy = sr_prepass.granite_abs_cam_[1], ccz = sr_prepass.granite_abs_cam_[2];
                    for (int i = 0; i < qt_count; ++i) {
                        const auto& nd = lod_nodes[i]; const int c0x = (int)floorf(nd.origin_x / 921.6f), c1x = (int)floorf((nd.origin_x + nd.size - 0.01f) / 921.6f), c0z = (int)floorf(nd.origin_z / 921.6f), c1z = (int)floorf((nd.origin_z + nd.size - 0.01f) / 921.6f);
                        const bool x = c0x != c1x || c0z != c1z; const int l = std::min(nd.level, 15); ++lvl_n[l]; area += (double)nd.size * nd.size; const double mx = nd.origin_x + 0.5 * nd.size - ccx, mz = nd.origin_z + 0.5 * nd.size - ccz, w = (double)nd.size * nd.size / (mx * mx + mz * mz + ccy * ccy + 1.0); sa += w; if (x) { ++cross; ++lvl_c[l]; area_c += (double)nd.size * nd.size; sa_c += w; }
                    }
                    double gap = 0; int pairs = 0;   // seam check: same-size neighbours share their edge lattice exactly (origin_B - (origin_A + size) = 0, same spacing)
                    for (int i = 0; i < qt_count; ++i) for (int j = 0; j < qt_count; ++j) {
                        const auto &A = lod_nodes[i], &B = lod_nodes[j]; if (i == j || A.size != B.size || A.level != B.level) continue;
                        if (fabsf(A.origin_z - B.origin_z) < 0.01f && fabsf(B.origin_x - (A.origin_x + A.size)) < 0.5f) { ++pairs; gap = std::max(gap, (double)fabsf(B.origin_x - (A.origin_x + A.size))); }
                        if (fabsf(A.origin_x - B.origin_x) < 0.01f && fabsf(B.origin_z - (A.origin_z + A.size)) < 0.5f) { ++pairs; gap = std::max(gap, (double)fabsf(B.origin_z - (A.origin_z + A.size))); }
                    }
                    MD_LOG(MD_LOG_INFO, "[GroundLive] SelectLod %.3f ms (this frame), minL %d, same-level edge pairs %d, max edge gap %.6f m", sel_ms, lp.min_level, pairs, gap);
                    char buf[512]; int o = snprintf(buf, sizeof(buf), "[GroundLive] nodes %d, crossing a cell %d (%.1f %%), area share %.1f %%, solid-angle share %.1f %%, by level (cross/all):", qt_count, cross, qt_count ? 100.0 * cross / qt_count : 0.0, area > 0 ? 100.0 * area_c / area : 0.0, sa > 0 ? 100.0 * sa_c / sa : 0.0);
                    for (int l = 0; l < 16; ++l) if (lvl_n[l] && o < 460) o += snprintf(buf + o, sizeof(buf) - o, " L%d %d/%d", l, lvl_c[l], lvl_n[l]);
                    MD_LOG(MD_LOG_INFO, "%s", buf);
                }
            }
            {   // #163: the geometry of the ground (selected nodes with their morph, the fine height window) for the ground reuse
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](uint32_t v) { h ^= v; h *= 1099511628211ull; };
                auto fb = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
                for (int i = 0; i < qt_count; ++i) { mix(fb(lod_nodes[i].origin_x)); mix(fb(lod_nodes[i].origin_z)); mix(fb(lod_nodes[i].size)); mix(fb(lod_nodes[i].morph)); mix((uint32_t)lod_nodes[i].level); }
                mix((uint32_t)sr_prepass.terrain_quadtree_renderer_.LodFineOx()); mix((uint32_t)sr_prepass.terrain_quadtree_renderer_.LodFineOz());
                sr_prepass.gr_geom_hash_ = h;
            }
            {   // #207 H14 (MD_STILL_REUSE=1): same view key as the last full frame -> the G-buffer and the ground passes are skipped (the ground target + the G-buffer depth stay as they were)
                static const bool still_env = !(getenv("MD_STILL_REUSE") && atoi(getenv("MD_STILL_REUSE")) == 0);   // H14: on by default (owner 30.10, gates + 18 views passed with it on); MD_STILL_REUSE=0 turns it off
                static const unsigned still_period = getenv("MD_STILL_N") ? (unsigned)atoi(getenv("MD_STILL_N")) : 16u;   // a normal (reuse) frame every N frames
                static const unsigned still_settle = getenv("MD_STILL_SETTLE") ? (unsigned)atoi(getenv("MD_STILL_SETTLE")) : 16u;   // normal frames with the same key before the first skip: the live block rotation covers the whole ground once
                auto& srs = sr_prepass;
                srs.still_skip_ = false;
                if (still_env) {
                    uint64_t k = srs.gr_geom_hash_;
                    auto mixk = [&](uint32_t v) { k ^= v; k *= 1099511628211ull; };
                    auto fbk = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
                    const float* vp = mat4_ptr(g_vp4);
                    for (int i = 0; i < 16; ++i) mixk(fbk(vp[i]));
                    mixk(fbk(g_abs_cam4.pos.x)); mixk(fbk(g_abs_cam4.pos.y)); mixk(fbk(g_abs_cam4.pos.z));
                    mixk((uint32_t)floorf(LightSystem::Get().current_hours * 100.f));   // 0.01 h ≈ 36 game seconds: the sun moves 0.04 degrees
                    { const auto tf = TownFoliage::Get().stats(); mixk((uint32_t)tf.loaded_total); mixk((uint32_t)tf.unloaded_total); mixk((uint32_t)tf.zones);   // town rocks in the G-buffer: zone loads / unloads
                      for (int i = 0; i < HubSpike::SLOTS; ++i) { mixk(HubSpike::Slot(i).Active() ? 1u : 0u); mixk((uint32_t)HubSpike::Slot(i).stats().instances); } }
                    mixk(sr_prepass.terrain_quadtree_renderer_.LodNormBakes());   // the LOD normal windows were re-baked
                    if (sr_prepass.terrain_shading_projected_.PvtReady()) mixk((uint32_t)sr_prepass.terrain_shading_projected_.PvtStats().baked_total);   // a PVT tile arrived
                    {   // every render setting that reaches the ground picture (F3 / md.set_*): a change shows on the next frame
                        auto mixv = [&](const void* p, size_t n) { const unsigned char* b = (const unsigned char*)p; for (size_t i = 0; i < n; ++i) mixk(b[i]); };
                        extern bool g_opt_ground_reuse; extern int g_opt_ground_reuse_chain, g_opt_ground_reuse_cr, g_opt_ground_reuse_period, g_kenshi_sky;
                        mixv(&g_opt_ground_reuse, sizeof g_opt_ground_reuse); mixv(&g_opt_ground_reuse_chain, sizeof g_opt_ground_reuse_chain); mixv(&g_opt_ground_reuse_cr, sizeof g_opt_ground_reuse_cr);
                        mixv(&g_opt_ground_reuse_period, sizeof g_opt_ground_reuse_period); mixv(&g_opt_skip_flat_on_cliff, sizeof g_opt_skip_flat_on_cliff); mixv(&g_kenshi_sky, sizeof g_kenshi_sky);
                        mixv(&g_terrain_cliff_normal_flip, sizeof g_terrain_cliff_normal_flip); mixv(&g_terrain_detail_n_atten, sizeof g_terrain_detail_n_atten); mixv(&g_terrain_ground_aniso_level, sizeof g_terrain_ground_aniso_level);
                        mixv(&g_terrain_light_clamp, sizeof g_terrain_light_clamp); mixv(&g_terrain_per_layer_mip, sizeof g_terrain_per_layer_mip);
                        mixv(&g_terrain_lod_near_px, sizeof g_terrain_lod_near_px); mixv(&g_terrain_lod_far_px, sizeof g_terrain_lod_far_px); mixv(&g_terrain_lod_near_dist, sizeof g_terrain_lod_near_dist); mixv(&g_terrain_lod_far_dist, sizeof g_terrain_lod_far_dist);
                        mixv(&g_terrain_lod_patch, sizeof g_terrain_lod_patch);
                        { const float fcr = RenderQualityConfig::Get().terrain_cr_m, fnr = GraphicsSettings::Get().fog_near_ratio; mixv(&fcr, sizeof fcr); mixv(&fnr, sizeof fnr); mixv(&GraphicsSettings::Get().fog_color, sizeof GraphicsSettings::Get().fog_color); }
                        const auto& lsk = LightSystem::Get(); mixv(&lsk.sun_dir, sizeof lsk.sun_dir); mixv(&lsk.sun_color, sizeof lsk.sun_color); mixv(&lsk.ambient_color, sizeof lsk.ambient_color);
                    }
                    mixk((uint32_t)g_kenshi_lighting_mode); mixk((uint32_t)g_opt_ground_split);
                    mixk((uint32_t)sr_prepass.terrain_shading_projected_.GBufferDepth().Width()); mixk((uint32_t)sr_prepass.terrain_shading_projected_.GBufferDepth().Height());
                    const bool eq = k == srs.still_key_ && srs.gr_have_ && !srs.gr_reset_;
                    srs.still_key_ = k;
                    srs.still_n_ = eq ? srs.still_n_ + 1 : 0;
                    const bool same = eq && srs.still_n_ >= still_settle && (srs.still_n_ % still_period) != 0;
                    srs.still_skip_ = same;
                    { static unsigned nf = 0, ns = 0; static const unsigned ln = getenv("MD_STILL_LOG_N") ? (unsigned)atoi(getenv("MD_STILL_LOG_N")) : 300u; ++nf; ns += same ? 1 : 0; if (nf == ln) { fprintf(stderr, "[still] %u of %u frames skipped\n", ns, ln); nf = ns = 0; } }
                    if (same) srs.terrain_shading_projected_.BeginFrame();   // swaps back: the history depth swap runs once per RENDERED G-buffer, a skipped frame must not advance it
                }
            }
        } else {
            qt_count = sr_prepass.terrain_quadtree_.SelectVisible(
                sr_prepass.granite_abs_cam_, sr_prepass.terrain_frustum_planes_,
                visible_nodes, TerrainQuadtree::kMaxNodesPublic,
                TerrainQuadtree::kGameplayMaxRenderDistance);
        }

        sr_prepass.terrain_quadtree_last_visible_count_ = qt_count;

        // B3 (Granite terrain finding): upload this frame's node data BEFORE
        // opening the G-buffer render pass -- see UploadNodeData's own doc
        // comment for why this must be a separate copy pass on the same
        // fc.cmd, not something issued inside BeginGBufferPass's render pass.
        bool use_batched = sr_prepass.terrain_quadtree_renderer_.IsBatchedReady() && qt_count > 0;
        if (use_batched) {
            GpuCopyPass node_cp;
            node_cp.Begin(fc.cmd);
            if (node_cp.SDLPass()) {
                if (lod_on)
                    sr_prepass.terrain_quadtree_renderer_.UploadLodNodes(md::GpuDevice::Get().SDLDevice(), node_cp.SDLPass(), lod_nodes, qt_count,
                                                                         sr_prepass.granite_abs_cam_[0], sr_prepass.granite_abs_cam_[2]);
                else
                    sr_prepass.terrain_quadtree_renderer_.UploadNodeData(
                        md::GpuDevice::Get().SDLDevice(), node_cp.SDLPass(), visible_nodes, qt_count);
                node_cp.End();
                if (lod_on) sr_prepass.terrain_quadtree_renderer_.BakeLodNormals(fc.cmd, sr_prepass.terrain_granite_hmap);   // #207 (MD_LOD_NORMTEX=1)
            } else {
                use_batched = false;
            }
        }

        // EnsureSize is called earlier, in RenderFrame before AcquireCommandBuffer
        // -- see that call site's doc comment for why it must never run here
        // (this point already has an active command buffer).
        // S7: depth-only G-buffer (own gbuf_depth_, no colour target / FS cost); the resolve
        // pass reconstructs world position from that depth and the normal from the world
        // normal texture. Needs the batched path (the per-node fallback below stays colour).
        const bool s7_on = false /* #142 phase 1b: S7 needs the old float G-buffer layout and flickered; unsupported with the 8 B G-buffer */ && use_batched && md::RenderPassGraph::Get().IsEnabled("terrain_s7")
                           && sr_prepass.terrain_quadtree_renderer_.IsDepthOnlyReady();
        {
            TerrainShadingProjected::S7State s7;
            Mat4 inv_vp = mat4_inv(g_vp4);
            std::memcpy(s7.inv_vp, mat4_ptr(inv_vp), sizeof(s7.inv_vp));
            s7.world_extent = sr_prepass.terrain_granite_hmap.WorldExtent();
            s7.res_texels   = (float)sr_prepass.terrain_granite_hmap.Resolution();
            s7.normal_tex   = sr_prepass.terrain_granite_hmap.NormalTexture();
            s7.normal_samp  = sr_prepass.terrain_granite_hmap.NormalSampler();
            s7.mode         = s7_on ? 1 : 0;
            sr_prepass.terrain_shading_projected_.SetS7(s7);
        }
        sr_prepass.terrain_shading_projected_.SetSkipFlatOnCliff(g_opt_skip_flat_on_cliff);
        sr_prepass.terrain_shading_projected_.SetGroundUnits(
            md::RenderPassGraph::Get().IsEnabled("terrain_units10"),
            (2.0f * tanf(g_abs_cam4.fovy * 0.5f * 3.14159265f / 180.0f) + (g_terrain_cliff_normal_flip ? 100.0f : 0.0f) + (g_terrain_detail_n_atten ? 0.0f : 200.0f) + (g_terrain_ground_aniso_level >= 1 ? 400.0f : 0.0f) + (g_terrain_light_clamp ? 0.0f : 800.0f) + (g_terrain_per_layer_mip ? 0.0f : 3200.0f)));
        s7_active = s7_on;
        if (s7_on) {
            GpuRenderPass::DepthDesc s7_dd;
            s7_dd.target      = &sr_prepass.terrain_shading_projected_.GBufferDepth();
            s7_dd.clear_depth = 1.0f;
            s7_dd.load_depth  = false; // CLEAR
            s7_rp.BeginDepthOnly(fc.cmd, s7_dd);
            gbuf_pass = s7_rp.SDLPass();
        } else {
            gbuf_pass = sr_prepass.still_skip_ ? nullptr : sr_prepass.terrain_shading_projected_.BeginGBufferPass(fc.cmd);
        }
        if (gbuf_pass) {
            if (s7_on) {
                int s7_count = qt_count > TerrainQuadtreeRenderer::kMaxBatchedNodes
                             ? TerrainQuadtreeRenderer::kMaxBatchedNodes : qt_count;
                sr_prepass.terrain_quadtree_renderer_.BeginBatchedDepthOnly(
                    gbuf_pass, fc.cmd, sr_prepass.terrain_granite_hmap, mat4_ptr(g_vp4),
                    sr_prepass.granite_abs_cam_[0], sr_prepass.granite_abs_cam_[1],
                    sr_prepass.granite_abs_cam_[2]);
                sr_prepass.terrain_quadtree_renderer_.DrawBatched(gbuf_pass, fc.cmd, s7_count);
                for (int i = s7_count; i < qt_count; ++i) {
                    sr_prepass.terrain_quadtree_renderer_.DrawNodeDepthOnly(
                        gbuf_pass, fc.cmd, sr_prepass.terrain_granite_hmap, mat4_ptr(g_vp4),
                        visible_nodes[i], sr_prepass.granite_abs_cam_[0],
                        sr_prepass.granite_abs_cam_[1], sr_prepass.granite_abs_cam_[2]);
                }
            } else if (use_batched && lod_on) {
                sr_prepass.terrain_quadtree_renderer_.BeginBatchedLod(
                    gbuf_pass, fc.cmd, sr_prepass.terrain_granite_hmap, mat4_ptr(g_vp4),
                    sr_prepass.granite_abs_cam_[0], sr_prepass.granite_abs_cam_[1], sr_prepass.granite_abs_cam_[2]);
                sr_prepass.terrain_quadtree_renderer_.DrawBatchedLod(gbuf_pass, fc.cmd, qt_count);
            } else if (use_batched) {
                int batched_count = qt_count > TerrainQuadtreeRenderer::kMaxBatchedNodes
                                  ? TerrainQuadtreeRenderer::kMaxBatchedNodes : qt_count;
                sr_prepass.terrain_quadtree_renderer_.BeginBatched(
                    gbuf_pass, fc.cmd, sr_prepass.terrain_granite_hmap, mat4_ptr(g_vp4),
                    sr_prepass.granite_abs_cam_[0], sr_prepass.granite_abs_cam_[1],
                    sr_prepass.granite_abs_cam_[2], sr_prepass.terrain);
                sr_prepass.terrain_quadtree_renderer_.DrawBatched(gbuf_pass, fc.cmd, batched_count);
                // Nodes beyond kMaxBatchedNodes (only possible with an
                // extremely dense selection well past this project's measured
                // real-world node counts, see #400's doc comment) fall back
                // to the per-node path so nothing is silently dropped.
                for (int i = batched_count; i < qt_count; ++i) {
                    sr_prepass.terrain_quadtree_renderer_.DrawNode(
                        gbuf_pass, fc.cmd, sr_prepass.terrain_granite_hmap,
                        mat4_ptr(g_vp4), visible_nodes[i],
                        sr_prepass.granite_abs_cam_[0], sr_prepass.granite_abs_cam_[1],
                        sr_prepass.granite_abs_cam_[2], sr_prepass.terrain);
                }
            } else {
                for (int i = 0; i < qt_count; ++i) {
                    sr_prepass.terrain_quadtree_renderer_.DrawNode(
                        gbuf_pass, fc.cmd, sr_prepass.terrain_granite_hmap,
                        mat4_ptr(g_vp4), visible_nodes[i],
                        sr_prepass.granite_abs_cam_[0], sr_prepass.granite_abs_cam_[1],
                        sr_prepass.granite_abs_cam_[2], sr_prepass.terrain);
                }
            }
        }
    if (gbuf_pass && !s7_active && FeatureScatterSystem::Get().TerrainInGbuffer())   // #176: TERRAIN-mode map features shaded as ground by the resolve
        FeatureScatterSystem::Get().DrawGbuf(gbuf_pass, fc.cmd, mat4_ptr(g_vp4), g_abs_cam4.pos.x, g_abs_cam4.pos.z, g_feature_terrain_radius);
    HubSpike::SetTerrainInGbuf((gbuf_pass || sr_prepass.still_skip_) && !s7_active);   // H14: on a skipped frame the terrain batches are in the cached ground, not drawn forward   // town foliage rocks of TERRAIN mode go into the same G-buffer
    if (gbuf_pass && !s7_active) TownFoliage::Get().DrawGbuf(gbuf_pass, fc.cmd, mat4_ptr(g_vp4), g_abs_cam4.pos.x, g_abs_cam4.pos.z, sr_prepass.terrain_frustum_planes_);
    // Exactly one raw SDL_EndGPURenderPass call site (GPU HAL Bypass
    // Ratchet: this file is already grandfathered for one such call, see
    // docs/audit/BASELINE.json).
    if (s7_active) s7_rp.End();
    else if (gbuf_pass) SDL_EndGPURenderPass(gbuf_pass);
    sr_prepass.terrain_shading_projected_.EndGBufferPass();
    md::GpuProfiler::Get().EndPass();
    if (md::GroundLiveDriver::Wanted() && gbuf_pass && !md::GroundLiveLightOn()) {   // with MD_GROUND_LIVE_LIGHT the live pass runs inside the ground pass block of npc_render_draw_scene.cpp   // MD_GROUND_LIVE=1: the original ground programs on the G-buffer (measurement path, the result is not shown yet)
        md::GpuProfiler::Get().BeginPass("Ground Live");
        auto& tsp = sr_prepass.terrain_shading_projected_;
        { SDL_GPUTextureSamplerBinding gsb[7]; sr_prepass.terrain.GetSharedGroundSamplers(gsb); md::GroundLiveDriver::Get().SetMaps(gsb[0].texture, gsb[3].texture); }
        md::GroundLiveDriver::Get().Frame(md::GpuDevice::Get().SDLDevice(), fc.cmd, tsp.GBufferDepth().SDLTexture(), tsp.GBufferColor().SDLTexture(), tsp.GBufferSlot(), tsp.GBufferDepth().Width(), tsp.GBufferDepth().Height(),
                                          tsp.S7InvVp(), sr_prepass.granite_abs_cam_, (uint64_t)sr_prepass.frame_counter_pvt_);
        if (md::GroundLiveLightOn()) tsp.SetLiveTargets(md::GroundLiveDriver::Get().Pass().AlbedoRgb(), md::GroundLiveDriver::Get().Pass().AuxB());   // MD_GROUND_LIVE_LIGHT=1: the ground resolve reads the live material
        md::GpuProfiler::Get().EndPass();
    }
    FS_END("TerrainPrepCPU");

    // Seed the shared `depth` with terrain depth so the NPC Early-Z prepass
    // below can reject NPCs occluded by terrain.
    //
    // Default (2026-08-10 architecture): a bandwidth-bound texture COPY
    // from gbuf_depth_ (already written by the G-buffer pass above) --
    // cheap relative to re-submitting terrain geometry a second time.
    //
    // FRAME_AUDIT_2026-09-27 follow-up, task-restore-depth-prepass
    // (2026-09-30): diagnostic alternative -- "terrain_depth_prepass"
    // toggle replaces the copy with a REAL, independent depth-only
    // rasterization of terrain straight into the shared `depth` texture,
    // recreating the PRE-2026-08-10 two-pass architecture (depth prepass
    // + G-buffer, each with its OWN separate depth target, gbuf_depth_
    // untouched by this -- the two passes never shared data back then
    // either, confirmed against engine 27ba8f2/terrain_shading_projected.
    // cpp:38, which already had gbuf_depth_ before the prepass was ever
    // removed). NOT the same experiment as the reverted "load gbuf_depth_
    // for G-buffer self-early-Z" attempt (measured worse, see git history)
    // -- this one targets the SHARED buffer for NPC occlusion, matching
    // what actually existed pre-08-10, to test whether THAT specific
    // change remains a net win now that terrain geometry is ~3.4x denser
    // (05.09 flat-LOD) than when it was measured as a win back then.
    // Mutually exclusive with the copy -- exactly one of the two must run.
    bool depth_prepass_shared_on = md::RenderPassGraph::Get().IsEnabled("terrain_depth_prepass")
                                    && sr_prepass.terrain_quadtree_renderer_.IsDepthOnlyReady();
    // #207 H13.3 (measurement, MD_DEPTH_NOCOPY=1): no depth copy; the NPC depth prepass and the main pass use the G-buffer depth itself as the scene depth (the prepass moves after the ground resolve, DrawScene)
    {
        static const bool nc_env = getenv("MD_DEPTH_NOCOPY") != nullptr;
        auto& srn = sr_prepass;
        srn.depth_alias_ = nc_env && !(getenv("MD_STILL_REUSE") ? atoi(getenv("MD_STILL_REUSE")) != 0 : true) && !depth_prepass_shared_on && srn.terrain_granite_ready && g_opt_ground_split > 0
            && srn.terrain_shading_projected_.StencilSplitReady() && g_kenshi_lighting_mode > 0 && srn.terrain.KenshiLightCubesReady()
            && !(srn.terrain_skyvis_ao_.IsEnabled() && md::RenderPassGraph::Get().IsEnabled("skyvis_ao")) && srn.terrain_resolve_repeat_debug == 1
            && srn.terrain_shading_projected_.GBufferDepth().Width() == depth.Width() && srn.terrain_shading_projected_.GBufferDepth().Height() == depth.Height();
    }
    if (depth_prepass_shared_on) {
        md::GpuProfiler::Get().BeginPass("Terrain Depth Prepass (shared)");
        GpuRenderPass pre_rp;
        GpuRenderPass::DepthDesc pre_dd;
        pre_dd.target      = &depth;
        pre_dd.clear_depth = 1.0f;
        pre_dd.load_depth  = false; // CLEAR -- first write of `depth` this frame
        pre_rp.BeginDepthOnly(fc.cmd, pre_dd);
        SDL_GPURenderPass* pre_pass = pre_rp.SDLPass();
        if (pre_pass) {
            if (use_batched) {
                int batched_count = qt_count > TerrainQuadtreeRenderer::kMaxBatchedNodes
                                  ? TerrainQuadtreeRenderer::kMaxBatchedNodes : qt_count;
                sr_prepass.terrain_quadtree_renderer_.BeginBatchedDepthOnly(
                    pre_pass, fc.cmd, sr_prepass.terrain_granite_hmap, mat4_ptr(g_vp4),
                    sr_prepass.granite_abs_cam_[0], sr_prepass.granite_abs_cam_[1],
                    sr_prepass.granite_abs_cam_[2]);
                sr_prepass.terrain_quadtree_renderer_.DrawBatched(pre_pass, fc.cmd, batched_count);
                for (int i = batched_count; i < qt_count; ++i) {
                    sr_prepass.terrain_quadtree_renderer_.DrawNodeDepthOnly(
                        pre_pass, fc.cmd, sr_prepass.terrain_granite_hmap, mat4_ptr(g_vp4),
                        visible_nodes[i], sr_prepass.granite_abs_cam_[0],
                        sr_prepass.granite_abs_cam_[1], sr_prepass.granite_abs_cam_[2]);
                }
            } else {
                for (int i = 0; i < qt_count; ++i) {
                    sr_prepass.terrain_quadtree_renderer_.DrawNodeDepthOnly(
                        pre_pass, fc.cmd, sr_prepass.terrain_granite_hmap, mat4_ptr(g_vp4),
                        visible_nodes[i], sr_prepass.granite_abs_cam_[0],
                        sr_prepass.granite_abs_cam_[1], sr_prepass.granite_abs_cam_[2]);
                }
            }
        }
        pre_rp.End();
        md::GpuProfiler::Get().EndPass();
    } else if (!sr_prepass.depth_alias_ && md::RenderPassGraph::Get().IsEnabled("terrain_depth_copy")) {
        md::GpuProfiler::Get().BeginPass("Terrain Depth Copy");
        GpuDepthTexture& gbuf_depth = sr_prepass.terrain_shading_projected_.GBufferDepth();
        GpuCopyPass depth_cp;
        depth_cp.Begin(fc.cmd);
        if (depth_cp.SDLPass()) {
            SDL_GPUTextureLocation cp_src = {};
            cp_src.texture = gbuf_depth.SDLTexture();
            SDL_GPUTextureLocation cp_dst = {};
            cp_dst.texture = depth.SDLTexture();
            depth_cp.CopyTextureToTexture(cp_src, cp_dst,
                                           (Uint32)gbuf_depth.Width(), (Uint32)gbuf_depth.Height(),
                                           1, false);
            depth_cp.End();
        }
        md::GpuProfiler::Get().EndPass();
    }
}

// Early-Z depth prepass — fills `depth` with NPC geometry, no color writes.
// TERRAIN_CA_REBUILD_PROMPT.md Phase 2 §3 (2026-08-01): originally also drew
// terrain here (see git history) -- since 2026-08-10 terrain's contribution
// to `depth` comes from the G-buffer-pass copy above instead (this pass now
// LOADs that copied depth via pre_dd.load_depth rather than clearing it, so
// NPCs still correctly depth-test/write against real terrain occlusion).
if (npc_prepass_active && !sr_prepass.depth_alias_) NpcDepthPrepass(fc, cam, false);
}

// the NPC early-Z depth prepass (was inline in CullAndPrepass); alias = true: it loads / writes the G-buffer depth (DrawScene, after the ground resolve)
void NpcRender::NpcDepthPrepass(FrameCtx& fc, const MdCamera& cam, bool alias)
{
    md::GpuProfiler::Get().BeginPass("Depth Prepass");
    GpuRenderPass pre_rp;
    GpuRenderPass::DepthDesc pre_dd;
    pre_dd.target      = alias ? &SceneRender::Get().terrain_shading_projected_.GBufferDepth() : &depth;
    pre_dd.clear_depth = 1.0f;
    pre_dd.load_depth  = SceneRender::Get().terrain_granite_ready;
    pre_rp.BeginDepthOnly(fc.cmd, pre_dd);
    SDL_GPURenderPass* pre_pass = pre_rp.SDLPass();
    if (pre_pass) {
        GpuPassView pv = GpuPassView::FromRaw(pre_pass, fc.cmd);
        pv.BindPipeline(&prepass_pipeline);
        pv.BindVertexBuffer(&body_color_buf);
        pv.BindIndexBuffer(&ibuf, idx_elem);
        SDL_GPUBuffer* pre_storage[4] = {
            TransformSoA::Get().SDLTransformBuffer(),
            vis_buf.SDLBuffer(),
            TransformSoA::Get().SDLFactionBuffer(),
            AnimationSoA::Get().SDLBonesBuffer()
        };
        pv.BindVertexStorageBuffers(0, pre_storage, 4);
        float pre_asp = (float)window_get_width() / (float)window_get_height();
        Mat4 pre_vp = cam.ViewProjMatrix(pre_asp);
        struct { float vp[16]; float fc[16]; } pre_ubo;
        memcpy(pre_ubo.vp, mat4_ptr(pre_vp), 64);
        static const float kFC0[16] = {
            0,0,0,1, 0.86f,0.2f,0.2f,1, 0.2f,0.59f,0.86f,1, 0.86f,0.78f,0.2f,1
        };
        memcpy(pre_ubo.fc, kFC0, 64);
        pv.PushVertexUniforms(0, &pre_ubo, sizeof(pre_ubo));
        // CPU-driven, not indirect, since 2026-08-09 -- t01_count_/t2_count_
        // were just computed above, same frame, no GPU round-trip needed.
        pv.DrawIndexed(index_count, t01_count_, 0, 0, 0);
        pv.DrawIndexed(index_count, t2_count_, 0, 0, (uint32_t)MAX_ANIMATED_NPC);
    }
    pre_rp.End();
    md::GpuProfiler::Get().EndPass();
}

// Task #158 (2026-07-15): removed the dithered mesh-LOD crossfade (task #43,
// ComputeLodDraws) and the dithered shader-pass crossfade (2026-07-11
// GPU-load fix, ComputeShaderPassDraws) that used to live here. Both
// screen-door-dithered the SAME chunk across two draws (adjacent LOD tiers,
// or terrain_pom vs terrain_forward) via DitherPattern4x4+clip() in the
// shaders. This was a monkey_dust-only invention: neither real Kenshi
// (re_docs/kenshi/terrain.md — hard near/DistantTerrain material switch at
// materialDistance, no crossfade) nor any of the 4 surveyed OSS engines
// (docs/OSS_TERRAIN_METHODS.md — Terrain3D/rbfx/Wicked/Flax all either keep
// one shader across all LOD tiers with geometry-only geomorph, or swap
// index-buffers without changing shader identity) dither between two
// different shader implementations. The editor's own terrain loop
// (tools/editor/editor_world_3d_sdlgpu.cpp) never had this dithering and
// never showed the resulting stark checkerboard artifact ("сітківка") that
// this dithering caused here. See docs plan (terrain dither removal) for
// the full root-cause analysis.
//
// Known accepted trade-off: TERRAIN_GEOMORPH_ENABLED's vertex-Y morph
// (terrain_pom.slang) only covers LOD0->LOD1 and only exists in the POM
// shader, not terrain_forward.slang — so the mesh-LOD0/1/2/3 hard cutoffs
// below can show a mild pop at the tier boundaries (500-900m/1500-2500m/
// 3000-5000m) where forward.slang draws. This is a known, deliberately
// accepted regression (favouring "no checkerboard" over "no pop at all"),
// not an oversight — a future fast-follow would port morph_y into
// terrain_forward.slang's vertex pipeline if this pop proves objectionable.


