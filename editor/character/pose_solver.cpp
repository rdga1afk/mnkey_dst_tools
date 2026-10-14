#include "pose_solver.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace md::character {
namespace {

struct Pose { float t[3]; float q[4]; };   // local translation, orientation (x, y, z, w)

void QMul(const float a[4], const float b[4], float o[4]) {   // a * b, (x, y, z, w)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void Sample(const md::ogre::Track& tr, float time, float t[3], float q[4]) {
    const auto& k = tr.keyframes;
    for (int i = 0; i < 3; ++i) t[i] = 0.f;
    q[0] = q[1] = q[2] = 0.f; q[3] = 1.f;
    if (k.empty()) return;
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].time <= time) ++i;
    const md::ogre::Keyframe& a = k[i];
    const md::ogre::Keyframe& b = k[std::min(i + 1, k.size() - 1)];
    float f = (b.time > a.time && time > a.time) ? std::min(1.f, (time - a.time) / (b.time - a.time)) : 0.f;
    for (int c = 0; c < 3; ++c) t[c] = a.translate[c] + (b.translate[c] - a.translate[c]) * f;
    float dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] + a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
    float sgn = dot < 0.f ? -1.f : 1.f, n = 0;
    for (int c = 0; c < 4; ++c) { q[c] = a.rotation[c] + (sgn * b.rotation[c] - a.rotation[c]) * f; n += q[c] * q[c]; }
    n = std::sqrt(n); if (n > 0) for (int c = 0; c < 4; ++c) q[c] /= n;
}

M4 Translate3(float x, float y, float z) { M4 m = Identity(); m[12] = x; m[13] = y; m[14] = z; return m; }

} // namespace

const md::ogre::Animation* FindAnimation(const md::ogre::Skeleton& sk, const std::string& name) {
    for (const md::ogre::Animation& a : sk.animations) if (a.name == name) return &a;
    return nullptr;
}

void SolveSkinMatrices(const SkinnedBody& body, const md::ogre::Skeleton& sk, const std::vector<AnimLayer>& layers,
                       const std::vector<BoneScale>& scales, std::vector<M4>& out) {
    const size_t nb = body.bone_name.size();
    std::map<std::string, const BoneScale*> by_name;
    for (const BoneScale& s : scales) by_name[s.bone] = &s;
    std::vector<std::map<uint16_t, const md::ogre::Track*>> track(layers.size());
    for (size_t l = 0; l < layers.size(); ++l)
        if (layers[l].anim) for (const md::ogre::Track& t : layers[l].anim->tracks) track[l][t.bone] = &t;

    std::vector<M4> world(nb, Identity());
    std::vector<char> done(nb, 0);
    // parents are resolved by recursion over bone_parent (file order may list a child first)
    std::vector<int> stack;
    for (size_t start = 0; start < nb; ++start) {
        stack.assign(1, int(start));
        while (!stack.empty()) {
            int i = stack.back();
            int p = body.bone_parent[i];
            if (done[i]) { stack.pop_back(); continue; }
            if (p >= 0 && !done[p]) { stack.push_back(p); continue; }
            const md::ogre::Bone& b = sk.bones[i];
            float t[3] = {b.position[0], b.position[1], b.position[2]}, q[4] = {b.orientation[0], b.orientation[1], b.orientation[2], b.orientation[3]};
            for (size_t l = 0; l < layers.size(); ++l) {
                auto it = track[l].find(b.handle);
                if (it == track[l].end()) continue;
                float kt[3], kq[4]; Sample(*it->second, layers[l].time, kt, kq);
                for (int c = 0; c < 3; ++c) t[c] += kt[c];
                float r[4]; QMul(q, kq, r); std::memcpy(q, r, sizeof q);
            }
            if (p >= 0) {   // OldBone::updateFromParentImpl (OgreMain 0x1801e20e0; M:formats/ogre-skeleton.md "Bone maths"): offset = local position x this bone's positional size, per axis,
                            // x the Y component of the parent's size on all three axes (the parent's own scale is not inherited)
                float py = 1.f;
                auto ps = by_name.find(body.bone_name[p]);
                if (ps != by_name.end()) py = ps->second->size[1];
                auto me = by_name.find(body.bone_name[i]);
                for (int c = 0; c < 3; ++c) t[c] *= py * (me != by_name.end() && me->second->has_positional ? me->second->positional[c] : 1.f);
            }
            M4 local = Trs(t, q, b.has_scale ? b.scale : nullptr);
            world[i] = p >= 0 ? Mul(world[p], local) : local;
            done[i] = 1; stack.pop_back();
        }
    }
    out.assign(nb, Identity());
    // OgreMain OldBone::_getOffsetTransform (0x1801e1dc0): v' = derivedPos + R_rel * (S * (v - bindPos)), R_rel = derivedOrientation * inverse(bindOrientation); the size vector is
    // applied in the model axes (x left-right, y up, z front-back), not in the bone's own axes.
    for (size_t i = 0; i < nb; ++i) {
        auto s = by_name.find(body.bone_name[i]);
        const float* z = s != by_name.end() ? s->second->size : nullptr;
        M4 rel = Identity();   // R_rel = world3 * bind3^T
        for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) { float v = 0; for (int k = 0; k < 3; ++k) v += world[i][k * 4 + r] * body.bind[i][k * 4 + c]; rel[c * 4 + r] = v; }
        M4 sm = Identity(); if (z) { sm[0] = z[0]; sm[5] = z[1]; sm[10] = z[2]; }
        M4 m = Mul(rel, Mul(sm, Translate3(-body.bind[i][12], -body.bind[i][13], -body.bind[i][14])));
        m[12] += world[i][12]; m[13] += world[i][13]; m[14] += world[i][14];
        out[i] = m;
    }
}

} // namespace md::character
