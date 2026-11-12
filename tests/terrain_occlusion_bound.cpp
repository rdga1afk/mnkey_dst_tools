// Hi-Z two-phase, step 0 (coordinator 6806948621, owner 11.11): an UPPER BOUND on what occlusion culling of terrain nodes could save in the G-buffer vertex load, on the CPU from the install's heightmap.
// For a pose (position, pitch, 8 yaws) the real quadtree selection (md::TerrainLodTree::Select) gives the nodes the G-buffer pass draws; a node counts as occluded when the rays from the camera to the
// four top corners and the top centre of its box are ALL blocked by terrain (height > ray height + 0.5 m somewhere before the point). Conservative on the node side (a partly visible node is kept), exact on the
// terrain side (the full 1.8 m heightmap; no props, no buildings). usage: md_terrain_occlusion_bound <KENSHI_DIR>
#include <monkey_dust/world/terrain_lod_tree.h>
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/io/tiff.h>
#include <algorithm>
#include <cmath>
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
    std::printf("%-15s %6s %8s %8s %8s\n", "pose", "yaw", "nodes", "occluded", "share");
    for (const Poses& P : poses) {
        double tot_n = 0, tot_o = 0;
        for (int yi = 0; yi < 8; ++yi) {
            const float yaw = yi * 45.f * 0.0174533f, pit = P.pitch_deg * 0.0174533f;
            MdCamera cam; cam.fovy = 60.f; cam.up = {0, 1, 0};
            cam.pos = {P.cx, height(P.cx, P.cz) + P.h, P.cz};
            const Vec3 d = {std::cos(pit) * std::cos(yaw), std::sin(pit), std::cos(pit) * std::sin(yaw)};
            cam.target = {cam.pos.x + d.x, cam.pos.y + d.y, cam.pos.z + d.z};
            float planes[16]; cam.FrustumPlanes(16.f / 9.f, planes); float c3[3] = {cam.pos.x, cam.pos.y, cam.pos.z};
            md::TerrainLodParams tp; const int n = tree.Select(c3, planes, tp, nodes.data(), (int)nodes.size());
            int occ = 0;
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
            }
            tot_n += n; tot_o += occ;
            std::printf("%-15s %6d %8d %8d %7.1f%%\n", P.name, yi * 45, n, occ, n ? 100.0 * occ / n : 0.0);
        }
        std::printf("%-15s %6s %8.0f %8.0f %7.1f%%  <- mean over 8 yaws\n", P.name, "all", tot_n / 8, tot_o / 8, tot_n ? 100.0 * tot_o / tot_n : 0.0);
    }
    return 0;
}
