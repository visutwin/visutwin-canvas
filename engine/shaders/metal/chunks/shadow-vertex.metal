// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#if VT_FEATURE_INSTANCING
// Shadow pass instancing: transform via per-instance model matrix.
// Per-instance data delivered via [[stage_in]] vertex attributes from vertex descriptor layout(5).
vertex RasterizerData VT_VERTEX_ENTRY(VertexData v [[stage_in]],
                                      constant SceneData &scene [[buffer(1)]],
                                      constant ModelData &model [[buffer(2)]],
                                      constant MaterialData &material [[buffer(3)]])
{
    (void)material;
    (void)model;
    RasterizerData rd;
    const float4x4 instanceModelMatrix = float4x4(v.instance_line1, v.instance_line2,
                                                    v.instance_line3, v.instance_line4);
    const float4 world = instanceModelMatrix * float4(v.position, 1.0);
    float4 clip = scene.projViewMatrix * world;
    // DEVIATION: OpenGL NDC z range is [-1,1]; Metal requires [0,1].
    clip.z = 0.5 * (clip.z + clip.w);
    rd.position = clip;
    rd.worldPos = world.xyz;
    const float3x3 normalMat = float3x3(instanceModelMatrix[0].xyz,
                                         instanceModelMatrix[1].xyz,
                                         instanceModelMatrix[2].xyz);
    rd.worldNormal = normalMat * v.normal;
    rd.worldTangent = float4(0.0);
    rd.uv0 = v.uv0;
    rd.uv1 = v.uv1;
    rd.instanceColor = v.instanceColor;
#if VT_FEATURE_POINT_SIZE
    rd.pointSize = 3.0;
#endif
    return rd;
}

#elif VT_FEATURE_DYNAMIC_BATCH
// Shadow pass dynamic batching: transform via per-vertex bone index + matrix palette.
// Uses Metal buffer (slot 6) for bone data.
vertex RasterizerData VT_VERTEX_ENTRY(VertexData v [[stage_in]],
                                      constant SceneData &scene [[buffer(1)]],
                                      constant ModelData &model [[buffer(2)]],
                                      constant MaterialData &material [[buffer(3)]],
                                      constant float4x4 *palette [[buffer(6)]])
{
    (void)material;
    (void)model;
    RasterizerData rd;
    const int boneIdx = int(v.boneIndex);
    const float4x4 boneMatrix = palette[boneIdx];
    const float4 world = boneMatrix * float4(v.position, 1.0);
    float4 clip = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);
    rd.position = clip;
    rd.worldPos = world.xyz;
    const float3x3 normalMat = float3x3(boneMatrix[0].xyz,
                                         boneMatrix[1].xyz,
                                         boneMatrix[2].xyz);
    rd.worldNormal = normalMat * v.normal;
    rd.worldTangent = float4(0.0);
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
    (void)material;
    RasterizerData rd;
    const float4 world = model.modelMatrix * float4(v.position, 1.0);
    float4 clip = scene.projViewMatrix * world;
    // DEVIATION: OpenGL NDC z range is [-1,1]; Metal requires [0,1].
    // Apply the same conversion as the forward vertex shader.
    clip.z = 0.5 * (clip.z + clip.w);
    rd.position = clip;
    rd.worldPos = world.xyz;
    rd.worldNormal = (model.normalMatrix * float4(v.normal, 0.0)).xyz;
    rd.worldTangent = float4(0.0);
    rd.uv0 = v.uv0;
    rd.uv1 = v.uv1;
#if VT_FEATURE_POINT_SIZE
    rd.pointSize = 3.0;
#endif
    return rd;
}

#endif
