// Glow Grid (interactive): a dot matrix that lights up and parts around
// the cursor.
#include "common.hlsli"

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    const float cells = 36.0;
    float2 p  = pos.xy / res.y * cells;
    float2 id = floor(p) + 0.5;
    float2 f  = p - id;

    float2 centre = id / cells * res.y; // cell centre, pixels
    float2 md = (centre - mouse) / res.y;
    float near = mouse_on * exp(-dot(md, md) * 30.0);
    float2 push = normalize(md + 1e-5) * near * 0.25; // dots part around the cursor

    float d = length(f - push);
    float breathe = 0.5 + 0.5 * sin(phase * 2.0 + (id.x + id.y) * 0.35);
    float rad = 0.06 + 0.03 * breathe + 0.16 * near;
    float edge = cells / res.y * 1.2; // about 1.2 px of anti-aliasing
    float dotm = 1.0 - smoothstep(rad, rad + edge, d);

    float3 bg  = lerp(float3(0.035, 0.035, 0.06), float3(0.015, 0.015, 0.03), pos.y / res.y);
    float3 dull = float3(0.22, 0.24, 0.34), lit = float3(0.50, 0.80, 1.00);
    float3 col = bg + dotm * lerp(dull, lit, near) + lit * 0.05 * near;

    col += dither(pos.xy);
    return float4(saturate(col) * (1 - dim), 1); // brightness
}
