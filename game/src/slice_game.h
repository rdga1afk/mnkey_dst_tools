#pragma once
// Q04 vertical slice in the running game (#212), behind MD_SLICE=1 (off by default: nothing here runs and the click-to-move of the player stays as before).
// The squad is the player entity plus three spawned NPCs; selection, move orders, steering and avoidance come from engine/include/monkey_dust/world/slice_squad.h;
// the path comes from the install navmesh (md::nav::NavWorld). Left click selects (shift adds), right click (quick, no orbit) orders a move (shift = walk, else jog),
// the camera follows the selection centre.
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/render/md_camera.h>

namespace md::slice {

struct Input {
    float mouse_x = 0.f, mouse_y = 0.f;
    int screen_w = 0, screen_h = 0;
    bool lmb_click = false, rmb_click = false, shift = false;   // one frame each (a click is a release after a short press)
};

bool Enabled();                                    // getenv("MD_SLICE") == "1"
void Update(MdEntity player, MdCamera& camera, const Input& in, float dt, float& cam_x, float& cam_z);   // spawns the squad on the first call; sets the camera centre

}  // namespace md::slice
