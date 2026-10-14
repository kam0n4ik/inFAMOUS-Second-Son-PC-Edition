#version 450
// SecondSonPC native renderer - basic lit mesh, fragment stage.
// Placeholder lighting (one sun + hemisphere ambient); not the game's lighting model.

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 world_row0;
    vec4 world_row1;
    vec4 world_row2;
    vec4 color;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 n = normalize(v_normal);
    vec3 sun = normalize(vec3(0.35, -0.45, 0.82));
    float ndl = max(dot(n, sun), 0.0);
    float hemi = 0.5 + 0.5 * n.z;
    vec3 ambient = mix(vec3(0.18, 0.17, 0.16), vec3(0.42, 0.47, 0.55), hemi);
    vec3 lit = pc.color.rgb * (ambient + vec3(1.0, 0.96, 0.88) * ndl * 0.9);
    out_color = vec4(lit, 1.0);
}
