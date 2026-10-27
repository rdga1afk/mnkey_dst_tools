// navtile_probe <kenshi_dir> <tileX> <tileY> [x0 z0 x1 z1]: sections of a tile, then (with 4 coordinates, engine metres) a Detour path between two points of the ground section.
#include "navtile.h"
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: navtile_probe <kenshi_dir> <x> <y> [x0 z0 x1 z1]\n"); return 2; }
    std::vector<md::navtile::Section> secs; std::string err;
    if (!md::navtile::LoadTile(argv[1], std::atoi(argv[2]), std::atoi(argv[3]), secs, err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    size_t nb = 0; for (auto& s : secs) nb += s.building;
    std::printf("sections %zu (buildings %zu)\n", secs.size(), nb);
    for (size_t i = 0; i < secs.size() && i < 4; ++i) std::printf("  [%zu] %s verts %zu faces %zu edges %zu info '%s'\n", i, secs[i].building ? "building" : "ground", secs[i].vertices.size() / 3, secs[i].faces.size(), secs[i].edges.size(), secs[i].info.c_str());
    if (argc < 8) return 0;
    for (auto& s : secs) {
        if (s.building) continue;
        dtNavMesh* nav = md::navtile::BuildDetour(s, err); if (!nav) { std::fprintf(stderr, "detour: %s\n", err.c_str()); return 1; }
        dtNavMeshQuery* q = dtAllocNavMeshQuery(); q->init(nav, 4096);
        float a[3] = {(float)std::atof(argv[4]), 0, (float)std::atof(argv[5])}, b[3] = {(float)std::atof(argv[6]), 0, (float)std::atof(argv[7])}, ext[3] = {20, 400, 20};
        dtQueryFilter f; f.setIncludeFlags(1); f.setExcludeFlags(0);
        dtPolyRef ra, rb; float pa[3], pb[3];
        q->findNearestPoly(a, ext, &f, &ra, pa); q->findNearestPoly(b, ext, &f, &rb, pb);
        std::printf("start poly %u at (%.1f %.1f %.1f), goal poly %u at (%.1f %.1f %.1f)\n", (unsigned)ra, pa[0], pa[1], pa[2], (unsigned)rb, pb[0], pb[1], pb[2]);
        if (!ra || !rb) return 3;
        dtPolyRef path[512]; int n = 0; q->findPath(ra, rb, pa, pb, &f, path, &n, 512);
        float sp[3 * 128]; unsigned char fl[128]; dtPolyRef pr[128]; int ns = 0; q->findStraightPath(pa, pb, path, n, sp, fl, pr, &ns, 128);
        std::printf("path: %d polys, %d straight points\n", n, ns);
        for (int i = 0; i < ns; ++i) std::printf("  (%.1f %.1f %.1f)\n", sp[i * 3], sp[i * 3 + 1], sp[i * 3 + 2]);
        return n > 0 && path[n - 1] == rb ? 0 : 4;
    }
    return 0;
}
