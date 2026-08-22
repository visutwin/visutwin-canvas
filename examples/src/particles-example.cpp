// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of the upstream "Compute - Particles" example.
//
// A million particles fall through three collision spheres. Everything about them
// lives on the GPU: an app-authored compute kernel advances a storage buffer of
// particle records with Verlet integration and sphere collisions, and an app-authored
// render shader expands each record into a camera-facing quad, coloured yellow at the
// moment of impact and fading to red as time since the last collision grows.
//
// Nothing here goes through the built-in ParticleSystemComponent — the point of the
// example is the application-facing compute path: Compute with storage-buffer and
// loose-uniform parameters, and MeshInstance::setStorageDraw for the matching draw.
//
// DEVIATIONS from upstream:
//  - upstream draws 6 indices per particle over a vertex-buffer-less mesh and derives
//    the particle from `vertexIndex / 4`; this port draws one 4-vertex triangle-strip
//    quad per instance and derives it from the instance id, which is how the engine's
//    own emitter and splat paths draw and avoids a 24 MB index buffer;
//  - upstream reflects the compute resources out of the WGSL source. This port has no
//    shader reflection, so bindings follow parameter name order — see compute.h.
//
// Esc quits; the camera orbits with the mouse.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <framework/assets/asset.h>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

// Upstream's particle count. 1M records x 48 bytes = 48 MB of storage.
constexpr uint32_t NUM_PARTICLES = 1024u * 1024u;
constexpr uint32_t WORKGROUP_SIZE = 64u;
constexpr uint32_t NUM_SPHERES = 3u;


// The particle record, laid out to match the struct both shaders declare. Upstream's
// WGSL Particle is 12 floats with padding after positionOld and originalVelocity; the
// same 48-byte layout falls out of MSL packed_float3 and GLSL std430 vec3.
struct GpuParticleRecord
{
    float position[3];
    float collisionTime;
    float positionOld[3];
    float pad0;
    float originalVelocity[3];
    float pad1;
};
static_assert(sizeof(GpuParticleRecord) == 48, "particle record must match the shader struct");

// Per-draw parameters for the render shader (Metal vertex slot 11 / Vulkan set 6
// binding 3). Upstream bakes both of these in as shader constants.
struct alignas(16) ParticleRenderParams
{
    float particleSize = 0.04f;
    float colorFadeTime = 7.0f;   // seconds since collision at which yellow reaches red
    float pad[2] = {0.0f, 0.0f};
};

// ---------------------------------------------------------------------------
// Simulation kernel. Verlet integration, sphere collisions, and a wrap that
// respawns a particle at its original velocity once it drifts past 300 units.
//
// Buffer slots follow Compute's name-order contract: buffers "particles" (0) and
// "spheres" (1), then the loose-uniform block (2) whose members are the scalar
// parameters in name order — count, dt, sphereCount.
// ---------------------------------------------------------------------------
static const char* kSimulationSourceMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Particle {
    packed_float3 position;
    float collisionTime;
    packed_float3 positionOld;
    float pad0;
    packed_float3 originalVelocity;
    float pad1;
};

struct SimParams {
    uint  count;
    float dt;
    uint  sphereCount;
};

kernel void simulateParticles(device Particle* particles      [[buffer(0)]],
                              const device float4* spheres    [[buffer(1)]],
                              constant SimParams& params      [[buffer(2)]],
                              uint3 gid [[thread_position_in_grid]])
{
    const uint index = gid.x * 1024u + gid.y;
    if (index >= params.count) {
        return;
    }

    Particle particle = particles[index];
    particle.collisionTime += params.dt;

    // Drifted too far: restart from the spawn point along the original velocity.
    float3 position = float3(particle.position);
    const float distance = length(position);
    if (distance > 300.0) {
        const float wrapDistance = distance - 300.0;
        particle.collisionTime = 100.0;
        particle.positionOld = packed_float3(wrapDistance * float3(particle.originalVelocity));
        particle.position = particle.originalVelocity;
        position = float3(particle.position);
    }

    // Verlet integration.
    const float3 delta = position - float3(particle.positionOld);
    float3 next = position + delta;

    for (uint i = 0u; i < params.sphereCount; ++i) {
        const float3 center = spheres[i].xyz;
        const float radius = spheres[i].w;
        if (length(next - center) < radius) {
            next = center + normalize(next - center) * radius;
            particle.collisionTime = 0.0;
        }
    }

    particle.positionOld = packed_float3(position);
    particle.position = packed_float3(next);
    particles[index] = particle;
}
)MSL";

static const char* kSimulationSourceGlsl = R"GLSL(
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Particle {
    vec3  position;
    float collisionTime;
    vec3  positionOld;
    float pad0;
    vec3  originalVelocity;
    float pad1;
};

layout(set = 0, binding = 0, std430) buffer Particles { Particle values[]; } particles;
layout(set = 0, binding = 1, std430) readonly buffer Spheres { vec4 values[]; } spheres;
layout(set = 0, binding = 2, std140) uniform SimParams {
    uint  count;
    float dt;
    uint  sphereCount;
} params;

void main()
{
    uint index = gl_GlobalInvocationID.x * 1024u + gl_GlobalInvocationID.y;
    if (index >= params.count) return;

    Particle particle = particles.values[index];
    particle.collisionTime += params.dt;

    float distance = length(particle.position);
    if (distance > 300.0) {
        float wrapDistance = distance - 300.0;
        particle.collisionTime = 100.0;
        particle.positionOld = wrapDistance * particle.originalVelocity;
        particle.position = particle.originalVelocity;
    }

    vec3 delta = particle.position - particle.positionOld;
    vec3 next = particle.position + delta;

    for (uint i = 0u; i < params.sphereCount; ++i) {
        vec3 center = spheres.values[i].xyz;
        float radius = spheres.values[i].w;
        if (length(next - center) < radius) {
            next = center + normalize(next - center) * radius;
            particle.collisionTime = 0.0;
        }
    }

    particle.positionOld = particle.position;
    particle.position = next;
    particles.values[index] = particle;
}
)GLSL";

// ---------------------------------------------------------------------------
// Render shader. One instance per particle; the camera's right/up axes come out of
// the view-projection matrix exactly as upstream's vertex shader extracts them.
// ---------------------------------------------------------------------------
static const char* kRenderSourceMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Particle {
    packed_float3 position;
    float collisionTime;
    packed_float3 positionOld;
    float pad0;
    packed_float3 originalVelocity;
    float pad1;
};

struct SceneData { float4x4 projViewMatrix; };
struct RenderParams { float particleSize; float colorFadeTime; float2 pad; };

struct Varyings {
    float4 position [[position]];
    float4 color;
};

constant float2 kQuad[4] = {
    float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0)
};

vertex Varyings particleVS(uint vid [[vertex_id]],
                           uint iid [[instance_id]],
                           constant SceneData& scene           [[buffer(1)]],
                           const device Particle* particles    [[buffer(7)]],
                           constant RenderParams& params       [[buffer(11)]])
{
    const Particle particle = particles[iid];

    // Camera right and up, read out of the view-projection matrix's first two rows.
    const float3 left = float3(scene.projViewMatrix[0][0],
                               scene.projViewMatrix[1][0],
                               scene.projViewMatrix[2][0]);
    const float3 up   = float3(scene.projViewMatrix[0][1],
                               scene.projViewMatrix[1][1],
                               scene.projViewMatrix[2][1]);

    const float2 corner = kQuad[vid] * params.particleSize;
    const float3 expanded = corner.x * left + corner.y * up;

    Varyings out;
    float4 clip = scene.projViewMatrix * float4(float3(particle.position) + expanded, 1.0);
    clip.z = 0.5 * (clip.z + clip.w);   // engine projections are GL-style [-1,1]
    out.position = clip;
    out.color = mix(float4(1.0, 1.0, 0.0, 1.0), float4(1.0, 0.0, 0.0, 1.0),
                    saturate(particle.collisionTime / params.colorFadeTime));
    return out;
}

fragment float4 particleFS(Varyings in [[stage_in]])
{
    return in.color;
}
)MSL";

static const char* kRenderSourceGlsl = R"GLSL(
#version 450

struct Particle {
    vec3  position;
    float collisionTime;
    vec3  positionOld;
    float pad0;
    vec3  originalVelocity;
    float pad1;
};

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

layout(set = 6, binding = 0, std430) readonly buffer Particles { Particle values[]; } particles;
layout(set = 6, binding = 3, std140) uniform RenderParams {
    float particleSize;
    float colorFadeTime;
    vec2  pad;
} params;

#ifdef VT_VERTEX_SHADER
layout(location = 0) out vec4 outColor;
void main() {
    Particle particle = particles.values[gl_InstanceIndex];

    vec3 left = vec3(pc.viewProjection[0][0], pc.viewProjection[1][0], pc.viewProjection[2][0]);
    vec3 up   = vec3(pc.viewProjection[0][1], pc.viewProjection[1][1], pc.viewProjection[2][1]);

    vec2 quad[4] = vec2[](vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, -1.0));
    vec2 corner = quad[gl_VertexIndex] * params.particleSize;
    vec3 expanded = corner.x * left + corner.y * up;

    vec4 clip = pc.viewProjection * vec4(particle.position + expanded, 1.0);
    clip.z = 0.5 * (clip.z + clip.w);
    gl_Position = clip;
    outColor = mix(vec4(1.0, 1.0, 0.0, 1.0), vec4(1.0, 0.0, 0.0, 1.0),
                   clamp(particle.collisionTime / params.colorFadeTime, 0.0, 1.0));
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = inColor;
}
#endif
)GLSL";

// Builds the initial pool: every particle starts at the origin with a random velocity
// inside a downward cone, and with no recent collision.
std::vector<GpuParticleRecord> buildInitialParticles()
{
    std::vector<GpuParticleRecord> particles(NUM_PARTICLES);
    std::mt19937 rng(12345u);   // DEVIATION: fixed seed, so runs are reproducible
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    for (auto& particle : particles) {
        const float r = 0.4f * std::sqrt(unit(rng));
        const float theta = unit(rng) * 2.0f * static_cast<float>(M_PI);
        const float speed = 0.6f + unit(rng) * 0.6f;

        Vector3 velocity(r * std::cos(theta), -1.0f, r * std::sin(theta));
        velocity = velocity.normalized() * speed;

        particle.position[0] = velocity.getX();
        particle.position[1] = velocity.getY();
        particle.position[2] = velocity.getZ();
        particle.collisionTime = 100.0f;   // large: no recent collision
        particle.positionOld[0] = 0.0f;
        particle.positionOld[1] = 0.0f;
        particle.positionOld[2] = 0.0f;
        particle.pad0 = 0.0f;
        particle.originalVelocity[0] = velocity.getX();
        particle.originalVelocity[1] = velocity.getY();
        particle.originalVelocity[2] = velocity.getZ();
        particle.pad1 = 0.0f;
    }
    return particles;
}

class ParticlesExample final: public ExampleApp
{
public:
    ParticlesExample(): ExampleApp({.title = "Compute Particles"}) {}

protected:
    bool create() override
    {
        const bool supportsCompute = device()->supportsCompute();
        if (!supportsCompute) {
            spdlog::error("This device reports no compute support — the particles will not move.");
        }

        // General scene rendering properties (upstream: skyboxMip 2, intensity 0.2).
        scene()->setSkyboxMip(2);
        scene()->setSkyboxIntensity(0.2f);

        _helipadAsset = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        if (const auto helipadResource = _helipadAsset->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));
        } else {
            spdlog::error("Failed to load the helipad environment atlas");
        }

        // -----------------------------------------------------------------------
        // Collision spheres — both the colliders the kernel reads and the visible
        // geometry the particles pile onto.
        // -----------------------------------------------------------------------
        _sphereMaterial = std::make_shared<StandardMaterial>();
        _sphereMaterial->setGloss(0.6f);
        _sphereMaterial->setMetalness(0.4f);

        struct SphereDesc { float x, y, z, radius; };
        constexpr SphereDesc sphereDescs[NUM_SPHERES] = {
            {28.0f, -70.0f, 0.0f, 27.0f},
            {-38.0f, -130.0f, 0.0f, 35.0f},
            {45.0f, -210.0f, 35.0f, 70.0f}
        };

        std::vector<float> sphereData(NUM_SPHERES * 4);
        for (uint32_t i = 0; i < NUM_SPHERES; ++i) {
            const auto& [x, y, z, radius] = sphereDescs[i];
            sphereData[i * 4 + 0] = x;
            sphereData[i * 4 + 1] = y;
            sphereData[i * 4 + 2] = z;
            sphereData[i * 4 + 3] = radius;

            createPrimitive("sphere", _sphereMaterial.get(), Vector3(x, y, z),
                Vector3(radius * 2.0f, radius * 2.0f, radius * 2.0f));
        }

        // -----------------------------------------------------------------------
        // Camera. Upstream places it here and focuses the orbit on the middle sphere.
        // -----------------------------------------------------------------------
        const Vector3 focusPoint(sphereDescs[1].x, sphereDescs[1].y, sphereDescs[1].z);

        auto* cameraEntity = createCamera(Vector3(-150.0f, -60.0f, 190.0f));
        if (auto* cameraComponent = cameraEntity->findComponent<CameraComponent>()) {
            cameraComponent->setToneMapping(TONEMAP_ACES);
            if (cameraComponent->camera()) {
                cameraComponent->camera()->setFarClip(1000.0f);
            }
        }
        addOrbitControls(cameraEntity, focusPoint);

        // -----------------------------------------------------------------------
        // Particle storage: the buffer the kernel writes and the vertex stage reads.
        // -----------------------------------------------------------------------
        const std::vector<GpuParticleRecord> initialParticles = buildInitialParticles();
        VertexBufferOptions particleBufferOptions;
        particleBufferOptions.data.resize(initialParticles.size() * sizeof(GpuParticleRecord));
        std::memcpy(particleBufferOptions.data.data(), initialParticles.data(),
            particleBufferOptions.data.size());
        auto particleFormat = std::make_shared<VertexFormat>(
            static_cast<int>(sizeof(GpuParticleRecord)), true, false);
        _particleBuffer = device()->createVertexBuffer(
            particleFormat, static_cast<int>(NUM_PARTICLES), particleBufferOptions);

        VertexBufferOptions sphereBufferOptions;
        sphereBufferOptions.data.resize(sphereData.size() * sizeof(float));
        std::memcpy(sphereBufferOptions.data.data(), sphereData.data(), sphereBufferOptions.data.size());
        auto sphereFormat = std::make_shared<VertexFormat>(4 * static_cast<int>(sizeof(float)), true, false);
        _sphereBuffer = device()->createVertexBuffer(
            sphereFormat, static_cast<int>(NUM_SPHERES), sphereBufferOptions);

        // -----------------------------------------------------------------------
        // Simulation compute shader.
        // -----------------------------------------------------------------------
        if (supportsCompute) {
            ShaderDefinition definition;
            definition.name = "SimulationShader";
            definition.cshader = "simulateParticles";
            // Both backends can be compiled in and selected at runtime (VISUTWIN_BACKEND),
            // so the source has to come from the live device rather than a build-time #ifdef.
            const char* source = device()->shaderLanguage() == ShaderLanguage::Glsl
                ? kSimulationSourceGlsl
                : kSimulationSourceMsl;
            _simulationShader = createShader(device().get(), definition, source);
            if (_simulationShader) {
                _compute = std::make_unique<Compute>(device().get(), _simulationShader, "ComputeParticles");
                _compute->setParameter("particles", _particleBuffer);
                _compute->setParameter("spheres", _sphereBuffer);
                _compute->setParameter("count", NUM_PARTICLES);
                _compute->setParameter("sphereCount", NUM_SPHERES);
                _compute->setThreadgroupSize(WORKGROUP_SIZE, 1u, 1u);
            } else {
                spdlog::error("Failed to compile the particle simulation shader");
            }
        }

        // -----------------------------------------------------------------------
        // Particle rendering: one instanced quad per record in the storage buffer.
        // -----------------------------------------------------------------------
        _particleMaterial = std::make_shared<ShaderMaterial>(
            device(), "ParticleRenderShader", "particleVS", "particleFS",
            ShaderSourceSet{.msl = kRenderSourceMsl, .glsl = kRenderSourceGlsl});
        // Screen-aligned quads carry no meaningful winding.
        _particleMaterial->setCullMode(CullMode::CULLFACE_NONE);

        // A 4-vertex triangle-strip quad. The vertex shader is vertex-id driven, so the
        // contents are never read — the buffer exists to give the draw its vertex count.
        auto quadFormat = std::make_shared<VertexFormat>(
            14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
        VertexBufferOptions quadOptions;
        quadOptions.data.assign(4 * 14 * sizeof(float), 0);
        auto quadBuffer = device()->createVertexBuffer(quadFormat, 4, quadOptions);

        _quadMesh = std::make_shared<Mesh>();
        _quadMesh->setVertexBuffer(quadBuffer);
        Primitive primitive;
        primitive.type = PRIMITIVE_TRISTRIP;
        primitive.base = 0;
        primitive.count = 4;
        primitive.indexed = false;
        _quadMesh->setPrimitive(primitive, 0);

        auto* particleEntity = new Entity();
        particleEntity->setEngine(engine());
        root()->addChild(particleEntity);

        if (auto* render = static_cast<RenderComponent*>(particleEntity->addComponent<RenderComponent>())) {
            auto meshInstance = std::make_unique<MeshInstance>(_quadMesh, _particleMaterial, particleEntity);
            meshInstance->setStorageDraw(_particleBuffer, static_cast<int>(NUM_PARTICLES),
                &_renderParams, sizeof(_renderParams));
            // Particles live in world space and roam far past the node's own bounds.
            meshInstance->setCull(false);
            meshInstance->setCastShadow(false);
            render->addMeshInstance(std::move(meshInstance));
        }

        spdlog::info("{} particles simulated on the GPU. LMB/RMB orbit, Wheel zoom, Esc quits.",
            NUM_PARTICLES);

        return true;
    }

    // Advance the simulation before the frame's render encoding. Upstream
    // dispatches 1024/64 x 1024 workgroups; the kernel folds those two axes
    // back into one particle index.
    void preRender() override
    {
        if (_compute) {
            _compute->setParameter("dt", _lastDt);
            _compute->setupDispatch(NUM_PARTICLES / 1024u / WORKGROUP_SIZE, 1024u, 1u);
            device()->computeDispatch({_compute.get()}, "ComputeParticlesDispatch");
        }
    }

    void update(const float dt) override
    {
        _lastDt = dt;
    }

    void destroy() override
    {
        // Holds device resources that must not outlive the graphics device.
        _compute.reset();
        _simulationShader.reset();
    }

private:
    std::unique_ptr<Asset> _helipadAsset;
    std::shared_ptr<StandardMaterial> _sphereMaterial;
    std::shared_ptr<ShaderMaterial> _particleMaterial;
    std::shared_ptr<Mesh> _quadMesh;
    std::shared_ptr<VertexBuffer> _particleBuffer;
    std::shared_ptr<VertexBuffer> _sphereBuffer;
    std::shared_ptr<Shader> _simulationShader;
    std::unique_ptr<Compute> _compute;

    // Referenced by the storage draw for the lifetime of the mesh instance.
    ParticleRenderParams _renderParams;

    float _lastDt = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesExample)
