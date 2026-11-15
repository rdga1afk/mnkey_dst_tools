// RestrictLodNodes (restricted quadtree, plugin neighbour rule): CPU only. A selection that refines toward a point has 2+ level jumps between neighbours
// (the crack the skirts used to cover); after the restriction no pair of edge-sharing leaves differs by more than 1 level, the covered area is the same,
// and edge_diff names the coarser side.
#include <monkey_dust/world/terrain_lod_tree.h>
#include <cmath>
#include <cstdio>
#include <vector>
using md::TerrainLodNode;

static const float kExtent = 29491.2f;
static void Refine(std::vector<TerrainLodNode>& out, int L, int i, int j, float px, float pz, int max_level) {
    const float size = kExtent / (float)(1 << L), x0 = i * size, z0 = j * size;
    const float dx = std::fmax(std::fmax(x0 - px, 0.f), px - (x0 + size)), dz = std::fmax(std::fmax(z0 - pz, 0.f), pz - (z0 + size));
    const float d = std::sqrt(dx * dx + dz * dz);
    if (L < max_level && d < 0.01f) {
        for (int q = 0; q < 4; ++q) Refine(out, L + 1, i * 2 + (q & 1), j * 2 + (q >> 1), px, pz, max_level);
        return;
    }
    TerrainLodNode n{}; n.origin_x = x0; n.origin_z = z0; n.size = size; n.morph = 1.f; n.skirt_depth = 3.f; n.level = L; n.y_min = 0; n.y_max = 10;
    out.push_back(n);
}
// brute-force oracle, independent of the code under test: the worst level difference between two leaves that share part of an edge
static int WorstJump(const std::vector<TerrainLodNode>& v) {
    int worst = 0;
    for (size_t a = 0; a < v.size(); ++a) for (size_t b = 0; b < v.size(); ++b) {
        const TerrainLodNode &A = v[a], &B = v[b]; if (a == b) continue;
        const float e = 0.01f;
        const bool xs = std::fabs(A.origin_x + A.size - B.origin_x) < e, zs = std::fabs(A.origin_z + A.size - B.origin_z) < e;
        const bool zo = A.origin_z < B.origin_z + B.size - e && B.origin_z < A.origin_z + A.size - e;
        const bool xo = A.origin_x < B.origin_x + B.size - e && B.origin_x < A.origin_x + A.size - e;
        if ((xs && zo) || (zs && xo)) worst = std::max(worst, std::abs(A.level - B.level));
    }
    return worst;
}
static double Area(const std::vector<TerrainLodNode>& v) { double s = 0; for (auto& n : v) s += (double)n.size * n.size; return s; }

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
    std::vector<TerrainLodNode> v;
    for (int j = 0; j < 16; ++j) for (int i = 0; i < 16; ++i) Refine(v, 4, i, j, 14000.f, 15000.f, 9);
    const int jump_before = WorstJump(v); const double area_before = Area(v); const size_t n_before = v.size();
    check(jump_before >= 2, "oracle sees a 2+ level jump before the restriction (the check can fail)");
    v.resize(65536);
    const int n = md::RestrictLodNodes(v.data(), (int)n_before, 65536);
    v.resize((size_t)n);
    check(WorstJump(v) <= 1, "after the restriction no edge-sharing pair differs by more than 1 level");
    check(std::fabs(Area(v) - area_before) < 1.0, "covered area unchanged");
    check(n > (int)n_before, "leaves were split");
    bool diff_ok = true;
    for (auto& a : v) for (auto& b : v) {
        if (&a == &b) continue;
        const float e = 0.01f;
        const bool zo = a.origin_z < b.origin_z + b.size - e && b.origin_z < a.origin_z + a.size - e;
        const bool xo = a.origin_x < b.origin_x + b.size - e && b.origin_x < a.origin_x + a.size - e;
        if (std::fabs(a.origin_x - (b.origin_x + b.size)) < e && zo && b.level < a.level && a.edge_diff[2] != a.level - b.level) diff_ok = false;   // b is on a's x min side
        if (std::fabs(a.origin_z + a.size - b.origin_z) < e && xo && b.level < a.level && a.edge_diff[1] != a.level - b.level) diff_ok = false;   // b is on a's z max side
    }
    check(diff_ok, "edge_diff = levels the neighbour is coarser");
    bool stitched = true; for (auto& a : v) if (!a.stitched || a.skirt_depth != 0.f) stitched = false;
    check(stitched, "every node is stitched, no skirt");
    return fails ? 1 : 0;
}
