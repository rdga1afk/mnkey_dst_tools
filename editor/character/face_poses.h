// Face (and any pose-driven) sliders -> mesh pose weights, as the original's pose randomiser / setter does (exe 0x140070010, spec §4): weight = value * 0.01 on `target`;
// a negative value goes to `target_opposite` with weight = -value * 0.01 when the slider has one, otherwise it stays signed on `target`. Poses of the same name add up.
#pragma once
#include "editor_limits.h"
#include <map>
#include <string>
#include <vector>

namespace md::character {

// `pose_names`: the body mesh's pose names in mesh order. `value_of(range)` gives the current slider value. Returns one weight per pose; `missing` receives targets the mesh lacks.
std::vector<float> FacePoseWeights(const Config& cfg, const std::vector<std::string>& pose_names, const std::map<std::string, float>& values,
                                   std::vector<std::string>* missing = nullptr);

} // namespace md::character
