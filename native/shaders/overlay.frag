#version 450
// SecondSonPC native renderer - screen overlay, fragment stage: glyph coverage from the font atlas alpha.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(v_color.rgb, v_color.a * texture(u_atlas, v_uv).a);
}
