// plasma.hlsl - an example custom wallpaper shader for livewall.
//
// Write a Shadertoy-style mainImage(). Available inputs:
//   iResolution  float3  screen size in pixels
//   iTime        float   seconds since start (respects the Speed slider)
//   iMouse       float4  xy = cursor, zw = last click (pixels, origin bottom-left)
// GLSL names vec2/vec3/vec4, mat2/mat3, fract, mix and mod also work.
// Add it from the settings window ("Add wallpaper") or the tray menu.
// See docs/CUSTOM_SHADERS.md.

void mainImage(out float4 fragColor, in float2 fragCoord)
{
    float2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime * 0.15;

    float v = sin(uv.x * 3.0 + t)
            + sin((uv.y * 3.0 + t) * 0.8)
            + sin((uv.x + uv.y) * 2.5 + t * 1.3)
            + sin(length(uv * 4.0) - t * 2.0);

    float3 col = 0.5 + 0.5 * cos(v * 1.2 + float3(0.0, 2.1, 4.2));
    col *= 0.35; // keep it calm behind desktop icons

    fragColor = float4(col, 1.0);
}
