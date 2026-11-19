// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#include "probeVolumeBaker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "core/math/halfFloat.h"
#include "core/math/quaternion.h"
#include "extras/probeVolume/probeVolume.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/componentRegistry.h"
#include "framework/components/componentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureSamplerState.h"
#include "scene/composition/layerComposition.h"
#include "scene/graphics/quadRender.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "scene/shader-lib/slangShaders.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // The rotations of the cameras capturing the six faces of a probe, as Euler angles: the
        // faces see along probeFaceDirection of common-probe-bake.slang.
        constexpr float kFaceEulers[6][3] = {
            {0.0f, 90.0f, 0.0f},
            {0.0f, -90.0f, 0.0f},
            {-90.0f, 0.0f, 180.0f},
            {90.0f, 0.0f, 180.0f},
            {0.0f, 180.0f, 0.0f},
            {0.0f, 0.0f, 0.0f},
        };

        constexpr int kFloatsPerProbe = ProbeVolume::kFloatsPerProbe;

        // The widest target receiving the octahedral maps of the probes.
        constexpr uint32_t kVisibilityTargetWidth = 2048;
        // The width of the texture of the probes' positions.
        constexpr uint32_t kPositionsWidth = 1024;
        // The texels along a side of a face of the sky atlas.
        constexpr uint32_t kSkyFaceSize = 32;
        // The layer of the quad lighting the geometry, which nothing else renders.
        constexpr int kLightingLayerId = 0x7ffe0001;

        // The projection modes of probe-volume-project.slang.
        constexpr int kProjectSh = 0;
        constexpr int kProjectValidity = 1;
        constexpr int kProjectVisibility = 2;

        // A pass of the baker's own quads, outside the frame graph. The device handle is
        // non-owning: the pass lives within one call and the device outlives it.
        struct BakePass final : RenderPass
        {
            BakePass(GraphicsDevice* device, const char* name)
                : RenderPass(std::shared_ptr<GraphicsDevice>(device, [](GraphicsDevice*) {}))
            {
                _name = name;
            }
        };

        std::shared_ptr<Texture> createAtlasTexture(GraphicsDevice* device, const char* name, const uint32_t width,
            const uint32_t height, const PixelFormat format, const bool linear = false)
        {
            TextureOptions options;
            options.name = name;
            options.width = width;
            options.height = height;
            options.format = format;
            options.mipmaps = false;
            options.minFilter = linear ? FilterMode::FILTER_LINEAR : FilterMode::FILTER_NEAREST;
            options.magFilter = linear ? FilterMode::FILTER_LINEAR : FilterMode::FILTER_NEAREST;
            auto texture = std::make_shared<Texture>(device, options);
            texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            return texture;
        }

        // Frees an entity the baker made: destroyed, then let go by its parent, which owns it.
        void freeEntity(Entity*& entity)
        {
            if (!entity) {
                return;
            }
            entity->destroy();
            if (entity->parent()) {
                const auto owned = entity->remove();
            } else {
                delete entity;
            }
            entity = nullptr;
        }

        uint32_t laneOf(const Texture* texture)
        {
            return texture ? packBindlessLane(texture->bindlessIndex(),
                BindlessSamplers::indexOf(textureSamplerState(*texture, 1.0f))) : 0u;
        }

        // A camera rect in the normalized, bottom-up form, for a rect of texels counted from the
        // top-left corner; a quarter texel in, so that the truncation to pixels lands on the texel.
        Vector4 cameraRect(const std::array<uint32_t, 4>& texels, const uint32_t width, const uint32_t height)
        {
            const float w = static_cast<float>(width);
            const float h = static_cast<float>(height);
            const float bottom = static_cast<float>(height - texels[1] - texels[3]);
            return Vector4((static_cast<float>(texels[0]) + 0.25f) / w, (bottom + 0.25f) / h,
                static_cast<float>(texels[2]) / w, static_cast<float>(texels[3]) / h);
        }
    }

    ProbeVolumeBaker::ProbeVolumeBaker(Engine* engine, ProbeVolume* volume, const ProbeVolumeBakerOptions& options)
        : _engine(engine), _device(engine ? engine->graphicsDevice().get() : nullptr), _volume(volume),
          _faceSize(std::max(options.faceSize, 1u)), _layers(options.layers), _emissive(options.emissive),
          _probesPerFrame(options.probesPerFrame), _nearClip(options.nearClip), _farClip(options.farClip)
    {
    }

    ProbeVolumeBaker::~ProbeVolumeBaker()
    {
        if (_baking) {
            _steps.clear();
            finish();
        }
        destroyLighting();
        destroyGeometry();
        for (CaptureRig& rig : _rigs) {
            freeEntity(rig.entity);
        }
        _rigs.clear();
    }

    std::optional<ProbeVolumeBaker::Formats> ProbeVolumeBaker::formatsFor(const GraphicsDevice* device)
    {
        // Half floats preferred for the radiance and the geometry, full floats for the precision
        // of the targets receiving the projections.
        if (!device) {
            return std::nullopt;
        }
        const bool half = device->textureHalfFloatRenderable();
        const bool full = device->textureFloatRenderable();
        if (!half && !full) {
            return std::nullopt;
        }
        Formats formats;
        formats.cubemap = half ? PixelFormat::PIXELFORMAT_RGBA16F : PixelFormat::PIXELFORMAT_RGBA32F;
        formats.target = full ? PixelFormat::PIXELFORMAT_RGBA32F : PixelFormat::PIXELFORMAT_RGBA16F;
        return formats;
    }

    bool ProbeVolumeBaker::isSupported(const GraphicsDevice* device)
    {
        return device && device->reportsUnsupportedFeatures() && formatsFor(device).has_value();
    }

    bool ProbeVolumeBaker::begin()
    {
        if (_baking) {
            spdlog::warn("ProbeVolumeBaker: a capture or bake is already in progress");
            return false;
        }
        const auto formats = isSupported(_device) ? formatsFor(_device) : std::nullopt;
        if (!formats || !_engine || !_volume) {
            spdlog::error("ProbeVolumeBaker: the device cannot bake probes, as it does not render to float "
                          "textures, see ProbeVolumeBaker::isSupported");
            return false;
        }
        _formats = *formats;
        _baking = true;
        if (!_updateHandle) {
            _updateHandle = _engine->on("update", [this]() { onUpdate(); });
        }
        return true;
    }

    void ProbeVolumeBaker::finish()
    {
        _baking = false;
        if (_updateHandle) {
            _updateHandle->off();
            _updateHandle.reset();
        }
    }

    void ProbeVolumeBaker::onUpdate()
    {
        while (!_steps.empty()) {
            // A step may queue steps of its own at the front, so it is taken off first.
            Step step = std::move(_steps.front());
            _steps.pop_front();
            if (step() == StepResult::Wait) {
                _steps.push_front(std::move(step));
                return;
            }
        }
        finish();
    }

    bool ProbeVolumeBaker::captureGeometry(const ProbeVolumeCaptureOptions& options)
    {
        if (!begin()) {
            return false;
        }
        queueCapture(options);
        return true;
    }

    bool ProbeVolumeBaker::bakeLighting(const ProbeVolumeBakeOptions& options)
    {
        if (!begin()) {
            return false;
        }
        if (!hasGeometry()) {
            queueCapture({});
        }
        queueBake(options);
        return true;
    }

    void ProbeVolumeBaker::queueCapture(const ProbeVolumeCaptureOptions& options)
    {
        struct State
        {
            ProbeVolumeCaptureOptions options;
            ProbeVolumeGeometryResult result;
            std::vector<float> positions;
            std::vector<float> pixels;
            std::vector<float> offsets;
            std::vector<float> movedPositions;
            std::vector<uint32_t> moved;
            std::vector<uint8_t> probeValid;
            bool validity = true;
            bool virtualOffset = true;
        };
        auto state = std::make_shared<State>();
        state->options = options;
        state->validity = options.validity;
        state->virtualOffset = options.validity && options.virtualOffset;
        const auto progress = [state](const float start, const float end) {
            return [state, start, end](const float p) {
                if (state->options.onProgress) {
                    state->options.onProgress(start + (end - start) * p);
                }
            };
        };

        // The atlases, the rigs and the positions the probes are captured at.
        _steps.push_back([this, state]() {
            const uint32_t count = _volume->probeCount();
            destroyGeometry();
            createAtlases();
            createRigs();
            state->positions.resize(static_cast<size_t>(count) * 3);
            for (uint32_t i = 0; i < count; ++i) {
                _volume->probePosition(i).store(&state->positions[i * 3]);
            }
            state->result.probeCount = count;
            state->probeValid.assign(count, 1u);
            return StepResult::Done;
        });

        // Every probe at its position: the capture cameras render the back faces, which make
        // probes invalid, and the blended materials do not render, as the geometry cannot be blended.
        const auto captureAll = [this, state, progress]() {
            std::vector<uint32_t> all(_volume->probeCount());
            for (uint32_t i = 0; i < all.size(); ++i) {
                all[i] = i;
            }
            _steps.push_front(captureTiles(_geometryTarget.get(), all, state->positions,
                {_geometryTarget.get(), true, {}}, progress(0.0f, 0.7f)));
            return StepResult::Done;
        };
        _steps.push_back(captureAll);

        _steps.push_back([this, state, progress]() {
            if (!state->validity) {
                return StepResult::Done;
            }
            const uint32_t count = _volume->probeCount();
            const float threshold = state->options.validityThreshold;
            state->pixels = project(kProjectValidity, 1, _volume->resolution()[0]);
            if (state->pixels.size() < static_cast<size_t>(count) * 4) {
                spdlog::error("ProbeVolumeBaker: the validity could not be read back");
                state->validity = false;
                return StepResult::Done;
            }
            if (!state->virtualOffset) {
                return StepResult::Done;
            }
            // The invalid probes moved out through the closest back face by up to 0.45 of a cell,
            // so that a moved probe stays closer to its own position than to its neighbours' (DDGI).
            const Vector3 cellVector = _volume->cellSize();
            const float cell = std::min({cellVector.getX(), cellVector.getY(), cellVector.getZ()});
            const float maxOffset = cell * 0.45f;
            const float margin = cell * 0.1f;
            state->offsets.assign(static_cast<size_t>(count) * 3, 0.0f);
            for (uint32_t probe = 0; probe < count; ++probe) {
                if (state->pixels[probe * 4] > threshold) {
                    const float x = state->pixels[probe * 4 + 1];
                    const float y = state->pixels[probe * 4 + 2];
                    const float z = state->pixels[probe * 4 + 3];
                    const float distance = std::sqrt(x * x + y * y + z * z);
                    if (distance > 0.0f && distance + margin <= maxOffset) {
                        const float scale = (distance + margin) / distance;
                        state->offsets[probe * 3] = x * scale;
                        state->offsets[probe * 3 + 1] = y * scale;
                        state->offsets[probe * 3 + 2] = z * scale;
                        state->moved.push_back(probe);
                    }
                }
            }
            if (state->moved.empty()) {
                return StepResult::Done;
            }

            // The geometry at the moved positions, kept for the moves that made the probes
            // valid, and captured again at the grid positions for the others.
            state->movedPositions = state->positions;
            for (size_t i = 0; i < state->movedPositions.size(); ++i) {
                state->movedPositions[i] += state->offsets[i];
            }
            _steps.push_front([this, state, progress]() {
                const float threshold = state->options.validityThreshold;
                const std::vector<float> movedPixels = project(kProjectValidity, 1, _volume->resolution()[0]);
                std::vector<uint32_t> reverted;
                for (const uint32_t probe : state->moved) {
                    if (probe * 4 < movedPixels.size() && movedPixels[probe * 4] <= threshold) {
                        state->pixels[probe * 4] = movedPixels[probe * 4];
                        std::copy_n(&state->movedPositions[probe * 3], 3, &state->positions[probe * 3]);
                        state->result.movedProbes++;
                    } else {
                        std::fill_n(&state->offsets[probe * 3], 3, 0.0f);
                        reverted.push_back(probe);
                    }
                }
                if (!reverted.empty()) {
                    _steps.push_front(captureTiles(_geometryTarget.get(), reverted, state->positions,
                        {_geometryTarget.get(), true, {}}, progress(0.8f, 0.85f)));
                }
                return StepResult::Done;
            });
            _steps.push_front(captureTiles(_geometryTarget.get(), state->moved, state->movedPositions,
                {_geometryTarget.get(), true, {}}, progress(0.7f, 0.8f)));
            return StepResult::Done;
        });

        _steps.push_back([this, state]() {
            const uint32_t count = _volume->probeCount();
            if (state->validity) {
                if (state->virtualOffset) {
                    _volume->setOffsets(state->result.movedProbes > 0 ? std::span<const float>(state->offsets)
                                                                      : std::span<const float>());
                }
                for (uint32_t probe = 0; probe < count; ++probe) {
                    if (state->pixels[probe * 4] > state->options.validityThreshold) {
                        state->probeValid[probe] = 0;
                        state->result.invalidProbes++;
                    }
                }
            }
            // The offsets of an earlier capture that moved probes do not apply to this one.
            if (!state->virtualOffset) {
                _volume->setOffsets({});
            }
            _volume->setValidity(state->probeValid);
            _probeValid = state->probeValid;
            _positions = state->positions;
            updatePositionsTexture();

            // The visibility, at the positions the probes are captured at.
            if (state->options.visibility) {
                const uint32_t size = std::max(state->options.visibilitySize, 1u);
                const std::vector<float> visibility = captureVisibility(size);
                _volume->setVisibility(visibility, visibility.empty() ? 0u : size);
            } else {
                _volume->setVisibility({});
            }
            if (state->options.onProgress) {
                state->options.onProgress(1.0f);
            }
            if (state->options.onComplete) {
                state->options.onComplete(state->result);
            }
            return StepResult::Done;
        });
    }

    void ProbeVolumeBaker::queueBake(const ProbeVolumeBakeOptions& options)
    {
        struct State
        {
            ProbeVolumeBakeOptions options;
            ProbeVolumeBakeResult result;
            uint32_t pass = 0;
        };
        auto state = std::make_shared<State>();
        state->options = options;

        _steps.push_back([this, state]() {
            _volume->setBakeData(std::vector<float>(static_cast<size_t>(_volume->probeCount()) * kFloatsPerProbe, 0.0f));

            // The directional lights render one shadow map for every camera, which the lighting
            // of the geometry needs, as its camera is not at the probes.
            std::optional<BoundingBox> bounds;
            if (!state->options.shadowBounds.has_value()) {
                const float blend = _volume->blendDistance();
                bounds = BoundingBox(_volume->aabb().center(), _volume->aabb().halfExtents() + Vector3(blend));
            } else {
                bounds = *state->options.shadowBounds;
            }
            _shadowLights = setShadowBounds(_layers, bounds);
            return StepResult::Done;
        });

        // The sky, seen through the texels without geometry, captured for each bake so that the
        // lighting follows the environment.
        if (options.source == ProbeSource::GBuffer) {
            _steps.push_back([this]() {
                _steps.push_front(captureSky());
                return StepResult::Done;
            });
            _steps.push_back([this]() {
                createLighting();
                return StepResult::Done;
            });
        } else {
            _steps.push_back([this]() {
                createRigs();
                return StepResult::Done;
            });
        }

        // A pass renders the radiance of the probes and projects it onto their spherical
        // harmonics, which the next pass lights the scene with.
        for (uint32_t pass = 0; pass <= options.bounces; ++pass) {
            const float start = static_cast<float>(pass) / static_cast<float>(options.bounces + 1);
            const float span = 1.0f / static_cast<float>(options.bounces + 1);
            if (options.source == ProbeSource::GBuffer) {
                // One frame with the lighting camera on.
                auto rendered = std::make_shared<bool>(false);
                _steps.push_back([this, rendered]() {
                    if (!*rendered) {
                        *rendered = true;
                        if (_lightingCamera) {
                            _lightingCamera->setEnabled(true);
                        }
                        return StepResult::Wait;
                    }
                    if (_lightingCamera) {
                        _lightingCamera->setEnabled(false);
                    }
                    return StepResult::Done;
                });
            } else {
                _steps.push_back([this, state, start, span]() {
                    std::vector<uint32_t> all(_volume->probeCount());
                    for (uint32_t i = 0; i < all.size(); ++i) {
                        all[i] = i;
                    }
                    _steps.push_front(captureTiles(_radianceTarget.get(), all, _positions,
                        {_radianceTarget.get(), false, _layers}, [state, start, span](const float p) {
                            if (state->options.onProgress) {
                                state->options.onProgress(start + p * span);
                            }
                        }));
                    return StepResult::Done;
                });
            }
            _steps.push_back([this, state, pass]() {
                const uint32_t count = _volume->probeCount();
                std::vector<float> data = project(kProjectSh, 3, _volume->resolution()[0]);
                data.resize(static_cast<size_t>(count) * kFloatsPerProbe, 0.0f);
                if (state->options.dilation) {
                    state->result.dilatedProbes = dilate(data, _probeValid, _volume->resolution());
                }
                if (pass == state->options.bounces) {
                    _volume->setData(data, state->options.index);
                } else {
                    _volume->setBakeData(data);
                }
                if (state->options.onProgress) {
                    state->options.onProgress(static_cast<float>(pass + 1) / static_cast<float>(state->options.bounces + 1));
                }
                return StepResult::Done;
            });
        }

        _steps.push_back([this, state]() {
            _volume->setBakeData({});
            for (const ShadowLight& entry : _shadowLights) {
                entry.light->setShadowBounds(entry.bounds);
            }
            _shadowLights.clear();
            destroyLighting();
            if (state->options.onComplete) {
                state->options.onComplete(state->result);
            }
            return StepResult::Done;
        });
    }

    void ProbeVolumeBaker::createAtlases()
    {
        const uint32_t size = _faceSize;
        const uint32_t count = _volume->probeCount();
        const uint32_t maxSize = static_cast<uint32_t>(std::max(_device->maxTextureSize(), 1));
        const uint32_t probesPerRow = std::max(1u, std::min(count, maxSize / (size * 3)));
        const uint32_t width = probesPerRow * size * 3;
        const uint32_t height = (count + probesPerRow - 1) / probesPerRow * size * 2;
        if (height > maxSize) {
            spdlog::error("ProbeVolumeBaker: the atlas of {} probes of face size {} exceeds the maximum texture size {}",
                count, size, maxSize);
        }
        _atlas = {size, probesPerRow, width, height};

        // The albedo and the hit flag; the back face flag, the distance and the octahedral
        // normal; and the emission.
        _geometryBuffers = {createAtlasTexture(_device, "ProbeVolumeGeometry0", width, height, PixelFormat::PIXELFORMAT_RGBA8),
            createAtlasTexture(_device, "ProbeVolumeGeometry1", width, height, PixelFormat::PIXELFORMAT_RGBA16F)};
        if (_emissive) {
            _geometryBuffers.push_back(
                createAtlasTexture(_device, "ProbeVolumeGeometry2", width, height, PixelFormat::PIXELFORMAT_RGBA16F));
        }
        RenderTargetOptions geometry;
        geometry.graphicsDevice = _device;
        geometry.name = "ProbeVolumeGeometry";
        for (const auto& buffer : _geometryBuffers) {
            geometry.colorBuffers.push_back(buffer.get());
        }
        geometry.depth = true;
        _geometryTarget = _device->createRenderTarget(geometry);

        _radianceBuffer = createAtlasTexture(_device, "ProbeVolumeRadiance", width, height, _formats.cubemap);
        RenderTargetOptions radiance;
        radiance.graphicsDevice = _device;
        radiance.name = "ProbeVolumeRadiance";
        radiance.colorBuffer = _radianceBuffer.get();
        radiance.depth = true;
        _radianceTarget = _device->createRenderTarget(radiance);
    }

    void ProbeVolumeBaker::destroyGeometry()
    {
        _geometryTarget.reset();
        _geometryBuffers.clear();
        _radianceTarget.reset();
        _radianceBuffer.reset();
        _skyTarget.reset();
        _skyAtlas.reset();
        _positionsTexture.reset();
        _positions.clear();
        _atlas = {};
    }

    void ProbeVolumeBaker::createRigs()
    {
        const uint32_t count = std::max(1u, std::min(_probesPerFrame, _volume->probeCount()));
        while (_rigs.size() < count) {
            CaptureRig rig;
            rig.entity = new Entity();
            rig.entity->setName("ProbeVolumeCapture" + std::to_string(_rigs.size()));
            rig.entity->setEngine(_engine);
            for (int face = 0; face < 6; ++face) {
                auto* faceEntity = new Entity();
                faceEntity->setName("Face" + std::to_string(face));
                faceEntity->setEngine(_engine);
                auto* camera = static_cast<CameraComponent*>(faceEntity->addComponent<CameraComponent>());
                Camera* c = camera->camera();
                c->setFov(90.0f);
                c->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
                c->setAspectRatio(1.0f);
                c->setNearClip(_nearClip);
                c->setFarClip(_farClip);
                c->setLinearOutput(true);
                c->setToneMapping(TONEMAP_NONE);
                c->setProbeVolumeCapture(true);
                // The baker clears each tile itself: a camera's clear is the whole atlas's.
                c->setClearColorBuffer(false);
                c->setClearDepthBuffer(false);
                c->setClearStencilBuffer(false);
                camera->setPriority(-100);
                faceEntity->setLocalRotation(Quaternion::fromEulerAngles(kFaceEulers[face][0], kFaceEulers[face][1],
                    kFaceEulers[face][2]));
                rig.entity->addChild(faceEntity);
                rig.cameras.push_back(camera);
            }
            rig.entity->setEnabled(false);
            _engine->root()->addChild(rig.entity);
            _rigs.push_back(std::move(rig));
        }
    }

    std::array<uint32_t, 4> ProbeVolumeBaker::tileRect(const uint32_t probe) const
    {
        const uint32_t size = _atlas.faceSize;
        return {(probe % _atlas.probesPerRow) * size * 3, (probe / _atlas.probesPerRow) * size * 2, size * 3, size * 2};
    }

    void ProbeVolumeBaker::clearTiles(RenderTarget* target, const std::vector<std::array<uint32_t, 4>>& rects,
        const bool geometry) const
    {
        if (!target || rects.empty()) {
            return;
        }
        const int outputs = std::clamp(target->colorBufferCount(), 1, 3);
        const char* variants[3] = {"mrt1", "mrt2", "mrt3"};
        const auto shader = getOrCreateSlangShader(_device, "probe-volume-clear", variants[outputs - 1]);
        if (!shader) {
            return;
        }
        static const auto clearDepth = [] {
            auto state = std::make_shared<DepthState>();
            state->setFunc(CompareFunction::Always);
            return state;
        }();
        // The geometry clears to no hit, the radiance to black.
        const float color[4] = {0.0f, 0.0f, 0.0f, geometry ? 0.0f : 1.0f};
        QuadRender quad(shader);
        quad.setUniformData(color, sizeof(color));

        BakePass pass(_device, "ProbeVolumeClear");
        pass.init(std::shared_ptr<RenderTarget>(target, [](RenderTarget*) {}));
        _device->beginOfflineWork();
        _device->startRenderPass(&pass);
        _device->setBlendState(BlendState::noBlend());
        _device->setDepthState(clearDepth);
        _device->setCullMode(CullMode::CULLFACE_NONE);
        _device->setStencilState();
        for (const auto& rect : rects) {
            const Vector4 viewport(static_cast<float>(rect[0]), static_cast<float>(rect[1]),
                static_cast<float>(rect[2]), static_cast<float>(rect[3]));
            quad.render(&viewport);
        }
        _device->endRenderPass(&pass);
        _device->endOfflineWork();
    }

    ProbeVolumeBaker::Step ProbeVolumeBaker::captureTiles(RenderTarget* target, std::vector<uint32_t> probes,
        std::vector<float> positions, const CaptureSetup& setup, std::function<void(float)> onProgress)
    {
        struct State
        {
            std::vector<uint32_t> probes;
            std::vector<float> positions;
            CaptureSetup setup;
            std::function<void(float)> onProgress;
            size_t next = 0;
            bool configured = false;
            size_t rigCount = 0;
        };
        auto state = std::make_shared<State>();
        state->probes = std::move(probes);
        state->positions = std::move(positions);
        state->setup = setup;
        state->onProgress = std::move(onProgress);
        (void)target;

        return [this, state]() {
            if (!state->configured) {
                // The rigs capturing the probes each frame; the pool can be larger after the
                // budget was lowered.
                createRigs();
                state->rigCount = std::max<size_t>(1, std::min<size_t>(_probesPerFrame, _rigs.size()));
                const std::vector<int> layers = state->setup.geometry
                    ? [this] {
                          std::vector<int> ids;
                          for (const int id : _layers) {
                              if (id != LAYERID_SKYBOX) {
                                  ids.push_back(id);
                              }
                          }
                          return ids;
                      }()
                    : state->setup.layers;
                const auto targetPtr = state->setup.target == _geometryTarget.get() ? _geometryTarget : _radianceTarget;
                for (size_t index = 0; index < _rigs.size(); ++index) {
                    CaptureRig& rig = _rigs[index];
                    if (index >= state->rigCount) {
                        rig.entity->setEnabled(false);
                        continue;
                    }
                    for (CameraComponent* camera : rig.cameras) {
                        camera->setLayers(layers);
                        camera->camera()->setRenderTarget(targetPtr);
                        camera->camera()->setCullFaces(!state->setup.geometry);
                        camera->camera()->setProbeVolumeGeometryPass(state->setup.geometry);
                    }
                }
                state->configured = true;
            }

            // The probes assigned on the previous update have rendered.
            if (state->next >= state->probes.size()) {
                for (CaptureRig& rig : _rigs) {
                    rig.entity->setEnabled(false);
                }
                return StepResult::Done;
            }

            std::vector<std::array<uint32_t, 4>> rects;
            for (size_t index = 0; index < state->rigCount; ++index) {
                CaptureRig& rig = _rigs[index];
                if (state->next >= state->probes.size()) {
                    rig.entity->setEnabled(false);
                    continue;
                }
                const uint32_t probe = state->probes[state->next++];
                rig.entity->setPosition(state->positions[probe * 3], state->positions[probe * 3 + 1],
                    state->positions[probe * 3 + 2]);
                const std::array<uint32_t, 4> tile = tileRect(probe);
                rects.push_back(tile);
                for (int face = 0; face < 6; ++face) {
                    const std::array<uint32_t, 4> faceRect = {tile[0] + static_cast<uint32_t>(face % 3) * _atlas.faceSize,
                        tile[1] + static_cast<uint32_t>(face / 3) * _atlas.faceSize, _atlas.faceSize, _atlas.faceSize};
                    const Vector4 rect = cameraRect(faceRect, _atlas.width, _atlas.height);
                    rig.cameras[face]->camera()->setRect(rect);
                    rig.cameras[face]->camera()->setScissorRect(rect);
                }
                rig.entity->setEnabled(true);
            }
            clearTiles(state->setup.target, rects, state->setup.geometry);
            if (state->onProgress) {
                state->onProgress(static_cast<float>(state->next) / static_cast<float>(state->probes.size()));
            }
            return StepResult::Wait;
        };
    }

    std::vector<float> ProbeVolumeBaker::project(const int mode, const uint32_t texelsPerProbe,
        const uint32_t probesPerRow, const float visibilitySize, const float maxDistance)
    {
        const uint32_t count = _volume->probeCount();
        const uint32_t rows = (count + probesPerRow - 1) / probesPerRow;
        const uint32_t width = probesPerRow * texelsPerProbe;
        const char* variants[3] = {"sh", "validity", "visibility"};
        const auto shader = getOrCreateSlangShader(_device, "probe-volume-project", variants[mode]);
        if (!shader) {
            return {};
        }

        auto texture = createAtlasTexture(_device, "ProbeVolumeProject", width, rows, _formats.target);
        RenderTargetOptions options;
        options.graphicsDevice = _device;
        options.name = "ProbeVolumeProject";
        options.colorBuffer = texture.get();
        options.depth = false;
        const auto target = _device->createRenderTarget(options);
        if (!target) {
            return {};
        }

        struct Uniforms
        {
            float atlas[4];
            float layout[4];
            float visibility[4];
        } uniforms = {
            {static_cast<float>(_atlas.faceSize), static_cast<float>(_atlas.probesPerRow), 0.0f, 0.0f},
            {static_cast<float>(texelsPerProbe), static_cast<float>(probesPerRow), static_cast<float>(count), 0.0f},
            {visibilitySize, maxDistance, 0.0f, 0.0f},
        };
        QuadRender quad(shader);
        const bool radiance = mode == kProjectSh;
        quad.setTexture(0, radiance ? _radianceBuffer.get() : _geometryBuffers[0].get());
        quad.setTexture(1, _geometryBuffers[1].get());
        quad.setUniforms(uniforms);

        BakePass pass(_device, "ProbeVolumeProject");
        pass.init(target);
        _device->beginOfflineWork();
        _device->startRenderPass(&pass);
        _device->setBlendState(BlendState::noBlend());
        _device->setDepthState(DepthState::noDepth());
        _device->setCullMode(CullMode::CULLFACE_NONE);
        _device->setStencilState();
        const Vector4 viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(rows));
        quad.render(&viewport);
        _device->endRenderPass(&pass);
        _device->endOfflineWork();

        std::vector<uint8_t> bytes;
        if (!texture->read(bytes)) {
            return {};
        }
        std::vector<float> pixels(static_cast<size_t>(width) * rows * 4);
        if (_formats.target == PixelFormat::PIXELFORMAT_RGBA32F) {
            std::memcpy(pixels.data(), bytes.data(), std::min(bytes.size(), pixels.size() * sizeof(float)));
        } else {
            const size_t n = std::min(pixels.size(), bytes.size() / 2);
            for (size_t i = 0; i < n; ++i) {
                uint16_t half;
                std::memcpy(&half, bytes.data() + i * 2, 2);
                pixels[i] = halfToFloat(half);
            }
        }
        return pixels;
    }

    std::vector<float> ProbeVolumeBaker::captureVisibility(const uint32_t size)
    {
        const uint32_t count = _volume->probeCount();
        const uint32_t texels = size * size;
        const uint32_t probesPerRow = std::max(1u, std::min(kVisibilityTargetWidth / texels, count));

        // Only the distances up to the probes around a surface matter, so they are clamped to
        // well beyond the diagonal of a cell, which keeps them precise in half floats.
        const float maxDistance = _volume->cellSize().length() * 1.5f;
        const std::vector<float> pixels = project(kProjectVisibility, texels, probesPerRow, static_cast<float>(size),
            maxDistance);
        if (pixels.empty()) {
            return {};
        }

        // The mean and the mean squared distance of each texel of each map.
        const size_t width = static_cast<size_t>(probesPerRow) * texels;
        std::vector<float> data(static_cast<size_t>(count) * texels * 2);
        for (uint32_t probe = 0; probe < count; ++probe) {
            const size_t src = ((probe / probesPerRow) * width + (probe % probesPerRow) * texels) * 4;
            for (uint32_t texel = 0; texel < texels; ++texel) {
                data[(static_cast<size_t>(probe) * texels + texel) * 2] = pixels[src + texel * 4];
                data[(static_cast<size_t>(probe) * texels + texel) * 2 + 1] = pixels[src + texel * 4 + 1];
            }
        }
        return data;
    }

    void ProbeVolumeBaker::updatePositionsTexture()
    {
        const uint32_t count = _volume->probeCount();
        const uint32_t width = std::min(kPositionsWidth, count);
        const uint32_t height = (count + width - 1) / width;
        std::vector<float> data(static_cast<size_t>(width) * height * 4, 0.0f);
        for (uint32_t i = 0; i < count; ++i) {
            std::copy_n(&_positions[i * 3], 3, &data[i * 4]);
        }
        _positionsTexture = createAtlasTexture(_device, "ProbeVolumePositions", width, height,
            PixelFormat::PIXELFORMAT_RGBA32F);
        _positionsTexture->setLevelData(0, reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(float));
        _positionsTexture->upload();
    }

    ProbeVolumeBaker::Step ProbeVolumeBaker::captureSky()
    {
        // DEVIATION: the sky is captured into a 3 x 2 atlas of faces laid out as a probe's tile,
        // where upstream renders a cubemap: every texture the lighting quad reads is then a 2D
        // texture of the bindless table, which holds no cube maps.
        struct State
        {
            Entity* entity = nullptr;
            int frames = 0;
        };
        auto state = std::make_shared<State>();
        return [this, state]() {
            if (!state->entity) {
                const uint32_t size = kSkyFaceSize;
                _skyAtlas = createAtlasTexture(_device, "ProbeVolumeSky", size * 3, size * 2, _formats.cubemap, true);
                RenderTargetOptions options;
                options.graphicsDevice = _device;
                options.name = "ProbeVolumeSky";
                options.colorBuffer = _skyAtlas.get();
                options.depth = true;
                _skyTarget = _device->createRenderTarget(options);

                state->entity = new Entity();
                state->entity->setName("ProbeVolumeSkyCapture");
                state->entity->setEngine(_engine);
                for (int face = 0; face < 6; ++face) {
                    auto* faceEntity = new Entity();
                    faceEntity->setEngine(_engine);
                    auto* camera = static_cast<CameraComponent*>(faceEntity->addComponent<CameraComponent>());
                    Camera* c = camera->camera();
                    c->setFov(90.0f);
                    c->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
                    c->setAspectRatio(1.0f);
                    c->setLinearOutput(true);
                    c->setToneMapping(TONEMAP_NONE);
                    c->setClearColorBuffer(false);
                    c->setClearDepthBuffer(false);
                    c->setClearStencilBuffer(false);
                    c->setRenderTarget(_skyTarget);
                    const std::array<uint32_t, 4> rect = {static_cast<uint32_t>(face % 3) * size,
                        static_cast<uint32_t>(face / 3) * size, size, size};
                    c->setRect(cameraRect(rect, size * 3, size * 2));
                    c->setScissorRect(cameraRect(rect, size * 3, size * 2));
                    camera->setLayers({LAYERID_SKYBOX});
                    camera->setPriority(-100);
                    faceEntity->setLocalRotation(Quaternion::fromEulerAngles(kFaceEulers[face][0],
                        kFaceEulers[face][1], kFaceEulers[face][2]));
                    state->entity->addChild(faceEntity);
                }
                state->entity->setPosition(_volume->aabb().center());
                _engine->root()->addChild(state->entity);
                // Without a sky, the directions without geometry receive no light.
                clearTiles(_skyTarget.get(), {{0u, 0u, size * 3, size * 2}}, false);
                return StepResult::Wait;
            }
            if (++state->frames < 2) {
                return StepResult::Wait;
            }
            freeEntity(state->entity);
            return StepResult::Done;
        };
    }

    void ProbeVolumeBaker::createLighting()
    {
        destroyLighting();

        // The material lights each texel as the surface its probe saw, with the probes of the
        // previous bounce as its ambient, and no specular.
        _lightingMaterial = std::make_shared<StandardMaterial>();
        _lightingMaterial->setName("ProbeVolumeLighting");
        _lightingMaterial->setProbeVolumeLighting(true);
        _lightingMaterial->setUseFog(false);
        _lightingMaterial->setCullMode(CullMode::CULLFACE_NONE);
        ProbeVolume::applyToMaterial(*_lightingMaterial);

        // The quad covers the target in clip space (MeshInstance::setScreenSpace).
        PrimitiveGeometry quad;
        quad.positions = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f};
        quad.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
        quad.uvs = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        quad.tangents = {1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f};
        quad.indices = {0, 1, 2, 0, 2, 3};
        _lightingMesh = createMeshFromGeometry(_engine->graphicsDevice(), quad);
        _lightingMeshInstance = std::make_unique<MeshInstance>(_lightingMesh.get(), _lightingMaterial.get());
        _lightingMeshInstance->setScreenSpace(true);
        _lightingMeshInstance->setCull(false);
        _lightingMeshInstance->setCastShadows(false);

        const auto layers = _engine->scene()->layers();
        _lightingLayer = std::make_shared<Layer>("ProbeVolumeLighting", kLightingLayerId);
        _lightingLayer->addMeshInstances({_lightingMeshInstance.get()});
        layers->pushOpaque(_lightingLayer);

        // The lights of the captured layers light the quad.
        for (LightComponent* light : _engine->components().instances<LightComponent>()) {
            if (!light) {
                continue;
            }
            const bool captured = std::any_of(_layers.begin(), _layers.end(),
                [light](const int id) { return light->rendersLayer(id); });
            if (captured) {
                std::vector<int> own = light->layers();
                std::vector<int> withLighting = own;
                withLighting.push_back(kLightingLayerId);
                light->setLayers(withLighting);
                _lightingLights.emplace_back(light, std::move(own));
            }
        }

        // The geometry and the sky the material reads, through the capture block.
        ProbeVolumeBlock capture = _volume->captureBlock();
        capture.bakeLanes[0] = laneOf(_geometryBuffers[0].get());
        capture.bakeLanes[1] = laneOf(_geometryBuffers[1].get());
        capture.bakeLanes[2] = _geometryBuffers.size() > 2 ? laneOf(_geometryBuffers[2].get()) : 0u;
        capture.bakeLanes[3] = laneOf(_positionsTexture.get());
        capture.bakeSkyLane[0] = laneOf(_skyAtlas.get());
        capture.bakeAtlas[0] = static_cast<float>(_atlas.faceSize);
        capture.bakeAtlas[1] = static_cast<float>(_atlas.probesPerRow);
        capture.bakeAtlas[2] = static_cast<float>(kSkyFaceSize);
        capture.bakeAtlas[3] = static_cast<float>(_positionsTexture ? _positionsTexture->width() : 1u);
        _volume->setBakeGeometry(capture);

        // An orthographic camera whose frustum encloses a sphere around the volume, so the
        // lights around it are not culled.
        const Vector3 center = _volume->aabb().center();
        const float radius = _volume->aabb().halfExtents().length() * 2.0f + 10.0f;
        _lightingCamera = new Entity();
        _lightingCamera->setName("ProbeVolumeLightingCamera");
        _lightingCamera->setEngine(_engine);
        auto* camera = static_cast<CameraComponent*>(_lightingCamera->addComponent<CameraComponent>());
        Camera* c = camera->camera();
        c->setProjection(ProjectionType::Orthographic);
        c->setOrthoHeight(radius);
        c->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
        c->setAspectRatio(1.0f);
        c->setNearClip(0.1f);
        c->setFarClip(radius * 2.0f + 1.0f);
        c->setLinearOutput(true);
        c->setToneMapping(TONEMAP_NONE);
        c->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        c->setRenderTarget(_radianceTarget);
        c->setProbeVolumeCapture(true);
        camera->setLayers({kLightingLayerId});
        camera->setPriority(-100);
        _lightingCamera->setPosition(center.getX(), center.getY(), center.getZ() + radius + 0.5f);
        _lightingCamera->setEnabled(false);
        _engine->root()->addChild(_lightingCamera);
    }

    void ProbeVolumeBaker::destroyLighting()
    {
        freeEntity(_lightingCamera);
        for (auto& [light, layers] : _lightingLights) {
            light->setLayers(layers);
        }
        _lightingLights.clear();
        if (_lightingLayer && _engine && _engine->scene()) {
            _lightingLayer->clearMeshInstances();
            _engine->scene()->layers()->remove(_lightingLayer);
        }
        _lightingLayer.reset();
        _lightingMeshInstance.reset();
        _lightingMesh.reset();
        _lightingMaterial.reset();
    }

    std::vector<ProbeVolumeBaker::ShadowLight> ProbeVolumeBaker::setShadowBounds(const std::vector<int>& layers,
        const std::optional<BoundingBox>& bounds) const
    {
        std::vector<ShadowLight> fitted;
        if (!bounds || !_engine) {
            return fitted;
        }
        for (LightComponent* light : _engine->components().instances<LightComponent>()) {
            if (!light || light->type() != LightType::LIGHTTYPE_DIRECTIONAL || !light->castShadows()) {
                continue;
            }
            const bool captured = std::any_of(layers.begin(), layers.end(),
                [light](const int id) { return light->rendersLayer(id); });
            if (captured) {
                fitted.push_back({light, light->shadowBounds()});
                light->setShadowBounds(bounds);
            }
        }
        return fitted;
    }

    uint32_t ProbeVolumeBaker::dilate(std::vector<float>& data, const std::vector<uint8_t>& validity,
        const std::array<uint32_t, 3>& resolution)
    {
        const int nx = static_cast<int>(resolution[0]);
        const int ny = static_cast<int>(resolution[1]);
        const int nz = static_cast<int>(resolution[2]);
        std::vector<uint8_t> filled = validity;
        float sum[kFloatsPerProbe];
        uint32_t filledCount = 0;

        for (int iteration = 0; iteration < 4; ++iteration) {
            std::vector<size_t> newlyFilled;
            for (size_t probe = 0; probe < filled.size(); ++probe) {
                if (filled[probe]) {
                    continue;
                }
                const int ix = static_cast<int>(probe % static_cast<size_t>(nx));
                const int iy = static_cast<int>((probe / static_cast<size_t>(nx)) % static_cast<size_t>(ny));
                const int iz = static_cast<int>(probe / static_cast<size_t>(nx * ny));

                std::fill(std::begin(sum), std::end(sum), 0.0f);
                float weightSum = 0.0f;
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int x = ix + dx;
                            const int y = iy + dy;
                            const int z = iz + dz;
                            if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) {
                                continue;
                            }
                            const size_t neighbor = static_cast<size_t>(x + nx * (y + ny * z));
                            if (neighbor != probe && filled[neighbor]) {
                                const float weight = 1.0f / static_cast<float>(dx * dx + dy * dy + dz * dz);
                                for (int i = 0; i < kFloatsPerProbe; ++i) {
                                    sum[i] += data[neighbor * kFloatsPerProbe + i] * weight;
                                }
                                weightSum += weight;
                            }
                        }
                    }
                }
                if (weightSum > 0.0f) {
                    for (int i = 0; i < kFloatsPerProbe; ++i) {
                        data[probe * kFloatsPerProbe + i] = sum[i] / weightSum;
                    }
                    newlyFilled.push_back(probe);
                }
            }
            if (newlyFilled.empty()) {
                break;
            }
            for (const size_t probe : newlyFilled) {
                filled[probe] = 1u;
            }
            filledCount += static_cast<uint32_t>(newlyFilled.size());
        }
        return filledCount;
    }
}
