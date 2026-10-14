#include "face_poses.h"
#include "appearance.h"

namespace md::character {

std::vector<float> FacePoseWeights(const Config& cfg, const std::vector<std::string>& pose_names, const std::map<std::string, float>& values, std::vector<std::string>* missing) {
    std::vector<float> w(pose_names.size(), 0.f);
    auto idx = [&](const std::string& n) { for (size_t i = 0; i < pose_names.size(); ++i) if (pose_names[i] == n) return int(i); if (missing) missing->push_back(n); return -1; };
    for (const Category& c : cfg.categories) for (const Range& r : c.ranges) {
        if (r.target.empty() && r.target_opposite.empty()) continue;
        auto it = values.find(r.name);
        float v = it == values.end() ? float(RangeMid(r)) : it->second;
        if (v < 0.f && !r.target_opposite.empty()) { int k = idx(r.target_opposite); if (k >= 0) w[k] += -v * 0.01f; }
        else if (!r.target.empty()) { int k = idx(r.target); if (k >= 0) w[k] += v * 0.01f; }
    }
    return w;
}

} // namespace md::character
