// A race body mesh (md::ogre::Mesh + its Skeleton) as the flat arrays a skinned preview needs: vertices with up to four bone influences, indices, bind
// pose and inverse bind matrices (column-major, glTF-like), bone names and parents, and the poses as morph deltas. Units stay the file's (Kenshi: 0.1 m).
// Bone indices are the order of Skeleton::bones (file order); the vertices' joint indices refer to that order, not to the bone handles.
#pragma once
#include <monkey_dust/io/ogre_mesh.h>
#include <monkey_dust/io/ogre_skeleton.h>
#include <string>
#include <vector>

namespace md::character {

struct SkinnedVertex {
    float pos[3] = {}, normal[3] = {}, uv[2] = {};
    uint8_t joint[4] = {};
    float weight[4] = {};
};

struct SkinnedPose {
    std::string name;
    std::vector<float> delta;   // 3 floats per vertex (zero where the pose does not move the vertex)
};

struct SkinnedBody {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;           // triangle list (submesh operation 4), all submeshes
    std::vector<std::string> bone_name;
    std::vector<int> bone_parent;            // -1 for a root
    std::vector<std::array<float, 16>> bind;          // bone -> model, column-major
    std::vector<std::array<float, 16>> inverse_bind;  // model -> bone
    std::vector<SkinnedPose> poses;          // poses of the body geometry (target 1 = submesh 0) when the mesh has one submesh
};

// Single-submesh meshes with their own geometry (what the race body meshes are). False + `error` otherwise or when a vertex has no bone influence or a bone is unknown.
bool BuildSkinnedBody(const md::ogre::Mesh& mesh, const md::ogre::Skeleton& skeleton, SkinnedBody& out, std::string& error);

} // namespace md::character
