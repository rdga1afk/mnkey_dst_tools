// Character editor body on the install fragment program (#207 item 8 step 3, docs/research/CHAR_BODY_INSTALL_FS_DESIGN_2026-11-13.md).
// Ours: the glue only; deferred/character.hlsl main_fs is included from KENSHI_DIR at run time and compiled as it is (nothing of its text is stored).
#include "kenshi_hlsl.h"
#include "spirv_combine.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace md {

// Compiles the original body fragment entry and prints the uniform block it expects. Returns false when the install is missing or the program does not compile.
bool CharBodyLiveCompile(std::vector<uint32_t>& spv_out) {
    const std::string kd = md::KenshiDir();
    if (kd.empty()) return false;
    const std::string dir = kd + "/data/materials/", orig = dir + "deferred/character.hlsl";
    if (!std::filesystem::exists(orig)) { fprintf(stderr, "[CharBodyLive] %s missing\n", orig.c_str()); return false; }
    std::vector<std::string> deps = { orig };
    for (const char* d : { "common", "deferred", "forward", "post" })
        for (const auto& e : std::filesystem::directory_iterator(dir + d))
            if (e.path().extension() == ".hlsl" && e.path().string() != orig) deps.push_back(e.path().string());
    const std::string glue = "#define main_fs main\n#include \"" + orig + "\"\n";
    hlsl::Compiled c;
    if (!hlsl::Compile(glue, deps, 'f', c)) { fprintf(stderr, "[CharBodyLive] fragment: %s\n", c.log.c_str()); return false; }
    std::vector<hlsl::UniformMember> m; uint32_t sz = 0;
    if (!hlsl::GlobalBlockMembers(c.spv, m, sz)) { fprintf(stderr, "[CharBodyLive] fragment: no $Global block\n"); return false; }
    fprintf(stderr, "[CharBodyLive] fragment OK: %zu words, $Global %u bytes, %zu members (%s)\n", c.spv.size(), sz, m.size(), c.from_cache ? "cache" : "compiled");
    spv_out = std::move(c.spv);
    return true;
}

}   // namespace md
