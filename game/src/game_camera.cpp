#include "game_common.h"

// Camera button click (bottom-right corner); called every frame, independent of the camera mode.
void HandleCamButtonClick()
{
    float mx = input_mouse_x(), my = input_mouse_y();
    int sw = window_get_width(), sh = window_get_height();
    int bx = sw - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN;
    int by = sh - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN;
    static bool prev_lmb_btn = false;
    bool cur_lmb = input_mouse_down(MOUSE_BUTTON_LEFT);
    if (cur_lmb && !prev_lmb_btn) {
        if ((int)mx >= bx && (int)mx < bx + SceneRender::CAM_BTN_SZ &&
            (int)my >= by && (int)my < by + SceneRender::CAM_BTN_SZ) {
            SceneRender::Get().recording ? SceneRender::Get().StopRecording() : SceneRender::Get().StartRecording();
        }
    }
    prev_lmb_btn = cur_lmb;
}

// ─── UpdateOrbitCamera ────────────────────────────────────────────────────────
// Processes RMB orbit, arrow-key rotation, scroll zoom, LMB click-to-move ray,
// terrain brush (editor only), spring-arm dist, and writes camera.pos/target.
void UpdateOrbitCamera(MdCamera& camera,
                               float cam_x, float cam_z,
                               float& g_cam_az, float& g_cam_el, float& g_cam_dist,
                               float& s_ctm_x, float& s_ctm_z, bool& s_ctm_active,
                               float dt,
                               char* save_msg, float& save_msg_timer)
{
    {
        float mx = input_mouse_x(), my = input_mouse_y();

        bool rmb = input_mouse_down(MOUSE_BUTTON_RIGHT);

        // Relative mouse mode: enables unlimited delta even at window/screen edge.
        // Toggle on RMB press/release so normal cursor works outside of orbit.
        {
            static bool s_rel = false;
            if (rmb != s_rel) {
                SDL_SetWindowRelativeMouseMode(_wnd::ptr(), rmb);
                s_rel = rmb;
            }
        }

        // Drain relative delta every frame — prevents jump on RMB re-press.
        float rdx = 0.f, rdy = 0.f;
        SDL_GetRelativeMouseState(&rdx, &rdy);

        if (rmb) {
            g_cam_az += rdx * 0.005f;
            g_cam_el  = fmaxf(fminf(g_cam_el + rdy * 0.005f, 1.45f), 0.10f);
        }

        // Arrow keys: rotate; scroll: zoom.
        const float ROT_SPD = 1.2f, ZOOM_SPD = 4.f;
        if (input_key_down(KEY_LEFT))  g_cam_az -= ROT_SPD * dt;
        if (input_key_down(KEY_RIGHT)) g_cam_az += ROT_SPD * dt;
        if (input_key_down(KEY_UP))    g_cam_el  = fminf(g_cam_el + ROT_SPD * dt, 1.45f);
        if (input_key_down(KEY_DOWN))  g_cam_el  = fmaxf(g_cam_el - ROT_SPD * dt, 0.10f);
        float scroll = input_get_scroll_y();
        if (scroll != 0.f && !WorldMapUI::Get().IsOpen() && !SceneRender::Get().terrain_edit) {
            g_cam_dist = fmaxf(fminf(g_cam_dist - scroll * ZOOM_SPD, 40.f), 3.f);
        }

        // Dual CTM modes (Titan Quest / Diablo pattern):
        //   Mode 1 — Hold LMB   : continuously follow cursor; STOP on release.
        //   Mode 2 — Quick click: single destination; character auto-walks there.
        // Threshold: hold < CLICK_MS → Mode 2 (keep target after release).
        //            hold ≥ CLICK_MS → Mode 1 (stop on release).
        // WASD always overrides both modes.
        bool lmb = input_mouse_down(MOUSE_BUTTON_LEFT);
        static bool  s_lmb_prev_ctm = false;
        static float s_lmb_hold_ms  = 0.f;
        static constexpr float CLICK_MS = 200.f;

        bool lmb_rising  = lmb  && !s_lmb_prev_ctm;
        bool lmb_falling = !lmb &&  s_lmb_prev_ctm;

        if (lmb)       s_lmb_hold_ms += dt * 1000.f;
        if (lmb_rising) s_lmb_hold_ms = 0.f;  // reset on each new press
        if (lmb_falling) {
            // Released: Mode 1 (long hold) → stop; Mode 2 (quick click) → keep going
            if (s_lmb_hold_ms >= CLICK_MS)
                s_ctm_active = false;           // long hold → stop on release
            // else: quick click → ctm_active stays true, character continues
        }
        s_lmb_prev_ctm = lmb;

        {
            int _sw = window_get_width(), _sh = window_get_height();
            int _bx = _sw - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN;
            int _by = _sh - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN;
            bool over_cam_btn = ((int)mx >= _bx && (int)mx < _bx + SceneRender::CAM_BTN_SZ &&
                                (int)my >= _by && (int)my < _by + SceneRender::CAM_BTN_SZ);
#ifdef MONKEY_DUST_EDITOR
        if (lmb && !rmb && !SceneRender::Get().terrain_edit && !ImGui::GetIO().WantCaptureMouse && !WorldMapUI::Get().IsOpen() && !over_cam_btn)
#else
        if (lmb && !rmb && !SceneRender::Get().terrain_edit && !WorldMapUI::Get().IsOpen() && !over_cam_btn)
#endif
        {
            int sw = window_get_width(), sh = window_get_height();
            if (sw > 0 && sh > 0) {
                static constexpr float DEG2R = 0.01745329251f;
                float asp = (float)sw / (float)sh;
                Mat4 view = camera.ViewMatrix();
                const float* vf = mat4_ptr(view);
                float thf = tanf(camera.fovy * 0.5f * DEG2R);
                float ndx = (2.f * mx / (float)sw) - 1.f;
                float ndy = 1.f - (2.f * my / (float)sh);
                float rdx = vf[0]*(ndx*thf*asp) + vf[1]*(ndy*thf) - vf[2];
                float rdy = vf[4]*(ndx*thf*asp) + vf[5]*(ndy*thf) - vf[6];
                float rdz = vf[8]*(ndx*thf*asp) + vf[9]*(ndy*thf) - vf[10];
                if (rdy < -1e-6f) {
                    float t = (camera.target.y - camera.pos.y) / rdy;
                    s_ctm_x = camera.pos.x + t * rdx;
                    s_ctm_z = camera.pos.z + t * rdz;
                    s_ctm_active = true;
                }
            }
        }
        } // over_cam_btn scope
    }


    // Camera target at character waist (not feet) — L2-style orbit.
    // GetHeight(x,z)'s convenience overload silently returns 0.f when the
    // player's own chunk isn't loaded/ready yet — keep the last known-good
    // ground height instead of snapping the target to world-Y=0 for a frame.
    static float s_last_ground_y = 0.f;
    float ground_y;
    if (!TerrainQuery::Get().GetHeight(cam_x, cam_z, ground_y))
        ground_y = s_last_ground_y;
    else
        s_last_ground_y = ground_y;
    camera.target.x = cam_x;
    camera.target.y = ground_y + 1.0f;   // ~waist/chest height
    camera.target.z = cam_z;

    // Camera spring arm — endpoint-only height clamp.
    // Ray-march approach caused false-positive snapping → camera trembling.
    // Jolt CastRay caused physics-mesh trembling at chunk seams.
    // Endpoint clamp: if camera endpoint would be underground, reduce dist.
    static float s_spring_dist = 22.f;
    {
        float ex = cam_x + g_cam_dist * cosf(g_cam_el) * sinf(g_cam_az);
        float ey = camera.target.y + g_cam_dist * sinf(g_cam_el);
        float ez = cam_z  - g_cam_dist * cosf(g_cam_el) * cosf(g_cam_az);
        // GetHeight(x,z)'s convenience overload silently returns 0.f (absolute
        // world Y=0) when the queried chunk isn't loaded/ready — e.g. the camera
        // endpoint straying into a chunk at the edge of the streaming window
        // during fast/continuous uphill movement. Comparing ey against a bogus
        // 0.f "ground" instead of skipping the clamp let the endpoint clip
        // straight through a hillside with no warning (task terrain-patches:
        // "провалюється під текстуру" — camera embedded in the slope, visible
        // as a screen-filling out-of-focus close-up until movement stopped and
        // this recovered on its own).
        float cam_gnd;
        bool  cam_gnd_ok = TerrainQuery::Get().GetHeight(ex, ez, cam_gnd);
        float safe_dist = (cam_gnd_ok && ey < cam_gnd + 4.0f)
            ? fmaxf((cam_gnd + 4.0f - camera.target.y) / fmaxf(sinf(g_cam_el), 0.05f), 6.f)
            : g_cam_dist;
        safe_dist = fmaxf(fminf(safe_dist, g_cam_dist), 6.f);
        s_spring_dist = s_spring_dist + (safe_dist - s_spring_dist) * fminf(dt * 6.f, 1.f);
    }
    float eff_dist = s_spring_dist;
    camera.pos.x = cam_x + eff_dist * cosf(g_cam_el) * sinf(g_cam_az);
    camera.pos.y = camera.target.y + eff_dist * sinf(g_cam_el);
    camera.pos.z = cam_z  - eff_dist * cosf(g_cam_el) * cosf(g_cam_az);
    // Y-clamp fallback: if Jolt terrain not ready yet, ensure camera
    // is never below terrain surface at its own XZ position.
    // Same silent-0.f hazard as above — only clamp when the query actually
    // succeeded, otherwise leave camera.pos.y as computed (unclamped is no
    // worse than before; clamping to a bogus world-Y=0 floor is actively wrong
    // on any terrain not near sea level).
    {
        float eye_gnd;
        if (TerrainQuery::Get().GetHeight(camera.pos.x, camera.pos.z, eye_gnd)
                && camera.pos.y < eye_gnd + 0.5f)
            camera.pos.y = eye_gnd + 0.5f;
    }
}  // UpdateOrbitCamera
