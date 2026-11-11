#pragma once
// Path 1 (docs/spec/shaders/path1_adapter.md): the two moons of the sky from the install: materials/planet01.mesh drawn with forward/moon.hlsl (+ moon_HI.dds / moon2_HI.dds).
// Our glue feeds the original vertex and fragment functions; placement follows what the exe does every frame (private notes: re/re_/skyx/moon_facts.md).
// Off unless MD_SKY_INSTALL is set and the build has MD_HLSL_RUNTIME.
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/md_camera.h>

namespace SkyMoons {
// Draw inside the already open scene pass right after the sky; `sun` = unit direction toward the sun (the sky's real sun), `hours` = game hour.
void Draw(GpuPassView& pv, const MdCamera& cam, const float sun[3], float hours);
// whole game days elapsed (the exe adds 24 h per day to the moon angle); 0 while a scenario pins the hour
void SetDay(int day);
}
