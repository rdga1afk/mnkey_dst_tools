#pragma once
// Path 1 (docs/spec/shaders/path1_adapter.md): compile a Kenshi HLSL file of the player's install at run time with glslang.
// Nothing of the original text is stored: the cache holds SPIR-V only.
#include <cstdint>
#include <string>
#include <vector>

namespace md::hlsl {

struct Compiled {
    std::vector<uint32_t> spv;
    std::string log;       // glslang info log on failure
    double compile_ms = 0; // 0 when taken from the cache
    bool from_cache = false;
    bool valid = true;      // spirv-val (Vulkan 1.1) verdict; an invalid module must never reach the driver (ANV hangs in pipeline creation)
    std::string valid_log;  // first validator message when !valid
    std::string cache_path; // the SPIR-V file of the cache (the HAL can load a pipeline from it)
};

// glue: our translation unit (macros, wrapper entry point); it #includes the original by absolute path.
// dep_files: the original files the glue includes (hashed into the cache key; read, never copied).
// stage: 'c' compute, 'f' fragment, 'v' vertex. Returns false (log filled) if it does not compile.
bool Compile(const std::string& glue, const std::vector<std::string>& dep_files, char stage, Compiled& out);

// Compatibility rule (path1_adapter.md s.2): DX9 functions may assign to their `uniform` parameters. Every such parameter becomes `<name>__w` and a local of the
// original name is declared at the top of the body. Applied to every included file; exposed for the unit test.
std::string MakeUniformParamsWritable(const std::string& src);

const char* Version();

} // namespace md::hlsl
