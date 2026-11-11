#version 460 core
// Clothing fragment shader — Lambert diffuse + flat tint. No textures.
// set=3 binding=0 = ClothFU
#ifdef SPIRV_COMPILE
layout(location=0) in  vec3 vN;
layout(std140, set=3, binding=0) uniform ClothFU {
    vec4 color;    // xyz = cloth color RGB, w = pad
} uf;
layout(location=0) out vec4 fragColor;
void main() {
    vec3 L = normalize(vec3(0.4, 1.0, 0.6));
    float diff = max(dot(normalize(vN), L), 0.0) * 0.60 + 0.40;
    fragColor = vec4(uf.color.rgb * diff, 1.0);
}
#else
in vec3 vN;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    vec3 L = normalize(vec3(0.4, 1.0, 0.6));
    float diff = max(dot(normalize(vN), L), 0.0) * 0.60 + 0.40;
    fragColor = vec4(uColor * diff, 1.0);
}
#endif
