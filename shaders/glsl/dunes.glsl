// Dunes: layered desert ridges drifting under a low sun. (GLSL twin of src/shaders/dunes.hlsl)
vec4 ps_main(vec2 pos)
{
    vec2 uv = pos / res; // 0..1, y down
    float aspect = res.x / res.y;
    float a = phase;

    vec3 col = mix(vec3(0.99, 0.66, 0.42), vec3(0.13, 0.10, 0.26), clamp((0.62 - uv.y) * 1.7, 0.0, 1.0));
    vec2 sd = (uv - vec2(0.68, 0.47 + 0.02 * sin(a))) * vec2(aspect, 1.0);
    float r2 = dot(sd, sd);
    col += vec3(1.0, 0.74, 0.46) * (0.6 * exp(-r2 * 140.0) + 0.22 * exp(-r2 * 7.0));

    for (int i = 0; i < 4; i++) {
        float fi = float(i);
        float k = 1.0 + fi;
        float x = uv.x * aspect * (1.2 + 0.6 * fi);
        float ridge = 0.53 + 0.11 * fi
                    + 0.045 * sin(x * 2.1 + a * k + fi * 1.9)
                    + 0.020 * sin(x * 4.7 - a * (k + 1.0) + fi * 0.7)
                    + 0.007 * sin(x * 11.0 + a * 2.0 * k);
        float edge = 1.5 / res.y;
        float m = smoothstep(ridge - edge, ridge + edge, uv.y);
        vec3 sand = mix(vec3(0.66, 0.38, 0.40), vec3(0.08, 0.05, 0.10), fi / 3.0);
        sand *= 1.12 - 0.45 * clamp((uv.y - ridge) * 5.0, 0.0, 1.0);
        col = mix(col, sand, m);
    }

    col += dither(pos);
    return vec4(clamp(col, 0.0, 1.0), 1.0);
}
