// Dynres v3 controller (game/src/render/dynres3.h), CPU only: model recovery under noise and outliers, settling, no flapping at steady load, the bias-aware rule, and a check that the test can fail.
#include "../game/src/render/dynres3.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <monkey_dust/render/size_slots.h>
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
        Dynres3 d; Sim sim{ 4.f, 10.f, 0.3f, 0.f, 0.f }; int ch; float tt; Run(d, sim, 7200, &ch, &tt);   // no spikes: two spikes in a row are a panic by design (0.8)
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
    { // 7. prediction (0.5): the GPU cost ramps up (F grows 3 -> 9 ms over 90 frames, as when a heavy view opens); with prediction the scale drops earlier and fewer frames go over the budget
        auto run = [](bool predict, int* over, float* first_drop) {
            Dynres3 d; d.predict = predict; d.F = 3.f; d.K = 10.f; d.Motion(20.f); *over = 0; *first_drop = -1.f;
            for (int i = 0; i < 400; ++i) {
                const float F = i < 100 ? 3.f : (i < 190 ? 3.f + 6.f * (float)(i - 100) / 90.f : 9.f);
                const float s = d.Scale(), t = F + 10.f * s * s;
                if (t > 0.95f * d.target_ms) ++*over;
                if (*first_drop < 0.f && d.Scale() < 1.f) *first_drop = (float)i;
                d.Sample(s, t);
            }
        };
        int o0, o1; float f0, f1; run(false, &o0, &f0); run(true, &o1, &f1);
        CHECK(o1 < o0, "frames over the band top: %d with prediction, %d without", o1, o0);
        CHECK(f1 >= 0.f && f0 >= 0.f && f1 < f0, "first scale drop at frame %.0f with prediction, %.0f without", f1, f0);
        printf("prediction: frames over the band top %d -> %d, first drop %.0f -> %.0f\n", o0, o1, f0, f1);
        Dynres3 st; st.predict = true; Sim ss{ 4.f, 10.f, 0.3f, 0.f, 0.f }; int ch; float tt; Run(st, ss, 7200, &ch, &tt);
        CHECK(st.Scale() == 1.f && ch == 0, "prediction on a light steady scene: scale %.2f, %d changes", st.Scale(), ch);
    }
    { // 8. dynres 0.8: one-step drop, panic, motion memory, noisy model, no oscillation
        auto fitted = []() { Dynres3 d; Sim sim{ 4.f, 24.f, 0.f, 0.f, 0.f }; for (int i = 0; i < 400; ++i) { const float s = 0.67f + 0.33f * (0.5f + 0.5f * std::sin(i * 0.05f)); d.Sample(s, sim.T(s, d.target_ms)); } d.scale = 1.f; d.since_change_s = 10.f; d.above = d.below = d.panic_run = 0; d.panic_sum = d.above_sum = 0.f; d.drops = d.panics = 0; return d; };
        { // a step load at native (T(1) = 28): the scale reaches s* within hold + 1 frames (old rule: 0.97 after the same frames)
            Dynres3 d = fitted(); int i = 0; for (; i < 20 && d.Scale() >= 0.999f; ++i) d.Sample(1.f, 28.f);
            const float sstar = std::sqrt((d.target_ms - 4.f - (0.3f + 1.5f * d.sigma)) / 24.f);
            CHECK(i <= d.hold + 1, "step load: first drop after %d frames", i);
            CHECK(d.Scale() == d.LevelDown(std::max(d.floor_scale, sstar)) && d.Scale() < 0.8f, "step load: scale %.3f, s* %.3f", d.Scale(), sstar);
            Dynres3 o = fitted(); o.one_step = false; o.levels_on = false; for (int k = 0; k <= o.hold; ++k) o.Sample(1.f, 28.f);
            CHECK(o.Scale() > 0.95f, "0.7 rule drops one step only: %.2f", o.Scale());
        }
        { // levels: every scale the controller ever outputs is one of the 5 levels (no other render size exists), s* rounds down, a raise goes one level up
            Dynres3 d; Sim sim{ 4.f, 24.f, 0.5f, 0.05f, 0.f }; bool ok = true; int distinct_mask = 0;
            for (int i = 0; i < 20000; ++i) { const float F = 3.f + 6.f * (0.5f + 0.5f * std::sin(i * 0.002f)); const float sc = d.Scale(); d.Sample(sc, std::max(0.5f, F + 24.f * sc * sc + sim.T(sc, 99.f) - sim.F - sim.K * sc * sc)); if (i % 7 == 0) d.StopMotion(); if (i % 7 == 3) d.StartMotion();
                bool lv = false; for (int k = 0; k < 5; ++k) if (d.Scale() == d.levels[k]) { lv = true; distinct_mask |= 1 << k; } ok = ok && lv; }
            CHECK(ok, "the scale left the level set"); CHECK(d.LevelDown(0.80f) == 0.75f && d.LevelDown(0.66f) == 0.67f && d.LevelUp(0.75f) == 0.83f && d.LevelUp(1.f) == 1.f, "level rounding");
            printf("levels visited mask %d\n", distinct_mask);
        }
        { // panic: two frames over 1.25 x target drop at once, below the hold
            Dynres3 d = fitted(); d.Sample(1.f, 30.f); CHECK(d.Scale() >= 0.999f, "one bad frame must not drop");
            d.Sample(1.f, 30.f); CHECK(d.Scale() < 0.9f && d.panics == 1, "panic: scale %.2f panics %d", d.Scale(), d.panics);
        }
        { // motion memory: standing keeps the model and the last motion scale; the start of motion resumes it, and standing frames at native never change it
            Dynres3 d = fitted(); for (int i = 0; i < 10; ++i) d.Sample(1.f, 28.f); const float sm = d.Scale();
            d.StopMotion(); for (int i = 0; i < 300; ++i) d.Sample(1.f, 12.f);
            CHECK(d.Scale() == sm, "standing changed the scale: %.2f -> %.2f", sm, d.Scale());
            d.StartMotion(); CHECK(d.Scale() == sm, "motion start scale %.2f, last in motion %.2f", d.Scale(), sm);
        }
        { // a noisy model: every sample is 3 ms noise and 15 %% outliers, the drop still lands at or above the floor and under the target
            Dynres3 d; Sim sim{ 4.f, 24.f, 3.f, 0.15f, 0.f }; int ch; float tt; Run(d, sim, 7200, &ch, &tt);
            CHECK(d.Scale() >= d.floor_scale && d.Scale() <= 1.f && tt <= 16.7f + 1.5f, "noisy: scale %.2f true T %.2f", d.Scale(), tt);
        }
        { // constant load: no oscillation (changes in the last minute)
            Dynres3 d; Sim sim{ 4.f, 24.f, 0.3f, 0.02f, 0.f }; int ch; float tt; Run(d, sim, 7200, &ch, &tt);
            CHECK(ch <= 6, "constant load: %d changes in the last minute", ch);
            printf("0.8 constant load: scale %.2f T %.2f changes/min %d\n", d.Scale(), tt, ch);
        }
    }
    { // 9. DE-362: after the warm-up a change of scale creates no target set. A fake pass with the same slot cache as the real ones (md::SlotSwapIn / SlotPushCur) is driven by the controller over
      // a step load, a panic, stop / start of motion and a ramp; the creation counter must stay 0 after the warm-up (and not be 0 before it: the check can fail)
        struct Fake { int w = 0, h = 0; bool Empty() const { return w == 0; } };
        auto size_of = [](float s) { return (int)(1920 * s + 0.5f); };
        Fake cur, alt[4]; int rr = 0;
        auto ensure = [&](int w) { if (cur.w == w) return; if (md::SlotSwapIn(cur, alt, [&](const Fake& f) { return f.w == w; })) return; md::SlotPushCur(cur, alt, rr, [](Fake& f) { f = Fake{}; }); cur.w = w; };
        Dynres3 d; const int before = md::TargetCreations();
        for (int k = 0; k < d.nlev; ++k) ensure(size_of(d.levels[k]));
        ensure(size_of(1.f));
        const int warm = md::TargetCreations() - before;
        CHECK(warm == d.nlev, "warm-up created %d sets, expected %d", warm, d.nlev);
        Sim sim{ 4.f, 24.f, 0.4f, 0.05f, 0.f }; int changes = 0; float last = 1.f;
        for (int i = 0; i < 30000; ++i) {
            const float F = 3.f + 7.f * (0.5f + 0.5f * std::sin(i * 0.003f)) + ((i / 2500) % 2 ? 6.f : 0.f);   // ramp + steps
            const float sc = d.Scale(); d.Sample(sc, std::max(0.5f, F + 24.f * sc * sc + (sim.T(sc, 99.f) - sim.F - sim.K * sc * sc)));
            if ((i / 900) % 3 == 1) d.StopMotion(); else d.StartMotion();
            const float shown = d.moving ? d.Scale() : 1.f; if (shown != last) { ++changes; last = shown; ensure(size_of(shown)); }
        }
        CHECK(md::TargetCreations() - before == warm, "%d target sets created after the warm-up over %d scale changes", md::TargetCreations() - before - warm, changes);
        CHECK(changes > 20, "the scenario made only %d scale changes", changes);
        Fake c2, a2[4]; int r2 = 0; int cr0 = md::TargetCreations();   // the control: without the warm-up every level is a creation
        auto ens2 = [&](int w) { if (c2.w == w) return; if (md::SlotSwapIn(c2, a2, [&](const Fake& f) { return f.w == w; })) return; md::SlotPushCur(c2, a2, r2, [](Fake& f) { f = Fake{}; }); c2.w = w; };
        for (int k = 0; k < d.nlev; ++k) ens2(size_of(d.levels[k]));
        CHECK(md::TargetCreations() - cr0 == d.nlev, "control: %d creations without a warm-up", md::TargetCreations() - cr0);
        printf("0.8 levels: %d scale changes, %d sets created at warm-up, 0 after\n", changes, warm);
    }
    { // dynres 0.9 (DE-366) items 1 and 2: the raise wait is a parameter, and a turn above turn_thr blocks a raise (the defaults 0.5 s / off are the 0.8 behaviour)
        auto first_raise_frame = [](float raise_gap, float turn_thr, float turn_dps, int turn_frames) {
            Dynres3 d; d.scale = 0.75f; d.moving = true; d.F = 12.f; d.K = 3.f; d.raise_gap_s = raise_gap; d.turn_thr = turn_thr; d.last_dt = 1.f / 60.f; d.since_change_s = 0.f;
            Sim sim{ 12.f, 3.f, 0.01f, 0.f, 0.f };   // T(0.75) = 13.7 ms (below 0.85 x target), T(0.83) = 14.1 < target - margin: room to raise
            for (int i = 0; i < 1200; ++i) { d.Motion(0.f, i < turn_frames ? turn_dps : 0.f); const float s = d.Scale(); d.Sample(s, sim.T(s, d.target_ms)); if (d.Scale() > 0.75f) return i; }
            return -1; };
        const int base = first_raise_frame(0.5f, 0.f, 0.f, 0), longer = first_raise_frame(2.0f, 0.f, 0.f, 0);
        CHECK(base >= 0 && longer > base + 60, "raise wait: 0.5 s raised at frame %d, 2.0 s at %d (must be later by more than 60 frames)", base, longer);
        const int off = first_raise_frame(0.5f, 0.f, 90.f, 300), on = first_raise_frame(0.5f, 30.f, 90.f, 300);
        CHECK(off >= 0 && off < 300, "turn off: raised at frame %d during the turn", off);
        CHECK(on >= 300, "turn_thr 30 deg/s: raised at frame %d, not after the 300-frame turn", on);
        const int slow = first_raise_frame(0.5f, 30.f, 10.f, 300);
        CHECK(slow >= 0 && slow < 300, "a turn below the threshold must not block the raise (frame %d)", slow);
        printf("0.9 items 1,2: raise at %d (0.5 s) / %d (2.0 s); turn blocks: %d vs %d\n", base, longer, off, on);
    }
    { // dynres 0.9 (DE-366) item 3: the grass half-resolution level (default 0 = the floor as 0.8) engages above the floor when gh_level is raised
        auto grass_on = [](int gh_level, float at_scale) {
            Dynres3 d; d.heavy = true; d.scale = at_scale; d.gh_level = gh_level; d.last_full_ms = d.target_ms + 5.f;
            for (int i = 0; i < 300; ++i) d.GrassStep(1.f / 60.f);
            return d.grass_half; };
        CHECK(!grass_on(0, 0.83f), "default: grass must stay full at scale 0.83");
        CHECK(grass_on(0, 0.67f), "default: grass half at the floor over budget");
        CHECK(grass_on(2, 0.83f), "gh_level 2: grass half at 0.83 over budget");
        CHECK(!grass_on(2, 0.92f), "gh_level 2: grass full at 0.92");
        printf("0.9 item 3 grass level checks done\n");
    }
    { // dynres 0.9: the run-time setter (md.set_dynres_params) reaches the controller fields and a fresh controller starts from scale 1.0 with empty rings
        Dynres3 d; for (int i = 0; i < 400; ++i) { const float sc = d.Scale(); d.Sample(sc, 24.f + 20.f * sc * sc); }
        CHECK(d.Scale() < 1.f && d.n > 0, "the controller must have moved before the reset (scale %.2f, n %d)", d.Scale(), d.n);
        Dynres3 f; f.SetLevelCount(d.nlev); f.SetParams(1.0f, 30.f, true, 2);
        CHECK(f.raise_gap_s == 1.0f && f.turn_thr == 30.f && f.predict && f.gh_level == 2, "SetParams must reach the fields");
        CHECK(f.Scale() == 1.f && f.n == 0, "a fresh controller: scale %.2f n %d", f.Scale(), f.n);
        Dynres3 g; g.SetParams(-1.f, 0.f, false, 0); CHECK(g.raise_gap_s == 0.5f && g.turn_thr == 0.f && !g.predict, "raise_s < 0 keeps the default");
        printf("0.9 SetParams checks done\n");
    }
    { // dynres 0.9 retest arm (DE-366): the PID arm drops under a load over the budget, holds in the dead band, raises when there is room, and an off arm (pid_kp 0) is the band controller
        auto run = [](float kp, float ms_at_1, int frames) { Dynres3 d; d.pid_kp = kp; d.last_dt = 1.f / 60.f;
            for (int i = 0; i < frames; ++i) { const float s = d.Scale(); d.Sample(s, ms_at_1 * s * s + 6.f); } return d.Scale(); };
        CHECK(run(0.12f, 40.f, 600) < 1.f, "PID: a load over the budget must lower the scale (%.2f)", run(0.12f, 40.f, 600));
        CHECK(run(0.12f, 4.f, 600) == 1.f, "PID: a load with room stays at 1.0 (%.2f)", run(0.12f, 4.f, 600));
        Dynres3 d; d.pid_kp = 0.12f; d.scale = d.pid_s = 0.67f; d.last_dt = 1.f / 60.f; for (int i = 0; i < 900; ++i) { const float s = d.Scale(); d.Sample(s, 4.f * s * s + 6.f); }
        CHECK(d.Scale() > 0.67f, "PID: with room the scale rises from the floor (%.2f)", d.Scale());
        Dynres3 e; e.pid_kp = 0.12f; e.scale = e.pid_s = 0.83f; e.last_dt = 1.f / 60.f; for (int i = 0; i < 600; ++i) e.Sample(e.Scale(), 16.5f);
        CHECK(e.Scale() == 0.83f, "PID: inside the dead band the scale holds (%.2f)", e.Scale());
        printf("0.9 PID arm checks done\n");
    }
    { // dynres 0.9 item 6: an alternating 14 / 30 ms load (mean 22, band top 15.9) never reaches 5 consecutive frames over the band: the consecutive rule keeps the scale, the ring-mean switch drops it
        auto drops = [](bool ring) { Dynres3 d; d.ring_mean = ring; d.scale = 0.83f; d.last_dt = 1.f / 60.f; int dn = 0; float prev = d.Scale();
            for (int i = 0; i < 300; ++i) { const float s = d.Scale(); d.Sample(s, (i & 1) ? 30.f : 14.f); if (d.Scale() < prev) ++dn; prev = d.Scale(); } return dn; };
        const int off = drops(false), on = drops(true);
        CHECK(off == 0, "alternating load, consecutive rule: %d drops (the defect: expected 0)", off);
        CHECK(on >= 1, "alternating load, ring mean: %d drops (expected >= 1)", on);
        printf("0.9 item 6 ring mean: drops off %d on %d\n", off, on);
    }
    { // dynres 0.9 item 6 on a stream whose mean fits: alternating 10 / 20 ms (mean 15 < band top 15.9, target 16.7) must not drop with the ring mean; a stream of mean 19 must
        auto drops = [](float a, float b) { Dynres3 d; d.ring_mean = true; d.scale = 0.83f; d.last_dt = 1.f / 60.f; int dn = 0; float prev = d.Scale();
            for (int i = 0; i < 600; ++i) { const float s = d.Scale(); d.Sample(s, (i & 1) ? b : a); if (d.Scale() < prev) ++dn; prev = d.Scale(); } return dn; };
        CHECK(drops(10.f, 20.f) == 0, "ring mean: a 10/20 ms stream (mean 15) dropped the scale");
        CHECK(drops(14.f, 30.f) >= 1, "ring mean: a 14/30 ms stream (mean 22) did not drop");
        printf("0.9 item 6 fit stream: drops 10/20 %d, 14/30 %d\n", drops(10.f, 20.f), drops(14.f, 30.f));
    }
    printf(fails ? "dynres3 test: FAIL (%d)\n" : "dynres3 test: PASS\n", fails);
    return fails ? 1 : 0;
}
