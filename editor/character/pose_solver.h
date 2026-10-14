// Skin matrices of a race skeleton: one animation sampled at a time (the idle stance), then the body-slider scales of bone_scales.h on top.
// Ogre keyframes are deltas from the bind pose: orientation = bind orientation * key rotation, position = bind position + key translation.
// A bone's size (setBoneSize) scales that bone's vertices and the offsets of its direct children in its own frame; it is not inherited further down (OgreMain OldBone::updateFromParentImpl
// 0x1801e20e0: the bone's derived scale is its own size). The size vector's first two components act on the bone's local Y and X (swizzled; checked by the standing height, pairs with Kenshi
// frames in phase 4 settle it). The positional size (setBonePositionalSize, OgreMain 0x180014940, stored at +0x174) is read by nothing in OgreMain, so it is not applied.
#pragma once
#include "bone_scales.h"
#include "mat4.h"
#include "skinned_body.h"
#include <monkey_dust/io/ogre_skeleton.h>
#include <vector>

namespace md::character {

// The animation of the skeleton named `name` (nullptr when absent).
const md::ogre::Animation* FindAnimation(const md::ogre::Skeleton& sk, const std::string& name);

// An animation sampled at a time, all layers with weight 1. The skeletons are cumulative (blend mode 1): the layers' rotations multiply onto the bind rotation in the order given
// and their translations add; the slider animations of the editor (spec §4, exe 0x14084b890) are layers on top of the idle stance.
struct AnimLayer { const md::ogre::Animation* anim = nullptr; float time = 0.f; };

// out[i] = world_i * S(size_i) * inverse_bind_i for the bones of `body` (SkinnedBody bone order). `layers` may be empty (bind pose); `scales` may be empty.
void SolveSkinMatrices(const SkinnedBody& body, const md::ogre::Skeleton& sk, const std::vector<AnimLayer>& layers,
                       const std::vector<BoneScale>& scales, std::vector<M4>& out);

} // namespace md::character
