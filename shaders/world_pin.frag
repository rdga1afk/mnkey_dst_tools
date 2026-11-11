#version 460 core
// Zone pin colour by state:
// 0=unvisited(dark), 1=visited(white), 2=current(gold), 3=hovered(cyan), 4=selected(green)

#ifdef SPIRV_COMPILE
layout(location=0) in  float vState;
layout(location=1) in  vec2  vUV;
layout(location=2) in  float vRot;
layout(location=0) out vec4  outColor;
#else
in  float vState;
in  vec2  vUV;
in  float vRot;
out vec4  outColor;
#endif

void main() {
    vec2  d    = vUV - 0.5;
    float dist = length(d) * 2.0;
    int   s    = int(vState + 0.5);

    if (s == 2) {
        // Player location: small bright-red triangle pointing in movement direction.
        // Rotate d so the apex tracks world rot_y:
        //   rot_y=0 (south/+Z) → points down;  rot_y=π (north/-Z) → points up.
        // Rotation angle = π - rot_y maps world facing to screen direction.
        float angle = 3.14159265 - vRot;
        float ca = cos(angle), sa = sin(angle);
        vec2  dr = vec2(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
        float t      = (dr.y + 0.38) / 0.76;
        bool  fill   = (t >= 0.0 && t <= 1.0 && abs(dr.x) < 0.30 * t);
        float tb     = (dr.y + 0.42) / 0.84;
        bool  border = (tb >= 0.0 && tb <= 1.0 && abs(dr.x) < 0.34 * tb) && !fill;
        if (!fill && !border) discard;
        vec3 col = fill ? vec3(1.0, 0.08, 0.08) : vec3(0.55, 0.0, 0.0);
        outColor = vec4(col, 0.97);
        return;
    }

    // All other pins: filled circle
    float ring = smoothstep(0.82, 0.94, dist);
    float fill = 1.0 - smoothstep(0.72, 0.84, dist);
    if (dist > 0.96) discard;

    vec3 col;
    if      (s == 3) col = vec3(0.30, 0.90, 0.90);  // hovered  — cyan
    else if (s == 4) col = vec3(0.20, 1.00, 0.40);  // selected — green
    else if (s == 1) col = vec3(0.90, 0.90, 0.90);  // visited  — white
    else             col = vec3(0.40, 0.40, 0.40);  // unvisited — grey

    vec3  border = col * 0.25;
    vec3  final  = mix(col, border, ring);
    float alpha  = fill * (s == 0 ? 0.60 : 0.88);
    outColor = vec4(final, alpha);
}
