// Ripples (interactive): calm water. Clicking the desktop sends out rings,
// and a soft light follows the cursor. (GLSL twin of src/shaders/ripples.hlsl)
vec4 ps_main(vec2 pos)
{
    vec2 uv = pos / res.y;
    float a = phase;
    vec3 col = mix(vec3(0.03, 0.16, 0.22), vec3(0.01, 0.05, 0.09), pos.y / res.y);

    float swell = sin(uv.x * 7.0 + a * 3.0) * sin(uv.y * 5.0 - a * 2.0)
                + 0.5 * sin((uv.x + uv.y) * 11.0 + a * 5.0);
    col += 0.015 * swell;

    float r = length(pos - click) / res.y;
    float t = click_age;
    float front = t * 0.35;
    float ring = sin((r - front) * 70.0) * exp(-abs(r - front) * 18.0) * exp(-t * 0.9);
    col += vec3(0.25, 0.55, 0.65) * 0.35 * ring;

    vec2 md = (pos - mouse) / res.y;
    col += vec3(0.20, 0.45, 0.55) * 0.18 * mouse_on * exp(-dot(md, md) * 25.0);

    col += dither(pos);
    return vec4(clamp(col, 0.0, 1.0), 1.0);
}
