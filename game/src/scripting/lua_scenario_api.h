#pragma once
// lua_scenario_api.h — md.* Lua API surface for the autonomy system
// (Etap 1, AUTONOMY_SYSTEM_PROMPT_v2.md). Separate from lua_game_api.h's
// pre-existing 17 flat md_foo() functions (dialog/quest/audio/power, used
// by BT actions and dialog scripts) — this file is scenario-script-only:
// world/terrain observation, assertions, and the coroutine driver's C
// glue. The 17 existing functions are aliased into md.* (same name, e.g.
// md.get_pos = md_get_pos) here too, so scenario scripts see ONE
// consistent md.* namespace regardless of which file a function's C
// implementation lives in.

struct GameState;
struct MdCamera;

// Called once from InitGame(), after RegisterLuaGameAPI() (the 17 flat
// functions must already be registered — this aliases them).
// cam_x/cam_z: same locals main.cpp's orbit camera and RunLogicTick use;
// md.teleport_camera writes through these pointers.
void RegisterLuaScenarioAPI(float* cam_x, float* cam_z);

// terrain-perf-measure (2026-08-13): wires md.set_camera_orbit(yaw_deg,
// pitch_deg, dist) to the release build's fixed third-person orbit camera
// state. Called separately from RegisterLuaScenarioAPI, from main.cpp's own
// scope right after g_cam_az/el/dist are declared (they don't exist yet
// when InitGame() -> RegisterLuaScenarioAPI(&cam_x,&cam_z) runs -- cam_x/z
// ARE declared before InitGame(), g_cam_az/el/dist are not). See
// l_md_set_camera_orbit's doc comment (lua_scenario_api_core.cpp) for why
// this exists: without it, a perf-measure script's only lever is
// teleport_player(x,z), and how much terrain ends up in frame depends
// entirely on which direction the DEFAULT orbit (az=0/south) happens to
// face from that point -- not reproducible run to run.
void RegisterLuaScenarioOrbitCamera(float* az, float* el, float* dist);

// md.render_frame(): request one on-demand render with NO swapchain capture/readback (md.screenshot() adds a copy pass + map to every
// frame it renders, so gpu_ms from screenshot frames includes that cost). Consumed by the scenario driver.
extern bool g_scenario_render_frame_pending;
extern bool g_scenario_burst_paced;           // md.set_burst_paced
extern int g_scenario_burst_n;                  // md.render_burst(n)
#include <vector>
extern std::vector<float> g_scenario_burst_ms;
extern std::vector<float> g_scenario_burst_cpu_ms; // per-frame CPU ms of the burst: render-thread CPU time (CLOCK_THREAD_CPUTIME_ID), blocked waits excluded
// md.render_burst_path(P, tick_every, dt): the burst follows a camera path (per frame x, z world, height above ground, yaw, pitch) at a fixed camera timestep dt;
// every tick_every frames the player is moved to the camera and the terrain streaming of the logic tick runs (as in the interactive loop). Per-frame outputs below.
extern std::vector<float> g_scenario_path;      // 5 floats per frame
extern int g_scenario_path_tick; extern float g_scenario_path_dt;
extern std::vector<float> g_scenario_burst_dynres;   // dynres scale in use (g_dynres_cur) per frame
extern std::vector<int> g_scenario_burst_stream;     // 1 when the terrain window shifted (zone_ox / zone_oz changed) in that frame
extern std::vector<float> g_scenario_burst_stream_ms; // ms the streaming step took in that frame (0 when none ran)
extern std::vector<int> g_scenario_burst_freq; // GT MHz sampled every 10th frame of the burst (-1 if unreadable)  // per-frame wall ms of the last burst

// Seeds the scenario RNG (md.rand()) — call once from CLI parsing (--seed N).
void ScenarioRng_Seed(unsigned int seed);

// Advances the scenario's tick counter (md.dump_state's "tick" field) —
// call once per RunLogicTick() from the scenario driver loop.
void ScenarioTick_Advance();
