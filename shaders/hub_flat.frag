#version 460 core
// #119 spike: constant-colour fragment shader to separate vertex / raster cost from pixel cost (instanced path, md.set_hub_spike_flat). No inputs used.
layout(location = 0) in  vec3  vNormal;
layout(location = 1) in  vec2  vUV;
layout(location = 2) in  float vLayer;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(0.5, 0.5, 0.5, 1.0) + vec4(vNormal * 0.0001 + vec3(vUV * 0.0001, vLayer * 0.0001), 0.0); }
