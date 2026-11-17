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

// Border snap (FUN_180005e10): the plugin's index entries for every side, 16- and 64-cell patches, level difference 0..3; sides 0 / 2 are the x min / z min
// borders, 1 / 3 the x max / z max ones.
TEST(LodPluginOracle, EdgeSnapEqualsThePluginsFunction) {
    std::ifstream in("tools/tests/data/plugin_edge_snap.tsv"); std::string line; int rows = 0, bad = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line); std::string f[4];
        for (int k = 0; k < 4; ++k) std::getline(ss, f[k], '\t');
        const int side = std::stoi(f[0]), n = std::stoi(f[1]), d = std::stoi(f[2]);
        std::istringstream vs(f[3]); std::string v; int i = 1;
        while (std::getline(vs, v, ',')) { if (md::TerrainSnapBorder(i, d, side == 1 || side == 3) != std::stoi(v)) ++bad; ++i; }
        ASSERT_EQ(i, n - 1);
        ++rows;
    }
    EXPECT_EQ(rows, 32);
    EXPECT_EQ(bad, 0);
}
