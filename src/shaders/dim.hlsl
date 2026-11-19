// Brightness pass: blended as dst * (1 - dim) over whatever was drawn.
#include "common.hlsli"

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    return float4(0, 0, 0, dim);
}
