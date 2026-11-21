#include "npc_hair_mesh.h"
#include "npc_skin_live.h"
#include <skinned_body.h>
#include <cstring>

namespace md::npcskin {

bool BuildAttachmentMesh(const std::string& mesh_path, const std::string& skeleton_path, std::vector<uint8_t>& vertices, std::vector<uint32_t>& indices) {
    md::ogre::Mesh mesh; md::ogre::Error oe;
    if (!md::ogre::ReadMesh(mesh_path, mesh, oe)) return false;
    md::ogre::Skeleton sk; md::ogre::Error se;
    if (!md::ogre::ReadSkeleton(skeleton_path, sk, se)) return false;
    vertices.clear(); indices.clear();
    constexpr float kUnit = 0.1f;   // file units -> metres, as the game body (HumanBodyData)
    for (const md::ogre::SubMesh& sub : mesh.submeshes) {
        md::ogre::Mesh one = mesh; one.submeshes.assign(1, sub);
        md::character::SkinnedBody part; std::string err;
        if (!md::character::BuildSkinnedBody(one, sk, part, err)) return false;
        const uint32_t base = (uint32_t)(vertices.size() / kStride);
        vertices.resize(vertices.size() + part.vertices.size() * kStride, 0);
        for (size_t i = 0; i < part.vertices.size(); ++i) {
            const md::character::SkinnedVertex& v = part.vertices[i];
            uint8_t* d = &vertices[((size_t)base + i) * kStride];
            const float pos[3] = { v.pos[0] * kUnit, v.pos[1] * kUnit, v.pos[2] * kUnit };
            const float tan4[4] = { v.tangent[0], v.tangent[1], v.tangent[2], 1.f };
            memcpy(d, pos, 12); memcpy(d + 12, v.normal, 12); memcpy(d + 24, v.uv, 8);
            memcpy(d + 32, v.joint, 4); memcpy(d + 36, v.weight, 16);
            memcpy(d + 56, tan4, 16); memcpy(d + 72, v.binormal, 12);
        }
        for (uint32_t ix : part.indices) indices.push_back(ix + base);
    }
    return !vertices.empty() && !indices.empty();
}

}  // namespace md::npcskin
