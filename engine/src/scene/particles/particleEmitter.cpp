// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026.
//
#include "particleEmitter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include <spdlog/spdlog.h>

#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    namespace
    {
        // Standalone Metal shader for particle billboards (gsplat-style branch).
        // One camera-facing quad per particle, driven by [[instance_id]] into the
        // compute-simulated particle pool. DEVIATION: self-contained source —
        // shares no code with the forward PBR chunks.
        // PARTICLE_SHADER_SOURCE is embedded from shaders/metal/embedded/particle-render.metal at build
        // time (see tools/embed_msl.cmake).
#ifdef VISUTWIN_HAS_METAL
#include "embedded_shaders/particle-render.metal.inc"
#else
        constexpr const char* PARTICLE_SHADER_SOURCE = "";
#endif
    }

    ParticleEmitterOptions::ParticleEmitterOptions()
    {
        // Upstream-like defaults: constant size 0.1, white -> transparent fade.
        scaleGraph.add(0.0f, 0.1f);
        colorGraph.curves.resize(3);
        colorGraph.curves[0].add(0.0f, 1.0f);
        colorGraph.curves[1].add(0.0f, 1.0f);
        colorGraph.curves[2].add(0.0f, 1.0f);
        alphaGraph.add(0.0f, 1.0f);
        alphaGraph.add(1.0f, 0.0f);
    }

    ParticleEmitter::ParticleEmitter(const std::shared_ptr<GraphicsDevice>& device,
        const ParticleEmitterOptions& options)
        : _device(device), _options(options)
    {
        _options.numParticles = std::clamp(_options.numParticles, 1u, 1u << 20);
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
        if (poolChanged) {
            createParticleBuffer();
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
        auto quadFormat = std::make_shared<VertexFormat>(14 * static_cast<int>(sizeof(float)), true, false);
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
            ShaderDefinition definition;
            definition.name = "particles";
            definition.vshader = "particleVS";
            definition.fshader = "particleFS";
            _shader = createShader(_device.get(), definition, PARTICLE_SHADER_SOURCE);
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

        auto blendState = std::make_shared<BlendState>();
        blendState->setEnabled(true);
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
        depthState->setDepthTest(true);
        depthState->setDepthWrite(_options.depthWrite);
        _material->setDepthState(depthState);
    }

    void ParticleEmitter::quantizeCurves()
    {
        auto scale = _options.scaleGraph.quantize(kCurveSamples);
        auto alpha = _options.alphaGraph.quantize(kCurveSamples);
        auto color = _options.colorGraph.quantize(kCurveSamples);
        const size_t colorChannels = _options.colorGraph.curves.size();
        for (int i = 0; i < kCurveSamples; ++i) {
            float r = 1.0f, g = 1.0f, b = 1.0f;
            if (colorChannels >= 3) {
                r = color[i * colorChannels + 0];
                g = color[i * colorChannels + 1];
                b = color[i * colorChannels + 2];
            }
            _renderParams.colorLut[i][0] = std::max(r, 0.0f);
            _renderParams.colorLut[i][1] = std::max(g, 0.0f);
            _renderParams.colorLut[i][2] = std::max(b, 0.0f);
            _renderParams.colorLut[i][3] = std::clamp(alpha[i], 0.0f, 1.0f);
            _renderParams.scaleLut[i][0] = std::max(scale[i], 0.0f);
        }
    }

    void ParticleEmitter::reset()
    {
        // Stagger births: particle i is born at t = i * birthInterval. The pool
        // reaches steady state after numParticles * interval seconds.
        const float birthInterval = (_options.rate > 0.0f)
            ? _options.rate
            : std::max(_options.lifetime, _options.lifetime2) / static_cast<float>(_options.numParticles);

        std::vector<GpuParticle> particles(_options.numParticles);
        for (uint32_t i = 0; i < _options.numParticles; ++i) {
            auto& p = particles[i];
            std::memset(&p, 0, sizeof(GpuParticle));
            p.posAge[3] = -(static_cast<float>(i) * birthInterval) - 1e-4f;  // unborn countdown
            p.velLifetime[3] = 0.0f;   // lifetime assigned at birth by the kernel
            p.rotSeedSize[2] = static_cast<float>(i) * 0.61803398875f;       // golden-ratio seed
        }
        std::vector<uint8_t> bytes(particles.size() * sizeof(GpuParticle));
        std::memcpy(bytes.data(), particles.data(), bytes.size());
        _particleBuffer->setData(bytes);
        _time = 0.0f;
    }

    void ParticleEmitter::update(const float dt, const Matrix4& emitterTransform)
    {
        if (!_playing || dt <= 0.0f) {
            return;
        }
        _time += dt;

        GpuParticleSimParams params{};
        params.emitterTransform = _options.localSpace ? Matrix4::identity() : emitterTransform;
        params.gravityDamping[0] = _options.gravity.getX();
        params.gravityDamping[1] = _options.gravity.getY();
        params.gravityDamping[2] = _options.gravity.getZ();
        params.gravityDamping[3] = std::clamp(_options.damping, 0.0f, 1.0f);
        if (_options.emitterShape == ParticleEmitterShape::EMITTERSHAPE_SPHERE) {
            params.shapeParams[0] = std::max(_options.emitterRadius, 0.0f);
            params.shapeParams[3] = 1.0f;
        } else {
            params.shapeParams[0] = _options.emitterExtents.getX();
            params.shapeParams[1] = _options.emitterExtents.getY();
            params.shapeParams[2] = _options.emitterExtents.getZ();
            params.shapeParams[3] = 0.0f;
        }
        params.velocityBase[0] = _options.initialVelocity.getX();
        params.velocityBase[1] = _options.initialVelocity.getY();
        params.velocityBase[2] = _options.initialVelocity.getZ();
        params.velocityBase[3] = _options.localSpace ? 1.0f : 0.0f;
        params.velocitySpread[0] = _options.velocitySpread.getX();
        params.velocitySpread[1] = _options.velocitySpread.getY();
        params.velocitySpread[2] = _options.velocitySpread.getZ();
        params.velocitySpread[3] = _options.loop ? 1.0f : 0.0f;
        const float birthInterval = (_options.rate > 0.0f)
            ? _options.rate
            : std::max(_options.lifetime, _options.lifetime2) / static_cast<float>(_options.numParticles);
        params.timeParams[0] = std::min(dt, 0.1f);   // clamp huge hitches
        params.timeParams[1] = _time;
        params.timeParams[2] = birthInterval;
        params.timeParams[3] = static_cast<float>(_options.numParticles);
        constexpr float degToRad = std::numbers::pi_v<float> / 180.0f;
        params.lifeRot[0] = std::max(_options.lifetime, 1e-4f);
        params.lifeRot[1] = std::max(_options.lifetime2, 1e-4f);
        params.lifeRot[2] = _options.rotationSpeed * degToRad;
        params.lifeRot[3] = _options.rotationSpeed2 * degToRad;
        params.angleParams[0] = _options.startAngle * degToRad;
        params.angleParams[1] = _options.startAngle2 * degToRad;
        params.angleParams[2] = _time;   // per-frame hash seed
        params.angleParams[3] = 1.0f;

        _device->simulateParticles(_particleBuffer, params);
    }

    void ParticleEmitter::prepareRender(const Matrix4& view, const Matrix4& projection,
        const Matrix4& model)
    {
        // Local-space emitters bake the node transform into modelView; world-space
        // particles already live in world coordinates.
        _renderParams.modelView = _options.localSpace ? (view * model) : view;
        _renderParams.projection = projection;
        _renderParams.animParams[0] = static_cast<float>(std::max(_options.animTilesX, 1));
        _renderParams.animParams[1] = static_cast<float>(std::max(_options.animTilesY, 1));
        _renderParams.animParams[2] = static_cast<float>(std::max(_options.animNumFrames, 1));
        _renderParams.animParams[3] = _options.animSpeed;
        _renderParams.miscParams[0] = std::max(_options.intensity, 0.0f);
        _renderParams.miscParams[1] = static_cast<float>(_options.numParticles);
        _renderParams.miscParams[2] = _options.colorMap ? 1.0f : 0.0f;
    }

    std::unique_ptr<MeshInstance> ParticleEmitter::createMeshInstance(GraphNode* node)
    {
        auto meshInstance = std::make_unique<MeshInstance>(_quadMesh, _material, node);
        meshInstance->setCastShadow(false);
        meshInstance->setReceiveShadow(false);
        // Particles move freely (world-space mode ignores the node transform
        // entirely) — skip frustum culling rather than track a moving AABB.
        meshInstance->setCull(false);
        meshInstance->setParticleEmitter(shared_from_this());
        return meshInstance;
    }
}
