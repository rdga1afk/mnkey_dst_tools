#version 460 core
// World map zone pin — coloured quad in screen space.
// set=1 binding=0: array of up to 40 pins + state

#ifdef SPIRV_COMPILE
layout(std140, set=1, binding=0) uniform PinUBO {
    // [0]=x_norm [1]=y_norm [2]=w_norm [3]=state
    // state: 0=unvisited 1=visited 2=current 3=hovered 4=selected
    vec4 pins[40];
    float screen_w, screen_h, player_angle, _p1;
} u;
layout(location=0) out float vState;
layout(location=1) out vec2  vUV;
layout(location=2) out float vRot;   // player facing angle (rot_y), only used for state==2
#else
uniform vec4 pins[40];
uniform float screen_w, screen_h;
out float vState;
out vec2  vUV;
out float vRot;
#endif

void main() {
    const vec2 c[6] = vec2[6](
        vec2(0,0),vec2(1,0),vec2(0,1),
        vec2(1,0),vec2(1,1),vec2(0,1)
    );
    vec2 uv = c[gl_VertexIndex];
    vUV = uv;

#ifdef SPIRV_COMPILE
    vec4 pin = u.pins[gl_InstanceIndex];
    vState   = pin.w;
    vRot     = u.player_angle;
    float sw = u.screen_w, sh = u.screen_h;
#else
    vec4 pin = pins[gl_InstanceIndex];
    vState   = pin.w;
    vRot     = 0.0;
    float sw = screen_w, sh = screen_h;
#endif

    float aspect = sw / sh;           // correct oval → circle on 16:9
    float px = pin.x + uv.x * pin.z;
    float py = pin.y + uv.y * pin.z * aspect;
    gl_Position = vec4(px * 2.0 - 1.0, -(py * 2.0 - 1.0), 0.0, 1.0);
}
