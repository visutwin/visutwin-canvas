#include <metal_stdlib>
using namespace metal;

struct Particle {
    float4 posAge;
    float4 velLifetime;
    float4 rotSeedSize;
};

struct ParticleSimParams {
    float4x4 emitterTransform;
    float4 gravityDamping;   // xyz = gravity, w = damping fraction/s
    float4 shapeParams;      // xyz = box half-extents (x = radius), w = shape type
    float4 velocityBase;     // xyz = base velocity, w = localSpace flag
    float4 velocitySpread;   // xyz = spread, w = loop flag
    float4 timeParams;       // dt, time, birth interval, particle count
    float4 lifeRot;          // lifetime min/max, rotSpeed min/max (rad/s)
    float4 angleParams;      // startAngle min/max (rad), seed, playing
};

static inline float hash1(float n) { return fract(sin(n) * 43758.5453123); }
static inline float3 hash3(float n)
{
    return float3(hash1(n), hash1(n + 17.1717), hash1(n + 41.4141));
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

    float age = p.posAge.w + dt;

    if (p.posAge.w < 0.0 && age >= 0.0) {
        // Birth: spawn position from the emitter shape, velocity from base +
        // spread, per-particle lifetime/rotation from the hashed seed.
        const float seed = p.rotSeedSize.z + params.angleParams.z;
        const float3 r3 = hash3(seed) * 2.0 - 1.0;
        const float3 r3b = hash3(seed + 7.77);

        float3 localPos;
        if (params.shapeParams.w > 0.5) {
            // Sphere: rejection-free radial spawn (cbrt for uniform density).
            const float3 dir = normalize(r3 + float3(1e-5, 0.0, 0.0));
            localPos = dir * (params.shapeParams.x * pow(r3b.x, 1.0 / 3.0));
        } else {
            localPos = r3 * params.shapeParams.xyz;
        }

        const float4 world = params.emitterTransform * float4(localPos, 1.0);
        p.posAge.xyz = world.xyz;

        float3 vel = params.velocityBase.xyz + (hash3(seed + 3.33) * 2.0 - 1.0) * params.velocitySpread.xyz;
        if (params.velocityBase.w < 0.5) {
            // World space: rotate the velocity by the emitter orientation.
            vel = (params.emitterTransform * float4(vel, 0.0)).xyz;
        }
        p.velLifetime.xyz = vel;
        p.velLifetime.w = mix(params.lifeRot.x, params.lifeRot.y, r3b.y);
        p.rotSeedSize.x = mix(params.angleParams.x, params.angleParams.y, r3b.z);
        p.rotSeedSize.y = mix(params.lifeRot.z, params.lifeRot.w, hash1(seed + 9.99));
        p.posAge.w = age;
    } else if (p.posAge.w >= 0.0) {
        const float lifetime = max(p.velLifetime.w, 1e-4);
        if (age > lifetime) {
            if (loop) {
                // Queue a rebirth next step (keeps the stream continuous).
                p.posAge.w = -1e-4;
            } else {
                p.posAge.w = age;   // dead — render shader clips it
            }
        } else {
            // Integrate: gravity, damping, advection.
            float3 vel = p.velLifetime.xyz + params.gravityDamping.xyz * dt;
            vel *= max(1.0 - params.gravityDamping.w * dt, 0.0);
            p.posAge.xyz += vel * dt;
            p.velLifetime.xyz = vel;
            p.posAge.w = age;
        }
    } else {
        p.posAge.w = age;   // still counting down to birth
    }

    particles[gid] = p;
}
