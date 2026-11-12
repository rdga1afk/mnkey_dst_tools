#include "prop_tri_live.h"
#ifdef MD_HLSL_RUNTIME
#include "kenshi_hlsl.h"
#include <monkey_dust/io/data_files.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/render/gpu_spirv_embed.h>
#include <fstream>
#include <sstream>
#endif

namespace md {
bool RegisterPropTriLive() {
#ifdef MD_HLSL_RUNTIME
    static std::vector<uint32_t> spv;   // registered by pointer: lives until exit
    if (!spv.empty()) return true;
    const std::string kd = KenshiDir();
    if (kd.empty()) return false;
    std::ifstream in(DataPath("game/src/render/prop_tri_live.hlsl").c_str());
    if (!in) { MD_LOG(MD_LOG_WARNING, "[PropTriLive] glue missing"); return false; }
    std::stringstream ss; ss << in.rdbuf(); std::string glue = ss.str();
    const std::string tri = kd + "/data/materials/deferred/triplanar.hlsl";
    const size_t at = glue.find("@TRI@");
    if (at == std::string::npos) return false;
    glue.replace(at, 5, tri);
    hlsl::Compiled c;
    if (!hlsl::Compile(glue, { tri }, 'f', c)) { MD_LOG(MD_LOG_WARNING, "[PropTriLive] compile failed: %s", c.log.substr(0, 300).c_str()); return false; }
    spv = std::move(c.spv);
    RegisterRuntimeSpirv("shaders/spirv/prop_tri.frag.spv", (const unsigned char*)spv.data(), spv.size() * 4);
    MD_LOG(MD_LOG_INFO, "[PropTriLive] triplanar features: install deferred/triplanar.hlsl (%zu words)", spv.size());
    return true;
#else
    return false;
#endif
}
}
