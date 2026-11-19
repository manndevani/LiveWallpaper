// Draws an image or GIF frame with the cover / contain / stretch mapping.
#include "common.hlsli"

Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy / res * fit.xy + fit.zw;
    if (uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1)
        return float4(0, 0, 0, 1);
    float4 c = tex.Sample(samp, uv);
    return float4(c.rgb, 1); // premultiplied, so transparent areas come out black
}
