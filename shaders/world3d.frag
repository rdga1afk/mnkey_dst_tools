#version 450 core
// world3d.frag — height-based color for 3D world geometry.
//   y ≈  0.0      : ground (grass green)
//   y >  0.05     : wall/cliff top (stone gray)
//   y < -0.25     : water (dark blue)
//   otherwise     : wall side (darker stone)

layout(location = 0) in float in_world_y;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 col;
    if (in_world_y < -0.25) {
        col = vec3(0.08, 0.20, 0.45);   // water — dark blue
    } else if (in_world_y > 0.05) {
        col = vec3(0.52, 0.48, 0.42);   // wall top — warm stone
    } else if (in_world_y > -0.05) {
        col = vec3(0.32, 0.52, 0.22);   // ground — grass green
    } else {
        col = vec3(0.38, 0.33, 0.28);   // wall side — dark stone
    }
    out_color = vec4(col, 1.0);
}
