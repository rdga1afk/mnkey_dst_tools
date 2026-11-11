#pragma once
// Shared includes/macros/constants/declarations for main.cpp's
// extracted per-concern translation units (game_init.cpp, game_camera.cpp,
// terrain_streaming.cpp, game_render_frame.cpp, game_frame_helpers.cpp).
// Single source of truth for the include list main.cpp used to carry alone.

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#endif
#include <monkey_dust/platform/md_fs.h>
#include <monkey_dust/md_config.h>
#include <ctime>
#include <cstdlib>
#include <chrono>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/platform/audio.h>
#include <monkey_dust/platform/window.h>
#if defined(DEBUG) || defined(MONKEY_DUST_EDITOR)
#include "imgui_platform.h"
#endif
#if defined(DEBUG) && defined(MONKEY_DUST_EDITOR)
#include <monkey_dust/platform/font_loader.h>
#endif
#include <monkey_dust/render/md_camera.h>

#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/ecs/component_warmup.h>
#include "game_state.h"
#include <monkey_dust/render/instancer.h>

#include <monkey_dust/world/world_transform.h>
#include "components/health.h"
#include "components/renderable.h"
#include "components/nav_agent.h"
#include <monkey_dust/components/animator.h>
#include "components/player_controller.h"
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/render/particle_soa.h>
#include <monkey_dust/scripting/lua_system.h>

#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/granite_backend.h>
#include <monkey_dust/render/md_shader.h>
#include <monkey_dust/render/particle_renderer.h>
#include <monkey_dust/render/animation_soa.h>
#include <monkey_dust/render/light_system.h>
#include <monkey_dust/render/shadow_system.h>
#include <monkey_dust/render/gpu_profiler.h>
#include <monkey_dust/render/render_quality.h>
#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/render/renderdoc_hook.h> // md.renderdoc_capture (2026-07-18 diag)
#include <monkey_dust/world/terrain_query.h>
#include <monkey_dust/render/prop_renderer.h>
#include <monkey_dust/render/skin_mesh.h>
#include <monkey_dust/render/foot_ik.h>
#include <monkey_dust/physics/jolt_world.h>
#include <monkey_dust/physics/physics_terrain_region.h>
#include <monkey_dust/render/rd_device.h>
#include <monkey_dust/render/rd_texture.h>
#include <monkey_dust/render/rd_pipeline.h>
#include <stb_image.h>
#include <monkey_dust/world/terrain_gen.h>
#include "world/char_def.h"
#include <monkey_dust/components/char_body_state.h>
#include "world/world_registry.h"
#include "world/world_map_ui.h"
#include "world/biome_def.h"


#include <monkey_dust/platform/timing_system.h>
#include <monkey_dust/platform/frame_stats.h>
#include <monkey_dust/render/md_draw2d.h>
#include "autonomy/scenario_driver.h"
#include <monkey_dust/platform/job_system.h>
// terrain-perf-measure (2026-08-12): lua_scenario_api.h/game_cmd_file.h have
// zero ImGui/EditorCore dependency (only md.set_camera_pose/md.screenshot
// internally no-op without EditorCore) -- available under MD_PERF_TEST_HOOKS
// too, not just MONKEY_DUST_EDITOR, so a no-ImGui release build can still be
// live-driven for perf measurement. lua_ui_driver_api.h/ui_driver.h stay
// MONKEY_DUST_EDITOR-only (deeply ImGuiTestEngine-dependent).
#if defined(MONKEY_DUST_EDITOR) || defined(MD_PERF_TEST_HOOKS)
#include "scripting/lua_scenario_api.h"
#include "ui_driver/game_cmd_file.h"
#endif
#ifdef MONKEY_DUST_EDITOR
#include "scripting/lua_ui_driver_api.h"
#include "ui_driver/ui_driver.h"
#endif
#include <cstdio>
#include <cmath>

#include <monkey_dust/tools/graphics_settings.h>
#include <monkey_dust/world/terrain_stream_queue.h>
#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include "editor_std_commands.h"
#include <monkey_dust/editor/cmd_registry.h>
#include "editor_game_context.h"
#include "editor_ui.h"
#endif
// PlayerInput.h uses WorldPlacer (DEBUG) and EditorCore (EDITOR) — must follow their includes.
#include "render/scene_render.h"
#include "render/npc_render.h"
#include "render/splash_screen.h"
#include "player_input.h"

#define SCREEN_W window_get_width()
#define SCREEN_H window_get_height()
static constexpr int   TARGET_FPS    = 60;
static constexpr float FOG_NEAR      = 80.0f;
static constexpr float FOG_FAR       = 150.0f;

#ifdef MONKEY_DUST_EDITOR
// Toolbar overlay offset (RenderDoc); editor_core.cpp/editor_toolbar.cpp extern
// this too. Definition lives in game_init.cpp (only InitGame writes it).
extern float s_overlay_top;
// EditorCmdRegistry owner id for the game's own (non-.so) editor command
// registration -- see RegisterStdEditorCommands() call site in InitGame()
// and the matching UnregisterModule() in ShutdownGame(). Header-only
// constexpr (each TU gets its own copy) since both InitGame (game_init.cpp)
// and ShutdownGame (game_frame_helpers.cpp) need it.
static constexpr uint32_t kGameEditorModuleId = 2;
#endif

// ── Functions extracted from main.cpp into separate translation units ──────
// (game_init.cpp, game_camera.cpp, terrain_streaming.cpp, game_render_frame.cpp,
// game_frame_helpers.cpp) -- declared here so main.cpp (and, for the terrain-
// streaming pair, scenario_driver.cpp) can call them across TUs.

void InitGame(MdEntity& player_entity, GameState& gs, MdCamera& camera, float& cam_x, float& cam_z);

void HandleCamButtonClick();
void UpdateOrbitCamera(MdCamera& camera,
                        float cam_x, float cam_z,
                        float& g_cam_az, float& g_cam_el, float& g_cam_dist,
                        float& s_ctm_x, float& s_ctm_z, bool& s_ctm_active,
                        float dt,
                        char* save_msg, float& save_msg_timer);

void HandleTerrainStreaming(float cam_x, float cam_z, GameState& gs);
#ifdef MONKEY_DUST_EDITOR
void HandleFlythroughStreaming();
#endif

void RenderFrame(const MdCamera& camera,
                 const GameState& gs,
                 MdEntity player_entity,
                 float cam_x, float cam_z,
                 float g_cam_az,
                 float logic_accum,
                 float now_s,
                 float dt,
                 bool paused);

void HandleDebugCaptures(char* save_msg, size_t save_msg_size, float& save_msg_timer, const MdCamera& camera);

void ShutdownGame(MdEntity player_entity, bool save_world = true);

extern bool g_no_dialogs;   // --exec runs show no modal windows (game_init.cpp)
extern bool g_choose_kenshi_folder;   // --choose-kenshi-folder (game_init.cpp)
