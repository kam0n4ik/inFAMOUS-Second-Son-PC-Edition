#version 450
// SecondSonPC native renderer - basic lit mesh, vertex stage.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 world_row0;  // rows of the world 3x3 (for normals)
    vec4 world_row1;
    vec4 world_row2;
    vec4 color;
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    v_normal = vec3(dot(pc.world_row0.xyz, in_normal),
                    dot(pc.world_row1.xyz, in_normal),
                    dot(pc.world_row2.xyz, in_normal));
    v_uv = in_uv;
}
