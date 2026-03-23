// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#if VT_FEATURE_INSTANCING
// hardware instancing path.
// Per-instance model matrix and diffuse color are delivered via [[stage_in]] vertex attributes
// (instance_line1..4 + instanceColor) from vertex descriptor layout(5) with perInstance step function.
// Normal matrix is derived from the upper-left 3x3 of the model matrix
// (matches upstream getNormalMatrix() for instancing — valid for uniform scale).
vertex RasterizerData VT_VERTEX_ENTRY(VertexData v [[stage_in]],
                                      constant SceneData &scene [[buffer(1)]],
                                      constant ModelData &model [[buffer(2)]],
                                      constant MaterialData &material [[buffer(3)]])
{
    (void)model;   // per-draw ModelData unused — instance attributes provide per-instance transform
    RasterizerData rd;

    // Reconstruct per-instance model matrix from 4 column vectors (Upstream: instance_line1..4).
    const float4x4 instanceModelMatrix = float4x4(v.instance_line1, v.instance_line2,
                                                    v.instance_line3, v.instance_line4);
    float4 world = instanceModelMatrix * float4(v.position, 1.0);
    float4 clip = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);

    rd.position = clip;
    rd.worldPos = world.xyz;

    // Derive normal matrix
    // from upper-left 3x3 of the model matrix. Valid for uniform-scale instances.
    const float3x3 normalMat = float3x3(instanceModelMatrix[0].xyz,
                                         instanceModelMatrix[1].xyz,
                                         instanceModelMatrix[2].xyz);
    rd.worldNormal = normalize(normalMat * v.normal);
    const float3 tangentWorld = normalize(normalMat * v.tangent.xyz);
    rd.worldTangent = float4(tangentWorld, v.tangent.w);
    rd.uv0 = v.uv0;
    rd.uv1 = v.uv1;

    // Pass per-instance color (sRGB) to the fragment shader.
    rd.instanceColor = v.instanceColor;

#if VT_FEATURE_POINT_SIZE
    rd.pointSize = 3.0;
#endif

    return rd;
}

#elif VT_FEATURE_DYNAMIC_BATCH
// dynamic batching path.
// Per-vertex boneIndex selects a world transform from the matrix palette buffer.
// Uses Metal buffer (slot 6) for bone data.
vertex RasterizerData VT_VERTEX_ENTRY(VertexData v [[stage_in]],
                                      constant SceneData &scene [[buffer(1)]],
                                      constant ModelData &model [[buffer(2)]],
                                      constant MaterialData &material [[buffer(3)]],
                                      constant float4x4 *palette [[buffer(6)]])
{
    (void)model;  // Identity — per-instance transform comes from palette
    RasterizerData rd;

    // Look up world transform from palette using per-vertex bone index.
    const int boneIdx = int(v.boneIndex);
    const float4x4 boneMatrix = palette[boneIdx];

    float4 world = boneMatrix * float4(v.position, 1.0);
    float4 clip = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);

    rd.position = clip;
    rd.worldPos = world.xyz;

    // Normal matrix from upper-left 3x3 (valid for uniform-scale transforms).
    const float3x3 normalMat = float3x3(boneMatrix[0].xyz,
                                         boneMatrix[1].xyz,
                                         boneMatrix[2].xyz);
    rd.worldNormal = normalize(normalMat * v.normal);
    const float3 tangentWorld = normalize(normalMat * v.tangent.xyz);
    rd.worldTangent = float4(tangentWorld, v.tangent.w);
    rd.uv0 = v.uv0;
    rd.uv1 = v.uv1;

#if VT_FEATURE_POINT_SIZE
    rd.pointSize = 3.0;
#endif

    return rd;
}

#else

vertex RasterizerData VT_VERTEX_ENTRY(VertexData v [[stage_in]],
                                      constant SceneData &scene [[buffer(1)]],
                                      constant ModelData &model [[buffer(2)]],
                                      constant MaterialData &material [[buffer(3)]])
{
    RasterizerData rd;
    float4 world = model.modelMatrix * float4(v.position, 1.0);
    float4 clip = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);

#if VT_FEATURE_SKYBOX
    // Force skybox to far Z.
    // Subtract a tiny fudge factor to ensure floating point errors don't
    // push pixels beyond far Z. See: https://community.khronos.org/t/skybox-problem/61857
    clip.z = clip.w - 1.0e-7;
#endif

    rd.position = clip;
    rd.worldPos = world.xyz;

#if VT_FEATURE_SKYBOX
    // Repurpose worldNormal to carry the pre-transform vertex position for
    // skybox view direction. world.xyz has cameraPosition baked in (~10M meters
    // at globe scale); subtracting it in the fragment shader causes catastrophic
    // float32 cancellation. Using the raw vertex position avoids this.
    rd.worldNormal = v.position;
#else
    rd.worldNormal = normalize((model.normalMatrix * float4(v.normal, 0.0)).xyz) * model.normalSign;
#endif
    const float3 tangentWorld = normalize((model.normalMatrix * float4(v.tangent.xyz, 0.0)).xyz) * model.normalSign;
    rd.worldTangent = float4(tangentWorld, v.tangent.w);
    rd.uv0 = v.uv0;
    rd.uv1 = v.uv1;

#if VT_FEATURE_VERTEX_COLORS
    // Pass vertex color to fragment shader. Apply sRGB → linear conversion
    // in the vertex shader (once per vertex) following upstream convention.
    rd.vertexColor = float4(pow(max(v.color.rgb, float3(0.0)), float3(2.2)), v.color.a);
#endif

#if VT_FEATURE_POINT_SIZE
    rd.pointSize = 3.0;
#endif

    return rd;
}

#endif
