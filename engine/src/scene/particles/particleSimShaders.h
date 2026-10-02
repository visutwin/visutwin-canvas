// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The GPU particle simulation kernel, authored once per language and dispatched
// through the generic Compute seam rather than a GraphicsDevice virtual.
//
// Binding layout follows Compute's name-order contract (see compute.h): one
// storage buffer, "particles", takes binding 0, and the uniform block follows at
// binding 1.
//
// The step is deterministic: birth state derives from an INTEGER hash (PCG) of the particle
// index and the emitter's step counter, so an emitter needs no CPU round trip, and both
// languages draw the same numbers bit for bit. Do not use fract(sin(x) * 43758): GPU sine
// at large arguments is neither uniform nor the same in MSL and SPIR-V, and a seed that
// includes the running time loses precision the longer an emitter runs.
// The per-life seed is kept in motion.w as a float holding 24 bits, which is exact.
//
// The particle clock is upstream's (particleUpdaterStart / Respawn / NoRespawn / OnStop):
// a life <= 0 is unborn and re-spawned every step; reaching the lifetime wraps the life
// back by the emission period, max(lifetime, numParticles * rate), which shows the
// particle again when the emitter loops and hides it when it does not; stopping hides
// every particle not yet born. A hidden particle keeps its clock running, so playing
// again brings it back at its next wrap, as upstream.
//
// Velocity is our integrated initial velocity with gravity and damping (a DEVIATION)
// PLUS upstream's velocity graphs: the local graph turned
// by the emitter, and the world graph, each a random point between graph and graph2 per
// particle life, and the radial speed graph (velocityLut.w / velocityLut2.w), along the
// direction from the emitter's centre. The graph speed of rotation is integrated into the
// angle. A wrap takes the emission period of a random rate between rate and rate2
// (graphParams.w is (rate2 - rate)), as upstream's particleUpdaterRespawn.
//
// The uniform block IS `GpuParticleSimParams` — two mat4s, eight vec4s and four
// 16-sample lookup tables, 1280 bytes, which its own static_assert pins. Both kernels declare that member list; changing
// one without the others silently misreads every field after the change. Nothing checks
// this layout by reflection: the source is compiled at runtime, outside the SPIR-V bundle
// the generator validates.
//
#pragma once

namespace visutwin::canvas::particle_sim_shaders
{
    constexpr const char* PARTICLE_SIM_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct Particle {
    float4 posAge;
    float4 velLifetime;
    float4 rotSeedSize;
    float4 motion;
};

struct ParticleSimParams {
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

static inline uint pcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
static inline float rnd(uint key) { return float(pcgHash(key) >> 8) * (1.0 / 16777216.0); }
static inline float3 rnd3(uint key) { return float3(rnd(key), rnd(key + 1u), rnd(key + 2u)); }

static inline float4 sampleLut(constant float4* lut, float nlife)
{
    const float position = nlife * 15.0;
    const int index = min(int(position), 15);
    return mix(lut[index], lut[min(index + 1, 15)], position - float(index));
}

kernel void particleSimKernel(device Particle* particles [[buffer(0)]],
                              constant ParticleSimParams& params [[buffer(1)]],
                              uint gid [[thread_position_in_grid]])
{
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
        // Spawn position from the emitter shape, velocity from base + spread,
        // per-particle lifetime/rotation from the hashed seed.
        const uint lifeSeed = pcgHash(gid ^ pcgHash(uint(params.angleParams.z))) & 0xFFFFFFu;
        const float3 r3 = rnd3(lifeSeed) * 2.0 - 1.0;
        const float3 r3b = rnd3(lifeSeed + 3u);

        float3 localPos;
        if (params.shapeParams.w > 0.5) {
            // Sphere: rejection-free radial spawn (cbrt for uniform density).
            const float3 dir = normalize(r3 + float3(1e-5, 0.0, 0.0));
            localPos = dir * (params.shapeParams.x * pow(r3b.x, 1.0 / 3.0));
        } else {
            localPos = r3 * params.shapeParams.xyz;
        }
        p.posAge.xyz = (params.emitterTransform * float4(localPos, 1.0)).xyz;

        float3 vel = params.velocityBase.xyz + (rnd3(lifeSeed + 6u) * 2.0 - 1.0) * params.velocitySpread.xyz;
        if (!localSpace) {
            // World space: rotate the velocity by the emitter orientation.
            vel = (params.emitterTransform * float4(vel, 0.0)).xyz;
        }
        p.velLifetime.xyz = vel;
        p.velLifetime.w = mix(params.lifeRot.x, params.lifeRot.y, r3b.y);
        p.rotSeedSize.x = mix(params.angleParams.x, params.angleParams.y, r3b.z);
        p.rotSeedSize.y = mix(params.lifeRot.z, params.lifeRot.w, rnd(lifeSeed + 9u));
        p.motion.w = float(lifeSeed);
    }

    // Integrate: gravity, damping, then the velocity graphs on top.
    float3 vel = p.velLifetime.xyz + params.gravityDamping.xyz * dt;
    vel *= max(1.0 - params.gravityDamping.w * dt, 0.0);
    p.velLifetime.xyz = vel;

    const float nlife = clamp(life / max(p.velLifetime.w, 1e-4), 0.0, 1.0);
    if (params.graphParams.x > 0.5) {
        const float3 r = rnd3(uint(p.motion.w) + 10u);
        const float3 localVelocity = mix(sampleLut(params.localVelocityLut, nlife).xyz,
                                         sampleLut(params.localVelocityLut2, nlife).xyz, r);
        const float3 worldVelocity = mix(sampleLut(params.velocityLut, nlife).xyz,
                                         sampleLut(params.velocityLut2, nlife).xyz, r);
        vel += localSpace
            ? localVelocity + (params.worldToEmitter * float4(worldVelocity, 0.0)).xyz
            : (params.emitterTransform * float4(localVelocity, 0.0)).xyz + worldVelocity;
    }
    if (params.graphParams.y > 0.5) {
        p.rotSeedSize.x += mix(sampleLut(params.localVelocityLut, nlife).w,
                               sampleLut(params.localVelocityLut2, nlife).w, rnd(uint(p.motion.w) + 13u)) * dt;
    }
    if (params.graphParams.z > 0.5) {
        // Radial speed, away from the emitter's centre (its origin in local space).
        const float radialSpeed = mix(sampleLut(params.velocityLut, nlife).w,
                                      sampleLut(params.velocityLut2, nlife).w, rnd(uint(p.motion.w) + 16u));
        const float3 radial = p.posAge.xyz - params.emitterTransform[3].xyz;
        vel += dot(radial, radial) > 1e-8 ? radialSpeed * normalize(radial) : float3(0.0);
    }
    p.posAge.xyz += vel * dt;
    p.posAge.w = life;
    p.rotSeedSize.w = hidden ? 1.0 : 0.0;
    p.motion.xyz = vel;

    particles[gid] = p;
}
)";

    constexpr const char* PARTICLE_SIM_GLSL = R"(
#version 450
layout(local_size_x = 256) in;
struct Particle { vec4 posAge; vec4 velLifetime; vec4 rotSeedSize; vec4 motion; };
layout(set = 0, binding = 0, std430) buffer Particles { Particle values[]; } particles;
layout(set = 0, binding = 1, std140) uniform SimParams {
    mat4 emitterTransform;
    mat4 worldToEmitter;
    vec4 gravityDamping;
    vec4 shapeParams;
    vec4 velocityBase;
    vec4 velocitySpread;
    vec4 timeParams;
    vec4 lifeRot;
    vec4 angleParams;
    vec4 graphParams;
    vec4 localVelocityLut[16];
    vec4 localVelocityLut2[16];
    vec4 velocityLut[16];
    vec4 velocityLut2[16];
} params;

uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(uint key) { return float(pcgHash(key) >> 8) * (1.0 / 16777216.0); }
vec3 rnd3(uint key) { return vec3(rnd(key), rnd(key + 1u), rnd(key + 2u)); }

#define SAMPLE_LUT(lut, nlife) mix(lut[min(int(nlife * 15.0), 15)], lut[min(int(nlife * 15.0) + 1, 15)], \
    nlife * 15.0 - float(min(int(nlife * 15.0), 15)))

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uint(params.timeParams.w)) return;
    Particle p = particles.values[id];
    float dt = params.timeParams.x;
    bool loop = params.velocitySpread.w > 0.5;
    bool localSpace = params.velocityBase.w > 0.5;

    float inLife = p.posAge.w;
    float life = inLife + dt;
    bool hidden = p.rotSeedSize.w > 0.5;
    bool respawn = inLife <= 0.0;
    if (!respawn && life >= max(p.velLifetime.w, 1e-4)) {
        float period = params.timeParams.z +
            params.timeParams.w * params.graphParams.w * rnd(pcgHash(id ^ uint(params.angleParams.z)) + 17u);
        life -= max(max(p.velLifetime.w, 1e-4), period);
        hidden = !loop;
        respawn = true;
    }
    if (loop && life < 0.0) hidden = false;
    if (params.angleParams.w > 0.5 && inLife < 0.0) hidden = true;

    if (respawn) {
        uint lifeSeed = pcgHash(id ^ pcgHash(uint(params.angleParams.z))) & 0xFFFFFFu;
        vec3 r = rnd3(lifeSeed) * 2.0 - 1.0, rb = rnd3(lifeSeed + 3u), localPos;
        if (params.shapeParams.w > 0.5)
            localPos = normalize(r + vec3(1e-5, 0, 0)) * params.shapeParams.x * pow(rb.x, 1.0 / 3.0);
        else
            localPos = r * params.shapeParams.xyz;
        p.posAge.xyz = (params.emitterTransform * vec4(localPos, 1)).xyz;
        vec3 velocity = params.velocityBase.xyz +
            (rnd3(lifeSeed + 6u) * 2.0 - 1.0) * params.velocitySpread.xyz;
        if (!localSpace)
            velocity = (params.emitterTransform * vec4(velocity, 0)).xyz;
        p.velLifetime = vec4(velocity, mix(params.lifeRot.x, params.lifeRot.y, rb.y));
        p.rotSeedSize.x = mix(params.angleParams.x, params.angleParams.y, rb.z);
        p.rotSeedSize.y = mix(params.lifeRot.z, params.lifeRot.w, rnd(lifeSeed + 9u));
        p.motion.w = float(lifeSeed);
    }

    vec3 velocity = p.velLifetime.xyz + params.gravityDamping.xyz * dt;
    velocity *= max(1.0 - params.gravityDamping.w * dt, 0.0);
    p.velLifetime.xyz = velocity;

    float nlife = clamp(life / max(p.velLifetime.w, 1e-4), 0.0, 1.0);
    if (params.graphParams.x > 0.5) {
        vec3 r = rnd3(uint(p.motion.w) + 10u);
        vec3 localVelocity = mix(SAMPLE_LUT(params.localVelocityLut, nlife).xyz,
                                 SAMPLE_LUT(params.localVelocityLut2, nlife).xyz, r);
        vec3 worldVelocity = mix(SAMPLE_LUT(params.velocityLut, nlife).xyz,
                                 SAMPLE_LUT(params.velocityLut2, nlife).xyz, r);
        velocity += localSpace
            ? localVelocity + (params.worldToEmitter * vec4(worldVelocity, 0)).xyz
            : (params.emitterTransform * vec4(localVelocity, 0)).xyz + worldVelocity;
    }
    if (params.graphParams.y > 0.5)
        p.rotSeedSize.x += mix(SAMPLE_LUT(params.localVelocityLut, nlife).w,
                               SAMPLE_LUT(params.localVelocityLut2, nlife).w, rnd(uint(p.motion.w) + 13u)) * dt;
    if (params.graphParams.z > 0.5) {
        float radialSpeed = mix(SAMPLE_LUT(params.velocityLut, nlife).w,
                                SAMPLE_LUT(params.velocityLut2, nlife).w, rnd(uint(p.motion.w) + 16u));
        vec3 radial = p.posAge.xyz - params.emitterTransform[3].xyz;
        velocity += dot(radial, radial) > 1e-8 ? radialSpeed * normalize(radial) : vec3(0.0);
    }
    p.posAge.xyz += velocity * dt;
    p.posAge.w = life;
    p.rotSeedSize.w = hidden ? 1.0 : 0.0;
    p.motion.xyz = velocity;
    particles.values[id] = p;
}
)";
}
