// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The gaussian splat render shader: one instance per splat, in the order the sort wrote,
// a quad (four vertices of a triangle strip) generated from the vertex id around the
// splat's projected centre and sized by its 2D covariance. The colour is GAMMA space and
// owes the target an output stage (gsplatPrepareOutput).
//
// Bindings, a splat draw's: the packed splats (10 words each: centre, RGBA8 colour, the
// covariance's upper triangle) at Metal buffer 7 / Vulkan set 6 binding 0, the order at 8 /
// binding 1, the spherical-harmonic coefficients (45 floats a splat) at 12 / binding 2, the
// parameters at 11 / binding 3.
#include "bindings.slang"

// The forward pass's tone mapping operators, without its lighting-block dispatch.
#define VT_TONEMAP_OPERATORS_ONLY
#include "forward/common-tonemap.slang"

struct GSplatParams
{
    float4x4 modelView;
    float4x4 projection;    // GL-style clip (z in [-w,w]); remapped below
    float4 viewport;        // width, height, 1/width, 1/height
    uint splatCount;
    uint shBands;           // 0 = SH0 only; 1-3 = view-dependent SH color
    uint cameraOrtho;       // 1: SH along the camera forward
    uint padding;
    float4 fogColor;        // linear rgb
    float4 fogParams;       // start, end, density, type (0 = none, 1 linear, 2 exp, 3 exp2)
    float4 outputParams;    // exposure, tone mapping mode, linear HDR target, unused
};

[[vk::binding(0, 6)]] StructuredBuffer<uint> splats : register(t7);
[[vk::binding(1, 6)]] StructuredBuffer<uint> drawOrder : register(t8);
[[vk::binding(2, 6)]] StructuredBuffer<float> shCoefficients : register(t12);
[[vk::binding(3, 6)]] ConstantBuffer<GSplatParams> params : register(b11);

struct GSplatVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
};

// The output stage for a gamma-space splat colour: a camera frame's scene pass is linear
// HDR with tone mapping left to compose, anything else is a gamma target that is tone
// mapped and encoded here, as the forward tail does. `depth` is the view depth, as the
// forward fog uses. Exposure scales before the curve and is skipped with it for NONE.
float3 gsplatPrepareOutput(float3 gammaColor, float depth)
{
    const bool linearTarget = params.outputParams.z > 0.5;
    const uint fogType = uint(params.fogParams.w + 0.5);
    const bool fog = fogType != 0u;
    // TONEMAP_NONE (6) skips tone mapping, and a linear target leaves it to compose.
    const bool tonemap = !linearTarget && params.outputParams.y < 5.5;

    float3 color = gammaColor;
    if (tonemap || linearTarget || fog) {
        color = pow(max(color, float3(0.0)), float3(2.2));
    }
    if (fog) {
        const float density = max(params.fogParams.z, 0.0);
        float fogFactor;
        if (fogType == 1u) {
            const float fogEnd = max(params.fogParams.y, params.fogParams.x + 1e-3);
            fogFactor = (fogEnd - depth) / (fogEnd - params.fogParams.x);
        } else if (fogType == 2u) {
            fogFactor = exp(-depth * density);
        } else {
            const float d = depth * density;
            fogFactor = exp(-d * d);
        }
        color = lerp(params.fogColor.xyz, color, saturate(fogFactor));
    }
    if (tonemap) {
        color = toneMapExposed(color, max(params.outputParams.x, 0.0), int(params.outputParams.y + 0.5));
    }
    if (tonemap || (!linearTarget && fog)) {
        color = pow(max(color, float3(0.0)) + 0.0000001, float3(1.0 / 2.2));
    }
    return color;
}

float3 loadFloat3(uint base)
{
    return float3(asfloat(splats[base]), asfloat(splats[base + 1u]), asfloat(splats[base + 2u]));
}

float4 unpackColor(uint c)
{
    return float4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float((c >> 24) & 255u)) / 255.0;
}

float3 shCoefficient(uint base, uint coefficient)
{
    const uint offset = base + coefficient * 3u;
    return float3(shCoefficients[offset], shCoefficients[offset + 1u], shCoefficients[offset + 2u]);
}

// The view-dependent colour of bands 1-3, added to the DC term in gamma space. The
// coefficients are interleaved per coefficient (rgb of coefficient k at base + 3k).
float3 evaluateSH(uint base, uint bands, float3 d)
{
    const float x = d.x, y = d.y, z = d.z;
    float3 result = 0.4886025119029199 * (-shCoefficient(base, 0) * y + shCoefficient(base, 1) * z
        - shCoefficient(base, 2) * x);
    if (bands > 1u) {
        const float xx = x * x, yy = y * y, zz = z * z;
        result += shCoefficient(base, 3) * (1.0925484305920792 * x * y)
            + shCoefficient(base, 4) * (-1.0925484305920792 * y * z)
            + shCoefficient(base, 5) * (0.31539156525252005 * (2.0 * zz - xx - yy))
            + shCoefficient(base, 6) * (-1.0925484305920792 * x * z)
            + shCoefficient(base, 7) * (0.5462742152960396 * (xx - yy));
    }
    if (bands > 2u) {
        const float xx = x * x, yy = y * y, zz = z * z;
        result += shCoefficient(base, 8) * (-0.5900435899266435 * y * (3.0 * xx - yy))
            + shCoefficient(base, 9) * (2.890611442640554 * x * y * z)
            + shCoefficient(base, 10) * (-0.4570457994644658 * y * (4.0 * zz - xx - yy))
            + shCoefficient(base, 11) * (0.3731763325901154 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy))
            + shCoefficient(base, 12) * (-0.4570457994644658 * x * (4.0 * zz - xx - yy))
            + shCoefficient(base, 13) * (1.445305721320277 * z * (xx - yy))
            + shCoefficient(base, 14) * (-0.5900435899266435 * x * (xx - 3.0 * yy));
    }
    return result;
}

[shader("vertex")]
GSplatVaryings gsplatVS(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID)
{
    GSplatVaryings o;
    o.position = float4(0.0, 0.0, 2.0, 1.0);  // clipped
    o.uv = float2(0.0);
    o.color = float4(0.0);

    const uint splatIndex = drawOrder[iid];
    if (splatIndex >= params.splatCount) {
        return o;
    }
    const uint base = splatIndex * 10u;
    const float3 center = loadFloat3(base);
    const float3 covA = loadFloat3(base + 4u);   // Sigma00, Sigma01, Sigma02
    const float3 covB = loadFloat3(base + 7u);   // Sigma11, Sigma12, Sigma22

    const float4 view = mul(float4(center, 1.0), params.modelView);
    float4 clip = mul(view, params.projection);
    if (clip.w <= 0.0) {
        return o;
    }

    // The 3D covariance in splat model space.
    const float3x3 covariance = float3x3(float3(covA.x, covA.y, covA.z),
                                         float3(covA.y, covB.x, covB.y),
                                         float3(covA.z, covB.y, covB.z));

    // The perspective Jacobian at the splat centre. The focal length in pixels is taken PER
    // AXIS: one focal from the width for both axes squashes every splat when the viewport's
    // pixel aspect differs from the projection's (a manual aspect ratio, a side-by-side
    // stereo target). Signs are kept: the footprint is added to the centre in clip space, so
    // a projection that flips an axis must flip the footprint with it.
    //
    // An orthographic projection has no perspective divide, so its Jacobian is the focal
    // length alone: the same expression evaluated at view position (0, 0, 1).
    const float2 focal = params.viewport.xy * float2(params.projection[0][0], params.projection[1][1]);
    const float3 v = params.cameraOrtho != 0u ? float3(0.0, 0.0, 1.0) : view.xyz;
    const float2 j1 = focal / v.z;
    const float2 j2 = -j1 / v.z * v.xy;
    const float3x3 jacobian = float3x3(float3(j1.x, 0.0, j2.x),
                                       float3(0.0, j1.y, j2.y),
                                       float3(0.0, 0.0, 0.0));

    // T = W * J and cov = T^T * Sigma * T, written for the row-major reading.
    const float3x3 w = transpose((float3x3)params.modelView);
    const float3x3 t = mul(jacobian, w);
    const float3x3 cov = mul(t, mul(covariance, transpose(t)));

    // The 2D covariance's eigen decomposition, with a 0.3 px low-pass dilation.
    const float diagonal1 = cov[0][0] + 0.3;
    const float offDiagonal = cov[0][1];
    const float diagonal2 = cov[1][1] + 0.3;
    const float mid = 0.5 * (diagonal1 + diagonal2);
    const float radius = length(float2((diagonal1 - diagonal2) * 0.5, offDiagonal));
    const float lambda1 = mid + radius;
    const float lambda2 = max(mid - radius, 0.1);

    // The kernel is limited to the smaller viewport dimension. Without it the perspective
    // Jacobian, which divides by view.z, blows the footprint up without bound as a splat
    // centre approaches the camera; clamping clip.z below keeps such a splat instead of
    // clipping its whole quad away, so this clamp is what stops it covering the screen.
    const float vmin = min(1024.0, min(params.viewport.x, params.viewport.y));
    const float l1 = 2.0 * min(sqrt(2.0 * lambda1), vmin);
    const float l2 = 2.0 * min(sqrt(2.0 * lambda2), vmin);

    // Cull sub-pixel gaussians.
    if (max(l1, l2) < 0.5) {
        return o;
    }

    // Cull against the frustum x/y planes.
    const float2 c = clip.w * params.viewport.zw;
    if (any(abs(clip.xy) - float2(max(l1, l2)) * c > clip.ww)) {
        return o;
    }

    // The epsilon keeps a circular footprint (offDiagonal 0, lambda1 == diagonal1), whose
    // eigenvector is the zero vector, from normalizing to NaN; it then takes the x axis.
    const float2 axis = normalize(float2(offDiagonal, lambda1 - diagonal1) + float2(1e-8, 0.0));
    const float2 cornerUV[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
    const float2 uv = cornerUV[vid];
    clip.xy += (uv.x * l1 * axis + uv.y * l2 * float2(axis.y, -axis.x)) * c;

    // GL-style projection (NDC z in [-1,1]) to the [0,1] depth both backends use, then the
    // splat is kept off the near and far planes. A splat is a footprint built around ONE
    // projected centre, so the whole quad shares that centre's depth: unclamped, a centre
    // crossing the near plane clips the entire splat away while its footprint still covers
    // visible pixels. A clamped splat depth-tests as though it sat on the plane, which only
    // affects its occlusion against opaque geometry (splats blend in sorted order).
    // clip.w > 0 here, the behind-camera case having returned.
    clip.z = clamp(0.5 * (clip.z + clip.w), 0.0, clip.w);

    const float4 color = unpackColor(splats[base + 3u]);
    float3 displayColor = color.rgb;
    if (params.shBands > 0u) {
        // The model-space view direction. An orthographic camera's rays all run along its
        // forward, (0, 0, -1) in view space, not from its position to the splat.
        const float3 viewDir = params.cameraOrtho != 0u ? float3(0.0, 0.0, -1.0) : view.xyz;
        const float3 direction = normalize(mul(viewDir, transpose((float3x3)params.modelView)));
        displayColor += evaluateSH(splatIndex * 45u, params.shBands, direction);
    }

    o.position = clip;
    o.uv = uv;
    // Fogged at the view depth, -view.z, the depth the forward fog uses: not clip.w, which
    // is 1 for every splat under an orthographic projection.
    o.color = float4(gsplatPrepareOutput(max(displayColor, float3(0.0)), -view.z), color.a);
    return o;
}

[shader("fragment")]
float4 gsplatFS(GSplatVaryings input) : SV_Target0
{
    const float q = dot(input.uv, input.uv);
    if (q > 1.0) {
        discard;
    }
    // A normalized exponential falloff: 1 at the centre, 0 at the edge.
    const float e4 = 0.018315638889;
    const float alpha = ((exp(-4.0 * q) - e4) / (1.0 - e4)) * input.color.a;
    return float4(input.color.rgb * alpha, alpha);
}
