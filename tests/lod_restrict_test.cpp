// RestrictLodNodes (restricted quadtree, plugin neighbour rule): CPU only. A selection that refines toward a point has 2+ level jumps between neighbours
// (the crack the skirts used to cover); after the restriction no pair of edge-sharing leaves differs by more than 1 level, the covered area is the same,
// and edge_diff names the coarser side.
#include <monkey_dust/world/terrain_lod_tree.h>
#include <cmath>
#include <algorithm>
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

// oracle written straight from the decompiled rule (FUN_1800042c0 + FUN_180006370), brute force and independent of the hash grid: a leaf whose edge neighbour covering cell
// is a leaf more than max_diff levels coarser splits that neighbour, until nothing splits; returns the sorted (level, i, j) set
struct Cell { int L, i, j; bool operator<(const Cell& o) const { return L != o.L ? L < o.L : (i != o.i ? i < o.i : j < o.j); } bool operator==(const Cell& o) const { return L == o.L && i == o.i && j == o.j; } };
static std::vector<Cell> Oracle(const std::vector<TerrainLodNode>& in, int max_diff) {
    std::vector<Cell> v;
    for (auto& n : in) v.push_back({ n.level, (int)std::lround(n.origin_x / n.size), (int)std::lround(n.origin_z / n.size) });
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t a = 0; a < v.size() && !changed; ++a) {
            const Cell A = v[a];
            const int di[4] = { 0, 0, -1, 1 }, dj[4] = { -1, 1, 0, 0 };
            for (int e = 0; e < 4 && !changed; ++e) {
                const int ni = A.i + di[e], nj = A.j + dj[e];
                for (size_t b = 0; b < v.size(); ++b) {   // the leaf that contains the neighbouring cell of A's level
                    const Cell B = v[b]; if (B.L >= A.L || A.L - B.L <= max_diff) continue;
                    const int sh = A.L - B.L;
                    if ((ni >> sh) == B.i && (nj >> sh) == B.j && ni >= 0 && nj >= 0) {
                        v.erase(v.begin() + (long)b);
                        for (int q = 0; q < 4; ++q) v.push_back({ B.L + 1, B.i * 2 + (q & 1), B.j * 2 + (q >> 1) });
                        changed = true; break;
                    }
                }
            }
        }
    }
    std::sort(v.begin(), v.end()); return v;
}

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
    const float camx[3] = { 14000.f, 5200.f, 22000.f }, camz[3] = { 15000.f, 9100.f, 3000.f };
    for (int max_diff = 1; max_diff <= 3; ++max_diff) for (int c = 0; c < 3; ++c) {
        std::vector<TerrainLodNode> v;
        for (int j = 0; j < 16; ++j) for (int i = 0; i < 16; ++i) Refine(v, 4, i, j, camx[c], camz[c], 10);
        const int jump_before = WorstJump(v); const double area_before = Area(v); const size_t n_before = v.size();
        const std::vector<Cell> want = Oracle(v, max_diff);
        v.resize(65536);
        const int n = md::RestrictLodNodes(v.data(), (int)n_before, 65536, max_diff);
        v.resize((size_t)n);
        std::vector<Cell> got; for (auto& a : v) got.push_back({ a.level, (int)std::lround(a.origin_x / a.size), (int)std::lround(a.origin_z / a.size) });
        std::sort(got.begin(), got.end());
        char what[160];
        std::snprintf(what, sizeof what, "max_diff %d cam %d: %zu -> %d leaves, node set equals the oracle (%zu)", max_diff, c, n_before, n, want.size()); check(got == want, what);
        std::snprintf(what, sizeof what, "max_diff %d cam %d: worst jump %d -> %d (limit %d), area unchanged", max_diff, c, jump_before, WorstJump(v), max_diff);
        check(WorstJump(v) <= max_diff && std::fabs(Area(v) - area_before) < 1.0 && (jump_before > max_diff) == (n > (int)n_before), what);
        bool diff_ok = true, stitched = true;
        for (auto& a : v) {
            if (!a.stitched || a.skirt_depth != 0.f) stitched = false;
            for (auto& b : v) {
                if (&a == &b) continue;
                const float e = 0.01f;
                const bool zo = a.origin_z < b.origin_z + b.size - e && b.origin_z < a.origin_z + a.size - e;
                const bool xo = a.origin_x < b.origin_x + b.size - e && b.origin_x < a.origin_x + a.size - e;
                if (std::fabs(a.origin_x - (b.origin_x + b.size)) < e && zo && b.level < a.level && a.edge_diff[2] != a.level - b.level) diff_ok = false;   // b on a's x min side
                if (std::fabs(a.origin_z + a.size - b.origin_z) < e && xo && b.level < a.level && a.edge_diff[1] != a.level - b.level) diff_ok = false;   // b on a's z max side
            }
        }
        check(diff_ok && stitched, "edge_diff = levels the neighbour is coarser, every node stitched, no skirt");
    }
    return fails ? 1 : 0;
}
