// Aurora: slow flowing teal and violet light. (GLSL twin of src/shaders/aurora.hlsl)
vec4 ps_main(vec2 pos)
{
    vec2 uv = (pos - 0.5 * res) / res.y;
    vec2 p  = uv * 2.4;
    float a = phase;

    for (int i = 1; i <= 6; i++) {
        float f = float(i);
        p.x += 0.5 / f * sin(f * p.y + a * (1.0 + mod(f, 3.0)) + 0.9 * f);
        p.y += 0.5 / f * cos(f * p.x - a * (1.0 + mod(f + 1.0, 3.0)) + 1.7 * f);
    }

    float bands = 0.5 + 0.5 * sin(p.x * 1.3 + p.y * 0.7);
    float glow  = 0.5 + 0.5 * cos(length(p) * 1.4 - 2.0 * a);

    vec3 base   = vec3(0.015, 0.020, 0.050);
    vec3 teal   = vec3(0.040, 0.320, 0.400);
    vec3 violet = vec3(0.300, 0.080, 0.400);
    vec3 col = base + teal * bands * bands + violet * glow * glow * bands;

    col *= 1.0 - 0.45 * dot(uv, uv); // soft vignette keeps icons readable
    col += dither(pos);
    return vec4(clamp(col, 0.0, 1.0), 1.0);
}
