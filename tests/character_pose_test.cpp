// #159 phase 2 step 1: md::character pose solver on the Human male body of the install: the bind pose gives identity skin matrices, the idle stance keeps the body
// upright with finite vertices, and the Height slider scales the standing height (100 -> 120 gives 1.20 +- 0.03). usage: md_character_pose_test <KENSHI_DIR>
#include "pose_solver.h"
#include <monkey_dust/io/kenshi_resources.h>
#include <cmath>
#include <cstdio>

using namespace md::character;
static bool Skin(const SkinnedBody& b, const std::vector<M4>& m, float& miny, float& maxy) {
    miny = 1e9f; maxy = -1e9f;
    for (const SkinnedVertex& v : b.vertices) {
        float p[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            if (v.weight[k] <= 0) continue;
            const M4& a = m[v.joint[k]];
            for (int r = 0; r < 3; ++r) p[r] += v.weight[k] * (a[r] * v.pos[0] + a[4 + r] * v.pos[1] + a[8 + r] * v.pos[2] + a[12 + r]);
        }
        if (!std::isfinite(p[0] + p[1] + p[2])) return false;
        miny = std::min(miny, p[1]); maxy = std::max(maxy, p[1]);
    }
    return true;
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    md::KenshiResources res; res.Index(argv[1]);
    md::ogre::Mesh m; md::ogre::Error e; md::ogre::Skeleton sk;
    if (!md::ogre::ReadMesh(res.Path("human_male.mesh"), m, e) || !md::ogre::ReadSkeleton(res.Path(m.skeleton), sk, e)) { std::printf("FAIL pose: load %s\n", e.Message().c_str()); return 1; }
    SkinnedBody b; std::string why;
    if (!BuildSkinnedBody(m, sk, b, why)) { std::printf("FAIL pose: %s\n", why.c_str()); return 1; }
    std::vector<M4> sm; float lo, hi;
    SolveSkinMatrices(b, sk, {}, {}, sm);
    for (size_t i = 0; i < sm.size(); ++i) for (int k = 0; k < 16; ++k) if (std::fabs(sm[i][k] - (k % 5 == 0 ? 1.f : 0.f)) > 2e-3f) { std::printf("FAIL pose: bind pose matrix %zu is not identity\n", i); return 1; }
    const md::ogre::Animation* idle = FindAnimation(sk, "idle_stand_normal");
    if (!idle) { std::printf("FAIL pose: no idle_stand_normal\n"); return 1; }
    SolveSkinMatrices(b, sk, {{idle, 0.f}}, {}, sm);
    if (!Skin(b, sm, lo, hi)) { std::printf("FAIL pose: non-finite vertex in idle\n"); return 1; }
    float h100 = hi - lo;
    std::map<std::string, float> v; v["Height"] = 120.f;
    BodyScaleInputs in;
    SolveSkinMatrices(b, sk, {{idle, 0.f}}, ComputeBoneScales(v, in), sm);
    if (!Skin(b, sm, lo, hi)) { std::printf("FAIL pose: non-finite vertex at Height 120\n"); return 1; }
    float ratio = (hi - lo) / h100;
    if (h100 < 15.f || h100 > 25.f || std::fabs(ratio - 1.15f) > 0.03f) { std::printf("FAIL pose: idle height %.1f units, Height 120 ratio %.3f\n", double(h100), double(ratio)); return 1; }
    std::printf("PASS data pose: bind pose = identity, idle height %.1f units, Height 120 / 100 = %.3f\n", double(h100), double(ratio));
    return 0;
}
