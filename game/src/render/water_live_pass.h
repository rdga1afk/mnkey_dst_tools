#pragma once
// Water live (path 1, glue order water -> props -> exposure -> characters; coordinator 6795311365): the ORIGINAL waterVP + waterFP (forward/water.hlsl, near variant with BLEND)
// of the player's install, compiled at run time from KENSHI_DIR. CompileWaterLive is CPU only (glslang), no GPU device: it is what the dry run (md_water_live_dry) exercises.
// The in-game draw is not written yet (needs the GPU slot to bind and check a frame).
#include "kenshi_hlsl.h"
#include "spirv_combine.h"
#include <string>
#include <vector>

namespace md {

// Our own HLSL glue (no Kenshi text) that turns the scene depth into the map the original's `depthmap` reads; compiled by WaterLivePass and by md_water_live_dry.
extern const char* const kWaterDepthVert; extern const char* const kWaterDepthFrag;

struct WaterLiveShaders {
    hlsl::Compiled vs, fs;
    std::vector<hlsl::UniformMember> vmem, fmem;   // the loose `uniform`s of each stage with their byte offsets (the original's own names)
    uint32_t vsz = 0, fsz = 0;                      // size of each default uniform block in bytes
};

// glue text is built here from the original's path; the original file is only #included, never copied. Returns false (log filled) when a stage does not compile.
bool CompileWaterLive(const std::string& kenshi_dir, WaterLiveShaders& out, std::string& log);

}   // namespace md
