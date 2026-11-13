#pragma once
// Shared includes/state/declarations for lua_scenario_api.cpp's split
// translation units (lua_scenario_api_core.cpp, _terrain.cpp, _diag.cpp,
// _misc.cpp) plus lua_scenario_api.cpp itself (RegisterLuaScenarioAPI).

#include "scripting/lua_scenario_api.h"
#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/world/terrain_query.h>
#include <monkey_dust/world/terrain_stream_queue.h>
#include "render/scene_render.h"
#include "world/world_map_ui.h"
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/md_camera.h>  // MdCamera::ViewProjMatrix — md.scan_terrain_screen_gaps
#include <monkey_dust/render/render_quality.h>  // RenderQualityConfig — md.scan_terrain_seam_tjunctions
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/platform/audio.h>  // frame_fps()
#include <monkey_dust/platform/window.h>  // window_set_vsync/window_set_size/frame_dt -- md.set_vsync etc.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <algorithm>

#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"       // EditorCore::Get() — md.set_camera_pose (task #158e)
#include "editor_screenshot.h" // EditorScreenshot_RequestPending (task #158f, re-diagnosing)
#endif
// renderdoc_hook.h has zero ImGui/EditorCore dependency -- available under
// MD_PERF_TEST_HOOKS too (2026-08-12), so a no-ImGui release build can still
// trigger RenderDoc captures for perf measurement.
#if defined(MONKEY_DUST_EDITOR) || defined(MD_PERF_TEST_HOOKS)
#include <monkey_dust/render/renderdoc_hook.h> // md.renderdoc_capture (2026-07-18 diag)
#endif

extern "C" {
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
}


// ── Cross-TU declarations ─────────────────────────────────────────────────
// s_cam_x/s_cam_z: defined + written by RegisterLuaScenarioAPI
// (lua_scenario_api.cpp itself); read by l_md_teleport_camera
// (lua_scenario_api_core.cpp).
// s_scenario_tick/s_scenario_rng: defined in lua_scenario_api.cpp (owned by
// ScenarioTick_Advance/ScenarioRng_Seed, the public API); s_scenario_tick
// also read by l_md_dump_state (lua_scenario_api_misc.cpp), s_scenario_rng
// also read by ScenarioRandF01 (lua_scenario_api_core.cpp).
extern uint32_t s_scenario_tick;
extern uint32_t s_scenario_rng;
extern float* s_cam_x;
extern float* s_cam_z;
// s_cam_az/s_cam_el/s_cam_dist: terrain-perf-measure (2026-08-13) -- the
// release build has no free F3 camera, so perf scenes were only reachable
// via whatever the fixed 22m third-person orbit (g_cam_az/el/dist,
// main.cpp) happened to be looking at from a given teleport_player(x,z) --
// not independently reproducible. Same registration pattern as s_cam_x/z
// but wired via RegisterLuaScenarioOrbitCamera (called separately, after
// g_cam_az/el/dist are declared in main()'s scope, unlike cam_x/z which
// exist before InitGame()). Written by l_md_set_camera_orbit
// (lua_scenario_api_core.cpp).
extern float* s_cam_az;
extern float* s_cam_el;
extern float* s_cam_dist;
// RegisterLuaScenarioOrbitCamera itself is declared in lua_scenario_api.h
// (included above) -- main.cpp only sees the public header, so that's
// where callers look; not re-declared here.

MdEntity ToEntity(lua_State* L, int idx);

int l_md_rand(lua_State* L);
int l_md_teleport_camera(lua_State* L);
int l_md_teleport_player(lua_State* L);

int l_md_terrain_height(lua_State* L);
int l_md_ray_ground(lua_State* L);
int l_md_pvt_sample(lua_State* L);
int l_md_chunk_stats(lua_State* L);
int l_md_zone_info(lua_State* L);
int l_md_world_to_local(lua_State* L);
int l_md_probe_world_heightmap(lua_State* L);
int l_md_granite_terrain_stats(lua_State* L);
int l_md_pvt_flush(lua_State* L);
int l_md_gpu_act_freq(lua_State* L);
int l_md_terrain_pixel_fraction(lua_State* L);
int l_md_dump_depth(lua_State* L);
int l_md_light_tick(lua_State* L);
int l_md_kenshi_sky(lua_State* L);
int l_md_light_sun_dir(lua_State* L);          // #140 L1
int l_md_adapt_lum_debug(lua_State* L);        // #140 L4
int l_md_adapt_lum_read(lua_State* L);
int l_md_ground_live_stats(lua_State* L);
int l_md_set_kenshi_lighting(lua_State* L);
int l_md_ground_reuse_stats(lua_State* L);   // #163 5b-2b
int l_md_splash_shot(lua_State* L);   // #200
int l_md_ground_reuse_dump(lua_State* L);    // #163 5b-2b
int l_md_debug_open_map(lua_State* L);

int l_md_scan_terrain_gaps(lua_State* L);
int l_md_scan_terrain_seam_tjunctions(lua_State* L);

int l_md_assert_eq(lua_State* L);
int l_md_assert_true(lua_State* L);
int l_md_assert_above_terrain(lua_State* L);
int l_md_screenshot(lua_State* L);
int l_md_bugcap_start(lua_State* L);
int l_md_bugcap_stop(lua_State* L);
int l_md_render_frame(lua_State* L);
int l_md_clock_probe(lua_State* L);
int l_md_gpu_freq_mhz(lua_State* L);
int l_md_pkg_temp_c(lua_State* L);
int l_md_place_window_on_largest_display(lua_State* L);
int l_md_proc_mem_kb(lua_State* L);
int l_md_render_burst(lua_State* L);
int l_md_render_burst_ms(lua_State* L);
int l_md_render_burst_freq(lua_State* L);
int l_md_set_burst_paced(lua_State* L);
int l_md_render_burst_cpu_ms(lua_State* L);
int l_md_render_burst_path(lua_State* L);
int l_md_render_burst_dynres(lua_State* L);
int l_md_render_burst_stream_ms(lua_State* L);
int l_md_render_burst_stream(lua_State* L);
int l_md_renderdoc_capture(lua_State* L);
int l_md_dump_frame_stats(lua_State* L);
int l_md_set_camera_pose(lua_State* L);
int l_md_set_camera_pose_world(lua_State* L);
int l_md_set_camera_fov(lua_State* L);
int l_md_set_camera_mode(lua_State* L);
int l_md_get_camera_pose(lua_State* L);
int l_md_get_player_pos(lua_State* L);
int l_md_dump_state(lua_State* L);
int l_md_state_hash(lua_State* L);    // per-tick state hashes for replay checks
int l_md_state_trace(lua_State* L);
void ScenarioStateTrace_Tick();
int l_md_log(lua_State* L);

int l_md_set_vsync(lua_State* L);
int l_md_set_fps_cap(lua_State* L);
int l_md_set_view_distance(lua_State* L);
int l_md_set_window_size(lua_State* L);
int l_md_get_frame_ms(lua_State* L);
int l_md_set_gpu_sync_timing(lua_State* L);
int l_md_get_gpu_ms(lua_State* L);
int l_md_gpu_poll(lua_State* L);
int l_md_set_terrain_resolve_repeat(lua_State* L);
int l_md_set_time_of_day(lua_State* L);
int l_md_set_weather(lua_State* L);
int l_md_set_resolution_scale(lua_State* L);
int l_md_set_terrain_wireframe(lua_State* L);
int l_md_set_editor_panels_hidden(lua_State* L);
int l_md_set_npc_prepass_disabled(lua_State* L);
int l_md_set_render_pass_enabled(lua_State* L);
int l_md_set_editor_open(lua_State* L);
int l_md_set_camera_orbit(lua_State* L);
