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

#elif VT_FEATURE_SKINNING
// GPU skinning path.
// 4-bone weighted blend against the matrix palette at buffer slot 6. The palette
// is relative to the mesh instance's node (see SkinInstance), so the model matrix
// is applied on top — the two cancel and vertices land in world space.
vertex RasterizerData VT_VERTEX_ENTRY(VertexData v [[stage_in]],
                                      constant SceneData &scene [[buffer(1)]],
                                      constant ModelData &model [[buffer(2)]],
                                      constant MaterialData &material [[buffer(3)]],
                                      constant float4x4 *palette [[buffer(6)]]
#if VT_FEATURE_MORPHS
                                    , constant float4 *morphDeltas [[buffer(9)]],
                                      constant MorphParams &morphParams [[buffer(10)]],
                                      uint vid [[vertex_id]]
#endif
                                      )
{
    RasterizerData rd;

    float3 localPos = v.position;
    float3 localNormal = v.normal;
#if VT_FEATURE_MORPHS
    // Morph deltas apply in bind space, before skinning (glTF semantics).
    applyMorph(localPos, localNormal, vid, morphDeltas, morphParams);
#endif

    // Matches upstream getSkinMatrix(): weighted sum of 4 bone matrices.
    const float4 w = v.blendWeights;
    const int4 j = int4(v.blendIndices);
    const float4x4 skinMatrix = w.x * palette[j.x] + w.y * palette[j.y] +
                                w.z * palette[j.z] + w.w * palette[j.w];

    float4 world = model.modelMatrix * (skinMatrix * float4(localPos, 1.0));
    float4 clip = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);

    rd.position = clip;
    rd.worldPos = world.xyz;

    // Skin the normal/tangent by the palette 3x3 (valid for uniform bone scale,
    // matching upstream), then apply the node's normal matrix.
    const float3x3 skinNormalMat = float3x3(skinMatrix[0].xyz,
                                             skinMatrix[1].xyz,
                                             skinMatrix[2].xyz);
    const float3 skinnedNormal = skinNormalMat * localNormal;
    rd.worldNormal = normalize((model.normalMatrix * float4(skinnedNormal, 0.0)).xyz) * model.normalSign;
    const float3 skinnedTangent = skinNormalMat * v.tangent.xyz;
    const float3 tangentWorld = normalize((model.normalMatrix * float4(skinnedTangent, 0.0)).xyz) * model.normalSign;
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
                                      constant MaterialData &material [[buffer(3)]]
#if VT_FEATURE_MORPHS
                                    , constant float4 *morphDeltas [[buffer(9)]],
                                      constant MorphParams &morphParams [[buffer(10)]],
                                      uint vid [[vertex_id]]
#endif
#if VT_FEATURE_DISPLACEMENT
                                    , texture2d<float> displacementTexture [[texture(0)]]
#endif
                                      )
{
    RasterizerData rd;
    float3 localPos = v.position;
    float3 localNormal = v.normal;
#if VT_FEATURE_MORPHS
    applyMorph(localPos, localNormal, vid, morphDeltas, morphParams);
#endif
#if VT_FEATURE_DISPLACEMENT
    // Vertex displacement along the local normal from a height map (vertex-stage
    // texture slot 0, explicit LOD as required in the vertex stage).
    // detailDisplacementParams: y = displacementScale, z = displacementBias.
    // DEVIATION: applied in the standard vertex path only (not the instanced /
    // dynamic-batch / skinned specializations).
    if (displacementTexture.get_width() > 0) {
        constexpr sampler displacementSampler(coord::normalized, filter::linear, address::repeat);
        const float height = displacementTexture.sample(displacementSampler, v.uv0, level(0)).r;
        localPos += localNormal * ((height - material.detailDisplacementParams.z)
            * material.detailDisplacementParams.y);
    }
#endif
    float4 world = model.modelMatrix * float4(localPos, 1.0);
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
    rd.worldNormal = normalize((model.normalMatrix * float4(localNormal, 0.0)).xyz) * model.normalSign;
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
