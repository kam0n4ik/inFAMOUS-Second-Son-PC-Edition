#version 440
// inFAMOUS PC Launcher - a glass panel: its part of the blurred art (blurred once when the art or the window size
// changes, not every frame), tinted with the theme's surface, a faint light at the top, fine grain, rounded corners.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float tintAmount;
    float light;
    float radius;
    vec2 size;
    vec4 tint;
};
layout(binding = 1) uniform sampler2D source;

float hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return fract((p.x + p.y) * p.x);
}

void main() {
    vec2 pos = qt_TexCoord0 * size;
    vec2 half_ = size * 0.5;
    vec2 q = abs(pos - half_) - (half_ - vec2(radius));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float mask = clamp(0.5 - d, 0.0, 1.0);
    vec3 bg = texture(source, qt_TexCoord0).rgb;
    vec3 c = mix(bg, tint.rgb, tintAmount);
    c += light * (1.0 - smoothstep(0.0, 0.4, qt_TexCoord0.y)) * 0.05;
    c += (hash(pos) - 0.5) * 0.012;
    fragColor = vec4(c, 1.0) * mask * qt_Opacity;
}
