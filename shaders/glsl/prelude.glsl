// prelude.glsl - put in front of every built-in and custom shader on Linux,
// Android and macOS (OpenGL ES 2.0 / OpenGL 2.1 GLSL). It plays the role of
// src/shaders/common.hlsli on Windows: same names, same meaning.
//
// Each shader defines  vec4 ps_main(vec2 pos)  where pos is in pixels with
// y pointing DOWN (like Direct3D), so the HLSL and GLSL versions match.
#ifdef GL_ES
precision highp float;
#endif

uniform vec2  res;        // drawing size in pixels
uniform float phase;      // 0..2*pi, loops every 120 s; use whole-number multiples to loop seamlessly
uniform float time;       // seconds of animation
uniform vec2  mouse;      // cursor, pixels (y down)
uniform vec2  click;      // last click, pixels (y down)
uniform float click_age;  // seconds since that click (very large when none)
uniform float mouse_on;   // 1 while the cursor is over the desktop

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Tiny noise so 8-bit gradients don't band.
float dither(vec2 px)
{
    return (fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
}
