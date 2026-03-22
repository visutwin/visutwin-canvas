// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.12.2025.
//
#include "cameraComponent.h"
#include <algorithm>
#include <cassert>

#include "framework/entity.h"
#include "framework/engine.h"
#include "framework/components/componentSystem.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/graphics/renderPassTAA.h"
#include "scene/graphNode.h"

namespace visutwin::canvas
{
    std::vector<CameraComponent*> CameraComponent::_instances;

    CameraComponent::CameraComponent(IComponentSystem* system, Entity* entity) : Component(system, entity)
    {
        _instances.push_back(this);
    }

    CameraComponent::~CameraComponent()
    {
        const auto it = std::find(_instances.begin(), _instances.end(), this);
        if (it != _instances.end()) {
            _instances.erase(it);
        }

        if (_cameraFrame) {
            _cameraFrame->destroy();
            _cameraFrame.reset();
        }

        if (_camera) {
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
        requestSceneDepthMap(enabled);
        requestSceneColorMap(enabled);

        if (_cameraFrame) {
            _cameraFrame->destroy();
            _cameraFrame.reset();
        }

        // DEVIATION: Do NOT call ensureDofRenderTarget() or updatePostprocessRenderTargetBinding()
        // here. CameraFrame owns its offscreen targets. See comment in setSsaoEnabled().
    }

    void CameraComponent::setTaaEnabled(const bool enabled)
    {
        if (_taa.enabled == enabled) {
            return;
        }

        _taa.enabled = enabled;
        // Upstream camera-frame parity: TAA uses camera-frame owned scene/depth targets,
        // not legacy scene grab passes.

        if (_camera) {
            _camera->setJitter(_taa.enabled ? std::max(_taa.jitter, 0.0f) : 0.0f);
        }

        if (_cameraFrame) {
            _cameraFrame->destroy();
            _cameraFrame.reset();
        }

        if (!enabled) {
            _taaPass.reset();
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
        // Upstream camera-frame parity: SSAO uses camera-frame owned scene/depth targets,
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

    std::shared_ptr<RenderPassTAA> CameraComponent::ensureTaaPass(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture)
    {
        if (!device || !sourceTexture || !_taa.enabled) {
            return nullptr;
        }

        if (!_taaPass) {
            _taaPass = std::make_shared<RenderPassTAA>(device, sourceTexture, this);
        } else {
            _taaPass->setSourceTexture(sourceTexture);
        }
        _taaPass->setHighQuality(_taa.highQuality);
        return _taaPass;
    }
}
