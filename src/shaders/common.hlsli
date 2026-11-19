// Shared by every shader. Mirrors FrameConsts in src/gfx.h (64 bytes).
cbuffer Frame : register(b0)
{
    float2 res;        // back buffer size in pixels
    float  phase;      // 0..2*pi, loops every 120 s; multiply by whole numbers to loop seamlessly
    float  time;       // seconds of animation (wraps daily)
    float2 mouse;      // cursor, back-buffer pixels
    float2 click;      // last click on the desktop, back-buffer pixels
    float  click_age;  // seconds since that click (very large when none)
    float  mouse_on;   // 1 while the cursor is over the desktop
    float  dim;        // used by the brightness pass
    float  _pad;
    float4 fit;        // images/video: uv scale (xy) and offset (zw)
};

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

// Tiny noise so 8-bit gradients don't band.
float dither(float2 px)
{
    return (frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
}
