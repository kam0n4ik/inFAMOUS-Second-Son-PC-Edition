#version 450
// SecondSonPC native renderer - skinned mesh, vertex stage (4 joints per vertex, linear blend).

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

layout(set = 1, binding = 0) readonly buffer Palette {
    mat4 joints[];
} pal;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 world_row0;
    vec4 world_row1;
    vec4 world_row2;
    vec4 color;
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;

void main() {
    mat4 skin = in_weights.x * pal.joints[in_joints.x] + in_weights.y * pal.joints[in_joints.y] +
                in_weights.z * pal.joints[in_joints.z] + in_weights.w * pal.joints[in_joints.w];
    vec4 p = skin * vec4(in_position, 1.0);
    vec3 n = mat3(skin) * in_normal;
    gl_Position = pc.mvp * p;
    v_normal = vec3(dot(pc.world_row0.xyz, n), dot(pc.world_row1.xyz, n), dot(pc.world_row2.xyz, n));
    v_uv = in_uv;
}
