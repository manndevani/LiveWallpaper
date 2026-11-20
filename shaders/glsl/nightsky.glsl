// Night Sky: twinkling stars over a faint drifting haze. (GLSL twin of src/shaders/nightsky.hlsl)
float star_layer(vec2 p, float a, float density)
{
    vec2 id = floor(p);
    vec2 f  = fract(p) - 0.5;
    float h = hash21(id);
    vec2 o  = vec2(hash21(id + 17.3), hash21(id + 41.7)) - 0.5;
    float d = length(f - o * 0.6);
    float size = 0.035 + 0.07 * h * h;
    float twinkle = 0.55 + 0.45 * sin(a * (1.0 + floor(h * 5.0)) + h * 60.0);
    return step(1.0 - density, h) * (1.0 - smoothstep(size * 0.2, size, d)) * twinkle;
}

vec4 ps_main(vec2 pos)
{
    vec2 uv = pos / res.y;
    float a = phase;
    vec3 col = mix(vec3(0.05, 0.05, 0.13), vec3(0.005, 0.008, 0.03), pos.y / res.y);

    vec2 q = uv * 1.5;
    float off = q.y - 0.55 - 0.18 * sin(q.x * 1.3 + 0.5);
    float band = exp(-off * off * 9.0);
    float haze = 0.5 + 0.5 * sin(q.x * 3.1 + sin(q.y * 2.3 + a) * 1.5 + a);
    col += vec3(0.10, 0.05, 0.16) * band * (0.6 + 0.4 * haze);

    float s = star_layer(uv * 18.0, a, 0.30)
            + star_layer(uv * 34.0 + 7.0, a, 0.25) * 0.7
            + star_layer(uv * 60.0 + 3.0, a, 0.20) * 0.5;
    col += s * vec3(0.85, 0.90, 1.0) * (0.7 + 0.6 * band);

    col += dither(pos);
    return vec4(clamp(col, 0.0, 1.0), 1.0);
}
