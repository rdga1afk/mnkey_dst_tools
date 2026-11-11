#pragma once
// Path 1: the cloud layer of the sky from the install (materials/SkyX/SkyX_Clouds.hlsl, the HDR variant), a full-screen pass over the sky, alpha blended.
// Our glue turns the pixel's view direction into the dome direction the original expects and feeds the layer parameters the exe sets. Off unless MD_SKY_INSTALL.
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/md_camera.h>

namespace SkyClouds {
// Draw inside the open scene pass after the sky and the moons; `hours` = game hour.
void Draw(GpuPassView& pv, const MdCamera& cam, float hours);
// The game clock in minutes since day 0 (the weather's time base); called once per frame before the draw.
void SetClock(double minutes);
// The camera in Kenshi world metres (0..29491), used to find the zone and its weather.
void SetCameraWorld(float x_m, float z_m);
// Scenario override (md.set_weather): fixed cloud density offset, darkness and ground wetness (the live ground pass uniform) instead of the zone's weather; off = the weather of the zone, wetness 0.
void SetWeatherOverride(bool on, float cloud_offset, float darkness, float wetness);
float OverrideWetness();
}
