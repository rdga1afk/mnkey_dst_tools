#pragma once
// Shared includes/declarations for npc_render.cpp's split translation units
// (npc_render_init.cpp, npc_render_frame_prep.cpp, npc_render_draw_scene.cpp,
// npc_render_deferred.cpp). Conditional directives must balance PER FILE
// (GCC rejects an #ifdef left open across a header/includer boundary, even
// though the combined token stream would technically balance) -- so this
// header's own guard was self-closed; the SDL_GPU guards were removed in #185 K2b
// (the engine is SDL_GPU-only).

#include "npc_render.h"
#include "scene_render.h"
#ifdef MONKEY_DUST_EDITOR
#  include "imgui.h"
#  include "backends/imgui_impl_sdlgpu3.h"
#endif
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#endif
#include <monkey_dust/render/char_customization.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/prop_mesh.h>
#include <monkey_dust/render/md_shader.h>
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/world/feature_scatter.h>
#include "hub_spike.h"
#include "hzb_pass.h"
extern float g_feature_terrain_radius;   // #176 md.set_feature_terrain radius
#include <monkey_dust/render/animation_soa.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/render/light_system.h>
#include <monkey_dust/render/ambient_probe.h>
#include <monkey_dust/render/shadow_system.h>
#include <monkey_dust/render/gpu_profiler.h>
#include <monkey_dust/render/render_pass_graph.h>
#include <monkey_dust/render/render_quality.h>
#include <monkey_dust/render/particle_renderer.h>
#include <monkey_dust/render/rd_device.h>
#include <monkey_dust/render/rd_pipeline.h>
#include <monkey_dust/render/foot_ik.h>
#include <monkey_dust/physics/jolt_world.h>
#include <monkey_dust/physics/ragdoll.h>
#include <monkey_dust/world/terrain_query.h>
#include <monkey_dust/platform/window.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/platform/md_fs.h>
#include <monkey_dust/platform/job_system.h>
#include <monkey_dust/platform/math_types.h>
#include <monkey_dust/platform/frame_stats.h>
#include <monkey_dust/tools/graphics_settings.h>
#include <monkey_dust/ecs/md_registry.h>
static constexpr int HPBAR_MAX = 128;
#include <stb_image.h>
#include <SDL3/SDL_gpu.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "../components/nav_agent.h"
#include <monkey_dust/components/animator.h>
#include <monkey_dust/components/char_body_state.h>
#include <monkey_dust/components/player_controller.h>
#include "../world/world_registry.h"
#include "../world/world_map_ui.h"
#include <monkey_dust/world/world_transform.h>
#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include "editor_screenshot.h"
#endif

// Implemented in zone_transition.cpp; declared in world_init.h.
void TransitionToZone(const char* zone_slug, float player_x, float player_z);

// ── Player clothing state ─────────────────────────────────────────────────
// Shared across the split TUs: definitions + InitClothPipeline() call live in
// npc_render_init.cpp; DrawPlayerCloth (npc_render_draw_scene.cpp) and
// SetPlayerClothSlot/SetClothColor (npc_render_deferred.cpp) also need them.
struct PlayerClothSlot {
    GpuStaticBuffer vbo, ibo;
    float color[3] = {0.5f,0.4f,0.3f};
    int   ni       = 0;
    bool  active   = false;
};
extern PlayerClothSlot s_player_cloth[3];
extern GpuPipeline     s_cloth_game_pipeline;
bool ClothDrawReady();
void InitClothPipeline();

// ── Player hair state ────────────────────────────────────────────────────
// Rigid single-bone attach (head bone, index 21 — matches editor's char_hair.vert)
// via the shared AnimationSoA FinalBonesBuf, same pattern as PlayerClothSlot above.
// PropMesh is reused for pos+norm+uv+layer static geometry (its uv/layer fields
// are simply unused here; the pipeline layout only declares pos+norm attribs).
struct PlayerHairState {
    PropMesh mesh;
    float    color[3] = {0.18f, 0.12f, 0.08f};
    bool     loaded    = false;
};
extern PlayerHairState s_player_hair;
extern GpuPipeline     s_hair_game_pipeline;
void InitHairPipeline();
