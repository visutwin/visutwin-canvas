// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// GPU particle emitter.
//
// Simulation runs entirely on the GPU: a compute kernel (engine/shaders/slang/programs/particle-sim.slang,
// dispatched through the backend-agnostic Compute seam) ages, integrates, and
// respawns particles in a persistent storage buffer; rendering draws one
// camera-facing quad per particle through a self-contained shader
// (gsplat-style renderer branch).
//
// DEVIATIONS from upstream: GPU path only (no CPU sim) — sorting, which upstream
// does on the CPU and which forces its CPU path, is a compute bitonic sort here
// (ParticleSortShaders.h), keyed on the first active camera rather than the one
// rendering the emitter; an initial velocity + spread with gravity/damping beside
// the velocity graphs. colorGraph2 is kept but, as on upstream's GPU path,
// not sampled.
//
#pragma once

#include <memory>
#include <optional>
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

    /// How a quad faces.
    enum class ParticleOrientation : uint32_t
    {
        SCREEN = 0,    // faces the camera
        WORLD = 1,     // faces particleNormal, in world space
        EMITTER = 2    // faces particleNormal turned by the emitter
    };

    /// The draw order of the particles.
    enum class ParticleSort : uint32_t
    {
        NONE = 0,
        DISTANCE = 1,      // farthest from the camera first
        NEWER_FIRST = 2,   // the youngest drawn first, the oldest on top
        OLDER_FIRST = 3
    };

    enum class ParticleBlendType : uint32_t
    {
        BLEND_ADDITIVE = 0,
        BLEND_NORMAL = 1,
        BLEND_PREMULTIPLIED = 2,
        BLEND_NONE = 3           // opaque, drawn with the opaque meshes
    };

    /// Authoring options.
    struct ParticleEmitterOptions
    {
        uint32_t numParticles = 256;      // particle pool size

        float lifetime = 2.0f;            // per-particle lifetime min (seconds)
        float lifetime2 = 2.0f;           // per-particle lifetime max
        // Seconds between births: particle i is born at i * rate, so 0 emits
        // the whole pool at once (a burst), and a looping particle comes back after
        // max(lifetime, numParticles * rate).
        float rate = 1.0f;
        // Each wrap of a particle takes a random rate between rate and
        // rate2 for its emission period. Unset means rate.
        std::optional<float> rate2;
        bool loop = true;                 // one-shot when false
        bool preWarm = false;             // start as if a looping emitter had run one lifetime
        bool autoPlay = true;             // play when built; false leaves it paused and hidden

        ParticleEmitterShape emitterShape = ParticleEmitterShape::EMITTERSHAPE_BOX;
        Vector3 emitterExtents = Vector3(0.0f, 0.0f, 0.0f);  // box size (full extents)
        float emitterRadius = 0.0f;                          // sphere radius

        Vector3 initialVelocity = Vector3(0.0f, 0.0f, 0.0f); // base velocity (units/s)
        Vector3 velocitySpread = Vector3(0.0f);              // ± random per axis
        Vector3 gravity = Vector3(0.0f, 0.0f, 0.0f);
        float damping = 0.0f;             // fraction of velocity lost per second (0..1)

        float startAngle = 0.0f;          // initial billboard rotation min (degrees)
        float startAngle2 = 0.0f;         // initial billboard rotation max
        float rotationSpeed = 0.0f;       // rotation speed min (degrees/s)
        float rotationSpeed2 = 0.0f;      // rotation speed max

        // Velocity graphs over normalized life (units/s): the local graph is
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
        // Speed away from the emitter's centre over normalized life (units/s), a random
        // point between graph and graph2 per particle life.
        Curve radialSpeedGraph;
        Curve radialSpeedGraph2;

        bool alignToMotion = false;       // turn each particle to face its direction of motion
        float stretch = 0.0f;             // pull the trailing vertices back by velocity * stretch

        bool localSpace = false;          // particles follow the emitter node when true
        // The emitter's world transform is already clip space (a child
        // of a screen-space element), and sizes are measured in viewport heights.
        bool screenSpace = false;
        std::vector<int> layers;          // layers to render on (empty: the render component's)

        Curve scaleGraph;                 // quad half-extent over normalized life
        CurveSet colorGraph;              // rgb over normalized life
        Curve alphaGraph;                 // alpha over normalized life
        // A random point between each graph and its graph2 per particle (empty: the
        // graph). colorGraph2 is kept for authoring, but never sampled.
        Curve scaleGraph2;
        Curve alphaGraph2;
        CurveSet colorGraph2;
        float intensity = 1.0f;           // color multiplier (HDR glow)

        // World-space particles wrap around the emitter's position within
        // a box of wrapBounds, so a field of them (rain, snow) follows a moving emitter.
        bool wrap = false;
        Vector3 wrapBounds = Vector3(0.0f);

        // Fade a particle where it nears the scene behind it.
        // Needs the camera's scene depth (CameraComponent::requestSceneDepthMap).
        float depthSoftening = 0.0f;

        // Shade with the light cube (scene ambient and directional
        // lights), from a normal map when one is set, else the quad's own normal.
        bool lighting = false;
        bool halfLambert = false;
        Texture* normalMap = nullptr;

        // Draw this mesh for each particle instead of a quad (its vertex
        // buffer must be the engine's packed 14-float layout: position, normal, uv0 ...).
        std::shared_ptr<Mesh> mesh;
        ParticleOrientation orientation = ParticleOrientation::SCREEN;
        Vector3 particleNormal = Vector3(0.0f, 1.0f, 0.0f);

        ParticleSort sort = ParticleSort::NONE;

        Texture* colorMap = nullptr;      // optional sprite texture (white quad if null)
        int animTilesX = 1;               // sprite-sheet tiles
        int animTilesY = 1;
        int animNumFrames = 1;            // frames played over each particle's life
        // Which animation in the sheet to play. Each animation is animNumFrames
        // tiles long and they run in reading order, so a 4x4 sheet at 4 frames
        // holds four animations, indices 0-3.
        int animIndex = 0;
        float animSpeed = 1.0f;

        ParticleBlendType blendType = ParticleBlendType::BLEND_NORMAL;
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

        /// Stop emitting: live particles finish their lives, the unborn
        /// never appear, and nothing respawns until play() restores the loop.
        void stop();

        /// Fill the render params consumed by the billboard vertex shader.
        /// Called by the renderer draw branch with the viewport it draws into.
        void prepareRender(const Matrix4& view, const Matrix4& projection,
            const Matrix4& model, float viewportWidth, float viewportHeight);

        /// Sort the pool for the camera at `cameraPosition` (world), when the options sort.
        /// Runs a compute pass, so it belongs outside a render pass (the component calls it
        /// after the simulation step).
        void sort(const Vector3& cameraPosition, const Matrix4& emitterTransform);

        /// Light the particles with a light cube: six colours for -X, +X, -Y, +Y,
        /// -Z, +Z (the scene ambient plus each directional light). Filled per draw.
        void setLightCube(const float (&colors)[6][3]);

        /// Depth softening's inputs: the camera's clip planes, and whether the scene
        /// depth grab it reads exists (without one the particles stay hard). Filled per draw.
        void setSoftening(float cameraNear, float cameraFar, bool sceneDepthAvailable);

        /// The output stage the fragment applies: the scene's exposure
        /// and tone mapping, and whether the target is a camera frame's linear HDR scene
        /// (tone mapping and gamma left to compose). Called by the renderer per draw.
        void setOutput(float exposure, int toneMapping, bool linearTarget);

        [[nodiscard]] std::unique_ptr<MeshInstance> createMeshInstance(GraphNode* node);

        [[nodiscard]] const std::shared_ptr<VertexBuffer>& particleBuffer() const { return _particleBuffer; }
        /// The sorted draw order, when the options sort; null otherwise.
        [[nodiscard]] std::shared_ptr<VertexBuffer> orderBuffer() const
        {
            return _options.sort != ParticleSort::NONE && _sorted ? _orderBuffer : nullptr;
        }
        /// A mesh emitter's vertices, bound as storage; null for quads.
        [[nodiscard]] std::shared_ptr<VertexBuffer> meshVertexBuffer() const;
        [[nodiscard]] const GpuParticleRenderParams& renderParams() const { return _renderParams; }
        [[nodiscard]] const ParticleEmitterOptions& options() const { return _options; }
        [[nodiscard]] uint32_t numParticles() const { return _options.numParticles; }
        [[nodiscard]] bool playing() const { return _playing; }
        void setPlaying(const bool value) { _playing = value; }
        /// Whether dead particles respawn; play() restores it from the options.
        void setLoop(const bool value) { _loop = value; }

    private:
        void createParticleBuffer();
        void createSortBuffers();
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
        void step(float dt, const Matrix4& emitterTransform, bool onStop, bool clampDt);

        std::shared_ptr<Shader> _simShader;
        std::unique_ptr<Compute> _simCompute;

        // Sorting: key/index pairs over the pool rounded up to a power of two, and the
        // resulting draw order.
        std::shared_ptr<VertexBuffer> _sortKeys;
        std::shared_ptr<VertexBuffer> _orderBuffer;
        std::shared_ptr<Shader> _sortShader;
        std::unique_ptr<Compute> _sortCompute;
        bool _sorted = false;
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
        bool _stopPending = false;     // the next step hides the unborn
        bool _prewarmPending = false;  // reset() asked for a pre-warm; run at the next update
    };
}
