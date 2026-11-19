// Aurora: slow flowing teal and violet light.
#include "common.hlsli"

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    float2 uv = (pos.xy - 0.5 * res) / res.y;
    float2 p  = uv * 2.4;
    float  a  = phase;

    [unroll] for (int i = 1; i <= 6; i++)
    {
        float f = (float)i;
        p.x += 0.5 / f * sin(f * p.y + a * (float)(1 + i % 3) + 0.9 * f);
        p.y += 0.5 / f * cos(f * p.x - a * (float)(1 + (i + 1) % 3) + 1.7 * f);
    }

    float bands = 0.5 + 0.5 * sin(p.x * 1.3 + p.y * 0.7);
    float glow  = 0.5 + 0.5 * cos(length(p) * 1.4 - 2.0 * a);

    float3 base   = float3(0.015, 0.020, 0.050);
    float3 teal   = float3(0.040, 0.320, 0.400);
    float3 violet = float3(0.300, 0.080, 0.400);
    float3 col = base + teal * bands * bands + violet * glow * glow * bands;

    col *= 1.0 - 0.45 * dot(uv, uv); // soft vignette keeps icons readable
    col += dither(pos.xy);
    return float4(saturate(col), 1);
}
