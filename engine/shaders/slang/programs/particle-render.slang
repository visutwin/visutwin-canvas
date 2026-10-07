// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The particle render shader: one instance per particle, its quad (four vertices of a
// triangle strip) or its mesh generated from the vertex id, with the particle read from
// the simulation's pool. The colour is linear and owes the target the forward pass's output
// stage: tone mapped with the scene's exposure and gamma-encoded, or left linear on a camera
// frame's HDR scene.
//
// Bindings, a particle draw's: the pool at Metal buffer 7 / Vulkan set 6 binding 0, the
// sort order at 8 / binding 1, a mesh emitter's vertices (14 floats each) at 9 / binding 2,
// the render parameters at 11 / binding 3; the colour and normal maps at the material's
// first two texture slots, the scene depth grab for softening at its scene slot.
#include "bindings.slang"

// The forward pass's tone mapping operators, without its lighting-block dispatch.
#define VT_TONEMAP_OPERATORS_ONLY
#include "forward/common-tonemap.slang"

struct Particle
{
    float4 posAge;        // xyz = position, w = age (<0 unborn, >lifetime dead)
    float4 velLifetime;   // xyz = velocity, w = lifetime
    float4 rotSeedSize;   // x = rotation (rad), y = rotSpeed (rad/s), z = unused, w = hidden
    float4 motion;        // xyz = velocity this step, w = per-life seed
};

struct ParticleRenderParams
{
    float4x4 modelView;
    float4x4 projection;
    float4x4 view;
    float4 animParams;    // tilesX, tilesY, numFrames, animSpeed
    float4 miscParams;    // intensity, particle count, hasColorMap, animIndex
    float4 motionParams;  // alignToMotion, stretch, screenSpace, viewport height / width
    float4 outputParams;  // exposure, tone mapping mode, linear HDR target, sorted
    float4 faceTangent;   // xyz, w = WORLD / EMITTER oriented quad
    float4 faceBinorm;    // xyz, w = mesh particles
    float4 wrapParams;    // xyz bounds, w = wrap
    float4 emitterPosition; // xyz, w = local space
    float4 softParams;    // softening, camera near, camera far, soft
    float4 lightCube[6];  // rgb; [0].w lit, [1].w half Lambert, [2].w normal map
    float4 colorLut[16];  // rgb + alpha over normalized life
    float4 scaleLut[16];  // x = size, y = size graph2, z = alpha graph2
};

[[vk::binding(0, 6)]] StructuredBuffer<Particle> particles : register(t7);
[[vk::binding(1, 6)]] StructuredBuffer<uint> drawOrder : register(t8);
[[vk::binding(2, 6)]] StructuredBuffer<float> meshVertices : register(t9);
[[vk::binding(3, 6)]] ConstantBuffer<ParticleRenderParams> params : register(b11);

// The colour and normal maps. Metal reads them through the linear clamped sampler (linear
// mips) at slot 7, Vulkan through each texture's own sampler.
#if defined(VT_TARGET_METAL)
[[vk::binding(0, 1)]] Texture2D colorMap : register(t0);
[[vk::binding(1, 1)]] Texture2D normalMap : register(t1);
[[vk::binding(12, 3)]] SamplerState linearClampSampler : register(s7);
float4 sampleColorMap(float2 uv) { return colorMap.Sample(linearClampSampler, uv); }
float4 sampleNormalMap(float2 uv) { return normalMap.Sample(linearClampSampler, uv); }
#else
[[vk::binding(0, 1)]] Sampler2D colorMap;
[[vk::binding(1, 1)]] Sampler2D normalMap;
float4 sampleColorMap(float2 uv) { return colorMap.Sample(uv); }
float4 sampleNormalMap(float2 uv) { return normalMap.Sample(uv); }
#endif
VT_DECLARE_sceneDepthGrab

struct ParticleVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float hasMap : TEXCOORD1;
    [[vk::location(3)]] nointerpolation float3 output : TEXCOORD2; // outputParams.xyz
    [[vk::location(4)]] float3 normal : NORMAL;     // world: a lit particle's normal (the TBN's N with a normal map)
    [[vk::location(5)]] float3 tangent : TANGENT;
    [[vk::location(6)]] float3 binormal : BINORMAL;
    [[vk::location(7)]] float viewDepth : TEXCOORD3; // linear depth, for softening
};

// The simulation's PCG hash, for the per-particle random points between a graph and its
// graph2.
uint particlePcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float particleRnd(uint key)
{
    return float(particlePcgHash(key) >> 8) * (1.0 / 16777216.0);
}

// A positive angle turns clockwise.
float2 particleRotate(float2 v, float c, float s)
{
    return float2(v.x * c + v.y * s, -v.x * s + v.y * c);
}

float glslMod(float x, float y)
{
    return x - y * floor(x / y);
}

[shader("vertex")]
ParticleVaryings particleVS(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID)
{
    ParticleVaryings o;
    o.position = float4(0.0, 0.0, 2.0, 1.0);  // clipped
    o.uv = float2(0.0);
    o.color = float4(0.0);
    o.hasMap = params.miscParams.z;
    o.output = params.outputParams.xyz;
    o.normal = float3(0.0, 0.0, 1.0);
    o.tangent = float3(1.0, 0.0, 0.0);
    o.binormal = float3(0.0, 1.0, 0.0);
    o.viewDepth = 0.0;

    if (iid >= uint(params.miscParams.y)) {
        return o;
    }
    // A sorted emitter draws its particles in the order the sort wrote.
    const uint particleIndex = params.outputParams.w > 0.5 ? drawOrder[iid] : iid;
    const Particle p = particles[particleIndex];
    const float age = p.posAge.w;
    const float lifetime = max(p.velLifetime.w, 1e-5);
    if (age <= 0.0 || age > lifetime || p.rotSeedSize.w > 0.5) {
        return o;   // unborn, dead or hidden
    }
    const float lifeT = saturate(age / lifetime);

    // The 16-sample LUTs, interpolated linearly.
    const float lutPos = lifeT * 15.0;
    const int lutIdx = int(lutPos);
    const int lutIdx2 = min(lutIdx + 1, 15);
    const float lutFrac = frac(lutPos);
    float4 color = lerp(params.colorLut[lutIdx], params.colorLut[lutIdx2], lutFrac);
    const float4 scaleSample = lerp(params.scaleLut[lutIdx], params.scaleLut[lutIdx2], lutFrac);
    // A random point between each graph and its graph2, per particle life.
    const uint seed = uint(p.motion.w);
    const float size = lerp(scaleSample.x, scaleSample.y, particleRnd(seed + 20u));
    color.a += (scaleSample.z - color.a) * particleRnd(seed + 21u);
    if (color.a <= 0.001 || size <= 0.0001) {
        return o;
    }

    // A world-space particle wraps into a box around the emitter.
    float3 particlePos = p.posAge.xyz;
    if (params.wrapParams.w > 0.5) {
        const float3 bounds = params.wrapParams.xyz;
        const float3 relative = particlePos - params.emitterPosition.xyz;
        particlePos = relative - bounds * floor(relative / bounds) - bounds * 0.5 + params.emitterPosition.xyz;
    }

    const float2 cornerUV[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
    const bool screenSpace = params.motionParams.z > 0.5;
    const bool useMesh = params.faceBinorm.w > 0.5;
    const bool customFace = params.faceTangent.w > 0.5;
    const float2 corner = useMesh ? float2(0.0) : cornerUV[min(vid, 3u)];

    float4 viewPos = mul(float4(particlePos, 1.0), params.modelView);
    const float3 viewVelocity = mul(float4(p.motion.xyz, 0.0), params.modelView).xyz;
    float2 velocityV = viewVelocity.xy;
    if (screenSpace) {
        // The offset x is scaled by height / width below, so the direction of motion is
        // measured in the same units.
        velocityV.x /= params.motionParams.w;
    }
    velocityV = length(velocityV) > 1e-6 ? normalize(velocityV) : velocityV;

    float angle = p.rotSeedSize.x + p.rotSeedSize.y * age;
    if (params.motionParams.x > 0.5) {
        angle = atan2(velocityV.x, velocityV.y);
    }
    const float ca = cos(angle);
    const float sa = sin(angle);
    const float2 offset = particleRotate(corner, ca, sa);

    // The camera's axes in world space (rows of the view's rotation).
    const float3 cameraRight = float3(params.view[0][0], params.view[1][0], params.view[2][0]);
    const float3 cameraUp = float3(params.view[0][1], params.view[1][1], params.view[2][1]);
    const float3 cameraBack = float3(params.view[0][2], params.view[1][2], params.view[2][2]);

    float2 meshUv = float2(0.0);
    float3 worldOffset = float3(0.0);     // unscaled, for a mesh or an oriented quad
    float4 clip;
    if (useMesh || customFace) {
        if (useMesh) {
            // The mesh's vertex, turned about z then x by the angle.
            const uint base = vid * 14u;
            float3 local = float3(meshVertices[base], meshVertices[base + 1u], meshVertices[base + 2u]);
            local.xy = particleRotate(local.xy, ca, sa);
            local.yz = particleRotate(local.yz, ca, sa);
            worldOffset = local;
            meshUv = float2(meshVertices[base + 6u], meshVertices[base + 7u]);
        } else {
            // The quad in the plane of the face vectors.
            worldOffset = params.faceTangent.xyz * offset.x + params.faceBinorm.xyz * offset.y;
        }
        const float3 viewOffset = mul(float4(worldOffset * size, 0.0), params.view).xyz;
        if (params.motionParams.y > 0.0 && dot(viewOffset.xy, viewOffset.xy) > 1e-12) {
            // Stretch, for any vertex: the ones trailing the motion are pulled back along it.
            const float3 previous = viewPos.xyz - viewVelocity * params.motionParams.y;
            const float interpolation = dot(-velocityV, normalize(viewOffset.xy)) * 0.5 + 0.5;
            viewPos.xyz = lerp(viewPos.xyz, previous, interpolation);
        }
        viewPos.xyz += viewOffset;
        clip = mul(viewPos, params.projection);
    } else {
        // A billboard in view space. In SCREEN SPACE the model matrix already lands in clip
        // space (the view and projection are identity) and the quad is sized in viewport
        // heights, its x scaled by height / width.
        if (params.motionParams.y > 0.0) {
            const float3 previous = viewPos.xyz - viewVelocity * params.motionParams.y;
            const float interpolation = dot(-velocityV, normalize(offset)) * 0.5 + 0.5;
            viewPos.xyz = lerp(viewPos.xyz, previous, interpolation);
        }
        float2 scaled = offset * size;
        if (screenSpace) {
            scaled.x *= params.motionParams.w;
            clip = float4(viewPos.xy + scaled, 0.0, 1.0);
        } else {
            viewPos.xy += scaled;
            clip = mul(viewPos, params.projection);
        }
        worldOffset = cameraRight * offset.x + cameraUp * offset.y;
    }
    // GL-style projection (NDC z in [-1,1]) to the [0,1] depth both backends use.
    clip.z = 0.5 * (clip.z + clip.w);
    o.viewDepth = -viewPos.z;

    // A lit particle: the normal bulges out from the quad's centre toward the vertex, and a
    // normal map turns the camera-aligned frame by the particle's angle.
    if (params.lightCube[0].w > 0.5) {
        o.normal = normalize(worldOffset + cameraBack);
        const float3 t = -cameraRight;
        const float3 b = -cameraUp;
        o.tangent = t * ca - b * sa;
        o.binormal = t * sa + b * ca;
        if (params.lightCube[2].w > 0.5) {
            o.normal = cameraBack;
        }
    }

    // The sprite-sheet frame from the particle's life (tile 0 = top-left). animIndex
    // selects WHICH animation in the sheet: each is numFrames tiles long and they run in
    // reading order, so a 4x4 sheet at 4 frames holds four of them.
    const float2 tiles = max(params.animParams.xy, float2(1.0));
    const float numFrames = max(params.animParams.z, 1.0);
    const float frame = floor(glslMod(lifeT * numFrames * max(params.animParams.w, 0.0001), numFrames))
        + params.miscParams.w * numFrames;
    const float2 frameOrigin = float2(glslMod(frame, tiles.x), floor(frame / tiles.x));
    const float2 tileUv = useMesh ? meshUv : float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    o.uv = (frameOrigin + tileUv) / tiles;

    o.position = clip;
    o.color = float4(color.rgb * params.miscParams.x, color.a);
    return o;
}

[shader("fragment")]
float4 particleFS(ParticleVaryings input) : SV_Target0
{
    float4 texel = float4(1.0);
    if (input.hasMap > 0.5) {
        texel = sampleColorMap(input.uv);
        // The colour map is sRGB-authored.
        texel.rgb = pow(max(texel.rgb, float3(0.0)), float3(2.2));
    } else {
        // A soft disc when no colour map is assigned.
        const float a = saturate(1.0 - length(frac(input.uv) * 2.0 - 1.0));
        texel.a = a * a;
    }
    float alpha = texel.a * input.color.a;
    float3 rgb = input.color.rgb * texel.rgb;

    // Soft particles: fade where the particle nears the scene behind it.
    if (params.softParams.w > 0.5) {
        const float near = params.softParams.y;
        const float far = params.softParams.z;
        const float raw = sceneDepthGrab.Load(int3(int2(input.position.xy), 0)).r;
        const float depth = (near * far) / (far - raw * (far - near));
        alpha *= saturate(abs(input.viewDepth - depth) * params.softParams.x);
    }

    // The light cube, by Lambert or half Lambert on the normal.
    if (params.lightCube[0].w > 0.5) {
        float3 normal = normalize(input.normal);
        if (params.lightCube[2].w > 0.5) {
            const float3 n = normalize(sampleNormalMap(input.uv).xyz * 2.0 - 1.0);
            normal = normalize(input.tangent * n.x + input.binormal * n.y + input.normal * n.z);
        }
        float3 negNormal;
        float3 posNormal;
        if (params.lightCube[1].w > 0.5) {
            negNormal = normal * 0.5 + 0.5;
            posNormal = -normal * 0.5 + 0.5;
            negNormal *= negNormal;
            posNormal *= posNormal;
        } else {
            negNormal = max(normal, float3(0.0));
            posNormal = max(-normal, float3(0.0));
        }
        rgb *= negNormal.x * params.lightCube[0].xyz + posNormal.x * params.lightCube[1].xyz +
               negNormal.y * params.lightCube[2].xyz + posNormal.y * params.lightCube[3].xyz +
               negNormal.z * params.lightCube[4].xyz + posNormal.z * params.lightCube[5].xyz;
    }

    // The colour is linear: a gamma target tone-maps it with the scene's exposure and
    // encodes it, a camera frame's linear HDR scene leaves both to compose.
    if (input.output.z < 0.5) {
        rgb = toneMapExposed(rgb, input.output.x, int(input.output.y + 0.5));
        rgb = pow(max(rgb, float3(0.0)) + 0.0000001, float3(1.0 / 2.2));
    }
    return float4(rgb, alpha);
}
