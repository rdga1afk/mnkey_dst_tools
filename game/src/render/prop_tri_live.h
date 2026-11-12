#pragma once
namespace md {
// Release build (no shader overlay): compiles tools/game/src/render/prop_tri_live.hlsl over the install's deferred/triplanar.hlsl and registers the
// SPIR-V as shaders/spirv/prop_tri.frag.spv, so PropRenderer's triplanar pipeline finds it. Call once before any map feature loads. false = not compiled.
bool RegisterPropTriLive();
}
