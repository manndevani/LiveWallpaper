// Glow Grid (interactive): a dot matrix that lights up and parts around
// the cursor. (GLSL twin of src/shaders/glowgrid.hlsl)
vec4 ps_main(vec2 pos)
{
    float cells = 36.0;
    vec2 p  = pos / res.y * cells;
    vec2 id = floor(p) + 0.5;
    vec2 f  = p - id;

    vec2 centre = id / cells * res.y;
    vec2 md = (centre - mouse) / res.y;
    float near = mouse_on * exp(-dot(md, md) * 30.0);
    vec2 push = normalize(md + 1e-5) * near * 0.25;

    float d = length(f - push);
    float breathe = 0.5 + 0.5 * sin(phase * 2.0 + (id.x + id.y) * 0.35);
    float rad = 0.06 + 0.03 * breathe + 0.16 * near;
    float edge = cells / res.y * 1.2;
    float dotm = 1.0 - smoothstep(rad, rad + edge, d);

    vec3 bg  = mix(vec3(0.035, 0.035, 0.06), vec3(0.015, 0.015, 0.03), pos.y / res.y);
    vec3 dim = vec3(0.22, 0.24, 0.34), lit = vec3(0.50, 0.80, 1.00);
    vec3 col = bg + dotm * mix(dim, lit, near) + lit * 0.05 * near;

    col += dither(pos);
    return vec4(clamp(col, 0.0, 1.0), 1.0);
}
