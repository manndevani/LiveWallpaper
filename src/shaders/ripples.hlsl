// Ripples (interactive): calm water. Clicking the desktop sends out rings,
// and a soft light follows the cursor.
#include "common.hlsli"

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy / res.y;
    float a = phase;
    float3 col = lerp(float3(0.03, 0.16, 0.22), float3(0.01, 0.05, 0.09), pos.y / res.y);

    // Gentle ambient swell.
    float swell = sin(uv.x * 7.0 + a * 3.0) * sin(uv.y * 5.0 - a * 2.0)
                + 0.5 * sin((uv.x + uv.y) * 11.0 + a * 5.0);
    col += 0.015 * swell;

    // Rings from the last click.
    float r = length(pos.xy - click) / res.y;
    float t = click_age;
    float front = t * 0.35;
    float ring = sin((r - front) * 70.0) * exp(-abs(r - front) * 18.0) * exp(-t * 0.9);
    col += float3(0.25, 0.55, 0.65) * 0.35 * ring;

    // Soft light under the cursor.
    float2 md = (pos.xy - mouse) / res.y;
    col += float3(0.20, 0.45, 0.55) * 0.18 * mouse_on * exp(-dot(md, md) * 25.0);

    col += dither(pos.xy);
    return float4(saturate(col) * (1 - dim), 1); // brightness
}
