#version 450
// SecondSonPC native renderer - basic lit mesh, fragment stage.
// Colour map of the original material (slot 1) times the tint; placeholder lighting (one sun +
// hemisphere ambient), not the game's lighting model.

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_albedo;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 world_row0;
    vec4 world_row1;
    vec4 world_row2;
    vec4 color;  // rgb tint, a > 0: alpha-test cutoff
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex = texture(u_albedo, v_uv);
    if (pc.color.a > 0.0 && tex.a < pc.color.a) discard;
    vec3 n = normalize(gl_FrontFacing ? v_normal : -v_normal);
    vec3 sun = normalize(vec3(0.35, -0.45, 0.82));
    float ndl = max(dot(n, sun), 0.0);
    float hemi = 0.5 + 0.5 * n.z;
    vec3 ambient = mix(vec3(0.20, 0.19, 0.18), vec3(0.45, 0.50, 0.58), hemi);
    vec3 base = tex.rgb * pc.color.rgb;
    out_color = vec4(base * (ambient + vec3(1.0, 0.96, 0.88) * ndl * 0.9), 1.0);
}
