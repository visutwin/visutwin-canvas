// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The GPU particle simulation, dispatched through the generic Compute seam: the storage
// buffer "particles" at binding 0 and the uniform block after it (Compute's name order).
//
// The step is deterministic: birth state derives from an INTEGER hash (PCG) of the particle
// index and the emitter's step counter, so an emitter needs no CPU round trip and every
// target draws the same numbers bit for bit. Do not use frac(sin(x) * 43758): GPU sine at
// large arguments is neither uniform nor the same across targets, and a seed that includes
// the running time loses precision the longer an emitter runs. The per-life seed is kept
// in motion.w as a float holding 24 bits, which is exact.
//
// The particle clock: a life <= 0 is unborn and re-spawned every step; reaching the
// lifetime wraps the life back by the emission period, max(lifetime, numParticles * rate),
// which shows the particle again when the emitter loops and hides it when it does not;
// stopping hides every particle not yet born. A hidden particle keeps its clock running, so
// playing again brings it back at its next wrap.
//
// Velocity is the integrated initial velocity with gravity and damping (a DEVIATION) PLUS
// the velocity graphs: the local graph turned by the emitter and the world graph, each a
// random point between graph and graph2 per particle life, and the radial speed graph
// (velocityLut.w / velocityLut2.w) along the direction from the emitter's centre. The graph
// speed of rotation is integrated into the angle. A wrap takes the emission period of a
// random rate between rate and rate2 (graphParams.w is rate2 - rate).
//
// The uniform block IS GpuParticleSimParams (1280 bytes, pinned by its static_assert).

struct Particle
{
    float4 posAge;
    float4 velLifetime;
    float4 rotSeedSize;
    float4 motion;
};

struct ParticleSimParams
{
    float4x4 emitterTransform;
    float4x4 worldToEmitter;
    float4 gravityDamping;   // xyz = gravity, w = damping fraction/s
    float4 shapeParams;      // xyz = box half-extents (x = radius), w = shape type
    float4 velocityBase;     // xyz = base velocity, w = localSpace flag
    float4 velocitySpread;   // xyz = spread, w = loop flag
    float4 timeParams;       // dt, time, emission period, particle count
    float4 lifeRot;          // lifetime min/max, rotSpeed min/max (rad/s)
    float4 angleParams;      // startAngle min/max (rad), step counter, on-stop flag
    float4 graphParams;      // velocity graphs on, rotation speed graph on, radial graph on, rate2 - rate
    float4 localVelocityLut[16];
    float4 localVelocityLut2[16];
    float4 velocityLut[16];
    float4 velocityLut2[16];
};

[[vk::binding(0, 0)]] RWStructuredBuffer<Particle> particles : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<ParticleSimParams> params : register(b1);

uint pcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(uint key) { return float(pcgHash(key) >> 8) * (1.0 / 16777216.0); }
float3 rnd3(uint key) { return float3(rnd(key), rnd(key + 1u), rnd(key + 2u)); }

float4 sampleLut(float4 lut[16], float nlife)
{
    const float position = nlife * 15.0;
    const int index = min(int(position), 15);
    return lerp(lut[index], lut[min(index + 1, 15)], position - float(index));
}

/// The emitter's position: the translation column of its transform.
float3 emitterOrigin()
{
    return float3(params.emitterTransform[0][3], params.emitterTransform[1][3], params.emitterTransform[2][3]);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void particleSimKernel(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint gid = dispatchId.x;
    if (gid >= uint(params.timeParams.w)) {
        return;
    }
    Particle p = particles[gid];
    const float dt = params.timeParams.x;
    const bool loop = params.velocitySpread.w > 0.5;
    const bool localSpace = params.velocityBase.w > 0.5;

    const float inLife = p.posAge.w;
    float life = inLife + dt;
    bool hidden = p.rotSeedSize.w > 0.5;
    bool respawn = inLife <= 0.0;
    if (!respawn && life >= max(p.velLifetime.w, 1e-4)) {
        const float period = params.timeParams.z +
            params.timeParams.w * params.graphParams.w * rnd(pcgHash(gid ^ uint(params.angleParams.z)) + 17u);
        life -= max(max(p.velLifetime.w, 1e-4), period);
        hidden = !loop;
        respawn = true;
    }
    if (loop && life < 0.0) {
        hidden = false;
    }
    if (params.angleParams.w > 0.5 && inLife < 0.0) {
        hidden = true;   // stopped: the unborn never appear
    }

    if (respawn) {
        // Spawn position from the emitter shape, velocity from base + spread, per-particle
        // lifetime and rotation from the hashed seed.
        const uint lifeSeed = pcgHash(gid ^ pcgHash(uint(params.angleParams.z))) & 0xFFFFFFu;
        const float3 r3 = rnd3(lifeSeed) * 2.0 - 1.0;
        const float3 r3b = rnd3(lifeSeed + 3u);

        float3 localPos;
        if (params.shapeParams.w > 0.5) {
            // Sphere: rejection-free radial spawn (cube root for uniform density).
            const float3 dir = normalize(r3 + float3(1e-5, 0.0, 0.0));
            localPos = dir * (params.shapeParams.x * pow(r3b.x, 1.0 / 3.0));
        } else {
            localPos = r3 * params.shapeParams.xyz;
        }
        p.posAge.xyz = mul(params.emitterTransform, float4(localPos, 1.0)).xyz;

        float3 vel = params.velocityBase.xyz + (rnd3(lifeSeed + 6u) * 2.0 - 1.0) * params.velocitySpread.xyz;
        if (!localSpace) {
            // World space: rotate the velocity by the emitter orientation.
            vel = mul(params.emitterTransform, float4(vel, 0.0)).xyz;
        }
        p.velLifetime.xyz = vel;
        p.velLifetime.w = lerp(params.lifeRot.x, params.lifeRot.y, r3b.y);
        p.rotSeedSize.x = lerp(params.angleParams.x, params.angleParams.y, r3b.z);
        p.rotSeedSize.y = lerp(params.lifeRot.z, params.lifeRot.w, rnd(lifeSeed + 9u));
        p.motion.w = float(lifeSeed);
    }

    // Integrate: gravity, damping, then the velocity graphs on top.
    float3 vel = p.velLifetime.xyz + params.gravityDamping.xyz * dt;
    vel *= max(1.0 - params.gravityDamping.w * dt, 0.0);
    p.velLifetime.xyz = vel;

    const float nlife = clamp(life / max(p.velLifetime.w, 1e-4), 0.0, 1.0);
    if (params.graphParams.x > 0.5) {
        const float3 r = rnd3(uint(p.motion.w) + 10u);
        const float3 localVelocity = lerp(sampleLut(params.localVelocityLut, nlife).xyz,
                                          sampleLut(params.localVelocityLut2, nlife).xyz, r);
        const float3 worldVelocity = lerp(sampleLut(params.velocityLut, nlife).xyz,
                                          sampleLut(params.velocityLut2, nlife).xyz, r);
        vel += localSpace
            ? localVelocity + mul(params.worldToEmitter, float4(worldVelocity, 0.0)).xyz
            : mul(params.emitterTransform, float4(localVelocity, 0.0)).xyz + worldVelocity;
    }
    if (params.graphParams.y > 0.5) {
        p.rotSeedSize.x += lerp(sampleLut(params.localVelocityLut, nlife).w,
                                sampleLut(params.localVelocityLut2, nlife).w, rnd(uint(p.motion.w) + 13u)) * dt;
    }
    if (params.graphParams.z > 0.5) {
        // Radial speed, away from the emitter's centre (its origin in local space).
        const float radialSpeed = lerp(sampleLut(params.velocityLut, nlife).w,
                                       sampleLut(params.velocityLut2, nlife).w, rnd(uint(p.motion.w) + 16u));
        const float3 radial = p.posAge.xyz - emitterOrigin();
        vel += dot(radial, radial) > 1e-8 ? radialSpeed * normalize(radial) : float3(0.0, 0.0, 0.0);
    }
    p.posAge.xyz += vel * dt;
    p.posAge.w = life;
    p.rotSeedSize.w = hidden ? 1.0 : 0.0;
    p.motion.xyz = vel;

    particles[gid] = p;
}
