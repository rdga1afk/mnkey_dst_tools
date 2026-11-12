#include "lua_scenario_api_internal.h"
#include "render/npc_render.h"
#include "render/scene_render.h"
#include "render/sky_clouds.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/render_pass_graph.h>
#include <monkey_dust/render/renderdoc_hook.h>
#include <monkey_dust/render/light_system.h>
#include <monkey_dust/render/kenshi_sky.h>
#include <monkey_dust/platform/frame_stats.h>

int l_md_assert_eq(lua_State* L) {
    double a     = luaL_checknumber(L, 1);
    double b     = luaL_checknumber(L, 2);
    double eps   = luaL_optnumber(L, 3, 0.0001);
    const char* label = luaL_optstring(L, 4, "");
    if (fabs(a - b) > eps) {
        printf("[ASSERT FAIL] %s: ОЧІКУВАНЕ=%.6f ФАКТ=%.6f (eps=%.6f)\n", label, a, b, eps);
        return luaL_error(L, "assert_eq failed: %s", label);
    }
    return 0;
}

int l_md_assert_true(lua_State* L) {
    bool cond = lua_toboolean(L, 1);
    const char* label = luaL_optstring(L, 2, "");
    if (!cond) {
        printf("[ASSERT FAIL] %s: ОЧІКУВАНЕ=true ФАКТ=false\n", label);
        return luaL_error(L, "assert_true failed: %s", label);
    }
    return 0;
}

int l_md_assert_above_terrain(lua_State* L) {
    auto& reg = MdRegistry::Get();
    MdEntity e = ToEntity(L, 1);
    double min_clearance = luaL_checknumber(L, 2);
    const char* label = luaL_optstring(L, 3, "");

    if (!reg.Valid(e) || !reg.Handle(e).has<WorldTransform>())
        return luaL_error(L, "assert_above_terrain: invalid entity: %s", label);

    const auto& tr = reg.Handle(e).get_mut<WorldTransform>();
    float ground = 0.f;
    if (!TerrainQuery::Get().GetHeight(tr.x, tr.z, ground))
        return luaL_error(L, "assert_above_terrain: no terrain loaded at (%.2f,%.2f): %s", tr.x, tr.z, label);

    double clearance = tr.y - ground;
    if (clearance < min_clearance) {
        printf("[ASSERT FAIL] %s: ОЧІКУВАНЕ clearance>=%.3f ФАКТ=%.3f (y=%.3f ground=%.3f)\n",
               label, min_clearance, clearance, (double)tr.y, (double)ground);
        return luaL_error(L, "assert_above_terrain failed: %s", label);
    }
    return 0;
}

// ── md.screenshot(path) ────────────────────────────────────────────────────────
// Requests a one-shot swapchain capture (editor's proven
// EditorScreenshot_RequestPending/CaptureAndSubmit) via an on-demand
// NpcRender::RenderFrame call scenario_driver.cpp's tick loop makes when a
// capture is pending — RunScenarioMode otherwise never renders a frame at
// all, by design. task #158f: this used to deadlock
// SDL_SubmitGPUCommandBufferAndAcquireFence at the kernel level; root cause
// was a duplicate SDL_AcquireGPUSwapchainTexture call on the same command
// buffer (NpcRender::DrawScene's forward path acquired it directly instead
// of going through the now-shared AcquireSwapchainCached). Fixed in
// npc_render.cpp/.h.
int l_md_screenshot(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
#ifdef MONKEY_DUST_EDITOR
    EditorScreenshot_RequestPending(path);
#else
    (void)path;
#endif
    return 0;
}

// md.bugcap_start() -> bool / md.bugcap_stop(): the bug video of the camera button (bugcap.cpp) as calls, for tests and for recording from a scenario.
#include "../bugcap.h"
int l_md_bugcap_start(lua_State* L) { lua_pushboolean(L, bugcap::Start()); return 1; }
int l_md_bugcap_stop(lua_State* L) { (void)L; bugcap::Stop(); return 0; }

// ── md.render_frame() ─────────────────────────────────────────────────────
// One on-demand render like md.screenshot() triggers, but without the swapchain capture (no copy pass, no readback, no PNG write).
// Renders during the following wait_ticks, same as md.screenshot().
int l_md_render_frame(lua_State* L) {
    (void)L;
    g_scenario_render_frame_pending = true;
    return 0;
}

// ── md.renderdoc_capture([tag]) ────────────────────────────────────────────────
// TEMP DIAG (2026-07-18, "гола скрізь" investigation): programmatic RenderDoc
// capture trigger — bypasses the unreliable F12 global-hotkey path (synthetic
// XTest key events don't reach RenderDoc's overlay hook in this environment)
// AND `TriggerCapture()`/`renderdoccmd` (baldurk/renderdoc#3255: renderdoccmd
// is internal/unsupported; TriggerCapture queues on the next present, which
// never fired here). Arms StartFrameCapture/EndFrameCapture bracketing around
// the next full frame instead (main.cpp RenderDocHook_BeginFrame/EndFrame).
// No-op when not running under `renderdoccmd capture` (LD_PRELOAD unset).
int l_md_renderdoc_capture(lua_State* L) {
    RenderDocHook_ArmCapture(lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr); // optional tag for the file name
    return 0;
}

// ── md.dump_frame_stats() ──────────────────────────────────────────────────
// RenderDoc's per-draw EventGPUDuration was proven unreliable on this
// hardware/driver for sub-5ms per-draw attribution (docs/
// SOUTH_HIVE_FPS_INVESTIGATION.md, 2026-08-27 closure -- floor artifact,
// non-deterministic even between replays of the SAME capture). FrameStats'
// CPU-side FS_BEGIN/FS_END sections (Shadows/Cull/DrawScene/Deferred/
// Terrain/...) already wrap every render pass and don't have that artifact
// -- but their periodic [PERF] report only flushes from main.cpp's
// interactive per-frame loop, which --exec/headless scenario runs never
// enter (on-demand renders go through scenario_driver.cpp's screenshot-
// triggered path instead, which accumulates real sum_ms/max_ms into the
// FrameStats singleton's slots but never calls EndFrame() to print/reset
// them). This forces a flush on demand: dt=6.0 unconditionally exceeds
// FS_REPORT_INTERVAL (5s), so whatever real per-pass time accumulated
// across this run's md.screenshot() calls prints immediately as a normal
// [PERF] line, no scenario-length tuning required.
int l_md_dump_frame_stats(lua_State* L) {
    (void)L;
    FrameStats::Get().EndFrame(6.0f, 0, 0);
    return 0;
}

// ── md.set_camera_pose(x, y, z, yaw_deg, pitch_deg) ───────────────────────────
// task #158e: script-drives the F3 flythrough camera (EditorCore's cam_target/
// fly_yaw/fly_pitch, same state editor_camera_panel.cpp's "Flythrough" radio
// button sets interactively) so a scenario can reproduce an exact reported
// camera pose instead of asking the user to manually fly there and screenshot
// every time a terrain visual bug needs re-checking. Angles in degrees to
// match the F3 Camera panel's on-screen Yaw/Pitch readout directly copy-
// pasteable from a bug report. Only exists in MONKEY_DUST_EDITOR builds (same
// build the user reports these bugs from).
#ifdef MONKEY_DUST_EDITOR
// 2026-09-05: shared by set_camera_pose (local coords) and
// set_camera_pose_world (world coords, below) -- was two near-duplicate
// blocks with only the world_to_local step differing.
//
// Pitch clamp widened from [-0.3,1.3]rad (~[-17,+74]deg) to [-1.48,1.48]rad
// (~[-85,+85]deg): the tight clamp was blocking legitimate debug/tooling
// use (a script trying to point the camera near-straight-down for a wide
// aerial screenshot got silently clamped to a shallow oblique angle
// instead -- confirmed live this session, wasted several capture attempts
// before the clamp itself was identified as the cause, not a camera-pose
// bug). This setter is scripting/debug-only (gated by MONKEY_DUST_EDITOR,
// never reachable from real gameplay input), so there's no interactive-
// mouse-drift concern the original narrow range might have been guarding
// against -- stop short of the true +-90 gimbal-adjacent singularity, not
// the old, much more conservative bound.
static void SetCameraPoseLocal(float x, float y, float z, float yaw_deg, float pitch_deg) {
    static constexpr float DEG2R = 3.14159265f / 180.f;
    auto& ec = EditorCore::Get();
    ec.editor_open   = true;
    ec.cam_game_mode = false;
    ec.cam_flying    = true;
    ec.cam_target    = { x, y, z };
    ec.fly_yaw       = yaw_deg * DEG2R;
    ec.fly_pitch     = pitch_deg * DEG2R;
    if (ec.fly_pitch < -1.48f) ec.fly_pitch = -1.48f;
    if (ec.fly_pitch >  1.48f) ec.fly_pitch =  1.48f;
}
#endif

// ── md.set_camera_pose(x, y, z, yaw_deg, pitch_deg) ───────────────────────────
// task #158e: script-drives the F3 flythrough camera (EditorCore's cam_target/
// fly_yaw/fly_pitch, same state editor_camera_panel.cpp's "Flythrough" radio
// button sets interactively) so a scenario can reproduce an exact reported
// camera pose instead of asking the user to manually fly there and screenshot
// every time a terrain visual bug needs re-checking. Angles in degrees to
// match the F3 Camera panel's on-screen Yaw/Pitch readout directly copy-
// pasteable from a bug report. Only exists in MONKEY_DUST_EDITOR builds (same
// build the user reports these bugs from).
int l_md_set_camera_pose(lua_State* L) {
    float x        = (float)luaL_checknumber(L, 1);
    float y        = (float)luaL_checknumber(L, 2);
    float z        = (float)luaL_checknumber(L, 3);
    float yaw_deg  = (float)luaL_optnumber(L, 4, 0.0);
    float pitch_deg= (float)luaL_optnumber(L, 5, 25.0);
#ifdef MONKEY_DUST_EDITOR
    auto& sr = SceneRender::Get();
    // task terrain-clutter (2026-07-28): x/y/z here are LOCAL floating-window
    // coords (sr.tnoff_x/z-anchored, see SceneRender's own doc comment above
    // its tnoff_x field), NOT absolute Kenshi world metres -- passing world
    // coords silently teleports the camera to the wrong place with no error,
    // which reads exactly like a render/streaming bug from the screenshot
    // alone (confirmed this session: chased a phantom terrain-geometry
    // hypothesis for a full round before finding the real cause was this).
    // Catch the common mistake early: local coords should land within a few
    // window-widths of tnoff_x/z; anything further out is almost certainly a
    // world-coord value someone forgot to convert -- use md.set_camera_pose_
    // world(wx,wy,wz,...) instead, which does that conversion internally so
    // this whole class of mistake (confirmed made repeatedly live, even
    // AFTER md.world_to_local() existed as a manual workaround) can't happen.
    float win_size = (float)SceneRender::TNKN * CHUNK_SIZE;
    float margin   = win_size * 3.f;
    if (x < sr.tnoff_x - margin || x > sr.tnoff_x + win_size + margin ||
        z < sr.tnoff_z - margin || z > sr.tnoff_z + win_size + margin) {
        MD_LOG(MD_LOG_WARNING,
               "[SCRIPT] md.set_camera_pose(%.1f,%.1f): far outside the loaded "
               "window (tnoff=(%.1f,%.1f), size=%.1f) -- looks like an absolute "
               "world coordinate, not local. Use md.set_camera_pose_world(wx,wy,wz,...) "
               "instead, or the camera will silently land in the wrong place.",
               x, z, sr.tnoff_x, sr.tnoff_z, win_size);
    }
    SetCameraPoseLocal(x, y, z, yaw_deg, pitch_deg);
#else
    (void)x; (void)y; (void)z; (void)yaw_deg; (void)pitch_deg;
    MD_LOG(MD_LOG_WARNING, "[SCRIPT] md.set_camera_pose: no-op, build without MONKEY_DUST_EDITOR");
#endif
    return 0;
}

// ── md.set_camera_pose_world(wx, wy, wz, yaw_deg, pitch_deg) ─────────────────
// 2026-09-05: world-coordinate counterpart of set_camera_pose above --
// converts via the same wx+tnoff_x-zone_ox*CHUNK_SIZE formula
// l_md_world_to_local (lua_scenario_api_terrain.cpp) already implements,
// inlined here rather than calling through Lua, so a scripted repro only
// ever needs ONE call instead of "world_to_local then set_camera_pose"
// (two calls, in the right order, is exactly the shape of mistake that
// kept recurring live this session -- passing raw world coords straight
// into set_camera_pose, or computing local coords but mixing up which
// call's return values fed which). This is now the recommended entry
// point for scripted camera repro from a bug report's Abs World reading.
// ── md.set_camera_fov(deg) ─────────────────────────────────────────────────
// Sets the F3 flythrough camera's vertical FOV (EditorCore::editor_cam.fovy, default 60) so a scenario can state it explicitly.
int l_md_set_camera_fov(lua_State* L) {
#ifdef MONKEY_DUST_EDITOR
    EditorCore::Get().editor_cam.fovy = (float)luaL_checknumber(L, 1);
#else
    (void)L;
#endif
    return 0;
}

int l_md_set_camera_pose_world(lua_State* L) {
    float wx       = (float)luaL_checknumber(L, 1);
    float wy       = (float)luaL_checknumber(L, 2);
    float wz       = (float)luaL_checknumber(L, 3);
    float yaw_deg  = (float)luaL_optnumber(L, 4, 0.0);
    float pitch_deg= (float)luaL_optnumber(L, 5, 25.0);
#ifdef MONKEY_DUST_EDITOR
    auto& sr = SceneRender::Get();
    float lx = wx + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE;
    float lz = wz + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
    SetCameraPoseLocal(lx, wy, lz, yaw_deg, pitch_deg);
#else
    (void)wx; (void)wy; (void)wz; (void)yaw_deg; (void)pitch_deg;
    MD_LOG(MD_LOG_WARNING, "[SCRIPT] md.set_camera_pose_world: no-op, build without MONKEY_DUST_EDITOR");
#endif
    return 0;
}

// ── md.set_camera_mode("orbit"|"fly"|"game") ──────────────────────────────────
// Sets EditorCore's cam_flying/cam_game_mode directly — the exact same state
// the F3 Camera panel's Orbit/Flythrough/Game radio buttons set on click.
// Bypasses the whole ImGui tab/radio widget tree entirely (no UiDriver
// find/click involved), so it's immune to any tab-activation flakiness.
int l_md_set_camera_mode(lua_State* L) {
    const char* mode = luaL_checkstring(L, 1);
#ifdef MONKEY_DUST_EDITOR
    auto& ec = EditorCore::Get();
    ec.editor_open = true;
    if (strcmp(mode, "game") == 0)      { ec.cam_game_mode = true;  ec.cam_flying = false; }
    else if (strcmp(mode, "fly") == 0)  { ec.cam_game_mode = false; ec.cam_flying = true;  }
    else                                { ec.cam_game_mode = false; ec.cam_flying = false; } // "orbit"
#else
    (void)mode;
    MD_LOG(MD_LOG_WARNING, "[SCRIPT] md.set_camera_mode: no-op, build without MONKEY_DUST_EDITOR");
#endif
    return 0;
}

// ── md.get_camera_pose() -> {x, y, z, world_x, world_z} ───────────────────────
// Independent readback of EditorCore's actual cam_target (C++-side ground
// truth), for verification scripts that need to confirm where the camera
// really ended up in the SAME frame as a screenshot -- not just trust that
// the rendered image "looks about right" (the failure mode this session's
// set_camera_pose range-check/world_to_local were added to catch).
int l_md_get_camera_pose(lua_State* L) {
#ifdef MONKEY_DUST_EDITOR
    auto& ec = EditorCore::Get();
    auto& sr = SceneRender::Get();
    lua_newtable(L);
    lua_pushnumber(L, ec.cam_target.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, ec.cam_target.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, ec.cam_target.z); lua_setfield(L, -2, "z");
    lua_pushnumber(L, ec.cam_target.x - sr.tnoff_x + (float)sr.zone_ox * CHUNK_SIZE);
    lua_setfield(L, -2, "world_x");
    lua_pushnumber(L, ec.cam_target.z - sr.tnoff_z + (float)sr.zone_oz * CHUNK_SIZE);
    lua_setfield(L, -2, "world_z");
    // 2026-08-28: added yaw/pitch -- this function's own doc comment above
    // promises "independent readback ... not just trust that the rendered
    // image looks about right", but omitted the one field a repro script
    // actually needs to point the camera the same way a bug report's
    // screenshot did. Mode-aware for the same reason editor_camera_panel.cpp's
    // Viewport Info readout is: Flythrough's real view direction is
    // ec.fly_yaw/ec.fly_pitch (radians), NOT ec.cam_yaw/ec.cam_pitch (degrees,
    // Orbit-mode-only) -- see that file's doc comment for the full bug this
    // distinction caused when a panel/API silently reported the wrong one.
    {
        bool flyMode = ec.cam_flying && !ec.cam_game_mode;
        double yawDeg   = flyMode ? ec.fly_yaw   * (180.0 / 3.14159265358979) : ec.cam_yaw;
        double pitchDeg = flyMode ? ec.fly_pitch * (180.0 / 3.14159265358979) : ec.cam_pitch;
        lua_pushnumber(L, yawDeg);   lua_setfield(L, -2, "yaw");
        lua_pushnumber(L, pitchDeg); lua_setfield(L, -2, "pitch");
    }
    return 1;
#else
    lua_pushnil(L);
    return 1;
#endif
}

// ── md.get_player_pos() -> {x, z} (world-space, via WorldTransform) ──────────
// Independent of camera mode/target — reads the actual player entity's
// transform directly, for verifying WASD movement in Game camera mode
// (where the camera follows the player, not the other way around; unlike
// md.get_camera_pose() which reads the F3 Flythrough camera's own target
// and does not move when the player walks in Game mode).
int l_md_get_player_pos(lua_State* L) {
    auto& reg = MdRegistry::Get();
    MdEntity player = MdEntity::Null();
    static auto q_player_lookup = reg.Raw().query().all<PlayerController>().all<WorldTransform>();
    player = MdFirst(q_player_lookup);
    lua_newtable(L);
    if (reg.Valid(player) && reg.Handle(player).has<WorldTransform>()) {
        const auto& tr = reg.Handle(player).get_mut<WorldTransform>();
        lua_pushnumber(L, tr.x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, tr.z); lua_setfield(L, -2, "z");
        lua_pushboolean(L, true); lua_setfield(L, -2, "ok");
    } else {
        lua_pushboolean(L, false); lua_setfield(L, -2, "ok");
    }
    return 1;
}

// ── md.dump_state(path) ───────────────────────────────────────────────────────
// Deterministic: entities sorted by id (flecs query iteration order is not
// guaranteed stable) — required for fast_check.lua's --fast vs normal diff.
static constexpr int MAX_DUMP_ENTITIES = 4096;

int l_md_dump_state(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    FILE* f = fopen(path, "w");
    if (!f) return luaL_error(L, "md.dump_state: cannot open '%s' for write", path);

    auto& reg = MdRegistry::Get();
    static uint32_t ids[MAX_DUMP_ENTITIES];
    int n = 0;
    // gaia's query.each() has no direct-entity-callback form the way
    // flecs's does (docs/archive/GAIA_SEAM_AUDIT.md) -- routed through MdEach,
    // which both backends support uniformly, instead of calling .each()
    // on the query object directly.
    // MdManagedTag is a genuinely empty/zero-size tag -- gaia has NO
    // backing storage for it at all, so BOTH it.view<T>() and
    // it.view_mut<T>() static_assert(!is_empty_v<U>) and hard-fail to
    // compile regardless of how this callback's 2nd param is spelled
    // (found 2026-09-08 building game/ under MD_ECS_GAIA -- md_tests
    // doesn't compile this file, so it went unnoticed through Phase 1-4).
    // Real fix is in md_registry.h's mdeach_view_for<Arg>: empty types
    // short-circuit to a stand-in returning T{} per row instead of ever
    // calling view/view_mut. const& here is just the honest spelling
    // (read-only, no mutation intent) -- both backends accept it: flecs's
    // each_delegate always dispatches an empty tag by VALUE (each_field)
    // regardless of what the query term says, and binding that rvalue to
    // const T& is legal.
    static auto q_dump_all = reg.Raw().query().all<MdManagedTag&>();
    MdEach(q_dump_all, [&](MdEntity fe, const MdManagedTag&) {
        if (n < MAX_DUMP_ENTITIES) ids[n++] = fe.ToIntegral();
    });
    // Simple insertion sort — n is small (scenario scale, not open-world).
    for (int i = 1; i < n; ++i) {
        uint32_t key = ids[i]; int j = i - 1;
        while (j >= 0 && ids[j] > key) { ids[j + 1] = ids[j]; --j; }
        ids[j + 1] = key;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"tick\": %u,\n", s_scenario_tick);
    fprintf(f, "  \"fps\": %d,\n", frame_fps());
    fprintf(f, "  \"chunk_stats\": {\"pending\": %d},\n",
            TerrainStreamQueue::Get().PendingCount());
    fprintf(f, "  \"entities\": [\n");
    for (int i = 0; i < n; ++i) {
        MdEntity e = reg.FromIndex(ids[i]);
        fprintf(f, "    {\"id\": %u", ids[i]);
        if (reg.Handle(e).has<WorldTransform>()) {
            const auto& tr = reg.Handle(e).get_mut<WorldTransform>();
            fprintf(f, ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f", tr.x, tr.y, tr.z);
        }
        if (reg.Handle(e).has<Health>()) {
            const auto& hp = reg.Handle(e).get_mut<Health>();
            fprintf(f, ", \"health\": %.4f", hp.TotalHp());
        }
        fprintf(f, "}%s\n", (i + 1 < n) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

// ── md.state_hash() / md.state_trace(path) ────────────────────────────────────
// Per-tick state hash for deterministic-replay checks (owner 31.10, OpenTTD / OpenRCT2 lesson): FNV-1a over the bit patterns of the simulation state, entities sorted by id,
// one hash per component group so a divergence names the group. Groups: transform (WorldTransform), health, nav (NavAgent), phys (PhysicsAgent velocity / ground flag), rng (scenario RNG).
// md.state_trace(path) appends "tick all transform health nav phys rng" (hex) every scenario tick; md.state_trace(nil) stops. scripts/replay_diff.py compares two traces.
struct StateHashes { uint64_t all, transform, health, nav, phys, rng; };
static inline void FnvU32(uint64_t& h, uint32_t v) { for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ull; } }
static inline void FnvF(uint64_t& h, float f) { uint32_t u; memcpy(&u, &f, 4); FnvU32(h, u); }
static StateHashes ComputeStateHashes() {
    auto& reg = MdRegistry::Get();
    static uint32_t ids[MAX_DUMP_ENTITIES];
    int n = 0;
    static auto q_all = reg.Raw().query().all<MdManagedTag&>();
    MdEach(q_all, [&](MdEntity fe, const MdManagedTag&) { if (n < MAX_DUMP_ENTITIES) ids[n++] = fe.ToIntegral(); });
    for (int i = 1; i < n; ++i) { uint32_t key = ids[i]; int j = i - 1; while (j >= 0 && ids[j] > key) { ids[j + 1] = ids[j]; --j; } ids[j + 1] = key; }
    StateHashes r{14695981039346656037ull, 14695981039346656037ull, 14695981039346656037ull, 14695981039346656037ull, 14695981039346656037ull, 14695981039346656037ull};
    for (int i = 0; i < n; ++i) {
        MdEntity e = reg.FromIndex(ids[i]); auto h = reg.Handle(e);
        FnvU32(r.transform, ids[i]); FnvU32(r.health, ids[i]); FnvU32(r.nav, ids[i]); FnvU32(r.phys, ids[i]);
        if (h.has<WorldTransform>()) { const auto& t = h.get_mut<WorldTransform>(); FnvF(r.transform, t.x); FnvF(r.transform, t.y); FnvF(r.transform, t.z); }
        if (h.has<Health>()) FnvF(r.health, h.get_mut<Health>().TotalHp());
        if (h.has<NavAgent>()) { const auto& a = h.get_mut<NavAgent>(); FnvF(r.nav, a.target_x); FnvF(r.nav, a.target_z); FnvU32(r.nav, a.is_moving ? 1u : 0u); FnvF(r.nav, a.move_speed); FnvF(r.nav, a.desired_vel_x); FnvF(r.nav, a.desired_vel_z); }
        if (h.has<PhysicsAgent>()) { const auto& a = h.get_mut<PhysicsAgent>(); FnvF(r.phys, a.desired_vel_x); FnvF(r.phys, a.desired_vel_z); FnvU32(r.phys, a.on_ground ? 1u : 0u); }
    }
    FnvU32(r.rng, s_scenario_rng);
    for (uint64_t g : {r.transform, r.health, r.nav, r.phys, r.rng}) { FnvU32(r.all, (uint32_t)g); FnvU32(r.all, (uint32_t)(g >> 32)); }
    return r;
}
static FILE* s_state_trace = nullptr;
void ScenarioStateTrace_Tick() {
    if (!s_state_trace) return;
    const StateHashes h = ComputeStateHashes();
    fprintf(s_state_trace, "%u %016llx %016llx %016llx %016llx %016llx %016llx\n", s_scenario_tick, (unsigned long long)h.all, (unsigned long long)h.transform, (unsigned long long)h.health,
            (unsigned long long)h.nav, (unsigned long long)h.phys, (unsigned long long)h.rng);
}
int l_md_state_hash(lua_State* L) {
    const StateHashes h = ComputeStateHashes();
    char b[20]; lua_newtable(L);
    auto put = [&](const char* k, uint64_t v) { snprintf(b, sizeof b, "%016llx", (unsigned long long)v); lua_pushstring(L, b); lua_setfield(L, -2, k); };
    put("all", h.all); put("transform", h.transform); put("health", h.health); put("nav", h.nav); put("phys", h.phys); put("rng", h.rng);
    return 1;
}
int l_md_state_trace(lua_State* L) {
    if (s_state_trace) { fclose(s_state_trace); s_state_trace = nullptr; }
    if (lua_isstring(L, 1)) { s_state_trace = fopen(lua_tostring(L, 1), "w"); if (!s_state_trace) return luaL_error(L, "md.state_trace: cannot open '%s'", lua_tostring(L, 1)); setvbuf(s_state_trace, nullptr, _IOLBF, 0); }
    return 0;
}

// ── md.set_vsync(fps) — 0=mailbox (engine default), <0=IMMEDIATE, >0=VSYNC ───
// terrain-perf-measure (2026-08-12): the resolution-scaling frame-time test
// needs a true uncapped present mode. window_set_vsync(fps)'s existing
// branching (window.h) only reaches SDL_GPU_PRESENTMODE_IMMEDIATE when
// neither the fps>0 nor the !fps branch matches -- pass a negative fps to
// force it.
int l_md_set_fps_cap(lua_State* L) { window_set_fps_cap((int)luaL_checkinteger(L, 1)); return 0; }   // md.set_fps_cap(n): 0 = unlimited (#191)

int l_md_set_vsync(lua_State* L) {
    int fps = (int)luaL_checkinteger(L, 1);
    window_set_vsync(fps);
    return 0;
}

// ── md.set_view_distance(v) — Kenshi's "view distance", 1500...12000 (default 5000) ─────
extern void md_settings_set_view_distance(float v);
int l_md_set_view_distance(lua_State* L) {
    md_settings_set_view_distance((float)luaL_checknumber(L, 1));
    return 0;
}

// ── md.set_window_size(w, h) ──────────────────────────────────────────────
// terrain-perf-measure (2026-08-12): live-resize the real OS window so a
// scenario can sweep resolutions in place (no relaunch) for the frame-time-
// vs-pixel-count regression test. Safe across TerrainShadingProjected's own
// EnsureSize (window resizes already tracked correctly, fixed 2026-08-02).
int l_md_set_window_size(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    window_set_size(w, h);
    return 0;
}

// ── md.get_frame_ms() -> last frame's dt in milliseconds (scenario render_frame path: wall ms between renders) ─────────────────
// Instantaneous (single-frame), not averaged -- a scenario sampling many
// ticks per resolution step should average client-side in Lua.
int l_md_get_frame_ms(lua_State* L) {
    extern float g_scenario_last_frame_ms;   // scenario_driver.cpp: wall ms between consecutive md.render_frame() renders
    lua_pushnumber(L, (lua_Number)(g_scenario_last_frame_ms > 0.f ? g_scenario_last_frame_ms : frame_dt() * 1000.0f));
    return 1;
}

// ── md.set_gpu_sync_timing(on) ────────────────────────────────────────────
// terrain-perf-measure (2026-08-12): see GpuDevice::SetSyncTiming's doc
// comment -- opt-in serialized-fence GPU timing, for when frame_dt() is
// blind to real render cost because the software TARGET_FPS cap (main.cpp)
// is eating the GPU's slack. NOT shippable (kills CPU/GPU pipelining) --
// test-only, always restore to false when done measuring.
int l_md_set_gpu_sync_timing(lua_State* L) {
    bool on = lua_toboolean(L, 1) != 0;
    md::GpuDevice::Get().SetSyncTiming(on);
    return 0;
}

// ── md.get_gpu_ms() -> last frame's serialized GPU execution time (ms) ────
// Only meaningful while md.set_gpu_sync_timing(true) is active -- 0 otherwise
// (GpuDevice::LastGpuMs() is never written by the normal async Submit() path).
int l_md_get_gpu_ms(lua_State* L) {
    lua_pushnumber(L, (lua_Number)md::GpuDevice::Get().LastGpuMs());
    return 1;
}

// ── md.set_terrain_resolve_repeat(n) ──────────────────────────────────────
// terrain-perf-measure falsification probe (2026-08-12): see scene_render.h
// SceneRender::terrain_resolve_repeat_debug's doc comment. Re-runs the
// terrain shading resolve pass n times per frame at a FIXED window size, so
// gpu_ms(n) can be checked for linear growth (real GPU-time measurement) vs
// ~16.7ms-quantized steps (fence/present-mode period-locked, invalidating
// the resolution-scaling test).
int l_md_set_terrain_resolve_repeat(lua_State* L) {
    int n = (int)luaL_checkinteger(L, 1);
    // 0 is a valid ablation value (skip DrawShadingResolve entirely) — only
    // negative input falls back to the 1x default.
    SceneRender::Get().terrain_resolve_repeat_debug = n >= 0 ? n : 1;
    return 0;
}

// ── md.set_time_of_day(hours) ──────────────────────────────────────────────
// Test-automation binding for LightSystem::time_override_enabled/hours
// (2026-09-20, F3 Settings tab doc comment, light_system.h) -- that field
// already existed for the editor UI toggle; this just exposes the same
// state to --exec/game_cmd_file scenarios, which otherwise have no way to
// skip a fresh launch's real-time-locked day cycle (game_time_hours =
// fmod(now_s/60, 24) in logic_tick_orchestration.cpp -- 1 game-hour = 60
// real seconds, so reaching daylight from a cold launch cost ~11-12
// minutes of wall-clock waiting per terrain-lighting verification this
// session, repeatedly). hours<0 disables the override, reverting to the
// normal elapsed-time clock.
int l_md_set_time_of_day(lua_State* L) {
    float hours = (float)luaL_checknumber(L, 1);
    if (hours < 0.f) {
        LightSystem::Get().time_override_enabled = false;
    } else {
        LightSystem::Get().time_override_enabled = true;
        LightSystem::Get().time_override_hours   = fmodf(hours, 24.f);
    }
    return 0;
}

// md.set_weather(cloud_offset, darkness, wetness): fixed weather for a scene (cloud layer needs MD_SKY_INSTALL, wetness needs MD_GROUND_LIVE); md.set_weather() with no arguments returns to the zone's weather.
int l_md_set_weather(lua_State* L) {
    if (lua_gettop(L) == 0) { SkyClouds::SetWeatherOverride(false, 0.f, 0.f, 0.f); return 0; }
    SkyClouds::SetWeatherOverride(true, (float)luaL_checknumber(L, 1), (float)luaL_optnumber(L, 2, 0.0), (float)luaL_optnumber(L, 3, 0.0));
    return 0;
}

// md.light_tick(hours): LightSystem::Tick(hours) now. The scenario driver renders without the per-frame Tick of the interactive loop (game_render_frame.cpp), so md.set_time_of_day alone never changes the light there (#163 light test).
int l_md_light_tick(lua_State* L) {
    LightSystem::Get().Tick((float)luaL_checknumber(L, 1));
    return 0;
}

// md.kenshi_sky(hour) -> {sun_dir={x,y,z}, sun_real={x,y,z}, sun_colour={r,g,b}, w, min_lum, horizon={r,g,b,a}, zenith={r,g,b}, fog={d,near,far,density}}: #140 L1, Kenshi's sky light at an hour (kenshi_sky.h).
// md.set_kenshi_lighting(mode): #140, 0 = the current lighting (default); the lighting steps L2..L4 read it (L1 changes nothing on screen).
int g_kenshi_lighting_mode = 2;   // #140: Kenshi sun + main_light by default (owner decision 16.10); 0 = the earlier lighting, 3 / 4 = + fog / + auto exposure
int md_settings_get_lighting() { return g_kenshi_lighting_mode; }   // F3 Settings
void md_settings_set_lighting(int mode) { g_kenshi_lighting_mode = mode; LightSystem::Get().use_kenshi_sky = mode >= 1; }
int l_md_set_kenshi_lighting(lua_State* L) { g_kenshi_lighting_mode = (int)luaL_checkinteger(L, 1); LightSystem::Get().use_kenshi_sky = g_kenshi_lighting_mode >= 1; return 0; }
// md.adapt_lum_debug(mean[, dt]): L4 oracle, a mean luminance >= 0 replaces the measured one (-1 = measure), dt > 0 replaces the frame time. md.adapt_lum_read() -> the adapted luminance (1x1 R32F) of the last frame, or -1.
extern float g_adapt_override_mean, g_adapt_override_dt;
int l_md_adapt_lum_debug(lua_State* L) { g_adapt_override_mean = (float)luaL_checknumber(L, 1); g_adapt_override_dt = (float)luaL_optnumber(L, 2, -1.0); return 0; }
int l_md_adapt_lum_read(lua_State* L) {
    auto& sr = SceneRender::Get();
    SDL_GPUTexture* t = sr.ground_split_pass_.LumTex();
    float v = -1.f;
    if (t && sr.lum_adapt_active_) {
        md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
        SDL_GPUTransferBufferCreateInfo tci{}; tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tci.size = 4;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tci);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureRegion reg{}; reg.texture = t; reg.w = 1; reg.h = 1; reg.d = 1;
        SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb;
        SDL_DownloadFromGPUTexture(cp, &reg, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f);
        void* m = SDL_MapGPUTransferBuffer(dev, tb, false);
        if (m) { memcpy(&v, m, 4); SDL_UnmapGPUTransferBuffer(dev, tb); }
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
    lua_pushnumber(L, v); return 1;
}
// md.light_sun_dir() -> {x, y, z}: LightSystem's direction TO the sun (the negated sun_dir).
int l_md_light_sun_dir(lua_State* L) {
    const auto& ls = LightSystem::Get();
    lua_newtable(L);
    lua_pushnumber(L, -ls.sun_dir.x); lua_rawseti(L, -2, 1); lua_pushnumber(L, -ls.sun_dir.y); lua_rawseti(L, -2, 2); lua_pushnumber(L, -ls.sun_dir.z); lua_rawseti(L, -2, 3);
    return 1;
}
int l_md_kenshi_sky(lua_State* L) {
    const float hour = (float)luaL_checknumber(L, 1);
    md::KenshiSkySettings s;
    const auto ml = s.GetMainLight(hour);
    const md::KsVec3 real = s.SunDirection(hour), z = s.ZenithLight(hour);
    const md::KsVec4 h = s.HorizonClouds(hour), f = s.FogParams();
    auto v3 = [&](const char* k, float a, float b, float c) { lua_newtable(L); lua_pushnumber(L, a); lua_rawseti(L, -2, 1); lua_pushnumber(L, b); lua_rawseti(L, -2, 2); lua_pushnumber(L, c); lua_rawseti(L, -2, 3); lua_setfield(L, -2, k); };
    auto v4 = [&](const char* k, float a, float b, float c, float d) { v3(k, a, b, c); lua_getfield(L, -1, k); lua_pushnumber(L, d); lua_rawseti(L, -2, 4); lua_pop(L, 1); };
    lua_newtable(L);
    v3("sun_dir", ml.dir.x, ml.dir.y, ml.dir.z); v3("sun_real", real.x, real.y, real.z); v3("sun_colour", ml.colour.x, ml.colour.y, ml.colour.z);
    lua_pushnumber(L, ml.w); lua_setfield(L, -2, "w"); lua_pushnumber(L, ml.min_lum); lua_setfield(L, -2, "min_lum");
    v4("horizon", h.x, h.y, h.z, h.w); v3("zenith", z.x, z.y, z.z); v4("fog", f.x, f.y, f.z, f.w);
    return 1;
}

// ── md.set_resolution_scale(s) ──────────────────────────────────────────────
// resolution_scale (#253): fixed override 0.5..1.0 (1.0 = off); 0 = back to AUTO (window-size based). See SceneRender::resolution_scale.
int l_md_set_resolution_scale(lua_State* L) {
    float v = (float)luaL_checknumber(L, 1);
    if (v > 0.f && v < 0.5f) v = 0.5f;
    if (v > 1.0f) v = 1.0f;
    if (v < 0.f) v = 0.f;
    SceneRender::Get().resolution_scale = v;
    return 0;
}

// ── md.set_terrain_wireframe(on) ────────────────────────────────────────
// CLAUDE_CONSTITUTION.md sec 7.7 diagnostic tool (2026-09-04) -- overlays
// the REAL terrain mesh (TerrainQuadtreeRenderer::DrawNodeWireframe, same
// vertex transform as the live draw) on top of the normally-shaded scene.
// Test-only, always false outside a live geometry investigation.
int l_md_set_terrain_wireframe(lua_State* L) {
    bool on = lua_toboolean(L, 1) != 0;
    SceneRender::Get().terrain_wireframe_debug_ = on;
    return 0;
}

// ── md.set_editor_panels_hidden(on) ─────────────────────────────────────
// 2026-09-04 tooling-gap fix (KENSHI_TERRAIN_STRUCTURE.md sec 8's
// documented debt): md.set_camera_pose forces editor_open=true (flythrough
// mode) so the F3 Scene/Hierarchy panel is drawn -- and in this project's
// saved dock layout that panel occupies nearly the full window, leaving
// only a thin sliver of the actual 3D view visible in a screenshot. This
// is NOT a rendering/camera bug (DrawScene/active_cam=editor_cam both run
// correctly regardless -- confirmed live via game_render_frame.cpp:98-101,
// 208's unconditional UpdateEditorCamera/Update calls and the real terrain
// sliver visible at the top edge of an "obstructed" screenshot): it is
// pure panel-visibility state (EditorCore::panels_visible[15], see that
// field's own index-map comment). Sets every entry to `on ? false : `
// this file's own documented defaults -- good enough for scripted
// screenshot purposes, not meant to preserve a user's custom layout.
int l_md_set_editor_panels_hidden(lua_State* L) {
#ifdef MONKEY_DUST_EDITOR
    bool hidden = lua_toboolean(L, 1) != 0;
    auto& pv = EditorCore::Get().panels_visible;
    if (hidden) {
        for (bool& v : pv) v = false;
    } else {
        // Matches EditorCore::panels_visible's own in-class default list.
        static const bool kDefaults[15] = { true,  true,  false, true,
                                             false, true,  true,  false,
                                             false, false, false, false,
                                             false, false, false };
        for (int i = 0; i < 15; ++i) pv[i] = kDefaults[i];
    }
#endif
    return 0;
}

// #399 (2026-08-23): A/B ablation for the NPC Early-Z depth prepass -- see
// NpcRender::npc_prepass_debug_disable's own doc comment (npc_render.h)
// for why this is safe to toggle live with Variant A active. NOT for
// shipping.
int l_md_set_npc_prepass_disabled(lua_State* L) {
    bool on = lua_toboolean(L, 1) != 0;
    NpcRender::Get().npc_prepass_debug_disable = on;
    return 0;
}

// task #142 (2026-09-16): generic RenderPassGraph toggle for live per-pass
// GPU-ms ablation -- md.set_render_pass_enabled("draw_sky", false) etc.
// See npc_render_draw_scene.cpp's DrawScene for which names are registered.
// NOT for shipping -- diagnostic only.
int l_md_set_render_pass_enabled(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    bool on = lua_toboolean(L, 2) != 0;
    md::RenderPassGraph::Get().SetEnabled(name, on);
    return 0;
}


// ── md.set_editor_open(on) ────────────────────────────────────────────────
// terrain-perf-measure (2026-08-12): md.set_camera_pose/md.set_camera_mode
// both force EditorCore::editor_open=true as a side effect -- every
// resolution-scaling/N-test/RenderDoc measurement this session used one of
// those, so the F3 ImGui overlay was rendering throughout. This lets a
// scenario turn it back off to isolate real game-render cost from ImGui
// overlay cost (identified via RenderDoc vertex-layout/constant-block
// inspection: the ~9ms/60%-of-frame "mystery" draws were ImDrawVert-
// shaped, i.e. Dear ImGui, not terrain or game content).
int l_md_set_editor_open(lua_State* L) {
    bool on = lua_toboolean(L, 1) != 0;
#ifdef MONKEY_DUST_EDITOR
    EditorCore::Get().editor_open = on;
#else
    (void)on;
#endif
    return 0;
}

// ── md.log(msg) — [SCRIPT] prefix (distinct from LuaSystem's bare md_log,
// which prefixes [Lua] and backs BT/dialog script logging) ───────────────────
int l_md_log(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    // Direct stdout, not MD_LOG (which defaults to stderr, md_log.h) — the
    // plan (AUTONOMY_SYSTEM_PROMPT_v2.md Etap 1) requires md.log on stdout
    // specifically, so scenario runners can grep it without stderr noise.
    if (msg) { printf("[SCRIPT] %s\n", msg); fflush(stdout); }
    return 0;
}

// ── Registration ──────────────────────────────────────────────────────────────
