// plasma.glsl - an example custom wallpaper shader for livewall on Linux
// (the same shader as plasma.hlsl, in GLSL, exactly as Shadertoy writes it).
//
// Inputs: iResolution (vec3), iTime (float), iMouse (vec4: xy = cursor,
// zw = last click, pixels, origin bottom-left).
//   livewall -set ~/plasma.glsl
// See docs/CUSTOM_SHADERS.md.

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime * 0.15;

    float v = sin(uv.x * 3.0 + t)
            + sin((uv.y * 3.0 + t) * 0.8)
            + sin((uv.x + uv.y) * 2.5 + t * 1.3)
            + sin(length(uv * 4.0) - t * 2.0);

    vec3 col = 0.5 + 0.5 * cos(v * 1.2 + vec3(0.0, 2.1, 4.2));
    col *= 0.35; // keep it calm behind desktop icons

    fragColor = vec4(col, 1.0);
}
