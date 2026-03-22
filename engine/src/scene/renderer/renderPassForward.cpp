// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#include "renderPassForward.h"

#include <cassert>
#include <unordered_map>

#include "renderer.h"
#include "spdlog/spdlog.h"
#include "scene/composition/layerComposition.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    RenderPassForward::RenderPassForward(const std::shared_ptr<GraphicsDevice>& device,
        LayerComposition* layerComposition, Scene* scene, Renderer* renderer)
        : RenderPass(device), _layerComposition(layerComposition), _scene(scene), _renderer(renderer)
    {
    }

    void RenderPassForward::addRenderAction(RenderAction* renderAction)
    {
        if (renderAction) {
            _renderActions.push_back(renderAction);
        }
    }

    void RenderPassForward::updateClears()
    {
        if (_renderActions.empty()) {
            return;
        }

        const auto* ra = _renderActions.front();
        const auto* cameraComp = ra ? ra->camera : nullptr;
        const auto* camera = cameraComp ? cameraComp->camera() : nullptr;
        if (!camera) {
            return;
        }

        // RenderPassForward uses clear flags precomputed on the first render action.
        // This avoids re-clearing when a pass starts in the middle of camera action sequence.
        const bool clearColor = ra->clearColor;
        const bool clearDepth = ra->clearDepth;
        const bool clearStencil = ra->clearStencil;

        if (clearColor) {
            const auto clear = camera->clearColor();
            setClearColor(&clear);
        } else {
            setClearColor(nullptr);
        }

        if (clearDepth) {
            const float clearDepthValue = 1.0f;
            setClearDepth(&clearDepthValue);
        } else {
            setClearDepth(nullptr);
        }

        if (clearStencil) {
            const int clearStencilValue = 0;
            setClearStencil(&clearStencilValue);
        } else {
            setClearStencil(nullptr);
        }
    }

    void RenderPassForward::before()
    {
        if (_beforeCalled) {
            spdlog::error("RenderPassForward parity violation: before() called twice without matching after()");
            assert(!_beforeCalled && "RenderPassForward::before called while pass is already active");
        }
        _beforeCalled = true;
        _executeCalled = false;

        // when CameraFrame is active, the scene forward passes
        // output linear HDR; the compose pass handles tonemapping and gamma.
        // The flag is set on the graphics device so the draw() call can propagate
        // it as a runtime uniform bit (not a compile-time variant).
        if (_hdrPass) {
            device()->setHdrPass(true);
        }

        refreshCameraUseFlags();

        if (!validateRenderActionOrder()) {
            spdlog::error("RenderPassForward parity violation: invalid render action ordering");
            assert(false && "Invalid render action order");
        }

        updateClears();

        for (const auto* ra : _renderActions) {
            if (ra && ra->firstCameraUse && _scene) {
                _scene->fire("prerender", ra->camera);
            }
        }
    }

    void RenderPassForward::renderRenderAction(RenderAction* renderAction, const bool firstRenderAction)
    {
        (void)firstRenderAction;
        if (!_beforeCalled) {
            spdlog::error("RenderPassForward parity violation: renderRenderAction() called before before()");
            assert(_beforeCalled && "Render action executed outside pass lifecycle");
        }
        if (!renderAction || !renderAction->camera || !_renderer) {
            return;
        }

        auto* camera = renderAction->camera->camera();
        auto* layer = renderAction->layer;
        auto* target = renderAction->renderTarget.get();

        if (_scene) {
            _scene->fire("prerender:layer", renderAction->camera, layer, renderAction->transparent);
        }

        _renderer->renderForwardLayer(camera, target, layer, renderAction->transparent);

        if (_scene) {
            _scene->fire("postrender:layer", renderAction->camera, layer, renderAction->transparent);
        }
    }

    void RenderPassForward::execute()
    {
        if (!_beforeCalled) {
            spdlog::error("RenderPassForward parity violation: execute() called before before()");
            assert(_beforeCalled && "RenderPassForward::execute requires before()");
        }
        _executeCalled = true;

        for (size_t i = 0; i < _renderActions.size(); ++i) {
            auto* ra = _renderActions[i];
            if (!ra || !_layerComposition || !_layerComposition->isEnabled(ra->layer, ra->transparent)) {
                continue;
            }
            renderRenderAction(ra, i == 0);
        }
    }

    void RenderPassForward::after()
    {
        if (!_beforeCalled) {
            spdlog::error("RenderPassForward parity violation: after() called before before()");
            assert(_beforeCalled && "RenderPassForward::after requires before()");
        }
        if (!_executeCalled) {
            spdlog::error("RenderPassForward parity violation: after() called before execute()");
            assert(_executeCalled && "RenderPassForward::after requires execute()");
        }

        for (const auto* ra : _renderActions) {
            if (ra && ra->lastCameraUse && _scene) {
                _scene->fire("postrender", ra->camera);
            }
        }

        // restore normal shader output after HDR pass completes.
        if (_hdrPass) {
            device()->setHdrPass(false);
        }

        _beforeCalled = false;
        _executeCalled = false;
    }

    void RenderPassForward::refreshCameraUseFlags()
    {
        for (size_t i = 0; i < _renderActions.size(); ++i) {
            auto* ra = _renderActions[i];
            if (!ra || !ra->camera) {
                continue;
            }

            const auto* camera = ra->camera;
            bool hasPreviousForCamera = false;
            bool hasNextForCamera = false;

            for (size_t j = 0; j < i; ++j) {
                if (_renderActions[j] && _renderActions[j]->camera == camera) {
                    hasPreviousForCamera = true;
                    break;
                }
            }

            for (size_t j = i + 1; j < _renderActions.size(); ++j) {
                if (_renderActions[j] && _renderActions[j]->camera == camera) {
                    hasNextForCamera = true;
                    break;
                }
            }

            ra->firstCameraUse = !hasPreviousForCamera;
            ra->lastCameraUse = !hasNextForCamera;
        }
    }

    bool RenderPassForward::validateRenderActionOrder() const
    {
        std::unordered_map<const CameraComponent*, bool> activeCameraSpan;

        for (const auto* ra : _renderActions) {
            if (!ra || !ra->camera || !ra->layer) {
                return false;
            }

            const auto* camera = ra->camera;
            const bool isActive = activeCameraSpan.contains(camera) && activeCameraSpan.at(camera);

            if (ra->firstCameraUse && isActive) {
                return false;
            }
            if (!ra->firstCameraUse && !isActive) {
                return false;
            }

            if (ra->firstCameraUse) {
                activeCameraSpan[camera] = true;
            }

            if (ra->lastCameraUse) {
                if (!activeCameraSpan.contains(camera) || !activeCameraSpan[camera]) {
                    return false;
                }
                activeCameraSpan[camera] = false;
            }
        }

        for (const auto& [camera, active] : activeCameraSpan) {
            (void)camera;
            if (active) {
                return false;
            }
        }
        return true;
    }
}
