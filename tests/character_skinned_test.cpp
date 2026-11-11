// #159 phase 2: md::character::BuildSkinnedBody over the race body meshes of the install (every single-submesh .mesh with a skeleton link and bone assignments):
// weights sum to 1, joints exist, inverse bind * bind = identity, and every vertex lies within 12 units (1.2 m) of its dominant bone's bind position.
// usage: md_character_skinned_test <KENSHI_DIR>
#include "skinned_body.h"
#include <monkey_dust/io/kenshi_resources.h>
#include <cmath>
#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    namespace fs = std::filesystem;
    md::KenshiResources res; res.Index(argv[1]);
    int built = 0, skipped = 0, bad = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(argv[1]) / "data" / "character" / "meshes")) {
        if (!e.is_regular_file() || e.path().extension() != ".mesh") continue;
        md::ogre::Mesh m; md::ogre::Error err;
        if (!md::ogre::ReadMesh(e.path().string(), m, err) || !m.has_skeleton || m.submeshes.size() != 1 || m.submeshes[0].bone_assignments.empty() || m.submeshes[0].use_shared) { ++skipped; continue; }
        md::ogre::Skeleton sk;
        std::string sp = res.Path(m.skeleton);
        if (sp.empty() || !md::ogre::ReadSkeleton(sp, sk, err)) { std::printf("FAIL %s: skeleton %s not readable\n", e.path().filename().c_str(), m.skeleton.c_str()); ++bad; continue; }
        md::character::SkinnedBody b; std::string why;
        if (!md::character::BuildSkinnedBody(m, sk, b, why)) {
            // whistler.mesh assigns bone handles up to 54 to female_skeleton.skeleton (30 bones): inconsistent data shipped with the game, not a reader error
            if (e.path().filename() == "whistler.mesh" && why == "bone assignment out of range") { ++skipped; continue; } std::printf("FAIL %s: %s\n", e.path().filename().c_str(), why.c_str()); ++bad; continue; }
        ++built; float worst = 0; bool ok = true;
        for (const auto& v : b.vertices) {
            float s = v.weight[0] + v.weight[1] + v.weight[2] + v.weight[3];
            if (std::fabs(s - 1.f) > 1e-3f || v.joint[0] >= b.bone_name.size()) ok = false;
            const auto& bm = b.bind[v.joint[0]];
            float d = std::sqrt((v.pos[0] - bm[12]) * (v.pos[0] - bm[12]) + (v.pos[1] - bm[13]) * (v.pos[1] - bm[13]) + (v.pos[2] - bm[14]) * (v.pos[2] - bm[14]));
            worst = std::max(worst, d);
        }
        for (size_t i = 0; i < b.bind.size(); ++i)
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
                float s = 0; for (int k = 0; k < 4; ++k) s += b.bind[i][k * 4 + r] * b.inverse_bind[i][c * 4 + k];
                if (std::fabs(s - (c == r ? 1.f : 0.f)) > 1e-3f) ok = false;
            }
        if (!ok || worst > 12.f) { std::printf("FAIL %s: ok=%d worst dominant-bone distance %.2f\n", e.path().filename().c_str(), int(ok), double(worst)); ++bad; }
    }
    if (bad || built == 0) { std::printf("FAIL skinned: %d bad, %d built\n", bad, built); return 1; }
    std::printf("PASS data skinned: %d body meshes built (%d others skipped), weights = 1, inverse bind exact, vertices near their dominant bone\n", built, skipped);
    return 0;
}
