// Body sliders -> bone scales of the original editor, in the file's own units (Ogre bone space x, y, z). The formulas are the facts tabled in
// docs/research/KENSHI_CHARACTER_EDITOR_SPEC.md §4 ("Масштаби кісток"), read from exe function 0x14052e6c0 (ghidra MCP); each constant carries its .rdata address there.
// setBoneSize replaces the bone's size, setBonePositionalSize the scale of the offset of its children; nothing is clamped (the original does not).
// НЕВІДОМО (#159): `race_scale` is the value of a virtual call (slot 0x390) on the race object (+0xf0); `skinny` / `muscle` are the character fields +0x1a8 / +0x1a0
// (not editor sliders); robot-leg and stump cases (thigh factor 0.8, hidden limbs) and the stance float +0x190 are not ported.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace md::character {

struct BoneScale {
    std::string bone;                   // "Bip01 Spine", "L Boob", ...
    float size[3] = {1.f, 1.f, 1.f};    // setBoneSize
    bool has_positional = false;
    float positional[3] = {1.f, 1.f, 1.f};   // setBonePositionalSize
};

struct BodyScaleInputs {
    bool female = false;                // `sex female`
    bool has_breast_bones = false;      // the skeleton has `L Boob`
    bool has_jaw_bone = false;          // the skeleton has `Bip01 Jaw`
    float race_scale = 1.f;             // see НЕВІДОМО above
    float skinny = 0.f, muscle = 0.f;   // 0..1
};

// `values`: slider name -> value as stored in a .bod2 (Height 100 = 1.0). A missing slider counts as 100. Bones in the order the original sets them.
std::vector<BoneScale> ComputeBoneScales(const std::map<std::string, float>& values, const BodyScaleInputs& in);

} // namespace md::character
