#pragma once
// Dynres v3 controller (DE-275 / DE-278 / DE-279): the cost model T = F + K * s^2 (s = linear scale, 1 = native) fitted online by IRLS-Huber with exponential forgetting on (scale, GPU ms) pairs,
// and Dagor's hysteresis (render/dynamicResolution.cpp: band 0.85 - 0.95 x target, a step only after the value stayed outside the band for more frames than the ring is long; design only, no code copied).
// Plain data, no allocation, no GPU calls: the caller feeds GpuDevice::WaiterGpuMs() and applies Scale().
#include <algorithm>
#include <cmath>

inline constexpr const char* kDynresVersion = "dynres 0.8";

struct Dynres3 {
    static constexpr int kN = 240;                         // samples kept (4 s at 60 fps)
    float target_ms = 16.7f;                               // the frame budget now in force: light_ms, or heavy_ms while a heavy scene is on (Scene)
    float light_ms = 16.7f, heavy_ms = 33.3f;              // dynres 0.4 (DE-296 / DE-307): 60 fps normally, 30 fps while grass (later particles / weather) is drawn
    float switch_s = 1.0f;                                 // the heavy flag follows the scene signal only after it held for this long (no flapping at a zone border)
    bool  heavy = false; float held_s = 0.f;
    float lambda = 0.985f;                                 // forgetting per sample: weight 0.985^age, effective window ~ 67 samples (1.1 s at 60 fps)
    float floor_scale = 0.67f, step = 0.03f;               // DE-258 floor; Dagor's step 0.01 * clamp(round(200 / 60), 1, 5)
    int   hold = 4;                                        // Dagor: frames outside the band must exceed the ring (waiter ring 4 + 2)
    float fit_min_frac = 0.8f;                             // bias-aware: the waiter reads high below the 60 fps budget, fit only on T > 0.8 * light_ms
    // dynres 0.5 (DE-307, off by default): prediction. Two moving averages of the GPU ms at the controlled scale give a trend (ms per frame); the band test uses
    // max(measured, trend-projected ms over a horizon that grows with the camera speed), so the scale drops before the frame goes over the budget. Raising still waits
    // for the measured value and the projection both to be under the band; the floor (DE-258) is unchanged. Speed only: altitude is not used.
    bool  predict = false;
    float speed_mps = 0.f;
    float ema_fast = 0.f, ema_slow = 0.f, projected = 0.f;
    // dynres 0.8 (DE-362, owner 19.11): (1) one-step drop: over the band for more than `hold` frames the scale goes straight to s* of the cost model (kept at -step while the model has no spread yet);
    // (2) panic (Unreal): two frames in a row over panic_k x target drop at once, no hold; (3) motion memory: StartMotion() resumes the last scale used in motion; (4) a raise waits raise_gap_s after any change.
    // s* = sqrt((target - Fnow - margin) / K) with Fnow = mean of the frames that triggered the drop minus K * scale^2 (the model intercept corrected by what was just measured, so a step in the load is followed at once).
    // levels (coordinator 7008783700): the scale is one of kLevels so every size has pre-warmed render targets and a change allocates nothing; s* rounds DOWN to a level, a raise goes one level up.
    bool  levels_on = true; int nlev = 5; float levels[5] = { 0.67f, 0.75f, 0.83f, 0.92f, 1.0f };
    float LevelDown(float x) const { float r = levels[0]; for (int i = 0; i < nlev; ++i) if (levels[i] <= x + 1e-4f) r = levels[i]; return r; }   // the highest level not above x
    float LevelUp(float x) const { for (int i = 0; i < nlev; ++i) if (levels[i] > x + 1e-4f) return levels[i]; return levels[nlev - 1]; }
    float LevelBelow(float x) const { float r = levels[0]; for (int i = 0; i < nlev; ++i) if (levels[i] < x - 1e-4f) r = levels[i]; return r; }
    void SetLevelCount(int n) { // 5: .67 .75 .83 .92 1; 4: .67 .78 .89 1; 3: .67 .83 1 (fewer sets of render targets when the memory of 5 is too much)
        static const float l5[5] = { 0.67f, 0.75f, 0.83f, 0.92f, 1.0f }, l4[4] = { 0.67f, 0.78f, 0.89f, 1.0f }, l3[3] = { 0.67f, 0.83f, 1.0f };
        const float* src = n >= 5 ? l5 : (n == 4 ? l4 : l3); nlev = n >= 5 ? 5 : (n == 4 ? 4 : 3); for (int i = 0; i < nlev; ++i) levels[i] = src[i];
    }
    bool  one_step = true;                                 // false = the 0.7 rule (scale - step)
    float panic_k = 1.25f, raise_gap_s = 0.5f, since_change_s = 10.f, last_dt = 1.f / 60.f, motion_scale = 1.f;
    int   panic_run = 0; float panic_sum = 0.f, above_sum = 0.f; bool fitted = false, moving = true;
    int   drops = 0, panics = 0;
    void StopMotion() { if (moving) { moving = false; motion_scale = scale; } }      // the caller: the camera came to rest (native resolution while standing)
    void StartMotion() { if (!moving) { moving = true; scale = std::min(1.f, std::max(floor_scale, motion_scale)); above = below = panic_run = 0; since_change_s = 0.f; } }
    // state
    float F = 6.5f, K = 20.f;                              // prior from docs/research/DYNRES_V2_2026-11-12.md (street / top40 / high150: F 5.2-7.4, K 19-23)
    float sigma = 1.0f;                                    // residual scale (ms), Huber
    float scale = 1.0f;
    int   above = 0, below = 0, n = 0, head = 0;
    float xs[kN] = {}, ys[kN] = {};

    // dynres 0.7 (owner 17.11, coordinator 6942547662): the grass overlay at half resolution is the LAST step of dynres in grass: it switches on only while the scale sits at its floor and the frame is still over the
    // budget in force (heavy_ms with grass), and off again once the frame fits with grass at full scale (normalised: the caller adds gh_saving to the ms of frames drawn with half-resolution grass), each after gh_hold_s.
    bool  gh_enable = true, grass_half = false;   // default on (owner 17.11) after the oracle checks
    float gh_saving = 9.f;                                 // ms saved by half-resolution grass at the floor scale (ABBA 17.11, zone 17,28, scene 0.67: 31.9 -> 21.9 = 10 ms; 9 keeps a margin)
    float gh_hold_s = 1.0f, gh_on_s = 0.f, gh_off_s = 0.f, last_full_ms = 0.f;
    float GrassScale() const { return grass_half ? 0.5f : 1.0f; }
    int   gh_level = 0;                                    // dynres 0.9 item 3: level index below which (inclusive) the grass goes to half resolution; 0 = the floor level as 0.8
    float GhScale() const { return gh_level > 0 ? levels[gh_level < nlev ? gh_level : nlev - 1] : floor_scale; }
    void GrassStep(float dt) {                             // once per frame
        if (!gh_enable || !heavy) { grass_half = false; gh_on_s = gh_off_s = 0.f; return; }
        const bool at_floor = scale <= GhScale() + 0.005f;
        if (!grass_half) { gh_on_s = (at_floor && last_full_ms > target_ms) ? gh_on_s + dt : 0.f; if (gh_on_s >= gh_hold_s) { grass_half = true; gh_on_s = gh_off_s = 0.f; } }
        else { gh_off_s = (!at_floor || last_full_ms <= 0.9f * target_ms) ? gh_off_s + dt : 0.f; if (gh_off_s >= gh_hold_s) { grass_half = false; gh_on_s = gh_off_s = 0.f; } }
    }

    void Scene(bool heavy_now, float dt) {                 // once per frame: the scene signal (data: grass blades in range) and the frame time
        last_dt = dt;
        if (heavy_now == heavy) { held_s = 0.f; return; }
        held_s += dt;
        if (held_s >= switch_s) { heavy = heavy_now; held_s = 0.f; target_ms = heavy ? heavy_ms : light_ms; above = below = 0; }
    }

    // dynres 0.9 (DE-366 item 2, off by default): a camera turn faster than turn_thr (deg/s, the smoothed yaw rate of the caller) counts as motion next to the speed: a raise waits while it lasts
    float turn_thr = 0.f, turn_dps = 0.f;
    void SetParams(float raise_s, float turn, bool pred, int gh) { if (raise_s >= 0.f) raise_gap_s = raise_s; turn_thr = turn; predict = pred; gh_level = gh; }   // dynres 0.9 switches (md.set_dynres_params)
    void Motion(float speed, float yaw_dps = 0.f) { speed_mps = speed; turn_dps = yaw_dps; }   // once per frame: the smoothed camera speed in m/s (0..) and yaw rate in deg/s
    void Sample(float s, float gpu_ms) {                   // one frame: the scale it was rendered at and its GPU ms
        if (!(gpu_ms > 0.f) || gpu_ms > 200.f) return;
        last_full_ms = gpu_ms;
        if (gpu_ms > fit_min_frac * light_ms) { xs[head] = s * s; ys[head] = gpu_ms; head = (head + 1) % kN; if (n < kN) ++n; Fit(); }
        if (!moving || std::fabs(s - scale) > 0.005f) return;            // a frame at another scale (standing = native) feeds the model only; the band is about the controlled scale
        const float hi = 0.95f * target_ms, lo = 0.85f * target_ms;
        float seen = gpu_ms;
        if (predict) {
            if (ema_fast == 0.f) ema_fast = ema_slow = gpu_ms;
            const float g = std::min(std::max(gpu_ms, ema_slow - 2.f), ema_slow + 2.f);   // an outlier moves the trend by at most 2 ms
            ema_fast += 0.2f * (g - ema_fast); ema_slow += 0.05f * (g - ema_slow);
            const float slope = std::max(0.f, (ema_fast - ema_slow) / 15.f);       // ms per frame (the averages lag 4 and 19 frames apart)
            const float horizon = 6.f + 0.5f * std::min(speed_mps, 24.f);          // frames ahead: 6 standing, up to 18 at 24 m/s
            projected = ema_fast + slope * horizon; seen = std::max(gpu_ms, projected);
        }
        since_change_s += last_dt;
        if (turn_thr > 0.f && turn_dps > turn_thr) since_change_s = 0.f;     // dynres 0.9 item 2: turning keeps the raise wait running
        above = seen > hi ? above + 1 : 0; below = (gpu_ms < lo && (!predict || projected < lo)) ? below + 1 : 0;
        above_sum = above > 0 ? above_sum + seen : 0.f;
        const bool over = gpu_ms > panic_k * target_ms;
        panic_run = over ? panic_run + 1 : 0; panic_sum = over ? panic_sum + gpu_ms : 0.f;
        const bool panic = one_step && panic_run >= 2;
        if (panic || above > hold) {
            const float mean = panic ? panic_sum / (float)panic_run : above_sum / (float)above;
            const float old = scale;
            scale = one_step ? DropTo(mean, panic ? panic_run : above) : (levels_on ? LevelBelow(scale) : std::max(floor_scale, scale - step));
            above = panic_run = 0; panic_sum = above_sum = 0.f;
            if (scale < old) { since_change_s = 0.f; ++drops; if (panic) ++panics; }
        }
        else if (below > hold) {
            below = 0;
            const float next = levels_on ? LevelUp(scale) : std::min(1.f, scale + step), margin = 0.3f + 1.5f * sigma;
            if (since_change_s >= raise_gap_s && F + K * next * next <= target_ms - margin) { scale = next; since_change_s = 0.f; }   // raise only when the model predicts room, minus a margin that grows with the residual spread, and not right after a change
        }
    }
    float Predict(float s) const { return F + K * s * s; }
    float DropTo(float mean_ms, int) const {               // s* for a measured mean `mean_ms` at the current scale; never above scale - step, never below the floor
        if (!fitted) return levels_on ? LevelBelow(scale) : std::max(floor_scale, scale - step);
        const float margin = 0.3f + 1.5f * sigma, fnow = mean_ms - K * scale * scale;
        const float sstar = std::sqrt(std::max(0.f, (target_ms - fnow - margin) / K));
        if (levels_on) return std::min(LevelBelow(scale), LevelDown(sstar));
        return std::max(floor_scale, std::min(scale - step, sstar));
    }
    float Scale() const { return scale; }

private:
    void Fit() {   // IRLS with Huber weights over the ring, weights lambda^age, ridge towards the prior (two pseudo-samples on the prior line)
        if (n < 8) return;
        double f = F, k = K;
        for (int it = 0; it < 3; ++it) {
            double Sw = 0.3, Sx = 0.3 * (0.5 + 1.0), Sy = 0.3 * ((6.5 + 20 * 0.5) + (6.5 + 20.0)), Sxx = 0.3 * (0.25 + 1.0), Sxy = 0.3 * (0.5 * (6.5 + 20 * 0.5) + 1.0 * (6.5 + 20.0));   // prior: x = 0.5, 1.0 (s = 0.71, 1.0), weight 0.15 each
            double wa = 1.0;
            for (int i = 1; i <= n; ++i) {
                const int j = (head - i + kN) % kN; const double x = xs[j], y = ys[j];
                const double r = std::fabs(y - (f + k * x)), c = 1.345 * sigma, hw = r <= c ? 1.0 : c / r;
                const double w = wa * hw; wa *= lambda;
                Sw += w; Sx += w * x; Sy += w * y; Sxx += w * x * x; Sxy += w * x * y;
            }
            const double det = Sw * Sxx - Sx * Sx;
            if (det < 1e-3 * Sw * Sw) return;                // the samples do not span two scales yet: keep the previous (F, K)
            k = (Sw * Sxy - Sx * Sy) / det; f = (Sy - k * Sx) / Sw;
        }
        if (k > 0.5 && k < 200.0 && f > -5.0 && f < 40.0) { K = (float)k; F = (float)f; fitted = true; }
        double a = 0, m = 0; for (int i = 1; i <= std::min(n, 60); ++i) { const int j = (head - i + kN) % kN; a += std::fabs(ys[j] - (F + K * xs[j])); ++m; }
        if (m > 0) sigma = std::max(0.15f, std::min(2.0f, 0.9f * sigma + 0.1f * (float)(1.4826 * a / m)));
    }
};
