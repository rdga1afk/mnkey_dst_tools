// Dynres v3 controller (game/src/render/dynres3.h), CPU only: model recovery under noise and outliers, settling, no flapping at steady load, the bias-aware rule, and a check that the test can fail.
#include "../game/src/render/dynres3.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Sim { float F, K, noise, outlier_p, bias_below; std::mt19937 rng{7}; float T(float s, float target) { std::normal_distribution<float> n(0.f, noise); std::uniform_real_distribution<float> u(0.f, 1.f);
    float t = F + K * s * s + n(rng); if (u(rng) < outlier_p) t += 8.f; if (t < target) t += bias_below; return t < 0.5f ? 0.5f : t; } };

// settle: run the controller against the simulated GPU, count scale changes in the last minute
static void Run(Dynres3& d, Sim& sim, int frames, int* changes_last60s, float* mean_t_tail) {
    int ch = 0; float last = d.Scale(); double tsum = 0; int tn = 0;
    for (int i = 0; i < frames; ++i) { const float s = d.Scale(); const float t = sim.T(s, d.target_ms); d.Sample(s, t); if (i >= frames - 3600 && d.Scale() != last) ++ch; last = d.Scale(); if (i >= frames - 600) { tsum += sim.F + sim.K * s * s; ++tn; } }
    *changes_last60s = ch; *mean_t_tail = (float)(tsum / tn);
}

int main() {
    { // 1. a heavy scene (F 4, K 24: T(1) = 28, T(0.67) = 14.8): the model is recovered, the scale settles in the band, few changes
        Dynres3 d; Sim sim{ 4.f, 24.f, 0.3f, 0.05f, 0.f }; int ch; float tt; Run(d, sim, 7200, &ch, &tt);
        CHECK(std::fabs(d.Predict(d.Scale()) - (4.f + 24.f * d.Scale() * d.Scale())) < 1.0f, "prediction at the operating scale %.2f vs true %.2f", d.Predict(d.Scale()), 4.f + 24.f * d.Scale() * d.Scale());   // at one operating scale F and K are not separable, the value there is
        CHECK(tt <= 16.7f, "true T at the settled scale %.2f over the target", tt);
        CHECK(d.Scale() >= 0.67f && d.Scale() <= 0.85f, "scale %.2f", d.Scale());
        CHECK(ch <= 12, "%d changes in the last minute", ch);
        printf("heavy: F %.2f K %.2f sigma %.2f scale %.2f T %.2f changes/min %d\n", d.F, d.K, d.sigma, d.Scale(), tt, ch);
    }
    { // 2. a light scene (T(1) = 14): native stays native, zero changes
        Dynres3 d; Sim sim{ 4.f, 10.f, 0.3f, 0.05f, 0.f }; int ch; float tt; Run(d, sim, 7200, &ch, &tt);
        CHECK(d.Scale() == 1.f && ch == 0, "light scene: scale %.2f changes %d", d.Scale(), ch);
    }
    { // 3. the floor: an unreachable load (T(0.67) = 24) stops at the floor, never below
        Dynres3 d; Sim sim{ 8.f, 36.f, 0.3f, 0.05f, 0.f }; int ch; float tt; Run(d, sim, 7200, &ch, &tt);
        CHECK(d.Scale() == d.floor_scale, "floor: scale %.2f", d.Scale());
    }
    { // 4. bias below the target (the waiter reads +1.8 ms there): the scale must not settle where the TRUE time is over the target
        Dynres3 d; Sim sim{ 4.f, 24.f, 0.3f, 0.05f, 1.8f }; int ch; float tt; Run(d, sim, 7200, &ch, &tt);
        CHECK(tt <= 16.7f + 0.2f, "biased signal: true T %.2f at scale %.2f", tt, d.Scale());
        printf("bias: scale %.2f true T %.2f changes/min %d\n", d.Scale(), tt, ch);
    }
    { // 5. a scale sweep (the scale moves between 0.67 and 1.0, e.g. turn / stand cycles): F and K are recovered, and with 15 %% outliers the Huber fit is closer than plain least squares over the same samples (the test can fail)
        Dynres3 d; Sim sim{ 4.f, 24.f, 0.3f, 0.15f, 0.f }; for (int i = 0; i < 1200; ++i) { const float s = 0.67f + 0.33f * (0.5f + 0.5f * std::sin(i * 0.05f)); d.Sample(s, sim.T(s, d.target_ms)); }
        CHECK(std::fabs(d.F - 4.f) < 1.5f && std::fabs(d.K - 24.f) < 4.f, "sweep: model F %.2f K %.2f (true 4 / 24)", d.F, d.K);
        double Sw = 0, Sx = 0, Sy = 0, Sxx = 0, Sxy = 0; for (int i = 0; i < d.n; ++i) { Sw += 1; Sx += d.xs[i]; Sy += d.ys[i]; Sxx += d.xs[i] * d.xs[i]; Sxy += d.xs[i] * d.ys[i]; }
        const double det = Sw * Sxx - Sx * Sx; const double kols = det > 1e-9 ? (Sw * Sxy - Sx * Sy) / det : 0, fols = det > 1e-9 ? (Sy - kols * Sx) / Sw : 0;
        const double eh = std::fabs(d.K - 24.f) + std::fabs(d.F - 4.f), eo = std::fabs(kols - 24.0) + std::fabs(fols - 4.0);
        CHECK(eh < eo, "Huber error %.2f not below least squares %.2f", eh, eo);
        printf("outliers: Huber |dF|+|dK| %.2f, OLS %.2f\n", eh, eo);
    }
    { // 6. dynres 0.4 scene budget: the target follows the scene signal only after it held 1 s; a signal that flaps faster never switches
        Dynres3 d;
        for (int i = 0; i < 59; ++i) d.Scene(true, 1.f / 60.f);
        CHECK(d.target_ms == d.light_ms && !d.heavy, "heavy before 1 s: target %.1f", d.target_ms);
        for (int i = 0; i < 3; ++i) d.Scene(true, 1.f / 60.f);
        CHECK(d.heavy && d.target_ms == d.heavy_ms, "heavy after 1 s: target %.1f", d.target_ms);
        Dynres3 f; for (int i = 0; i < 600; ++i) f.Scene((i / 40) % 2 == 0, 1.f / 60.f);   // flips every 0.67 s
        CHECK(!f.heavy, "a signal that flips every 0.67 s switched the target");
        for (int i = 0; i < 70; ++i) d.Scene(false, 1.f / 60.f);
        CHECK(!d.heavy && d.target_ms == d.light_ms, "back to light after 1 s: target %.1f", d.target_ms);
        // under the heavy target a 24 ms scene (T(1) = 24) stays native; under the light one it is pushed down
        Dynres3 h; h.target_ms = h.heavy_ms; h.heavy = true; Sim sh{ 6.f, 18.f, 0.3f, 0.05f, 0.f }; int ch; float tt; Run(h, sh, 7200, &ch, &tt);
        Dynres3 l; Sim sl{ 6.f, 18.f, 0.3f, 0.05f, 0.f }; Run(l, sl, 7200, &ch, &tt);
        CHECK(h.Scale() == 1.f && l.Scale() < 1.f, "T(1)=24: heavy target scale %.2f, light target scale %.2f", h.Scale(), l.Scale());
    }
    printf(fails ? "dynres3 test: FAIL (%d)\n" : "dynres3 test: PASS\n", fails);
    return fails ? 1 : 0;
}
