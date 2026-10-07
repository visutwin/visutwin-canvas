// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026
//
#include "particleEmitter.h"
#include "particleSortShaders.h"
#include "platform/graphics/compute.h"
#include "scene/shader-lib/slangShaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/math/color.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/materials/material.h"
#include "scene/shader-lib/slangShaders.h"

namespace visutwin::canvas
{
    ParticleEmitterOptions::ParticleEmitterOptions()
    {
        // The defaults: constant scale 1, white, opaque.
        scaleGraph.add(0.0f, 1.0f);
        colorGraph.curves.resize(3);
        colorGraph.curves[0].add(0.0f, 1.0f);
        colorGraph.curves[1].add(0.0f, 1.0f);
        colorGraph.curves[2].add(0.0f, 1.0f);
        alphaGraph.add(0.0f, 1.0f);
    }

    ParticleEmitter::ParticleEmitter(const std::shared_ptr<GraphicsDevice>& device,
        const ParticleEmitterOptions& options)
        : _device(device), _options(options)
    {
        _options.numParticles = std::clamp(_options.numParticles, 1u, 1u << 20);
        _loop = _options.loop;
        createParticleBuffer();
        createQuadMesh();
        createMaterial();
        quantizeCurves();
        reset();
    }

    void ParticleEmitter::rebuild(const ParticleEmitterOptions& options)
    {
        const bool poolChanged = options.numParticles != _options.numParticles;
        _options = options;
        _options.numParticles = std::clamp(_options.numParticles, 1u, 1u << 20);
        _loop = _options.loop;
        if (poolChanged) {
            createParticleBuffer();
            _simCompute.reset();
            _orderBuffer.reset();
            reset();
        }
        createMaterial();
        quantizeCurves();
    }

    void ParticleEmitter::createParticleBuffer()
    {
        // Initial pool: all particles unborn, staggered births handled in reset().
        std::vector<uint8_t> bytes(_options.numParticles * sizeof(GpuParticle), 0);
        auto format = std::make_shared<VertexFormat>(static_cast<int>(sizeof(GpuParticle)), true, false);
        VertexBufferOptions bufferOptions;
        bufferOptions.data = std::move(bytes);
        _particleBuffer = _device->createVertexBuffer(format, static_cast<int>(_options.numParticles), bufferOptions);
    }

    void ParticleEmitter::createQuadMesh()
    {
        // 4 dummy vertices, one triangle-strip quad per instance — the vertex
        // shader is [[vertex_id]]-driven (same trick as the gsplat quad).
        auto quadFormat = std::make_shared<VertexFormat>(
            14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
        VertexBufferOptions quadOptions;
        quadOptions.data.assign(4 * 14 * sizeof(float), 0);
        auto quadBuffer = _device->createVertexBuffer(quadFormat, 4, quadOptions);

        _quadMesh = std::make_shared<Mesh>();
        _quadMesh->setVertexBuffer(quadBuffer);
        Primitive primitive;
        primitive.type = PRIMITIVE_TRISTRIP;
        primitive.base = 0;
        primitive.count = 4;
        primitive.indexed = false;
        _quadMesh->setPrimitive(primitive, 0);
    }

    void ParticleEmitter::createMaterial()
    {
        if (!_shader) {
            // The Slang program particle-render, whose fragment stage carries the forward
            // pass's tone mapping operators.
            _shader = getOrCreateSlangShader(_device.get(), "particle-render");
        }
        if (!_material) {
            _material = std::make_shared<Material>();
            _material->setName("particles");
            _material->setShaderOverride(_shader);
            _material->setTransparent(true);
            _material->setCullMode(CullMode::CULLFACE_NONE);
        }

        // Color map binds at fragment texture 0 through the standard material
        // texture path.
        _material->setBaseColorTexture(_options.colorMap);
        _material->setHasBaseColorTexture(_options.colorMap != nullptr);
        // A lit particle's normal map, at the material's normal slot (bound only
        // with lighting on).
        Texture* normalMap = _options.lighting ? _options.normalMap : nullptr;
        _material->setNormalTexture(normalMap);
        _material->setHasNormalTexture(normalMap != nullptr);

        // An opaque emitter (BLEND_NONE) draws in the opaque sublayer with no blending.
        const bool opaque = _options.blendType == ParticleBlendType::BLEND_NONE;
        _material->setTransparent(!opaque);
        auto blendState = std::make_shared<BlendState>();
        blendState->setEnabled(!opaque);
        blendState->setColorOp(BLENDEQUATION_ADD);
        blendState->setAlphaOp(BLENDEQUATION_ADD);
        switch (_options.blendType) {
            case ParticleBlendType::BLEND_ADDITIVE:
                blendState->setColorSrcFactor(BLENDMODE_SRC_ALPHA);
                blendState->setColorDstFactor(BLENDMODE_ONE);
                blendState->setAlphaSrcFactor(BLENDMODE_SRC_ALPHA);
                blendState->setAlphaDstFactor(BLENDMODE_ONE);
                break;
            case ParticleBlendType::BLEND_PREMULTIPLIED:
                blendState->setColorSrcFactor(BLENDMODE_ONE);
                blendState->setColorDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
                blendState->setAlphaSrcFactor(BLENDMODE_ONE);
                blendState->setAlphaDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
                break;
            case ParticleBlendType::BLEND_NONE:
                blendState->setColorSrcFactor(BLENDMODE_ONE);
                blendState->setColorDstFactor(BLENDMODE_ZERO);
                blendState->setAlphaSrcFactor(BLENDMODE_ONE);
                blendState->setAlphaDstFactor(BLENDMODE_ZERO);
                break;
            case ParticleBlendType::BLEND_NORMAL:
            default:
                blendState->setColorSrcFactor(BLENDMODE_SRC_ALPHA);
                blendState->setColorDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
                blendState->setAlphaSrcFactor(BLENDMODE_SRC_ALPHA);
                blendState->setAlphaDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
                break;
        }
        _material->setBlendState(blendState);

        auto depthState = std::make_shared<DepthState>();
        // Screen-space particles draw in the UI's order, like the screen-space elements
        // around them, which do not depth test either.
        depthState->setDepthTest(!_options.screenSpace);
        depthState->setDepthWrite(_options.depthWrite);
        _material->setDepthState(depthState);
    }

    void ParticleEmitter::quantizeCurves()
    {
        auto scale = _options.scaleGraph.quantize(kCurveSamples);
        auto alpha = _options.alphaGraph.quantize(kCurveSamples);
        auto color = _options.colorGraph.quantize(kCurveSamples);
        // graph2: an unset one is the graph itself.
        auto scale2 = _options.scaleGraph2.length() > 0 ? _options.scaleGraph2.quantize(kCurveSamples) : scale;
        auto alpha2 = _options.alphaGraph2.length() > 0 ? _options.alphaGraph2.quantize(kCurveSamples) : alpha;
        const size_t colorChannels = _options.colorGraph.curves.size();
        for (int i = 0; i < kCurveSamples; ++i) {
            float r = 1.0f, g = 1.0f, b = 1.0f;
            if (colorChannels >= 3) {
                r = color[i * colorChannels + 0];
                g = color[i * colorChannels + 1];
                b = color[i * colorChannels + 2];
            }
            // The colour graph is authored in gamma space, like the colour map it
            // multiplies, and the shaders work in linear: decode the rgb here, each
            // sample clamped to [0, 1] first. The shader interpolates between decoded
            // samples, so the ramp is blended in linear space. Alpha is coverage and is
            // not decoded.
            _renderParams.colorLut[i][0] = gammaToLinear(std::clamp(r, 0.0f, 1.0f));
            _renderParams.colorLut[i][1] = gammaToLinear(std::clamp(g, 0.0f, 1.0f));
            _renderParams.colorLut[i][2] = gammaToLinear(std::clamp(b, 0.0f, 1.0f));
            _renderParams.colorLut[i][3] = std::clamp(alpha[i], 0.0f, 1.0f);
            _renderParams.scaleLut[i][0] = std::max(scale[i], 0.0f);
            _renderParams.scaleLut[i][1] = std::max(scale2[i], 0.0f);
            _renderParams.scaleLut[i][2] = std::clamp(alpha2[i], 0.0f, 1.0f);
        }

        // The velocity and rotation-speed graphs go to the simulation. A missing graph2 is
        // the graph itself, and a missing graph contributes zero. "Missing" means
        // no curve has a key: a default CurveSet holds one EMPTY curve, and reading that as a
        // zero graph2 halved every velocity graph on average (a random point between it and 0).
        const auto quantizeSet = [](CurveSet& graph, float (*lut)[4]) {
            const size_t channels = graph.curves.size();
            const bool hasKeys = std::any_of(graph.curves.begin(), graph.curves.end(),
                [](const Curve& curve) { return curve.length() > 0; });
            if (!hasKeys) {
                return false;
            }
            const auto samples = graph.quantize(kCurveSamples);
            for (int i = 0; i < kCurveSamples; ++i) {
                for (size_t c = 0; c < 3; ++c) {
                    lut[i][c] = c < channels ? samples[i * channels + c] : 0.0f;
                }
            }
            return true;
        };
        const auto quantizeRotation = [](Curve& graph, float (*lut)[4]) {
            if (graph.length() == 0) {
                return false;
            }
            constexpr float degToRad = std::numbers::pi_v<float> / 180.0f;
            const auto samples = graph.quantize(kCurveSamples);
            for (int i = 0; i < kCurveSamples; ++i) {
                lut[i][3] = samples[i] * degToRad;
            }
            return true;
        };
        for (auto* lut : {_simParams.localVelocityLut, _simParams.localVelocityLut2,
                          _simParams.velocityLut, _simParams.velocityLut2}) {
            std::memset(lut, 0, sizeof(_simParams.localVelocityLut));
        }
        bool velocityGraphs = quantizeSet(_options.localVelocityGraph, _simParams.localVelocityLut);
        if (!quantizeSet(_options.localVelocityGraph2, _simParams.localVelocityLut2)) {
            std::memcpy(_simParams.localVelocityLut2, _simParams.localVelocityLut, sizeof(_simParams.localVelocityLut));
        } else {
            velocityGraphs = true;
        }
        velocityGraphs |= quantizeSet(_options.velocityGraph, _simParams.velocityLut);
        if (!quantizeSet(_options.velocityGraph2, _simParams.velocityLut2)) {
            std::memcpy(_simParams.velocityLut2, _simParams.velocityLut, sizeof(_simParams.velocityLut));
        } else {
            velocityGraphs = true;
        }
        const bool rotationGraph = quantizeRotation(_options.rotationSpeedGraph, _simParams.localVelocityLut);
        if (!quantizeRotation(_options.rotationSpeedGraph2, _simParams.localVelocityLut2)) {
            for (int i = 0; i < kCurveSamples; ++i) {
                _simParams.localVelocityLut2[i][3] = _simParams.localVelocityLut[i][3];
            }
        }
        _simParams.graphParams[0] = velocityGraphs ? 1.0f : 0.0f;
        _simParams.graphParams[1] = rotationGraph || _options.rotationSpeedGraph2.length() > 0 ? 1.0f : 0.0f;

        // The radial speed pair rides in the world velocity tables' w (units/s).
        const auto quantizeRadial = [](Curve& graph, float (*lut)[4]) {
            if (graph.length() == 0) {
                return false;
            }
            const auto samples = graph.quantize(kCurveSamples);
            for (int i = 0; i < kCurveSamples; ++i) {
                lut[i][3] = samples[i];
            }
            return true;
        };
        bool radialGraph = quantizeRadial(_options.radialSpeedGraph, _simParams.velocityLut);
        if (!quantizeRadial(_options.radialSpeedGraph2, _simParams.velocityLut2)) {
            for (int i = 0; i < kCurveSamples; ++i) {
                _simParams.velocityLut2[i][3] = _simParams.velocityLut[i][3];
            }
        } else {
            radialGraph = true;
        }
        _simParams.graphParams[2] = radialGraph ? 1.0f : 0.0f;
    }

    void ParticleEmitter::reset()
    {
        // Start times: particle i is born at i * rate, so rate 0 is a burst.
        // A life <= 0 is unborn; the kernel places the particle when it is born.
        const float rate = std::max(_options.rate, 0.0f);
        std::vector<GpuParticle> particles(_options.numParticles);
        for (uint32_t i = 0; i < _options.numParticles; ++i) {
            auto& p = particles[i];
            std::memset(&p, 0, sizeof(GpuParticle));
            p.posAge[3] = -static_cast<float>(i) * rate;
            p.velLifetime[3] = 0.0f;   // lifetime assigned at birth by the kernel
        }
        std::vector<uint8_t> bytes(particles.size() * sizeof(GpuParticle));
        std::memcpy(bytes.data(), particles.data(), bytes.size());
        _particleBuffer->setData(bytes);
        _time = 0.0f;
        _stopPending = false;
        // The pre-warm dispatches the simulation, which has to happen inside a frame's
        // update like any other step, so it waits for the next one.
        _prewarmPending = _options.preWarm;
    }

    void ParticleEmitter::stop()
    {
        _loop = false;
        _stopPending = true;
    }

    void ParticleEmitter::update(const float dt, const Matrix4& emitterTransform)
    {
        if (_prewarmPending) {
            // Prewarm: the simulation runs one lifetime in 32 steps.
            _prewarmPending = false;
            constexpr int kPrewarmSteps = 32;
            const float lifetime = std::max(std::max(_options.lifetime, _options.lifetime2), 1e-4f);
            for (int i = 0; i < kPrewarmSteps; ++i) {
                step(lifetime / kPrewarmSteps, emitterTransform, false, false);
            }
        }
        if (!_playing || dt <= 0.0f) {
            return;
        }
        step(dt, emitterTransform, _stopPending, true);
        _stopPending = false;
    }

    void ParticleEmitter::step(const float dt, const Matrix4& emitterTransform, const bool onStop,
        const bool clampDt)
    {
        _time += dt;

        GpuParticleSimParams& params = _simParams;
        params.emitterTransform = _options.localSpace ? Matrix4::identity() : emitterTransform;
        // A world velocity graph is carried into the emitter's space for local particles.
        // (The kernel applies it with w = 0, so its translation never enters.)
        params.worldToEmitter = _options.localSpace ? emitterTransform.inverse() : Matrix4::identity();
        _options.gravity.store(params.gravityDamping);
        params.gravityDamping[3] = std::clamp(_options.damping, 0.0f, 1.0f);
        if (_options.emitterShape == ParticleEmitterShape::EMITTERSHAPE_SPHERE) {
            params.shapeParams[0] = std::max(_options.emitterRadius, 0.0f);
            params.shapeParams[1] = 0.0f;
            params.shapeParams[2] = 0.0f;
            params.shapeParams[3] = 1.0f;
        } else {
            // The kernel spawns at [-1, 1] times these, so the box SIZE goes in halved.
            (_options.emitterExtents * 0.5f).store(params.shapeParams);
            params.shapeParams[3] = 0.0f;
        }
        _options.initialVelocity.store(params.velocityBase);
        params.velocityBase[3] = _options.localSpace ? 1.0f : 0.0f;
        _options.velocitySpread.store(params.velocitySpread);
        params.velocitySpread[3] = _loop ? 1.0f : 0.0f;
        // A frame's step is clamped against huge hitches; the pre-warm's steps are not,
        // since together they have to cover a whole lifetime.
        params.timeParams[0] = clampDt ? std::min(dt, 0.1f) : dt;
        params.timeParams[1] = _time;
        params.timeParams[2] = static_cast<float>(_options.numParticles) * std::max(_options.rate, 0.0f);
        params.graphParams[3] = std::max(_options.rate2.value_or(_options.rate), 0.0f) - std::max(_options.rate, 0.0f);
        params.timeParams[3] = static_cast<float>(_options.numParticles);
        constexpr float degToRad = std::numbers::pi_v<float> / 180.0f;
        params.lifeRot[0] = std::max(_options.lifetime, 1e-4f);
        params.lifeRot[1] = std::max(_options.lifetime2, 1e-4f);
        params.lifeRot[2] = _options.rotationSpeed * degToRad;
        params.lifeRot[3] = _options.rotationSpeed2 * degToRad;
        params.angleParams[0] = _options.startAngle * degToRad;
        params.angleParams[1] = _options.startAngle2 * degToRad;
        // The hash's step counter: a whole number, exact in a float up to 2^24 steps.
        params.angleParams[2] = static_cast<float>(_step++ & 0xFFFFFFu);
        params.angleParams[3] = onStop ? 1.0f : 0.0f;

        simulate(params);
    }

    // One simulation step over the generic Compute seam. The kernel's bindings
    // follow Compute's name-order contract: the single "particles" storage buffer
    // takes binding 0 and the parameter block follows at binding 1.
    void ParticleEmitter::simulate(const GpuParticleSimParams& params)
    {
        if (_simUnavailable || !_device || !_particleBuffer) {
            return;
        }
        if (!_simCompute) {
            if (!_device->supportsCompute()) {
                _simUnavailable = true;
                return;
            }
            _simShader = getOrCreateSlangShader(_device.get(), "particle-sim");
            if (!_simShader) {
                _simUnavailable = true;
                return;
            }
            _simCompute = std::make_unique<Compute>(_device.get(), _simShader, "ParticleSim");
            _simCompute->setParameter("particles", _particleBuffer);
            _simCompute->setThreadgroupSize(kSimThreadgroupSize, 1u, 1u);
        }

        _simCompute->setUniformBlock(&params, sizeof(params));
        const uint32_t count = _options.numParticles;
        _simCompute->setupDispatch(
            (count + kSimThreadgroupSize - 1u) / kSimThreadgroupSize, 1u, 1u);

        Compute* dispatch = _simCompute.get();
        _device->computeDispatch({dispatch}, "particle-sim");
    }

    void ParticleEmitter::prepareRender(const Matrix4& view, const Matrix4& projection,
        const Matrix4& model, const float viewportWidth, const float viewportHeight)
    {
        if (_options.screenSpace) {
            // Screen space: the node's world transform lands in clip
            // space already, so neither the camera's view nor its projection applies.
            _renderParams.modelView = _options.localSpace ? model : Matrix4::identity();
            _renderParams.projection = Matrix4::identity();
        } else {
            // Local-space emitters bake the node transform into modelView; world-space
            // particles already live in world coordinates.
            _renderParams.modelView = _options.localSpace ? (view * model) : view;
            _renderParams.projection = projection;
        }
        _renderParams.animParams[0] = static_cast<float>(std::max(_options.animTilesX, 1));
        _renderParams.animParams[1] = static_cast<float>(std::max(_options.animTilesY, 1));
        _renderParams.animParams[2] = static_cast<float>(std::max(_options.animNumFrames, 1));
        _renderParams.animParams[3] = _options.animSpeed;
        _renderParams.miscParams[0] = std::max(_options.intensity, 0.0f);
        _renderParams.miscParams[1] = static_cast<float>(_options.numParticles);
        _renderParams.miscParams[2] = _options.colorMap ? 1.0f : 0.0f;
        _renderParams.miscParams[3] = static_cast<float>(std::max(_options.animIndex, 0));
        _renderParams.motionParams[0] = _options.alignToMotion ? 1.0f : 0.0f;
        _renderParams.motionParams[1] = std::max(_options.stretch, 0.0f);
        _renderParams.motionParams[2] = _options.screenSpace ? 1.0f : 0.0f;
        // A screen-space quad's x is scaled by height / width to stay square.
        _renderParams.motionParams[3] = viewportWidth > 0.0f && viewportHeight > 0.0f
            ? viewportHeight / viewportWidth : 1.0f;
        _renderParams.view = view;
        _renderParams.outputParams[3] = orderBuffer() ? 1.0f : 0.0f;

        // A WORLD or EMITTER oriented quad lies in the
        // plane of particleNormal (turned by the emitter for EMITTER).
        const bool useMesh = meshVertexBuffer() != nullptr;
        if (_options.orientation != ParticleOrientation::SCREEN && !_options.screenSpace) {
            Vector3 n = _options.orientation == ParticleOrientation::WORLD
                ? _options.particleNormal
                : Vector3(model * Vector4(_options.particleNormal.getX(), _options.particleNormal.getY(),
                      _options.particleNormal.getZ(), 0.0f));
            n = n.lengthSquared() > 1e-12f ? n.normalized() : Vector3(0.0f, 1.0f, 0.0f);
            Vector3 t(1.0f, 0.0f, 0.0f);
            if (std::abs(t.dot(n)) == 1.0f) {
                t = Vector3(0.0f, 0.0f, 1.0f);
            }
            const Vector3 b = n.cross(t).normalized();
            t = b.cross(n).normalized();
            t.store(_renderParams.faceTangent);
            b.store(_renderParams.faceBinorm);
            _renderParams.faceTangent[3] = 1.0f;
        } else {
            _renderParams.faceTangent[3] = 0.0f;
        }
        _renderParams.faceBinorm[3] = useMesh ? 1.0f : 0.0f;

        // Wrap: GPU, world-space particles only, around the emitter's position.
        const bool wrap = _options.wrap && !_options.localSpace && !_options.screenSpace &&
            _options.wrapBounds.getX() > 0.0f && _options.wrapBounds.getY() > 0.0f && _options.wrapBounds.getZ() > 0.0f;
        _options.wrapBounds.store(_renderParams.wrapParams);
        _renderParams.wrapParams[3] = wrap ? 1.0f : 0.0f;
        Vector3(model.getColumn(3)).store(_renderParams.emitterPosition);
        _renderParams.emitterPosition[3] = _options.localSpace ? 1.0f : 0.0f;

        _renderParams.lightCube[0][3] = _options.lighting && !_options.screenSpace ? 1.0f : 0.0f;
        _renderParams.lightCube[1][3] = _options.halfLambert ? 1.0f : 0.0f;
        _renderParams.lightCube[2][3] = _options.lighting && _options.normalMap ? 1.0f : 0.0f;
    }

    void ParticleEmitter::setLightCube(const float (&colors)[6][3])
    {
        for (int i = 0; i < 6; ++i) {
            for (int c = 0; c < 3; ++c) {
                _renderParams.lightCube[i][c] = colors[i][c];
            }
        }
    }

    void ParticleEmitter::setSoftening(const float cameraNear, const float cameraFar, const bool sceneDepthAvailable)
    {
        // The softening is remapped to be more perceptually linear: 1 / (s^2 * 100).
        const float s = _options.depthSoftening;
        _renderParams.softParams[0] = s > 0.0f ? 1.0f / (s * s * 100.0f) : 0.0f;
        _renderParams.softParams[1] = cameraNear;
        _renderParams.softParams[2] = cameraFar;
        _renderParams.softParams[3] = s > 0.0f && !_options.screenSpace && sceneDepthAvailable ? 1.0f : 0.0f;
    }

    std::shared_ptr<VertexBuffer> ParticleEmitter::meshVertexBuffer() const
    {
        if (!_options.mesh) {
            return nullptr;
        }
        auto vertexBuffer = _options.mesh->getVertexBuffer();
        // The shader reads the engine's packed vertex: position, normal, uv0 in the first
        // eight of 14 floats.
        if (!vertexBuffer || !vertexBuffer->format() ||
            vertexBuffer->format()->size() != 14 * static_cast<int>(sizeof(float))) {
            return nullptr;
        }
        return vertexBuffer;
    }

    void ParticleEmitter::createSortBuffers()
    {
        uint32_t size = 1u;
        while (size < _options.numParticles) {
            size <<= 1u;
        }
        auto keyFormat = std::make_shared<VertexFormat>(static_cast<int>(2 * sizeof(float)), true, false);
        VertexBufferOptions keyOptions;
        keyOptions.data.assign(size * 2 * sizeof(float), 0);
        _sortKeys = _device->createVertexBuffer(keyFormat, static_cast<int>(size), keyOptions);

        auto orderFormat = std::make_shared<VertexFormat>(static_cast<int>(sizeof(uint32_t)), true, false);
        VertexBufferOptions orderOptions;
        orderOptions.data.assign(_options.numParticles * sizeof(uint32_t), 0);
        _orderBuffer = _device->createVertexBuffer(orderFormat, static_cast<int>(_options.numParticles), orderOptions);
        _sortCompute.reset();
        _sorted = false;
    }

    void ParticleEmitter::sort(const Vector3& cameraPosition, const Matrix4& emitterTransform)
    {
        if (_options.sort == ParticleSort::NONE || !_particleBuffer || !_device || !_device->supportsCompute()) {
            return;
        }
        if (!_orderBuffer || _orderBuffer->numVertices() != static_cast<int>(_options.numParticles)) {
            createSortBuffers();
        }
        if (!_sortShader) {
            _sortShader = getOrCreateSlangShader(_device.get(), "particle-sort");
            if (!_sortShader) {
                return;
            }
        }
        if (!_sortCompute) {
            _sortCompute = std::make_unique<Compute>(_device.get(), _sortShader, "ParticleSort");
            _sortCompute->setParameter("order", _orderBuffer);
            _sortCompute->setParameter("particles", _particleBuffer);
            _sortCompute->setParameter("sortKeys", _sortKeys);
            _sortCompute->setThreadgroupSize(particle_sort_shaders::kSortThreads, 1u, 1u);
            _sortCompute->setupDispatch(1u, 1u, 1u);
        }

        // A local-space pool is in the emitter's space, and so is the camera then.
        const Vector3 camera = _options.localSpace
            ? emitterTransform.inverse().transformPoint(cameraPosition) : cameraPosition;
        particle_sort_shaders::SortUniforms uniforms{};
        camera.store(uniforms.cameraPosition);
        uniforms.cameraPosition[3] = static_cast<float>(_options.sort);
        uniforms.counts[0] = static_cast<float>(_options.numParticles);
        uniforms.counts[1] = static_cast<float>(_sortKeys->numVertices());
        _sortCompute->setUniformBlock(&uniforms, sizeof(uniforms));
        Compute* dispatch = _sortCompute.get();
        _device->computeDispatch({dispatch}, "particle-sort");
        _sorted = true;
    }

    void ParticleEmitter::setOutput(const float exposure, const int toneMapping, const bool linearTarget)
    {
        _renderParams.outputParams[0] = std::max(exposure, 0.0f);
        _renderParams.outputParams[1] = static_cast<float>(toneMapping);
        _renderParams.outputParams[2] = linearTarget ? 1.0f : 0.0f;
    }

    std::unique_ptr<MeshInstance> ParticleEmitter::createMeshInstance(GraphNode* node)
    {
        // A mesh emitter draws its mesh once per particle; its vertices are
        // also read as storage, by index.
        const bool useMesh = meshVertexBuffer() != nullptr;
        if (_options.mesh && !useMesh) {
            spdlog::warn("ParticleEmitter: the particle mesh is not the packed 14-float vertex layout; drawing quads");
        }
        auto meshInstance = std::make_unique<MeshInstance>(useMesh ? _options.mesh : _quadMesh, _material, node);
        meshInstance->setCastShadow(false);
        meshInstance->setReceiveShadow(false);
        // Particles move freely (world-space mode ignores the node transform
        // entirely) — skip frustum culling rather than track a moving AABB.
        meshInstance->setCull(false);
        meshInstance->setParticleEmitter(shared_from_this());
        // Screen space skips culling, shadows and depth passes and draws on any camera,
        // like a screen-space element.
        meshInstance->setScreenSpace(_options.screenSpace);
        return meshInstance;
    }
}
