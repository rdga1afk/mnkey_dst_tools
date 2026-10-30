// navtile_probe <kenshi_dir> <tileX> <tileY> [x0 z0 x1 z1]: sections of a tile, then (with 4 coordinates, engine metres) a Detour path between two points of the ground section.
#include "navtile.h"
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
// navtile_probe <dir> region tx0 ty0 tx1 ty1 [ax az bx bz ...]: one Detour navmesh over the tiles (convex polygons <= 6 vertices, portals), the face statistics, the links that cross a tile border and, per coordinate quadruple (engine metres), a path.
static int Region(int argc, char** argv) {
    dtNavMesh* nav = nullptr; md::navtile::PolyStats st; std::string err;
    if (!md::navtile::BuildDetourTiles(argv[1], std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]), &nav, &st, err)) { std::fprintf(stderr, "region: %s\n", err.c_str()); return 1; }
    long polys = 0, cross = 0, tiles = 0;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* t = ((const dtNavMesh*)nav)->getTile(i); if (!t || !t->header) continue;
        ++tiles; polys += t->header->polyCount;
        for (int pi = 0; pi < t->header->polyCount; ++pi) for (unsigned k = t->polys[pi].firstLink; k != DT_NULL_LINK; k = t->links[k].next) {
            if ((int)nav->decodePolyIdTile(t->links[k].ref) != i) ++cross;
        }
    }
    std::printf("tiles %ld polygons %ld | faces %ld (kept %ld, split %ld into %ld triangles, dropped %ld) -> %ld polygons | portal edges %ld, links across tile borders %ld\n", tiles, polys, st.faces, st.kept, st.split, st.triangles, st.dropped, st.polys, st.portal_edges, cross);
    dtNavMeshQuery* q = dtAllocNavMeshQuery(); q->init(nav, 8192); dtQueryFilter f; f.setIncludeFlags(1); f.setExcludeFlags(0);
    int bad = 0;
    for (int i = 7; i + 3 < argc; i += 4) {
        float a[3] = {(float)std::atof(argv[i]), 0, (float)std::atof(argv[i + 1])}, b[3] = {(float)std::atof(argv[i + 2]), 0, (float)std::atof(argv[i + 3])}, ext[3] = {20, 600, 20};
        dtPolyRef ra = 0, rb = 0; float pa[3], pb[3]; q->findNearestPoly(a, ext, &f, &ra, pa); q->findNearestPoly(b, ext, &f, &rb, pb);
        if (!ra || !rb) { std::printf("path %d: no start / goal polygon\n", (i - 7) / 4); ++bad; continue; }
        static dtPolyRef path[4096]; int n = 0; q->findPath(ra, rb, pa, pb, &f, path, &n, 4096);
        float sp[3 * 256]; unsigned char fl[256]; dtPolyRef pr[256]; int ns = 0; q->findStraightPath(pa, pb, path, n, sp, fl, pr, &ns, 256);
        double len = 0; for (int k = 1; k < ns; ++k) { double dx = sp[k * 3] - sp[k * 3 - 3], dz = sp[k * 3 + 2] - sp[k * 3 - 1]; len += std::sqrt(dx * dx + dz * dz); }
        const double straight = std::hypot(pb[0] - pa[0], pb[2] - pa[2]); const bool ok = n > 0 && path[n - 1] == rb;
        std::printf("path %d: %s, %d polys, %d points, length %.1f m, straight %.1f m (ratio %.2f)\n", (i - 7) / 4, ok ? "reaches the goal" : "PARTIAL", n, ns, len, straight, straight > 0 ? len / straight : 0.0);
        if (!ok) ++bad;
    }
    return bad ? 4 : 0;
}
int main(int argc, char** argv) {
    if (argc >= 7 && std::string(argv[2]) == "region") return Region(argc, argv);
    if (argc < 4) { std::fprintf(stderr, "usage: navtile_probe <kenshi_dir> <x> <y> [x0 z0 x1 z1] | <kenshi_dir> region <tx0> <ty0> <tx1> <ty1> [x0 z0 x1 z1 ...]\n"); return 2; }
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
