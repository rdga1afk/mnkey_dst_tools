#include "skinned_body.h"
#include "mat4.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace md::character {
bool BuildSkinnedBody(const md::ogre::Mesh& mesh, const md::ogre::Skeleton& sk, SkinnedBody& out, std::string& error) {
    out = SkinnedBody();
    if (mesh.submeshes.size() != 1 || mesh.submeshes[0].use_shared || !mesh.submeshes[0].has_geometry) { error = "expected one submesh with its own geometry"; return false; }
    const md::ogre::SubMesh& sm = mesh.submeshes[0];
    const md::ogre::Geometry& g = sm.geometry;
    if (sk.bones.empty() || sk.bones.size() > 255) { error = "skeleton bone count"; return false; }

    // bones: handle -> index in file order; bind pose world matrices (parents may come later in the file: resolve by recursion)
    std::map<uint16_t, int> by_handle;
    for (size_t i = 0; i < sk.bones.size(); ++i) by_handle[sk.bones[i].handle] = int(i);
    const size_t nb = sk.bones.size();
    out.bone_name.resize(nb); out.bone_parent.assign(nb, -1); out.bind.assign(nb, Identity()); out.inverse_bind.assign(nb, Identity());
    std::vector<M4> local(nb); std::vector<int> state(nb, 0);
    for (size_t i = 0; i < nb; ++i) {
        const md::ogre::Bone& b = sk.bones[i];
        out.bone_name[i] = b.name;
        if (b.has_parent) { auto it = by_handle.find(b.parent); if (it == by_handle.end()) { error = "bone " + b.name + " has an unknown parent"; return false; } out.bone_parent[i] = it->second; }
        local[i] = Trs(b.position, b.orientation, b.has_scale ? b.scale : nullptr);
    }
    for (size_t i = 0; i < nb; ++i) {
        std::vector<int> chain; int c = int(i);
        while (c >= 0 && state[c] == 0) { chain.push_back(c); state[c] = 2; c = out.bone_parent[c]; if (chain.size() > nb) { error = "bone parent cycle"; return false; } }
        if (c >= 0 && state[c] == 2) { error = "bone parent cycle"; return false; }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            int p = out.bone_parent[*it];
            out.bind[*it] = p >= 0 ? Mul(out.bind[p], local[*it]) : local[*it];
            state[*it] = 1;
        }
    }
    for (size_t i = 0; i < nb; ++i) out.inverse_bind[i] = InvAffine(out.bind[i]);

    // vertices
    std::vector<float> pos, nrm, uv;
    if (!g.Positions(pos)) { error = "no float position element"; return false; }
    bool has_n = g.Normals(nrm), has_uv = g.Uvs(0, uv);
    const size_t nv = g.vertex_count;
    out.vertices.assign(nv, SkinnedVertex());
    std::vector<std::vector<std::pair<float, int>>> infl(nv);   // weight, bone index
    for (const md::ogre::BoneAssignment& a : sm.bone_assignments) {
        auto it = by_handle.find(a.bone);
        if (a.vertex >= nv || it == by_handle.end()) { error = "bone assignment out of range"; return false; }
        infl[a.vertex].push_back({a.weight, it->second});
    }
    for (size_t v = 0; v < nv; ++v) {
        SkinnedVertex& o = out.vertices[v];
        std::memcpy(o.pos, &pos[v * 3], 12);
        if (has_n) std::memcpy(o.normal, &nrm[v * 3], 12);
        if (has_uv) std::memcpy(o.uv, &uv[v * 2], 8);
        auto& w = infl[v];
        if (w.empty()) { error = "vertex " + std::to_string(v) + " has no bone influence"; return false; }
        std::stable_sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        float sum = 0; for (size_t k = 0; k < w.size() && k < 4; ++k) sum += w[k].first;
        if (!(sum > 0)) { error = "vertex " + std::to_string(v) + " has zero total weight"; return false; }
        for (size_t k = 0; k < w.size() && k < 4; ++k) { o.joint[k] = uint8_t(w[k].second); o.weight[k] = w[k].first / sum; }
    }
    if (sm.operation != md::ogre::OP_TRIANGLE_LIST) { error = "submesh operation is not a triangle list"; return false; }
    out.indices = sm.indices;

    for (const md::ogre::Pose& p : mesh.poses) {
        if (p.target != 1) continue;
        SkinnedPose sp; sp.name = p.name; sp.delta.assign(nv * 3, 0.f);
        for (const auto& o : p.offsets) if (o.first < nv) std::memcpy(&sp.delta[size_t(o.first) * 3], o.second.data(), 12);
        out.poses.push_back(std::move(sp));
    }
    return true;
}

} // namespace md::character
