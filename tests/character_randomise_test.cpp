// #159 oracle (c): md::character RANDOMISE / RESET over every Range of every editor_data_*.xml. usage: md_character_randomise_test <editor_data dir>
// Rules and their exe sources: tools/editor/character/appearance.h. Fixed seed, 10 000 samples per slider.
#include "appearance.h"
#include <cmath>
#include <cstdio>

using namespace md::character;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 10) { std::printf("FAIL " __VA_ARGS__); std::printf("\n"); } } } while (0)

int main(int argc, char** argv) {
    std::vector<EditorLimits> all; std::string err;
    if (argc < 2 || !LoadEditorLimitsDir(argv[1], all, err)) { std::printf("FAIL load: %s\n", err.c_str()); return 1; }
    const int N = 10000; int sliders = 0, varying = 0, group_checks = 0;
    for (const EditorLimits& el : all) for (const Config& cfg : el.configs) {
        Rng rng(12345u);
        Appearance reset = MakeDefault(cfg);
        for (const Category& cat : cfg.categories) {
            std::map<std::string, std::vector<float>> samples;
            for (int i = 0; i < N; ++i) {
                Appearance a = reset;
                RandomiseCategory(cfg, cat.name, a, rng);
                for (const Range& r : cat.ranges) samples[r.name + "#" + std::to_string(&r - &cat.ranges[0])].push_back(a.values[r.name]);
                // sliders of one random_group move together: the offsets in units of their variation agree up to rounding
                for (size_t x = 0; x < cat.ranges.size(); ++x) for (size_t y = x + 1; y < cat.ranges.size(); ++y) {
                    const Range &p = cat.ranges[x], &q = cat.ranges[y];
                    int vp = RangeVariation(p), vq = RangeVariation(q);
                    if (!(p.has_random_group && q.has_random_group && p.random_group == q.random_group && vp >= 5 && vq >= 5)) continue;
                    float tp = (a.values[p.name] - RangeMid(p)) / vp, tq = (a.values[q.name] - RangeMid(q)) / vq;
                    ++group_checks;
                    CHECK(std::fabs(tp - tq) <= 1.f / vp + 1.f / vq + 1e-4f, "%s group %d: %s %.3f vs %s %.3f", el.file.c_str(), p.random_group, p.name.c_str(), tp, q.name.c_str(), tq);
                }
            }
            for (size_t k = 0; k < cat.ranges.size(); ++k) {
                const Range& r = cat.ranges[k]; ++sliders;
                const auto& s = samples[r.name + "#" + std::to_string(k)];
                double mean = 0; float lo = 1e9f, hi = -1e9f;
                for (float v : s) { mean += v; lo = std::min(lo, v); hi = std::max(hi, v); }
                mean /= s.size();
                int mid = RangeMid(r), var = RangeVariation(r);
                CHECK(mid < r.min || mid > r.max || (lo >= r.min && hi <= r.max), "%s %s: [%g,%g] outside [%d,%d]", el.file.c_str(), r.name.c_str(), lo, hi, r.min, r.max);
                CHECK(lo >= mid - var && hi <= mid + var, "%s %s: spread [%g,%g] > mid %d +- %d", el.file.c_str(), r.name.c_str(), lo, hi, mid, var);
                CHECK(std::fabs(mean - mid) <= 0.02 * std::max(1, r.max - r.min) + 0.5 * 0, "%s %s: mean %.3f vs mid %d", el.file.c_str(), r.name.c_str(), mean, mid);
                if (var > 0) { ++varying; CHECK(hi > lo, "%s %s: no spread with variation %d", el.file.c_str(), r.name.c_str(), var); }
                CHECK(reset.values[r.name] == float(mid), "%s %s: RESET %g != mid %d", el.file.c_str(), r.name.c_str(), reset.values[r.name], mid);
            }
        }
        Appearance a = MakeDefault(cfg); RandomiseAll(cfg, a, rng); Reset(cfg, a);
        for (const auto& kv : reset.values) CHECK(a.values[kv.first] == kv.second, "RESET after RANDOMISE ALL: %s", kv.first.c_str());
    }
    {   // the shipped data has almost no random_variation: check the group rule on a synthetic configuration
        Config cfg; Category cat; cat.name = "Body";
        for (int i = 0; i < 3; ++i) { Range r; r.name = "g" + std::to_string(i); r.min = 0; r.max = 200; r.has_mid = true; r.mid = 100; r.has_random_group = true; r.random_group = 0; r.has_random_variation = true; r.random_variation = 20 + 10 * i; cat.ranges.push_back(r); }
        Range solo = cat.ranges[0]; solo.name = "solo"; solo.has_random_group = false; cat.ranges.push_back(solo);
        cfg.categories.push_back(cat);
        Rng rng(7u); double sum = 0, tdiff = 0; int moved = 0;
        for (int i = 0; i < N; ++i) {
            Appearance a = MakeDefault(cfg); RandomiseCategory(cfg, "Body", a, rng);
            float t0 = (a.values["g0"] - 100) / 40.f, t1 = (a.values["g1"] - 100) / 60.f, t2 = (a.values["g2"] - 100) / 80.f;   // variation = 200 * pct / 100
            CHECK(std::fabs(t0 - t1) <= 0.03f && std::fabs(t0 - t2) <= 0.03f, "synthetic group offsets %.3f %.3f %.3f", t0, t1, t2);
            tdiff += std::fabs((a.values["solo"] - 100) / 40.f - t0); sum += a.values["g0"]; moved += a.values["g0"] != 100;
            CHECK(a.values["g1"] >= 40 && a.values["g1"] <= 160, "synthetic spread g1 %g", a.values["g1"]);
        }
        CHECK(std::fabs(sum / N - 100) <= 4 && moved > N / 2 && tdiff / N > 0.2, "synthetic: mean %.2f moved %d solo-vs-group diff %.3f", sum / N, moved, tdiff / N);
        ++group_checks;
    }
    if (fails) { std::printf("FAIL randomise: %d failures\n", fails); return 1; }
    std::printf("PASS randomise: %d sliders x %d samples (%d with random_variation > 0, %d group checks), bounds / spread / mean / RESET ok\n", sliders, N, varying, group_checks);
    return 0;
}
