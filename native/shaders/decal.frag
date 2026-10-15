#version 450
// SecondSonPC native renderer - alpha-blended surface layers (stencil art), fragment stage.
// Layer textures and the spray mask come from the original graffiti package / the native minigame;
// the composite below is a native approximation (the game's spray shader is not decoded):
//   mode 0  stencil board:  colour map, alpha = board alpha (the cut-out holes are transparent)
//   mode 1  stage paint:    stage colour, alpha = shape (texture red) x spray mask
//   mode 2  stage paint:    colour map, alpha = texture alpha x spray mask
//   mode 3  solid colour (markers, cursor)

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_layer;
layout(set = 1, binding = 0) uniform sampler2D u_mask;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 world_row0;  // w: mode
    vec4 world_row1;  // w: opacity
    vec4 world_row2;
    vec4 color;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    const int mode = int(pc.world_row0.w + 0.5);
    const float opacity = pc.world_row1.w;
    vec4 tex = texture(u_layer, v_uv);
    float mask = texture(u_mask, v_uv).r;
    vec3 n = normalize(gl_FrontFacing ? v_normal : -v_normal);
    vec3 sun = normalize(vec3(0.35, -0.45, 0.82));
    float ndl = max(dot(n, sun), 0.0);
    float hemi = 0.5 + 0.5 * n.z;
    vec3 light = mix(vec3(0.20, 0.19, 0.18), vec3(0.45, 0.50, 0.58), hemi) + vec3(1.0, 0.96, 0.88) * ndl * 0.9;
    vec3 rgb;
    float a;
    if (mode == 0) {
        rgb = tex.rgb * pc.color.rgb;
        a = tex.a;
    } else if (mode == 1) {
        rgb = pc.color.rgb;
        a = tex.r * mask;
    } else if (mode == 2) {
        rgb = tex.rgb;
        a = tex.a * mask;
    } else {
        rgb = pc.color.rgb;
        a = 1.0;
        light = vec3(1.0);
    }
    out_color = vec4(rgb * light, clamp(a * opacity, 0.0, 1.0));
}
