# Writing your own shader wallpaper

livewall can run any HLSL pixel shader written Shadertoy-style. Save it as a `.hlsl` file and add it in **Settings → Add wallpaper**, or from the tray menu.

```hlsl
void mainImage(out float4 fragColor, in float2 fragCoord)
{
    float2 uv = fragCoord / iResolution.xy;
    fragColor = float4(uv, 0.5 + 0.5 * sin(iTime), 1.0);
}
```

See [`examples/plasma.hlsl`](../examples/plasma.hlsl) for a complete example.

## Inputs

| Name | Type | Meaning |
|---|---|---|
| `iResolution` | `float3` | Render size in pixels. Half the screen size when Quality is *Half*. |
| `iTime` | `float` | Seconds since the wallpaper started, scaled by the Speed slider. |
| `iMouse` | `float4` | `xy` = cursor position, `zw` = last click position, in pixels with the origin at the bottom-left, as in Shadertoy. Updates only while the cursor is over the desktop. |
| `fragCoord` | `float2` | Pixel position, origin bottom-left. |

These GLSL names also work, so most Shadertoy code needs few edits: `vec2 vec3 vec4 mat2 mat3 fract mix mod`.
Still to change by hand: `texture()` and iChannels (not supported), `const` arrays, and matrix multiply order (`mul()` in HLSL).

## Rules of thumb

- **Keep it cheap.** It runs behind your work all day. Avoid long loops and raymarching with many steps.
- **Keep it dim.** Icons and text need to stay readable on top. Multiplying the final color by 0.3–0.5 usually helps.
- The output is clamped to 0..1, and alpha is ignored.
- If the shader doesn't compile, livewall shows the default wallpaper instead. The compiler error is written to `%APPDATA%\livewall\livewall.log`; open it with **Open folder** in the settings window.
- Shaders are compiled with `ps_4_0`, so any GPU since about 2008 works.

## Sharing it

Share your shader with everyone in the [livewall gallery](https://github.com/manndevani/livewall-gallery). Every shared shader is checked first: it must compile without `#include`, draw more than one flat colour, and take at most 15 times the work of the gallery's reference shader. You can run the same check yourself: `livewall.exe -check myshader.hlsl out.png` writes a picture and an `out.png.txt` with the result.

## On Linux

Linux uses GLSL, the same language as Shadertoy, so most Shadertoy shaders work as they are. Save the shader as a `.glsl` (or `.frag`) file with a `mainImage(out vec4 fragColor, in vec2 fragCoord)` function and run `livewall -set file.glsl`. The inputs are the same: `iResolution`, `iTime`, `iMouse`. See [`examples/plasma.glsl`](../examples/plasma.glsl). `.hlsl` files only work on Windows. Compiler errors go to `~/.config/livewall/livewall.log`.
