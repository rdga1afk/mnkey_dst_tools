#pragma once
// Rule M-A variant: include ONLY from main.cpp.
// Uses input_key_pressed/input_key_down macros which main.cpp defines
// via platform_input.h before this header is included.
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include "components/player_controller.h"
#include "components/nav_agent.h"
#include <cstdio>
#include <cmath>

inline void HandlePlayerMove(MdEntity player_entity, float& cam_x, float& cam_z,
                              float cam_az, float dt, float ctm_x, float ctm_z, bool& ctm,
                              bool& out_sprinting)
{
    auto& ptr   = MdRegistry::Get().Handle(player_entity).get_mut<WorldTransform>();
    auto& pctrl = MdRegistry::Get().Handle(player_entity).get_mut<PlayerController>();
    const bool shift = input_key_down(KEY_LEFT_SHIFT);
    out_sprinting = !shift;  // default = jog; Shift = walk
    float spd = pctrl.move_speed * (shift ? 1.0f : 2.5f) * dt;

    // Camera-relative WASD: forward = camera's horizontal forward vector.
    // Camera formula: pos.z = cam_z - dist*cos(el)*cos(az)
    //   → cam_fwd = (-sin(az), 0, cos(az)), cam_rgt = (-cos(az), 0, -sin(az))
    float fwd_x = -sinf(cam_az), fwd_z =  cosf(cam_az);
    float rgt_x = -cosf(cam_az), rgt_z = -sinf(cam_az);

    // Accumulate movement vector from all held keys, then normalize.
    // Fixes: (1) diagonal rot_y overwrite (W+A faced sideways, not 45°),
    //        (2) diagonal speed boost (sqrt(2) factor), (3) A/D strafing instead of turning.
    // rot_y rule: model faces +Z when rotY=0 (Biped 3DS Max export convention).
    // Therefore: rot_y = atan2f(move_x, move_z) — no minus on z.
    float move_x = 0.f, move_z = 0.f;
    if (input_key_down(KEY_W)) { move_x += fwd_x; move_z += fwd_z; }
    if (input_key_down(KEY_S)) { move_x -= fwd_x; move_z -= fwd_z; }
    if (input_key_down(KEY_A)) { move_x -= rgt_x; move_z -= rgt_z; }
    if (input_key_down(KEY_D)) { move_x += rgt_x; move_z += rgt_z; }
    bool wasd = (move_x != 0.f || move_z != 0.f);
    if (wasd) {
        float len = sqrtf(move_x*move_x + move_z*move_z);
        move_x /= len; move_z /= len;
        ptr.x    += move_x * spd;
        ptr.z    += move_z * spd;
        ptr.rot_y = atan2f(move_x, move_z);
        ctm = false;
    }

    // Sync is_moving + move_speed for animation blend.
    if ((MdRegistry::Get().Handle(player_entity).has<NavAgent>())) {
        auto& pnav = MdRegistry::Get().Handle(player_entity).get_mut<NavAgent>();
        bool moving = wasd || ctm;
        pnav.is_moving  = moving;
        pnav.move_speed = moving ? (out_sprinting ? 2.0f : 1.0f) : 0.0f;
    }

    // Click-to-move: glide toward target, stop when within 0.3 m.
    if (ctm) {
        float dx = ctm_x - ptr.x, dz = ctm_z - ptr.z;
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist > 0.3f) {
            float inv = spd / dist;
            ptr.x += dx * inv;
            ptr.z += dz * inv;
            ptr.rot_y = atan2f(dx, dz);  // CTM: same rule — atan2(move_x, move_z)
        } else {
            ctm = false;
        }
    }

    // ptr.y is set by TerrainQuery grounding in the logic tick — don't override here.
    cam_x = ptr.x;
    cam_z = ptr.z;
}
