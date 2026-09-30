// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GPU particle emitter (upstream particle-emitter.js, GPU-simulation subset).
//
// Simulation runs entirely on the GPU: a compute kernel (particleSimShaders.h,
// dispatched through the backend-agnostic Compute seam) ages, integrates, and
// respawns particles in a persistent storage buffer; rendering draws one
// camera-facing quad per particle through a self-contained shader
// (gsplat-style renderer branch).
//
// DEVIATIONS from upstream: GPU path only (no CPU sim), no particle sorting,
// unlit only, camera-facing billboards only (no mesh particles or custom face),
// an initial velocity + spread with gravity/damping beside upstream's velocity
// graphs (no radial speed graph), no scale/alpha/color graph2, no wrap, no depth
// softening, one rate (no rate2).
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "core/math/vector3.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class GraphNode;
    class Material;
    class Mesh;
    class MeshInstance;
    class Shader;
    class Texture;
    class VertexBuffer;

    enum class ParticleEmitterShape : uint32_t
    {
        EMITTERSHAPE_BOX = 0,    // uniform spawn inside a box (extents)
        EMITTERSHAPE_SPHERE = 1  // uniform spawn inside a sphere (radius)
    };

    enum class ParticleBlendType : uint32_t
    {
        BLEND_ADDITIVE = 0,
        BLEND_NORMAL = 1,
        BLEND_PREMULTIPLIED = 2
    };

    /// Authoring options (upstream particle-system component property subset).
    struct ParticleEmitterOptions
    {
        uint32_t numParticles = 256;      // particle pool size

        float lifetime = 2.0f;            // per-particle lifetime min (seconds)
        float lifetime2 = 2.0f;           // per-particle lifetime max
        // Seconds between births, as upstream: particle i is born at i * rate, so 0 emits
        // the whole pool at once (a burst), and a looping particle comes back after
        // max(lifetime, numParticles * rate).
        float rate = 1.0f;
        bool loop = true;                 // one-shot when false
        bool preWarm = false;             // start as if a looping emitter had run one lifetime
        bool autoPlay = true;             // play when built; false leaves it paused and hidden

        ParticleEmitterShape emitterShape = ParticleEmitterShape::EMITTERSHAPE_BOX;
        Vector3 emitterExtents = Vector3(0.0f, 0.0f, 0.0f);  // box half-extents
        float emitterRadius = 0.0f;                          // sphere radius

        Vector3 initialVelocity = Vector3(0.0f, 0.0f, 0.0f); // base velocity (units/s)
        Vector3 velocitySpread = Vector3(0.0f);              // ± random per axis
        Vector3 gravity = Vector3(0.0f, 0.0f, 0.0f);
        float damping = 0.0f;             // fraction of velocity lost per second (0..1)

        float startAngle = 0.0f;          // initial billboard rotation min (degrees)
        float startAngle2 = 0.0f;         // initial billboard rotation max
        float rotationSpeed = 0.0f;       // rotation speed min (degrees/s)
        float rotationSpeed2 = 0.0f;      // rotation speed max

        // Upstream's velocity graphs over normalized life (units/s): the local graph is
        // turned by the emitter, the world graph is not, and each particle takes a random
        // point between a graph and its graph2 (an empty graph2 means the graph itself).
        // Empty graphs contribute nothing.
        CurveSet localVelocityGraph;
        CurveSet localVelocityGraph2;
        CurveSet velocityGraph;
        CurveSet velocityGraph2;
        // Rotation speed over normalized life (degrees/s), added to rotationSpeed.
        Curve rotationSpeedGraph;
        Curve rotationSpeedGraph2;

        bool alignToMotion = false;       // turn each particle to face its direction of motion
        float stretch = 0.0f;             // pull the trailing vertices back by velocity * stretch

        bool localSpace = false;          // particles follow the emitter node when true
        // Upstream screenSpace: the emitter's world transform is already clip space (a child
        // of a screen-space element), and sizes are measured in viewport heights.
        bool screenSpace = false;
        std::vector<int> layers;          // layers to render on (empty: the render component's)

        Curve scaleGraph;                 // quad half-extent over normalized life, as upstream
        CurveSet colorGraph;              // rgb over normalized life
        Curve alphaGraph;                 // alpha over normalized life
        float intensity = 1.0f;           // color multiplier (HDR glow)

        Texture* colorMap = nullptr;      // optional sprite texture (white quad if null)
        int animTilesX = 1;               // sprite-sheet tiles
        int animTilesY = 1;
        int animNumFrames = 1;            // frames played over each particle's life
        // Which animation in the sheet to play. Each animation is animNumFrames
        // tiles long and they run in reading order, so a 4x4 sheet at 4 frames
        // holds four animations, indices 0-3 (upstream animIndex).
        int animIndex = 0;
        float animSpeed = 1.0f;

        ParticleBlendType blendType = ParticleBlendType::BLEND_NORMAL;   // upstream default
        bool depthWrite = false;

        ParticleEmitterOptions();
    };

    /**
     * Owns the GPU particle pool, the quantized parameter LUTs, and the
     * billboard quad mesh/material. One instance per particle-system component.
     */
    class ParticleEmitter : public std::enable_shared_from_this<ParticleEmitter>
    {
    public:
        static constexpr int kCurveSamples = 16;   // LUT resolution over particle life

        ParticleEmitter(const std::shared_ptr<GraphicsDevice>& device,
            const ParticleEmitterOptions& options);

        /// Rebuild LUTs/material state after mutating options (curves, blend, map).
        void rebuild(const ParticleEmitterOptions& options);

        /// Reset the pool to the initial staggered-birth state (also restarts
        /// one-shot emitters).
        void reset();

        /// Advance the GPU simulation one step. Called per frame by the
        /// particle-system component with the emitter node's world transform.
        void update(float dt, const Matrix4& emitterTransform);

        /// Stop emitting (upstream `stop`): live particles finish their lives, the unborn
        /// never appear, and nothing respawns until play() restores the loop.
        void stop();

        /// Fill the render params consumed by the billboard vertex shader.
        /// Called by the renderer draw branch with the viewport it draws into.
        void prepareRender(const Matrix4& view, const Matrix4& projection,
            const Matrix4& model, float viewportWidth, float viewportHeight);

        /// The output stage the fragment applies (upstream particle_end): the scene's exposure
        /// and tone mapping, and whether the target is a camera frame's linear HDR scene
        /// (tone mapping and gamma left to compose). Called by the renderer per draw.
        void setOutput(float exposure, int toneMapping, bool linearTarget);

        [[nodiscard]] std::unique_ptr<MeshInstance> createMeshInstance(GraphNode* node);

        [[nodiscard]] const std::shared_ptr<VertexBuffer>& particleBuffer() const { return _particleBuffer; }
        [[nodiscard]] const GpuParticleRenderParams& renderParams() const { return _renderParams; }
        [[nodiscard]] const ParticleEmitterOptions& options() const { return _options; }
        [[nodiscard]] uint32_t numParticles() const { return _options.numParticles; }
        [[nodiscard]] bool playing() const { return _playing; }
        void setPlaying(const bool value) { _playing = value; }
        /// Whether dead particles respawn; play() restores it from the options.
        void setLoop(const bool value) { _loop = value; }

    private:
        void createParticleBuffer();
        void createQuadMesh();
        void createMaterial();
        void quantizeCurves();

        std::shared_ptr<GraphicsDevice> _device;
        ParticleEmitterOptions _options;

        std::shared_ptr<VertexBuffer> _particleBuffer;  // GpuParticle pool (compute-written)

        // The simulation step, dispatched through the generic Compute seam rather
        // than a GraphicsDevice virtual. Built lazily on the first update so a
        // device without compute simply never simulates.
        static constexpr uint32_t kSimThreadgroupSize = 256u;  // matches local_size_x

        void simulate(const GpuParticleSimParams& params);
        void step(float dt, const Matrix4& emitterTransform, bool onStop);

        std::shared_ptr<Shader> _simShader;
        std::unique_ptr<Compute> _simCompute;
        bool _simUnavailable = false;
        std::shared_ptr<Mesh> _quadMesh;
        std::shared_ptr<Material> _material;
        std::shared_ptr<Shader> _shader;

        GpuParticleRenderParams _renderParams{};
        GpuParticleSimParams _simParams{};   // the LUTs live here; step() fills the rest
        float _time = 0.0f;
        uint32_t _step = 0;            // simulation steps taken, the kernel's hash seed
        bool _playing = true;
        bool _loop = true;
        bool _stopPending = false;     // the next step hides the unborn (upstream OnStop)
        bool _prewarmPending = false;  // reset() asked for a pre-warm; run at the next update
    };
}
