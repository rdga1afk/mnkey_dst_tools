#version 430 core
#ifdef SPIRV_COMPILE
layout(location = 0) in  vec4 vColor;
layout(location = 0) out vec4 fragColor;
#else
in  vec4 vColor;
out vec4 fragColor;
#endif

void main() {
    vec2 coord = gl_PointCoord * 2.0 - 1.0;
    float r = dot(coord, coord);
    if (r > 1.0) discard;
    float alpha = vColor.a * (1.0 - r);
    fragColor = vec4(vColor.rgb, alpha);
}
