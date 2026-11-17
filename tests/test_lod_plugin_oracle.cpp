// The terrain LOD far ramp against the plugin's own function (Plugin_Terrain_x64.dll FUN_180003b30 run in the Unicorn emulator,
// tools_private/research/plugin_oracle.py; the vectors hold numbers only). The plugin multiplies the node value by ramp = t * f8 + 1 - t
// (t = clamp((d - fc) / fc, 0, 1), f8 = near / far), so a node splits when value * ramp > near: the threshold is near / ramp.
#include <gtest/gtest.h>
#include <monkey_dust/world/terrain_lod_tree.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct Row { float d, f8, fc, ramp, flat; };
float Bits(const std::string& h) { uint32_t u = (uint32_t)std::stoul(h, nullptr, 16); float f; std::memcpy(&f, &u, 4); return f; }
std::vector<Row> Load() {
    std::vector<Row> v; std::ifstream in("tools/tests/data/plugin_node_value.tsv"); std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == 'd') continue;
        std::istringstream ss(line); std::string d, f8, fc, rb, fb; std::getline(ss, d, '\t'); std::getline(ss, f8, '\t'); std::getline(ss, fc, '\t'); std::getline(ss, rb, '\t'); std::getline(ss, fb, '\t');
        v.push_back({ std::stof(d), std::stof(f8), std::stof(fc), Bits(rb), Bits(fb) });
    }
    return v;
}
}  // namespace

TEST(LodPluginOracle, FarRampEqualsThePluginsFunction) {
    const std::vector<Row> rows = Load();
    ASSERT_GE(rows.size(), 40u);
    double worst_plugin = 0, worst_linear = 0;
    for (const Row& r : rows) {
        md::TerrainLodParams p; p.near_px = 5.f; p.far_px = 5.f / r.f8; p.near_dist = r.fc; p.far_dist = 2.f * r.fc;
        const double ramp = (double)r.ramp / (double)r.flat;          // the plugin's multiplier at this distance
        p.plugin_ramp = true;
        const double want = p.near_px / ramp;                          // the threshold it implies
        worst_plugin = std::fmax(worst_plugin, std::fabs(md::TerrainLodThreshold(p, r.d) - want) / want);
        p.plugin_ramp = false;
        worst_linear = std::fmax(worst_linear, std::fabs(md::TerrainLodThreshold(p, r.d) - want) / want);
    }
    std::printf("threshold vs the plugin, worst relative difference over %zu vectors: plugin_ramp %.2e, linear (the default) %.2f\n", rows.size(), worst_plugin, worst_linear);
    EXPECT_LT(worst_plugin, 1e-5);
    EXPECT_GT(worst_linear, 0.3) << "the linear ramp matches the plugin: the finding no longer holds";
}
