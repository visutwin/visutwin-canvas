// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// The lens flare effect's passes (LensFlareEffect), one variant each:
//   shape      copies a shape texture (input 0) into a layer of the shape library, a texture
//              array, decoded from gamma space when it is sRGB (pow 2.2, the decode every colour
//              texture here takes), storing the colour as its square root, which keeps precision in a
//              shape's dark tail, and the coverage in alpha;
//   occlusion  measures the visibility of each flare's source into its texel of a 32 x 1
//              target: 24 taps of the scene depth (input 0) in a disc around the source,
//              counting the ones the scene does not cover, moved from the previous frame's
//              visibility (input 1) by the flare's blend factor, by at least one step of the
//              8-bit storage so that a fade never stalls short of its target;
//   sprite     draws the flare elements, added into the flare texture: each element's shape
//              (a layer of the library, input 0), squared back, tinted by the colour the effect
//              resolved and faded by its flare's visibility (input 1).
// The occlusion and sprite variants draw geometry the effect builds every frame, in the
// standard 56-byte vertex layout (see lensFlareEffect.cpp for what each attribute carries).
//
// DEVIATION: upstream draws the sprites instanced, reading the elements from a data texture and
// the flares from a uniform array in its vertex stage; here the effect places the elements on
// the CPU and the visibility is applied in the fragment stage, with the same result.
//
// @variant shape: LF_PASS=0
// @variant occlusion: LF_PASS=1
// @variant sprite: LF_PASS=2
import vtquad;
#include "bindings.slang"

struct LensFlareUniforms
{
    float4 camera;   // occlusion: x near, y far, z 1 for an orthographic camera, w height / width
    float4 params;   // occlusion: x 1 when there is no previous measurement (it reads as 0);
                     // shape: y 1 for a gamma-encoded (sRGB) shape, decoded first;
                     // sprite: z the shape library's resolution
};
VT_DECLARE_QUAD_UNIFORMS(LensFlareUniforms, u)

// The shape pass draws the device's fullscreen triangle, which has no tangent.
struct LensFlareVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
#if LF_PASS != 0
    [[vk::location(3)]] float4 tangent : TANGENT;
#endif
};

struct LensFlareVarying
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float3 a : TEXCOORD1;
    nointerpolation float4 b : TEXCOORD2;
    nointerpolation float2 c : TEXCOORD3;
};

// One entry per stage for the three variants (the bundle finds entries in the source text).
[shader("vertex")]
LensFlareVarying lensFlareVertex(LensFlareVertexIn input)
{
    LensFlareVarying output;
    output.uv = input.uv0;
#if LF_PASS == 0
    output.position = float4(input.position, 1.0);
    output.a = float3(0.0, 0.0, 0.0);
    output.b = float4(0.0, 0.0, 0.0, 0.0);
#else
    output.position = float4(input.position.xy, 0.0, 1.0);
    output.a = input.normal;
    output.b = input.tangent;
#endif
    output.c = input.uv0;
    return output;
}

#if LF_PASS == 0

VT_DECLARE_QUAD_INPUT0(shapeImage)

float4 lensFlareShade(LensFlareVarying input)
{
    const float4 image = shapeImage.Sample(input.uv);
    const float3 rgb = clamp(image.rgb, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
    return float4(sqrt(u.params.y > 0.5 ? pow(rgb, float3(2.2, 2.2, 2.2)) : rgb), image.a);
}

#elif LF_PASS == 1

VT_DECLARE_QUAD_DEPTH_INPUT0(depthTexture)
VT_DECLARE_QUAD_TEXEL_INPUT1(previousVisibility)

// a: the source in normalized device coordinates (y up), the linear depth it is tested at;
// c: the radius of the disc in screen heights, the blend factor; b: x 1 on screen, 0 off screen
// (the visibility is kept), -1 behind the camera, y the flare's texel.
float4 lensFlareShade(LensFlareVarying input)
{
    const int index = int(input.b.y + 0.5);
    const float previous = u.params.x > 0.5 ? 0.0 : previousVisibility.Load(int3(index, 0, 0)).r;

    float visibility = previous;
    if (input.b.x > 0.5) {
        const float2 center = float2(input.a.x * 0.5 + 0.5, 0.5 - input.a.y * 0.5);
        const float radius = max(input.c.x, 0.0005);
        float visible = 0.0;
        for (int i = 0; i < 24; ++i) {
            const float t = (float(i) + 0.5) / 24.0;
            const float angle = float(i) * 2.39996323;
            const float2 offset = sqrt(t) * radius * float2(cos(angle) * u.camera.w, -sin(angle));
            const float raw = depthTexture.SampleLevel(center + offset, 0.0);
            const float depth = quadLinearDepth(raw, u.camera.x, u.camera.y, u.camera.z > 0.5);
            visible += depth > input.a.z ? 1.0 : 0.0;
        }
        visibility = visible / 24.0;
    } else if (input.b.x < -0.5) {
        visibility = 0.0;
    }

    const float diff = visibility - previous;
    const float stepSize = sign(diff) * max(abs(diff) * input.c.y, 1.0 / 255.0);
    const float result = abs(stepSize) >= abs(diff) ? visibility : previous + stepSize;
    return float4(result, 0.0, 0.0, 1.0);
}

#else

VT_DECLARE_QUAD_ARRAY_INPUT0(shapes)
VT_DECLARE_QUAD_TEXEL_INPUT1(visibilityTexture)

// a: the colour; b: x the layer of the shape, y the flare's texel.
float4 lensFlareShade(LensFlareVarying input)
{
    const float visibility = visibilityTexture.Load(int3(int(input.b.y + 0.5), 0, 0)).r;
    // The mip level of an isotropic trilinear filter, taken explicitly: an explicit level is
    // filtered without anisotropy on both backends, where Vulkan's sampler would otherwise
    // keep a long thin streak sharper than Metal's quad sampler (and upstream) does.
    const float2 texel = input.uv * u.params.z;
    const float2 dx = ddx(texel);
    const float2 dy = ddy(texel);
    const float lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8));
    const float4 shape = shapes.SampleLevel(float3(input.uv, floor(input.b.x + 0.5)), max(lod, 0.0));
    const float3 value = shape.rgb * shape.rgb * shape.a;
    return float4(value * input.a * visibility, 1.0);
}

#endif

[shader("fragment")]
float4 lensFlareFragment(LensFlareVarying input) : SV_Target0
{
    return lensFlareShade(input);
}
