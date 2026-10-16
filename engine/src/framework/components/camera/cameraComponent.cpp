// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.12.2025
//
#include "cameraComponent.h"

#include "framework/components/componentRegistry.h"
#include <algorithm>
#include <cassert>

#include <spdlog/spdlog.h>

#include "framework/entity.h"
#include "framework/engine.h"
#include "framework/components/componentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "scene/light.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/graphics/cameraFrameEffect.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/graphics/taaEffect.h"
#include "scene/graphNode.h"

namespace visutwin::canvas
{
    CameraComponent::CameraComponent(IComponentSystem* system, Entity* entity) : Component(system, entity)
    {
        listInstance(this);
        _taaEffect = std::make_shared<TaaEffect>(this);
        rebuildAllFrameEffects();
    }

    CameraComponent::~CameraComponent()
    {
        ComponentRegistry* const registry = this->registry();
        unlistInstance();

        // An application may keep an effect beyond its camera.
        for (const auto& effect : _frameEffects) {
            effect->_camera = nullptr;
        }

        if (_cameraFrame) {
            _cameraFrame->destroy();
            _cameraFrame.reset();
        }

        if (_camera) {
            // Lights cache LightRenderData keyed on raw Camera* — purge before
            // the pointer dies (a future camera at the same address would
            // otherwise reuse the stale entry).
            // Every instance, including inactive ones: a disabled light still holds
            // render data keyed on this camera, and that is what is being purged.
            for (auto* lightComponent : instancesOf<LightComponent>(registry)) {
                if (lightComponent && lightComponent->light()) {
                    lightComponent->light()->invalidateRenderData(_camera);
                }
            }

            delete _camera;
            _camera = nullptr;
        }
    }

    void CameraComponent::initializeComponentData()
    {
        if (!_camera) {
            _camera = new Camera();
            _camera->setNode(_entity);
        }
    }

    void CameraComponent::requestSceneColorMap(const bool enabled)
    {
        _renderSceneColorMap += enabled ? 1 : -1;
        assert(_renderSceneColorMap >= 0);
        if (_renderSceneColorMap < 0) {
            _renderSceneColorMap = 0;
        }

        if (const auto* systemPtr = system()) {
            if (const auto* engine = systemPtr->engine()) {
                _camera->_enableRenderPassColorGrab(engine->graphicsDevice(), renderSceneColorMap());
            }
        }
    }

    void CameraComponent::requestSceneDepthMap(const bool enabled)
    {
        _renderSceneDepthMap += enabled ? 1 : -1;
        assert(_renderSceneDepthMap >= 0);
        if (_renderSceneDepthMap < 0) {
            _renderSceneDepthMap = 0;
        }

        if (const auto* systemPtr = system()) {
            if (const auto* engine = systemPtr->engine()) {
                _camera->_enableRenderPassDepthGrab(engine->graphicsDevice(), renderSceneDepthMap());
            }
        }
    }

    void CameraComponent::setDofEnabled(const bool enabled)
    {
        if (_dof.enabled == enabled) {
            return;
        }

        _dof.enabled = enabled;
        // DOF uses camera-frame owned scene/depth targets,
        // not legacy scene grab passes. Do NOT call requestSceneDepthMap() here — it would
        // cause the DEPTH layer to be treated as a grab pass, splitting the render block and
        // preventing WORLD actions from reaching the CameraFrame. See comment in setSsaoEnabled().

        if (_cameraFrame) {
            _cameraFrame->destroy();
            _cameraFrame.reset();
        }
    }

    void CameraComponent::setTaaEnabled(const bool enabled)
    {
        if (_taa.enabled == enabled) {
            return;
        }

        _taa.enabled = enabled;
        // TAA uses camera-frame owned scene/depth targets,
        // not legacy scene grab passes.

        if (_camera) {
            _camera->setJitter(_taa.enabled ? std::max(_taa.jitter, 0.0f) : 0.0f);
        }

        if (_cameraFrame) {
            _cameraFrame->destroy();
            _cameraFrame.reset();
        }

        // DEVIATION: Do NOT call ensureDofRenderTarget() or updatePostprocessRenderTargetBinding()
        // here. CameraFrame owns its offscreen targets. See comment in setSsaoEnabled().
    }

    void CameraComponent::setSsaoEnabled(const bool enabled)
    {
        if (_ssao.enabled == enabled) {
            return;
        }

        _ssao.enabled = enabled;
        // SSAO uses camera-frame owned scene/depth targets,
        // not legacy scene grab passes. Do NOT call requestSceneDepthMap() here — it would
        // cause the DEPTH layer to be treated as a grab pass, splitting the render block and
        // preventing WORLD actions from reaching the CameraFrame.

        // Force CameraFrame recreation so it picks up the new SSAO state.
        // Without this, the cached CameraFrame's _options may match (stale ssaoType),
        // causing needsReset() to return false and leaving broken internal pass state.
        if (_cameraFrame) {
            _cameraFrame->destroy();
            _cameraFrame.reset();
        }

        // DEVIATION: Do NOT call ensureDofRenderTarget() or updatePostprocessRenderTargetBinding()
        // here. The CameraFrame creates its own offscreen render targets and clones render
        // actions to them internally. Setting the camera's render target to an offscreen target
        // poisons the LayerComposition's cached render actions — when SSAO is later toggled off,
        // those stale actions still reference the offscreen target, causing the scene to render
        // to an invisible buffer with no back-buffer pass, starving the ring-buffer semaphores.
    }

    void CameraComponent::ensureDofRenderTarget()
    {
        const auto* componentSystem = system();
        const auto* app = componentSystem ? componentSystem->engine() : nullptr;
        const auto graphicsDevice = app ? app->graphicsDevice() : nullptr;
        if (!graphicsDevice) {
            return;
        }

        const auto [width, height] = graphicsDevice->size();
        const bool needsCreate = !_dofSceneColorTexture || !_dofSceneRenderTarget ||
            static_cast<int>(_dofSceneColorTexture->width()) != width ||
            static_cast<int>(_dofSceneColorTexture->height()) != height;

        if (!needsCreate) {
            return;
        }

        TextureOptions textureOptions;
        textureOptions.name = "DofSceneColor";
        textureOptions.width = std::max(width, 1);
        textureOptions.height = std::max(height, 1);
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _dofSceneColorTexture = std::make_shared<Texture>(graphicsDevice.get(), textureOptions);
        _dofSceneColorTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _dofSceneColorTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = graphicsDevice.get();
        targetOptions.colorBuffer = _dofSceneColorTexture.get();
        targetOptions.depth = true;
        targetOptions.stencil = false;
        targetOptions.name = "DofSceneTarget";
        _dofSceneRenderTarget = graphicsDevice->createRenderTarget(targetOptions);
    }

    void CameraComponent::updatePostprocessRenderTargetBinding() const
    {
        if (!_camera) {
            return;
        }

        if (requiresPostprocessRenderTarget()) {
            if (_dofSceneRenderTarget) {
                _camera->setRenderTarget(_dofSceneRenderTarget);
            }
        } else if (_camera->renderTarget() == _dofSceneRenderTarget) {
            _camera->setRenderTarget(nullptr);
        }
    }

    void CameraComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const CameraComponent*>(source);
        if (!src) {
            return;
        }
        // The Camera holds most of the camera's properties, so it copies itself.
        if (_camera && src->_camera) {
            _camera->copy(*src->_camera);
        }
        _layers = src->_layers;
        _priority = src->_priority;
        setDof(src->_dof);
        setSsao(src->_ssao);
        setTaa(src->_taa);
        _volumetricFog = src->_volumetricFog;
        _rendering = src->_rendering;
        // renderSceneColorMap / renderSceneDepthMap are properties whose
        // setter makes ONE request; the source's count belongs to its own requesters.
        if (src->renderSceneColorMap()) {
            requestSceneColorMap(true);
        }
        if (src->renderSceneDepthMap()) {
            requestSceneDepthMap(true);
        }
    }

    bool CameraComponent::addFrameEffect(const std::shared_ptr<CameraFrameEffect>& effect)
    {
        if (!effect) {
            return false;
        }
        if (frameEffect(effect->id())) {
            spdlog::warn("CameraComponent: a frame effect with id '{}' is already registered", effect->id());
            return false;
        }
        _frameEffects.push_back(effect);
        effect->_camera = this;
        rebuildAllFrameEffects();
        return true;
    }

    void CameraComponent::removeFrameEffect(const std::shared_ptr<CameraFrameEffect>& effect)
    {
        if (effect && std::erase(_frameEffects, effect) > 0) {
            effect->_camera = nullptr;
        }
        rebuildAllFrameEffects();
    }

    void CameraComponent::rebuildAllFrameEffects()
    {
        _allFrameEffects.clear();
        _allFrameEffects.push_back(_taaEffect);
        _allFrameEffects.insert(_allFrameEffects.end(), _frameEffects.begin(), _frameEffects.end());
    }

    std::shared_ptr<CameraFrameEffect> CameraComponent::frameEffect(const std::string_view id) const
    {
        for (const auto& effect : _frameEffects) {
            if (effect->id() == id) {
                return effect;
            }
        }
        return nullptr;
    }

    bool CameraComponent::hasActiveFrameEffect() const
    {
        return std::any_of(_frameEffects.begin(), _frameEffects.end(),
            [](const auto& effect) { return effect->active(); });
    }

    bool CameraComponent::frameEffectRendersSky() const
    {
        return std::any_of(_allFrameEffects.begin(), _allFrameEffects.end(),
            [](const auto& effect) { return effect->active() && effect->rendersSky(); });
    }

    void CameraComponent::applyFrameEffectJitter(const bool rendersThroughFrame)
    {
        if (!_camera) {
            return;
        }
        if (rendersThroughFrame) {
            _camera->setJitter(frameEffectJitter());
            _jitterFromFrame = true;
        } else if (_jitterFromFrame) {
            // Leaving its frame, the camera drops what the frame's effects set on it.
            _camera->setJitter(0.0f);
            _camera->setExposureScale(1.0f);
            _jitterFromFrame = false;
        }
    }

    float CameraComponent::frameEffectJitter() const
    {
        float jitter = 0.0f;
        for (const auto& effect : _allFrameEffects) {
            if (effect->active()) {
                jitter = std::max(jitter, effect->cameraJitter());
            }
        }
        return jitter;
    }

    void CameraComponent::setPriority(const int value)
    {
        _priority = value;
    }

    Vector3 CameraComponent::screenToWorld(const float x, const float y, const float z) const
    {
        const Engine* engine = _entity ? _entity->engine() : nullptr;
        const auto [w, h] = engine ? engine->canvasSize() : std::pair<int, int>{0, 0};
        if (!_camera || w <= 0 || h <= 0) {
            return Vector3(0.0f, 0.0f, 0.0f);
        }
        return _camera->screenToWorld(x, y, z, static_cast<float>(w), static_cast<float>(h));
    }

    Vector3 CameraComponent::worldToScreen(const Vector3& worldCoord) const
    {
        const Engine* engine = _entity ? _entity->engine() : nullptr;
        const auto [w, h] = engine ? engine->canvasSize() : std::pair<int, int>{0, 0};
        if (!_camera || w <= 0 || h <= 0) {
            return Vector3(0.0f, 0.0f, 0.0f);
        }
        return _camera->worldToScreen(worldCoord, static_cast<float>(w), static_cast<float>(h));
    }
}
