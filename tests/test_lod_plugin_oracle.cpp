// The terrain LOD far ramp against the plugin's own function (Plugin_Terrain_x64.dll FUN_180003b30 run in the Unicorn emulator,
// tools_private/research/plugin_oracle.py; the vectors hold numbers only). The plugin multiplies the node value by ramp = t * f8 + 1 - t
// (t = clamp((d - fc) / fc, 0, 1), f8 = near / far), so a node splits when value * ramp > near: the threshold is near / ramp.
#include <gtest/gtest.h>
#include <monkey_dust/world/terrain_lod_tree.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    std::vector<Row> v; std::ifstream in(MD_TOOLS_TEST_DATA "/plugin_node_value.tsv"); std::string line;
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
    std::ifstream in(MD_TOOLS_TEST_DATA "/plugin_edge_snap.tsv"); std::string line; int rows = 0, bad = 0;
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
    std::ifstream in(MD_TOOLS_TEST_DATA "/plugin_patch_size.tsv"); std::string line; int rows = 0;
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
    std::ifstream in(MD_TOOLS_TEST_DATA "/exe_sun_direction.tsv"); std::string line; int rows = 0; double worst = 0;
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

// Orig: the fog material's uSkydomeRadius is the SkyXFogParams default of the install's common.program (70000); nothing in the exe names it and the SkyX DLL writes it only on its own
// ground pass, so it does not depend on the view distance (the earlier min(0.6 D, D) was our own guess: 30000 at D = 50000).
TEST(LodPluginOracle, FogSkydomeRadiusIsTheInstallsDefault) {
    EXPECT_EQ(md::kFogSkydomeRadius, 70000.f);
    const char* dir = std::getenv("KENSHI_DIR");
    if (!dir || !*dir) return;   // the constant above is the check without an install
    std::ifstream in(std::string(dir) + "/data/materials/common/common.program"); std::string line; bool found = false;
    while (std::getline(in, line)) {
        if (line.find("shared_param_named uSkydomeRadius") == std::string::npos) continue;
        std::istringstream ss(line); std::string a, b, c; float v = 0.f; ss >> a >> b >> c >> v;   // shared_param_named <name> float <value>
        EXPECT_EQ(v, md::kFogSkydomeRadius); found = true;
    }
    EXPECT_TRUE(found);
}

// Neighbour restriction (Plugin_Terrain_x64.dll FUN_1800042c0 run in the Unicorn emulator, tools/tests/data/plugin_neighbours.tsv; the node split is replaced by a counting stub):
// per side the plugin splits the neighbour while its level < level - limit + 1. Ours works on a flat list, so each side is built alone: the node at (level, i, j), its neighbour on that side
// at the vector's level (a finer one is the same level, ours only looks for the same or a coarser cell). One split adds three nodes, so the count is 2 + 3 * splits; only sides with at most one split are compared.
// FUN_180003d80 (opposite side) and FUN_180003da0 (link neighbours) have no counterpart in ours (flat grid).
TEST(LodPluginOracle, RestrictNeighboursSplitCountEqualsThePlugins) {
    std::ifstream in(MD_TOOLS_TEST_DATA "/plugin_neighbours.tsv"); std::string line; int rows = 0, sides = 0, bad = 0;
    const int di[4] = {0, 0, -1, 1}, dj[4] = {-1, 1, 0, 0};
    while (std::getline(in, line)) {
        if (line.rfind("restrict\t", 0) != 0) continue;
        std::istringstream ss(line); std::string f[6]; for (int k = 0; k < 6; ++k) std::getline(ss, f[k], '\t');
        const int L = std::stoi(f[1]), limit = std::stoi(f[2]);
        std::vector<int> calls; { std::istringstream cs(f[5]); std::string c; while (std::getline(cs, c, ',')) calls.push_back(std::stoi(c)); }
        std::vector<std::string> spec; { std::istringstream cs(f[3]); std::string c; while (std::getline(cs, c, ',')) spec.push_back(c); }
        ASSERT_EQ(calls.size(), 4u); ASSERT_EQ(spec.size(), 4u);
        for (int e = 0; e < 4; ++e) {
            if (spec[e] == "-" || calls[e] > 1) continue;   // with more splits the plugin's own split function keeps restricting the new children, ours balances the whole list
            const int l = std::min(std::stoi(spec[e].substr(1)), L), d = L - l;
            const float size = float(1024 >> L), size_l = float(1024 >> l);
            const int i = 5 * (1 << d) - (di[e] > 0 ? 1 : 0), j = 5 * (1 << d) - (dj[e] > 0 ? 1 : 0);
            md::TerrainLodNode nodes[64] = {};
            nodes[0].origin_x = float(i) * size; nodes[0].origin_z = float(j) * size; nodes[0].size = size; nodes[0].level = L;
            nodes[1].origin_x = float((i + di[e]) >> d) * size_l; nodes[1].origin_z = float((j + dj[e]) >> d) * size_l; nodes[1].size = size_l; nodes[1].level = l;
            const int n = md::RestrictLodNodes(nodes, 2, 64, limit - 1);
            if (n != 2 + 3 * calls[e]) ++bad;
            ++sides;
        }
        ++rows;
    }
    std::printf("restrict neighbours vs the plugin: %d scenarios, %d sides, %d differ\n", rows, sides, bad);
    EXPECT_EQ(rows, 40);
    EXPECT_GT(sides, 60);
    EXPECT_EQ(bad, 0);
}
