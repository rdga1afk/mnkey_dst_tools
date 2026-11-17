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

// The vertex shader's own border-snap lines (between the EDGE-SNAP markers, extracted by CMake and compiled here as C++) against the C++ twin, which the emulator
// vectors above pin to the plugin: every border vertex, 16- and 64-cell patches, all four level differences on each of the four sides.
static void ShaderSnap(int edgeDiff, int cells, int& col, int& row) {
#include "edge_snap_shader.inc"
}
TEST(LodPluginOracle, ShaderEdgeSnapEqualsTheCppTwin) {
    int checked = 0, bad = 0;
    for (int cells : { 16, 64 })
        for (int side = 0; side < 4; ++side)
            for (int d = 0; d <= 3; ++d)
                for (int i = 0; i <= cells; ++i) {
                    const int edgeDiff = d << (2 * side);   // bits: z min, z max, x min, x max
                    int col = (side < 2) ? i : (side == 2 ? 0 : cells), row = (side == 0) ? 0 : (side == 1 ? cells : i);
                    int want_col = col, want_row = row;
                    if (side < 2) want_col = md::TerrainSnapBorder(i, d, side == 1); else want_row = md::TerrainSnapBorder(i, d, side == 3);
                    ShaderSnap(edgeDiff, cells, col, row);
                    ++checked;
                    if (col != want_col || row != want_row) ++bad;
                }
    EXPECT_GT(checked, 600);
    EXPECT_EQ(bad, 0);
}

// Sky controller FUN_14066f190 (read in the exe): pFogParams = (D, D*k1, min(D, D*k2), 0), D = the `view distance` option * 10. The settings loader FUN_1403e84f0 uses the
// default 0x459c4000 (5000.0) when the file has no key; the install's settings.cfg has none. k1 = 0.06 and k2 = 0.6 are not in the exe's code (runtime constants): they come
// from MeitouClient / Okran, both agreeing.
#include <monkey_dust/render/kenshi_sky.h>
TEST(LodPluginOracle, FogParamsXIsTheViewDistanceTimesTen) {
    const uint32_t kDefaultBits = 0x459c4000u; float def; std::memcpy(&def, &kDefaultBits, 4);
    ASSERT_EQ(def, 5000.f);
    md::g_view_distance_m = def;
    const md::KsVec4 fp = md::KenshiSkySettings().FogParams();
    EXPECT_EQ(fp.x, 50000.f);
    EXPECT_EQ(fp.x, md::ViewDistanceUnits());
    EXPECT_FLOAT_EQ(fp.y, 50000.f * 0.06f);
    EXPECT_FLOAT_EQ(fp.z, 50000.f * 0.6f);
}

// setPatchSize (FUN_1800016c0): vertices per side N + 1 and the neighbour limit log2(N) - 1, N = 2..128.
TEST(LodPluginOracle, NeighbourLimitEqualsThePluginsSetPatchSize) {
    std::ifstream in("tools/tests/data/plugin_patch_size.tsv"); std::string line; int rows = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        int n, verts, f0; std::istringstream ss(line); ss >> n >> verts >> f0;
        EXPECT_EQ(md::TerrainNeighbourLimit(n), f0) << "N " << n;
        ++rows;
    }
    EXPECT_EQ(rows, 7);
}

// The sun's direction (exe SkyXController::update, FUN_14066d400, run in the emulator): rows hour / sunrise / sunset / tilt -> x, y, z (float bits).
TEST(LodPluginOracle, SunDirectionEqualsTheExesUpdate) {
    std::ifstream in("tools/tests/data/exe_sun_direction.tsv"); std::string line; int rows = 0; double worst = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line); std::string f[7]; for (int k = 0; k < 7; ++k) std::getline(ss, f[k], '\t');
        md::KenshiSkySettings s; s.sunrise = std::stof(f[1]); s.sunset = std::stof(f[2]); s.tilt = std::stof(f[3]);
        const md::KsVec3 v = s.SunDirection(std::stof(f[0]));
        worst = std::fmax(worst, std::fmax(std::fabs(v.x - Bits(f[4])), std::fmax(std::fabs(v.y - Bits(f[5])), std::fabs(v.z - Bits(f[6])))));
        ++rows;
    }
    std::printf("sun direction vs the exe: %d rows, worst abs difference %.2e\n", rows, worst);
    EXPECT_EQ(rows, 432);
    EXPECT_LT(worst, 2e-6);
}
