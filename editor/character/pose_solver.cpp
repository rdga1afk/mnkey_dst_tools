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

} // namespace

const md::ogre::Animation* FindAnimation(const md::ogre::Skeleton& sk, const std::string& name) {
    for (const md::ogre::Animation& a : sk.animations) if (a.name == name) return &a;
    return nullptr;
}

void SolveSkinMatrices(const SkinnedBody& body, const md::ogre::Skeleton& sk, const md::ogre::Animation* anim, float time,
                       const std::vector<BoneScale>& scales, std::vector<M4>& out) {
    const size_t nb = body.bone_name.size();
    std::map<std::string, const BoneScale*> by_name;
    for (const BoneScale& s : scales) by_name[s.bone] = &s;
    std::map<uint16_t, const md::ogre::Track*> track;
    if (anim) for (const md::ogre::Track& t : anim->tracks) track[t.bone] = &t;

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
            auto it = track.find(b.handle);
            if (it != track.end()) {
                float kt[3], kq[4]; Sample(*it->second, time, kt, kq);
                for (int c = 0; c < 3; ++c) t[c] += kt[c];
                float r[4]; QMul(b.orientation, kq, r); std::memcpy(q, r, sizeof q);
            }
            if (p >= 0) {   // the parent's size scales this bone's offset in the parent's frame (OldBone::updateFromParentImpl, OgreMain 0x1801e20e0)
                auto ps = by_name.find(body.bone_name[p]);
                if (ps != by_name.end()) { const float* z = ps->second->size; t[0] *= z[1]; t[1] *= z[0]; t[2] *= z[2]; }
            }
            M4 local = Trs(t, q, b.has_scale ? b.scale : nullptr);
            world[i] = p >= 0 ? Mul(world[p], local) : local;
            done[i] = 1; stack.pop_back();
        }
    }
    out.assign(nb, Identity());
    for (size_t i = 0; i < nb; ++i) {
        M4 sz = Identity();
        auto s = by_name.find(body.bone_name[i]);
        if (s != by_name.end()) { sz[0] = s->second->size[1]; sz[5] = s->second->size[0]; sz[10] = s->second->size[2]; }
        out[i] = Mul(world[i], Mul(sz, body.inverse_bind[i]));
    }
}

} // namespace md::character
