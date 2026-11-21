#pragma once
// Hair / beard attachment meshes in the NPC skin vertex layout (stride 84, metres), skinned by the body skeleton's file-order bone indices.
#include <cstdint>
#include <string>
#include <vector>

namespace md::npcskin {

// `mesh_path` / `skeleton_path` are install files (the attachment mesh and the body mesh's skeleton). All submeshes are appended into one buffer.
// False: a file does not read, a vertex has no bone influence or a bone handle is unknown.
bool BuildAttachmentMesh(const std::string& mesh_path, const std::string& skeleton_path, std::vector<uint8_t>& vertices, std::vector<uint32_t>& indices);

}  // namespace md::npcskin
