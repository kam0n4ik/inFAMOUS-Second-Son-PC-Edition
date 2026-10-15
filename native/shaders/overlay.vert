#version 450
// SecondSonPC native renderer - screen overlay (HUD text and panels), vertex stage. Positions in pixels.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 mvp;  // pixels -> clip space
    vec4 r0;
    vec4 r1;
    vec4 r2;
    vec4 color;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
