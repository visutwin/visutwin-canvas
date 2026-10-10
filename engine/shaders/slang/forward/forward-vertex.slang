// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The forward program's vertex entries, one per vertex layout. Vulkan builds a module of each
// and its pipeline picks one per draw (VulkanRenderPipeline::create); Metal picks the entry
// from the variant (ProgramLibrary). They share the transforms below and differ only in what
// they read. Every entry writes every varying, so one fragment stage serves them all.
//
// Clip z is remapped from the projection's GL [-1, 1] to [0, 1] in every entry, as the depth
// convention requires. The model transform and view-projection come from vtdraw (Metal buffers
// 1 / 2, Vulkan push constants); the normal matrix too (uploaded on Metal, built on Vulkan).

// ---- Vertex-stage resources ----
// The displacement height map (a separate image through the material sampler; Metal vertex
// texture 41), the matrix palette of a skinned or dynamically batched draw (Metal vertex buffer
// 6, Vulkan set 4 binding 0) and a morphed draw's target deltas and weights (Metal buffers 9 /
// 10, Vulkan set 4 bindings 1 / 2).
VT_DECLARE_displacementMap
[[vk::binding(0, 4)]] StructuredBuffer<float4x4> palette : register(t6, space1);
[[vk::binding(1, 4)]] StructuredBuffer<float4> morphDeltas : register(t9, space1);
struct MorphParams
{
    uint4 counts;          // x active targets (<= 8), y vertices per target
    uint4 indices0;        // target indices into the delta buffer
    uint4 indices1;
    float4 weights0;       // matching blend weights
    float4 weights1;
};
[[vk::binding(2, 4)]] ConstantBuffer<MorphParams> morphParams : register(b10);

// ---- Inputs, by vertex layout (Vulkan location n is Metal attribute n). ----
struct ForwardVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
};

struct ForwardColorVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
    [[vk::location(5)]] float4 color : COLOR;
};

struct ForwardPointVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(5)]] float4 color : COLOR;
};

struct ForwardDynamicBatchVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
    [[vk::location(5)]] float paletteIndex : BLENDINDICES1;
};

struct ForwardSkinnedVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
    [[vk::location(11)]] float4 blendWeights : BLENDWEIGHT;
    [[vk::location(12)]] float4 blendIndices : BLENDINDICES;
};

// The instance matrix's columns arrive as four attributes. The two backends number them
// differently (Metal 6-9 with the colour at 10, Vulkan 5-8), and Vulkan's instance layout
// describes no colour: the pipeline never picks the colour entry there, which is still
// compiled for both so the entry list is one list (location 9 on Vulkan, unused).
#if defined(VT_TARGET_METAL)
#define VT_INSTANCE_LOCATION0 6
#define VT_INSTANCE_LOCATION1 7
#define VT_INSTANCE_LOCATION2 8
#define VT_INSTANCE_LOCATION3 9
#define VT_INSTANCE_COLOR_LOCATION 10
#else
#define VT_INSTANCE_LOCATION0 5
#define VT_INSTANCE_LOCATION1 6
#define VT_INSTANCE_LOCATION2 7
#define VT_INSTANCE_LOCATION3 8
#define VT_INSTANCE_COLOR_LOCATION 9
#endif
struct ForwardInstancedVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
    [[vk::location(VT_INSTANCE_LOCATION0)]] float4 instance0 : INSTANCE0;
    [[vk::location(VT_INSTANCE_LOCATION1)]] float4 instance1 : INSTANCE1;
    [[vk::location(VT_INSTANCE_LOCATION2)]] float4 instance2 : INSTANCE2;
    [[vk::location(VT_INSTANCE_LOCATION3)]] float4 instance3 : INSTANCE3;
};
struct ForwardInstancedColorVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
    [[vk::location(VT_INSTANCE_LOCATION0)]] float4 instance0 : INSTANCE0;
    [[vk::location(VT_INSTANCE_LOCATION1)]] float4 instance1 : INSTANCE1;
    [[vk::location(VT_INSTANCE_LOCATION2)]] float4 instance2 : INSTANCE2;
    [[vk::location(VT_INSTANCE_LOCATION3)]] float4 instance3 : INSTANCE3;
    [[vk::location(VT_INSTANCE_COLOR_LOCATION)]] float4 instanceColor : INSTANCECOLOR;
};

struct ForwardPointVarying
{
    ForwardVarying v;
    float pointSize : SV_PointSize;
};

// ---- Shared transforms ----

// Clip position of a world point: the view-projection, then GL [-1, 1] depth to [0, 1].
float4 forwardClip(float4 world)
{
    float4 clip = mul(world, drawViewProjection());
    clip.z = 0.5 * (clip.z + clip.w);
    return clip;
}

float3 forwardModelScale(float4x4 model)
{
    return float3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
}

// Linear vertex colour, decoded from gamma once per vertex when the material says it is
// encoded (vertexColorGamma).
float4 forwardVertexColor(float4 color)
{
    return vtFeatureEnabled(VT_FEATURE_VERTEX_COLOR_GAMMA_BIT)
        ? float4(pow(max(color.rgb, float3(0.0)), float3(2.2)), color.a) : color;
}

// Everything after the world position: clip, depth, normal frame, UVs and the defaults.
ForwardVarying forwardFinish(float4 world, float4 clip, float3x3 normalMatrix, float3 normal, float4 tangent,
    float2 uv0, float2 uv1, float4x4 model)
{
    ForwardVarying o;
    o.position = clip;
    o.worldPos = world.xyz;
    o.uv0 = uv0;
    o.uv1 = uv1;
    // clip.w is the view depth under a perspective projection: the cascade pick and fog.
    o.viewDepth = clip.w;
    o.color = float4(1.0, 1.0, 1.0, 1.0);
#if defined(VT_SHADOW_PROGRAM)
    // A shadow fragment reads the position, the world position and the UVs alone; constants
    // here let the compiler drop the normal frame (and a skinned draw's normal blend), which
    // a depth-only pass would otherwise pay for on every vertex.
    o.worldNormal = float3(0.0, 0.0, 1.0);
    o.worldTangent = float4(1.0, 0.0, 0.0, 1.0);
    o.modelScale = float3(1.0, 1.0, 1.0);
    o.instanceColor = float4(1.0, 1.0, 1.0, 1.0);
#else
    o.worldNormal = normalize(mul(normal, normalMatrix));
    o.worldTangent = float4(normalize(mul(tangent.xyz, normalMatrix)), tangent.w);
    o.modelScale = forwardModelScale(model);
    o.instanceColor = material.baseColor;
#endif
    return o;
}

// Morph deltas apply in bind space, before skinning: up to eight targets, two float4s per
// vertex per target (position, normal).
void applyMorph(inout float3 position, inout float3 normal, uint vertexIndex)
{
    for (uint k = 0u; k < morphParams.counts.x; ++k) {
        const uint target = k < 4u ? morphParams.indices0[k] : morphParams.indices1[k - 4u];
        const float weight = k < 4u ? morphParams.weights0[k] : morphParams.weights1[k - 4u];
        const uint base = (target * morphParams.counts.y + vertexIndex) * 2u;
        position += weight * morphDeltas[base].xyz;
        normal += weight * morphDeltas[base + 1u].xyz;
    }
}

// The 4-bone weighted blend of the palette, relative to the mesh instance's node (the model
// matrix applies on top).
float4x4 skinMatrix(float4 weights, float4 indices)
{
    const uint4 joints = uint4(indices);
    return weights.x * palette[joints.x] + weights.y * palette[joints.y]
         + weights.z * palette[joints.z] + weights.w * palette[joints.w];
}

#include "forward/forward-vertex-local.slang"

// ---- Entries ----

[shader("vertex")]
ForwardVarying forwardVertex(ForwardVertexIn input)
{
    float3 localPos = input.position;
    // Vertex displacement along the local normal from a height map; explicit LOD (no
    // derivatives in the vertex stage). detailDisplacementParams: y scale, z bias.
    if (vtFeatureEnabled(VT_FEATURE_DISPLACEMENT_BIT)) {
        const float height = displacementMap.SampleLevel(materialSampler, input.uv0, 0.0).r;
        localPos += input.normal * ((height - material.detailDisplacementParams.z) *
            material.detailDisplacementParams.y);
    }
    float3 normal = input.normal;
    float4 tangent = input.tangent;
    forwardLocalVertex(localPos, normal, tangent, input.uv0, input.uv1);
    const float4x4 model = drawModel();
    const float4 world = mul(float4(localPos, 1.0), model);
    float4 clip = forwardClip(world);
    if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BAKE_BIT)) {
        // Lightmap bake: rasterise the mesh across its own UV1 unwrap; the world position
        // and normal are untouched, so the fragment stage lights the real surface point.
        clip = float4(input.uv1.x * 2.0 - 1.0, 1.0 - input.uv1.y * 2.0, 0.0, 1.0);
    }
    if (vtFeatureEnabled(VT_FEATURE_SCREEN_SPACE_BIT)) {
        // Screen-space UI: the element's world transform already carries it to clip space.
        clip = float4(world.x, world.y, 0.5, 1.0);
    }
    return forwardFinish(world, clip, drawNormalMatrix(), normal, tangent, input.uv0, input.uv1, model);
}

[shader("vertex")]
ForwardVarying forwardSkyVertex(ForwardVertexIn input)
{
    const float4x4 model = drawModel();
    const float4 world = mul(float4(input.position, 1.0), model);
    float4 clip = forwardClip(world);
    // On the far plane, a hair inside it so rounding cannot push the sky past it. A dome
    // or box sky that writes depth keeps its real depth instead, so the depth effects
    // (depth of field, fog) see its projected floor where it is.
    // DEVIATION: upstream keeps that sky on the far plane too and writes its linear depth
    // into a separate scene-depth output; here geometry beyond the sky mesh is hidden by it.
    if (!vtFeatureEnabled(VT_FEATURE_SKY_DEPTH_BIT)) {
        clip.z = clip.w - 1.0e-7;
    }
    ForwardVarying o = forwardFinish(world, clip, drawNormalMatrix(), input.normal, input.tangent, input.uv0,
        input.uv1, model);
    // The view ray: the pre-transform vertex position, carried in the normal. The world
    // position minus the camera would cancel catastrophically at globe scale (both ~1e7 m).
    o.worldNormal = input.position;
    return o;
}

[shader("vertex")]
ForwardVarying forwardColorVertex(ForwardColorVertexIn input)
{
    float3 position = input.position;
    float3 normal = input.normal;
    float4 tangent = input.tangent;
    forwardLocalVertex(position, normal, tangent, input.uv0, input.uv1);
    const float4x4 model = drawModel();
    const float4 world = mul(float4(position, 1.0), model);
    ForwardVarying o = forwardFinish(world, forwardClip(world), drawNormalMatrix(), normal, tangent,
        input.uv0, input.uv1, model);
    o.color = forwardVertexColor(input.color);
    return o;
}

[shader("vertex")]
ForwardPointVarying forwardPointVertex(ForwardPointVertexIn input)
{
    const float4x4 model = drawModel();
    const float4 world = mul(float4(input.position, 1.0), model);
    const float4 clip = forwardClip(world);
    ForwardPointVarying o;
    o.v.position = clip;
    o.v.worldPos = world.xyz;
    o.v.worldNormal = float3(0.0);   // the fragment stage's unlit sentinel
    o.v.worldTangent = float4(1.0, 0.0, 0.0, 1.0);
    o.v.uv0 = float2(0.0);
    o.v.uv1 = float2(0.0);
    o.v.viewDepth = clip.w;
    o.v.color = forwardVertexColor(input.color);
    o.v.modelScale = forwardModelScale(model);
    o.v.instanceColor = material.baseColor;
    // DEVIATION between the backends, kept: Metal draws a point 3 pixels across, Vulkan 1.
    __target_switch
    {
    case metal:
        o.pointSize = 3.0;
    default:
        o.pointSize = 1.0;
    }
    return o;
}

[shader("vertex")]
ForwardVarying forwardDynamicBatchVertex(ForwardDynamicBatchVertexIn input)
{
    // The world transform comes from the palette, by the vertex's own index; the bare 3x3
    // carries the normal (uniform-scale batches).
    const float4x4 model = palette[uint(input.paletteIndex)];
    const float4 world = mul(float4(input.position, 1.0), model);
    const float3x3 normalMatrix = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    ForwardVarying o = forwardFinish(world, forwardClip(world), normalMatrix, input.normal, input.tangent,
        input.uv0, input.uv1, drawModel());
    return o;
}

ForwardVarying forwardSkinned(float3 position, float3 normal, ForwardSkinnedVertexIn input)
{
    float4 tangent = input.tangent;
    forwardLocalVertex(position, normal, tangent, input.uv0, input.uv1);
    const float4x4 skin = skinMatrix(input.blendWeights, input.blendIndices);
    const float4x4 model = drawModel();
    const float4 world = mul(mul(float4(position, 1.0), skin), model);
    // The skin's bare 3x3 first (uniform bone scale), then the node's normal matrix.
    const float3x3 skin3 = float3x3(skin[0].xyz, skin[1].xyz, skin[2].xyz);
    const float3x3 normalMatrix = drawNormalMatrix();
    ForwardVarying o = forwardFinish(world, forwardClip(world), normalMatrix, mul(normal, skin3),
        float4(mul(tangent.xyz, skin3), tangent.w), input.uv0, input.uv1, model);
    return o;
}

[shader("vertex")]
ForwardVarying forwardSkinnedVertex(ForwardSkinnedVertexIn input)
{
    return forwardSkinned(input.position, input.normal, input);
}

[shader("vertex")]
ForwardVarying forwardSkinnedMorphedVertex(ForwardSkinnedVertexIn input, uint vertexIndex : SV_VulkanVertexID)
{
    float3 position = input.position;
    float3 normal = input.normal;
    applyMorph(position, normal, vertexIndex);
    return forwardSkinned(position, normal, input);
}

[shader("vertex")]
ForwardVarying forwardMorphedVertex(ForwardVertexIn input, uint vertexIndex : SV_VulkanVertexID)
{
    float3 position = input.position;
    float3 normal = input.normal;
    applyMorph(position, normal, vertexIndex);
    float4 tangent = input.tangent;
    forwardLocalVertex(position, normal, tangent, input.uv0, input.uv1);
    const float4x4 model = drawModel();
    const float4 world = mul(float4(position, 1.0), model);
    return forwardFinish(world, forwardClip(world), drawNormalMatrix(), normal, tangent, input.uv0,
        input.uv1, model);
}

ForwardVarying forwardInstanced(ForwardVertexIn input, float4x4 instance)
{
    float3 position = input.position;
    float3 normal = input.normal;
    float4 tangent = input.tangent;
    forwardLocalVertex(position, normal, tangent, input.uv0, input.uv1);
    // Each instance is placed in its NODE's space: model * instance.
    const float4x4 model = mul(instance, drawModel());
    const float4 world = mul(float4(position, 1.0), model);
    // The bare 3x3 carries the normal (uniform-scale instances).
    const float3x3 normalMatrix = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    return forwardFinish(world, forwardClip(world), normalMatrix, normal, tangent, input.uv0,
        input.uv1, drawModel());
}

ForwardVertexIn forwardVertexOf(ForwardInstancedVertexIn input)
{
    ForwardVertexIn v;
    v.position = input.position;
    v.normal = input.normal;
    v.uv0 = input.uv0;
    v.tangent = input.tangent;
    v.uv1 = input.uv1;
    return v;
}

[shader("vertex")]
ForwardVarying forwardInstancedVertex(ForwardInstancedVertexIn input)
{
    return forwardInstanced(forwardVertexOf(input),
        float4x4(input.instance0, input.instance1, input.instance2, input.instance3));
}

[shader("vertex")]
ForwardVarying forwardInstancedColorVertex(ForwardInstancedColorVertexIn input)
{
    ForwardVertexIn v;
    v.position = input.position;
    v.normal = input.normal;
    v.uv0 = input.uv0;
    v.tangent = input.tangent;
    v.uv1 = input.uv1;
    ForwardVarying o = forwardInstanced(v, float4x4(input.instance0, input.instance1, input.instance2, input.instance3));
    o.instanceColor = input.instanceColor;
    return o;
}
