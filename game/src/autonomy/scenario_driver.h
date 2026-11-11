#pragma once
// scenario_driver.h — CLI parsing + coroutine-driven tick loop for
// `--exec` script mode (Etap 1, AUTONOMY_SYSTEM_PROMPT_v2.md).
//
// Entirely separate from the interactive game loop in main.cpp: when
// --exec is passed, main() calls RunScenarioMode() right after InitGame()
// and returns its result immediately, instead of entering the normal
// `while (!input_should_quit())` loop.

#include "game_state.h"
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/render/md_camera.h>

class SpatialGrid;
class AISystem;
class CombatSystem;

struct ScenarioConfig {
    bool        active      = false;   // --exec was passed
    char        script_path[256] = {};
    unsigned    seed        = 42u;
    long        max_frames  = 0;       // 0 = unset
    double      max_seconds = 0.0;     // 0 = unset
    bool        fast        = false;
    bool        headless    = false;
    char        capture_dir[256] = {};
    char        data_dir[256]    = {};
    bool        jobgraph_serial  = false;  // wired to JobSystem::SetForceSerial (Audit Phase 2b, AUDIT_PLAN_v1.md)
};

// Parses argv into cfg. Returns false + writes to stderr on a malformed
// flag (e.g. --exec with no path). Unrecognized args are ignored (keeps
// the door open for engine/SDL flags without a strict allowlist).
bool ParseScenarioArgs(int argc, char** argv, ScenarioConfig& cfg);

// Runs the scenario to completion. Returns the process exit code:
//   0   — script finished naturally or called md.quit(0)
//   N   — md.quit(N)
//   1   — assert failure or uncaught Lua error (failure artifact collected)
//   124 — watchdog timeout (--max-frames/--max-seconds exceeded; artifact collected)
int RunScenarioMode(const ScenarioConfig& cfg,
                    MdEntity player_entity,
                    GameState& gs,
                    MdCamera& camera, float& cam_x, float& cam_z);
