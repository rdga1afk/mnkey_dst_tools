// Hi-Z two-phase, step 0 (coordinator 6806948621, owner 11.11): an UPPER BOUND on what occlusion culling of terrain nodes could save in the G-buffer vertex load, on the CPU from the install's heightmap.
// For a pose (position, pitch, 8 yaws) the real quadtree selection (md::TerrainLodTree::Select) gives the nodes the G-buffer pass draws; a node counts as occluded when the rays from the camera to the
// four top corners and the top centre of its box are ALL blocked by terrain (height > ray height + 0.5 m somewhere before the point). Conservative on the node side (a partly visible node is kept), exact on the
// terrain side (the full 1.8 m heightmap; no props, no buildings). usage: md_terrain_occlusion_bound <KENSHI_DIR>
// Horizon map (12.11, docs/research/HIZ_UPPER_BOUND_2026-11-12.md design note): the same poses also run the CPU horizon table (360 azimuth bins x 20 log distance steps of the running maximum tangent of the terrain elevation angle);
// a node is rejected when its highest possible tangent (top / nearest distance) is below the horizon of EVERY bin its box spans (+1 bin each side) up to the distance before it. The table prints the share it rejects, the share of those the exact rays call visible (false rejects, must be ~0) and the table build time.
#include <monkey_dust/world/terrain_lod_tree.h>
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/io/tiff.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static const int kS = 16385; static const float kCell = 1.8f, kScale = 980.f / 65535.f;
struct Poses { const char* name; float cx, cz, h, pitch_deg; };

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string tif = std::string(argv[1]) + "/data/newland/land/fullmap.tif";
    md::TerrainLodTree tree; tree.BuildAsync(tif.c_str(), 29491.2f, 980.f, 16);
    while (!tree.Ready() && !tree.Failed()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (tree.Failed()) { std::printf("tree build failed\n"); return 1; }
    const float CX = 9647.7f, CZ = 15038.8f;
    const Poses poses[] = { {"street", CX - 40, CZ + 10, 2.f, 0.f}, {"top40", CX - 30, CZ + 30, 40.f, -45.f}, {"high150", CX - 30, CZ + 30, 150.f, -30.f}, {"street_pitch18", CX - 40, CZ + 10, 2.f, -18.f} };
    // heightmap window around the Hub: 4400 m radius
    md::tiff::Image img; std::string err; if (!img.Open(tif.c_str(), err)) { std::printf("tif: %s\n", err.c_str()); return 1; }
    const int R = int(4400.f / kCell), cxs = int(CX / kCell), czs = int(CZ / kCell), x0 = std::max(0, cxs - R), z0 = std::max(0, czs - R), W = std::min(kS - x0, 2 * R), Hh = std::min(kS - z0, 2 * R);
    std::vector<uint16_t> raw; if (!img.Window(x0, z0, W, Hh, 1, raw, err)) { std::printf("window: %s\n", err.c_str()); return 1; }
    auto height = [&](float wx, float wz) -> float {
        float fx = wx / kCell - x0, fz = wz / kCell - z0; int ix = int(fx), iz = int(fz);
        if (ix < 0 || iz < 0 || ix >= W - 1 || iz >= Hh - 1) return -1e9f;
        float tx = fx - ix, tz = fz - iz;
        auto h = [&](int x, int z) { return float(raw[(size_t)z * W + x]) * kScale; };
        return (h(ix, iz) * (1 - tx) + h(ix + 1, iz) * tx) * (1 - tz) + (h(ix, iz + 1) * (1 - tx) + h(ix + 1, iz + 1) * tx) * tz;
    };
    std::vector<md::TerrainLodNode> nodes(md::TerrainLodTree::kMaxNodes);
    constexpr int kBins = 360, kSteps = 20; constexpr float kD0 = 4.f, kD1 = 4300.f;
    static float horizon[kBins][kSteps]; float edge[kSteps];
    for (int k = 0; k < kSteps; ++k) edge[k] = kD0 * std::pow(kD1 / kD0, float(k + 1) / kSteps);
    auto build_horizon = [&](float cx, float cy, float cz) {
        for (int b = 0; b < kBins; ++b) {
            const float a = (b + 0.5f) * 6.2831853f / kBins, ux = std::cos(a), uz = std::sin(a);
            float mx = -1e9f; int k = 0; float d = 2.f;
            for (; d < kD1 && k < kSteps; d += 1.5f + 0.02f * d) {
                const float h = height(cx + ux * d, cz + uz * d); if (h > -1e8f) mx = std::max(mx, (h - cy) / d);
                while (k < kSteps && d >= edge[k]) horizon[b][k++] = mx;
            }
            while (k < kSteps) horizon[b][k++] = mx;
        }
    };
    std::printf("%-15s %6s %8s %8s %8s\n", "pose", "yaw", "nodes", "occluded", "share");
    for (const Poses& P : poses) {
        double tot_n = 0, tot_o = 0, tot_h = 0, tot_hf = 0, hor_ms = 0;
        for (int yi = 0; yi < 8; ++yi) {
            const float yaw = yi * 45.f * 0.0174533f, pit = P.pitch_deg * 0.0174533f;
            MdCamera cam; cam.fovy = 60.f; cam.up = {0, 1, 0};
            cam.pos = {P.cx, height(P.cx, P.cz) + P.h, P.cz};
            const Vec3 d = {std::cos(pit) * std::cos(yaw), std::sin(pit), std::cos(pit) * std::sin(yaw)};
            cam.target = {cam.pos.x + d.x, cam.pos.y + d.y, cam.pos.z + d.z};
            float planes[16]; cam.FrustumPlanes(16.f / 9.f, planes); float c3[3] = {cam.pos.x, cam.pos.y, cam.pos.z};
            md::TerrainLodParams tp; const int n = tree.Select(c3, planes, tp, nodes.data(), (int)nodes.size());
            const auto th0 = std::chrono::steady_clock::now(); build_horizon(cam.pos.x, cam.pos.y, cam.pos.z); hor_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - th0).count();
            int occ = 0, hrej = 0, hfalse = 0;
            for (int i = 0; i < n; ++i) {
                const md::TerrainLodNode& nd = nodes[i];
                const float px[5] = {nd.origin_x, nd.origin_x + nd.size, nd.origin_x, nd.origin_x + nd.size, nd.origin_x + nd.size * 0.5f}, pz[5] = {nd.origin_z, nd.origin_z, nd.origin_z + nd.size, nd.origin_z + nd.size, nd.origin_z + nd.size * 0.5f};
                bool all = true;
                for (int k = 0; k < 5 && all; ++k) {
                    const float dx = px[k] - cam.pos.x, dy = nd.y_max - cam.pos.y, dz = pz[k] - cam.pos.z, len = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (len < 3.f) { all = false; break; }
                    bool blocked = false; const int steps = std::min(4000, int(len / 2.f));
                    for (int s = 1; s < steps && !blocked; ++s) { const float t = float(s) / float(steps) * 0.985f; blocked = height(cam.pos.x + dx * t, cam.pos.z + dz * t) > cam.pos.y + dy * t + 0.5f; }
                    all = blocked;
                }
                occ += all;
                {   // horizon test
                    const float x0b = nd.origin_x - cam.pos.x, x1b = x0b + nd.size, z0b = nd.origin_z - cam.pos.z, z1b = z0b + nd.size;
                    const float ndx = (x0b > 0 ? x0b : (x1b < 0 ? x1b : 0.f)), ndz = (z0b > 0 ? z0b : (z1b < 0 ? z1b : 0.f)), dmin = std::sqrt(ndx * ndx + ndz * ndz);
                    const float fdx = std::max(std::fabs(x0b), std::fabs(x1b)), fdz = std::max(std::fabs(z0b), std::fabs(z1b)), dmax = std::sqrt(fdx * fdx + fdz * fdz);
                    bool rej = false;
                    if (dmin > 8.f) {
                        const float num = nd.y_max - cam.pos.y, tmax = num >= 0 ? num / dmin : num / dmax;
                        float amin = 1e9f, amax = -1e9f, a0 = std::atan2(z0b, x0b);   // angular span of the four corners relative to the first
                        const float cxs4[4] = {x0b, x1b, x0b, x1b}, czs4[4] = {z0b, z0b, z1b, z1b};
                        for (int c = 0; c < 4; ++c) { float da = std::atan2(czs4[c], cxs4[c]) - a0; while (da > 3.14159265f) da -= 6.2831853f; while (da < -3.14159265f) da += 6.2831853f; amin = std::min(amin, da); amax = std::max(amax, da); }
                        if (amax - amin < 3.0f) {
                            int k = 0; while (k < kSteps - 1 && edge[k + 1] <= dmin * 0.985f) ++k;   // largest step whose edge is <= dmin: prefix up to there
                            k = std::max(0, k - 1 + (edge[0] <= dmin * 0.985f ? 0 : 0));
                            if (edge[0] > dmin * 0.985f) { rej = false; }
                            else {
                                const int b0 = int(std::floor((a0 + amin) / 6.2831853f * kBins)) - 3, b1 = int(std::floor((a0 + amax) / 6.2831853f * kBins)) + 3;
                                bool all_b = true; for (int b = b0; b <= b1 && all_b; ++b) { const int bb = ((b % kBins) + kBins) % kBins; all_b = tmax < horizon[bb][k] - 0.01f; }
                                rej = all_b;
                            }
                        }
                    }
                    hrej += rej; if (rej && !all) ++hfalse;
                }
            }
            tot_n += n; tot_o += occ; tot_h += hrej; tot_hf += hfalse;
            std::printf("%-15s %6d %8d %8d %7.1f%%\n", P.name, yi * 45, n, occ, n ? 100.0 * occ / n : 0.0);
        }
        std::printf("%-15s %6s %8.0f %8.0f %7.1f%%  <- mean over 8 yaws\n", P.name, "all", tot_n / 8, tot_o / 8, tot_n ? 100.0 * tot_o / tot_n : 0.0);
        std::printf("%-15s horizon map: rejects %6.1f%% of the nodes (exact rays: %.1f%%), false rejects (exact says visible) %.2f%% of the nodes, table build %.2f ms per pose\n", P.name, tot_n ? 100.0 * tot_h / tot_n : 0.0, tot_n ? 100.0 * tot_o / tot_n : 0.0, tot_n ? 100.0 * tot_hf / tot_n : 0.0, hor_ms / 8);
    }
    return 0;
}
