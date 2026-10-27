#version 440
// inFAMOUS PC Launcher - the ambient layer over the game's art: mode 0 (Second Son) drifting smoke and rising embers,
// mode 1 (First Light) slow neon light lines and fine dust. Drawn at ~30 updates a second only while the window is
// active; with "reduce visual effects" it is not drawn at all.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float time;
    float mode;
    float intensity;
    vec2 resolution;
    vec4 colorA;
    vec4 colorB;
};

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += a * noise(p);
        p = p * 2.03 + vec2(1.7, 9.2);
        a *= 0.5;
    }
    return v;
}

// points rising through a grid of cells, one per cell, each with its own speed, sway and life
float particles(vec2 uv, float aspect, float density, float speed, float size, float t) {
    vec2 p = vec2(uv.x * aspect, uv.y) * density;
    p.y += t * speed;
    vec2 cell = floor(p);
    vec2 f = fract(p);
    float h = hash(cell);
    if (h < 0.55) return 0.0;
    vec2 c = vec2(0.2 + 0.6 * hash(cell + 3.1), 0.2 + 0.6 * hash(cell + 7.7));
    c.x += 0.15 * sin(t * (0.6 + h) + h * 40.0);
    float d = length(f - c);
    float flicker = 0.55 + 0.45 * sin(t * (2.0 + 3.0 * h) + h * 90.0);
    return smoothstep(size, 0.0, d) * flicker;
}

void main() {
    vec2 uv = qt_TexCoord0;
    float aspect = resolution.x / max(resolution.y, 1.0);
    vec3 col = vec3(0.0);
    float alpha = 0.0;
    if (mode < 0.5) {
        // smoke: two layers of noise drifting up and sideways, heavier near the bottom
        vec2 q = vec2(uv.x * aspect * 1.6, uv.y * 1.6);
        float s = fbm(q + vec2(time * 0.025, time * 0.06));
        s = fbm(q * 1.4 + vec2(s * 1.8, time * 0.04) );
        float low = smoothstep(0.15, 1.0, uv.y);
        float smoke = smoothstep(0.42, 0.95, s) * (0.35 + 0.65 * low);
        col += colorA.rgb * smoke * 0.55;
        alpha += smoke * 0.32;
        // embers rising: inverted y so they go up
        vec2 eu = vec2(uv.x, 1.0 - uv.y);
        float e = particles(eu, aspect, 9.0, 0.22, 0.06, time) + particles(eu + 0.37, aspect, 15.0, 0.35, 0.05, time * 1.1) * 0.7;
        e *= smoothstep(0.0, 0.6, uv.y);
        col += colorB.rgb * e * 1.6;
        alpha += e * 0.9;
    } else {
        // neon light lines: thin diagonal bands sweeping slowly
        float d = uv.x * aspect * 0.9 + uv.y * 0.55;
        float lines = 0.0;
        for (int i = 0; i < 3; ++i) {
            float fi = float(i);
            float pos = fract(d * (0.8 + 0.35 * fi) - time * (0.015 + 0.007 * fi) + fi * 0.31);
            lines += smoothstep(0.012, 0.0, abs(pos - 0.5)) * (0.5 + 0.5 * sin(time * 0.7 + fi * 2.0));
        }
        lines *= smoothstep(0.0, 0.5, uv.y) * 0.6;
        vec3 neon = mix(colorA.rgb, colorB.rgb, 0.5 + 0.5 * sin(d * 3.0 + time * 0.2));
        col += neon * lines;
        alpha += lines * 0.55;
        // haze and dust
        float haze = fbm(vec2(uv.x * aspect * 1.2 + time * 0.02, uv.y * 1.2 - time * 0.01));
        float h = smoothstep(0.5, 0.95, haze) * 0.25;
        col += colorB.rgb * h * 0.5;
        alpha += h * 0.2;
        vec2 du = vec2(uv.x, 1.0 - uv.y);
        float dust = particles(du, aspect, 22.0, 0.05, 0.04, time) * 0.6;
        col += mix(colorA.rgb, vec3(1.0), 0.4) * dust;
        alpha += dust * 0.5;
    }
    alpha = clamp(alpha * intensity, 0.0, 1.0);
    fragColor = vec4(col * intensity, alpha) * qt_Opacity;
}
