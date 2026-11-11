#pragma once
// SDL_GPU wants combined image samplers in fixed descriptor sets (vertex: samplers 0, uniform buffers 1; fragment: samplers 2, uniform buffers 3), glslang's HLSL front end
// gives separate textures and sampler states in set 0. CombineSamplers rewrites such a module: every sampled texture variable becomes a combined image sampler (its sampler
// state is dropped: the engine binds one sampler per texture), textures go to the sampler set, uniform buffers to the uniform-buffer set, bindings unchanged.
// Modules without separate samplers are left as they are. Pure function on the SPIR-V words, no Kenshi content.
#include <cstdint>
#include <string>
#include <vector>

namespace md::hlsl {
// stage: 'v' or 'f'. Returns false (err filled) for a shape it does not handle (arrays of textures, sampler function parameters, storage buffers in a stage that needs a shift).
bool CombineSamplers(std::vector<uint32_t>& spv, char stage, std::string& err);
}

namespace md::hlsl {
struct UniformMember { std::string name; uint32_t offset = 0; uint32_t array_stride = 0; };
// The members of the module's default uniform block ("$Global": the loose `uniform` globals of an HLSL file) with their byte offsets (std140 as glslang laid it out),
// so the engine can fill the block by the original's own names.
bool GlobalBlockMembers(const std::vector<uint32_t>& spv, std::vector<UniformMember>& out, uint32_t& size_bytes);
}
