#version 460 core
// Player hair fragment shader — Lambert diffuse + flat tint. No textures.
// Double-sided (cull_back=false in pipeline) so N is faced toward the light.
// set=3 binding=0 = HairFU
#ifdef SPIRV_COMPILE
layout(location=0) in  vec3 vN;
layout(std140, set=3, binding=0) uniform HairFU {
    vec3  color;
    float pad;
} uf;
layout(location=0) out vec4 fragColor;
void main() {
    vec3 N = normalize(vN);
    vec3 L = normalize(vec3(0.4, 1.0, 0.6));
    float diff = abs(dot(N, L)) * 0.60 + 0.40;
    fragColor = vec4(uf.color * diff, 1.0);
}
#else
in vec3 vN;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    vec3 N = normalize(vN);
    vec3 L = normalize(vec3(0.4, 1.0, 0.6));
    float diff = abs(dot(N, L)) * 0.60 + 0.40;
    fragColor = vec4(uColor * diff, 1.0);
}
#endif
