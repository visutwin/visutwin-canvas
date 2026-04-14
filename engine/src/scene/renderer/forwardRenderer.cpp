// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#include "forwardRenderer.h"

#include <unordered_set>

#include "framework/components/light/lightComponent.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/light.h"
#include "renderPassForward.h"
#include "renderPassPostprocessing.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    void ForwardRenderer::buildFrameGraph(FrameGraph* frameGraph, LayerComposition* layerComposition)
    {
        frameGraph->reset();

        if (_scene->clusteredLightingEnabled())
        {
            const auto lighting = _scene->lighting();
            _renderPassUpdateClustered->update(frameGraph, lighting.shadowsEnabled, lighting.cookiesEnabled,
                _lights, _localLights);
            frameGraph->addRenderPass(_renderPassUpdateClustered);
        }
        else
        {
            // Cull local light shadow maps: allocate shadow maps, position shadow cameras,
            // compute VP matrices for all shadow-casting local (spot/point) lights.
            std::vector<Light*> localShadowLights;
            {
                for (auto* lightComponent : LightComponent::instances()) {
                    if (!lightComponent || !lightComponent->enabled()) {
                        continue;
                    }
                    if (lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                        continue;
                    }
                    if (!lightComponent->castShadows()) {
                        continue;
                    }
                    Light* sceneLight = lightComponent->light();
                    if (sceneLight) {
                        localShadowLights.push_back(sceneLight);
                    }
                }
                if (!localShadowLights.empty()) {
                    _shadowRendererLocal->cullLocalLights(localShadowLights, _device, _ownedShadowMaps);
                }
            }

            // Build shadow render passes using the actual shadow-casting lights
            // (not _localLights which is only populated in the clustered path).
            _shadowRendererLocal->buildNonClusteredRenderPasses(frameGraph, localShadowLights);
        }

        // Cull directional shadow maps for each unique camera in the layer composition.
        // This positions shadow cameras and populates _cameraDirShadowLights.
        // Also dispatches per-camera GPU instance culling for any MeshInstances
        // that opted in via enableGpuInstanceCulling().
        {
            const auto& actions = layerComposition->renderActions();
            std::unordered_set<Camera*> culledCameras;
            for (const auto* action : actions) {
                if (action && action->camera) {
                    Camera* cam = action->camera->camera();
                    if (cam && culledCameras.insert(cam).second) {
                        cullShadowmaps(cam);
                        dispatchGpuInstanceCulling(cam);
                    }
                }
            }
        }

        if (auto* directionalShadowRenderer = shadowRendererDirectional()) {
            directionalShadowRenderer->buildNonClusteredRenderPasses(frameGraph, _cameraDirShadowLights);
        }

        int startIndex = 0;
        bool newStart = true;
        RenderTarget* renderTarget = nullptr;
        const auto& renderActions = layerComposition->renderActions();

        for (int i = startIndex; i < renderActions.size(); i++) {
            if (auto* renderAction = renderActions[i]; renderAction->useCameraPasses)  {
                // schedule render passes from the camera
                for (auto renderPass : renderAction->camera->renderPasses()) {
                    if (renderPass) {
                        frameGraph->addRenderPass(renderPass);
                    }
                };
            } else {
                const auto isDepthLayer = renderAction->layer->id() == LAYERID_DEPTH;
                const auto  isGrabPass = isDepthLayer &&
                    (renderAction->camera->renderSceneColorMap() || renderAction->camera->renderSceneDepthMap());

                // start of block of render actions rendering to the same render target
                if (newStart) {
                    newStart = false;
                    startIndex = i;
                    renderTarget = renderAction->renderTarget.get();
                }

                // info about the next render action
                auto* nextRenderAction = (i + 1 < renderActions.size()) ? renderActions[i + 1] : nullptr;
                const auto isNextLayerDepth = nextRenderAction ? (!nextRenderAction->useCameraPasses && nextRenderAction->layer->id() == LAYERID_DEPTH) : false;
                const auto isNextLayerGrabPass = isNextLayerDepth &&
                    (renderAction->camera->renderSceneColorMap() || renderAction->camera->renderSceneDepthMap());

                auto* camera = (nextRenderAction && nextRenderAction->camera) ? nextRenderAction->camera->camera() : nullptr;
                const auto nextNeedDirShadows = nextRenderAction ?
                    (nextRenderAction->firstCameraUse && _cameraDirShadowLights.contains(camera)) : false;

                // The depth layer uses a null render target (separate from the camera's target).
                // When it is NOT a grab pass it will be skipped as depth-only, so it should not
                // break the current block — otherwise the camera-frame postprocessing pass ends
                // up missing the opaque world layer that precedes the depth layer, causing the
                // scene to render black when TAA/DOF is enabled.
                const bool nextIsNonGrabDepth = isNextLayerDepth && !isNextLayerGrabPass;
                const bool rtChanged = nextRenderAction && nextRenderAction->renderTarget.get() != renderTarget && !nextIsNonGrabDepth;

                // end of the block using the same render target if the next render action uses a different render target or needs directional shadows
                // rendered before it or similar or needs another pass before it.
                if (!nextRenderAction || rtChanged || nextNeedDirShadows ||
                    isNextLayerGrabPass || isGrabPass) {

                    const bool useCameraFrame = renderAction->triggerPostprocess && renderAction->camera &&
                        renderAction->camera->onPostprocessing() != nullptr;

                    // render the render actions in the range
                    if (const auto isDepthOnly = isDepthLayer && startIndex == i; !isDepthOnly) {
                        if (useCameraFrame) {
                            std::vector<RenderAction*> blockActions;
                            blockActions.reserve(static_cast<size_t>(i - startIndex + 1));
                            for (int actionIndex = startIndex; actionIndex <= i; ++actionIndex) {
                                auto* blockAction = renderActions[actionIndex];
                                if (blockAction) {
                                    blockActions.push_back(blockAction);
                                }
                            }
                            if (!blockActions.empty()) {
                                // Get or create persistent CameraFrame on the camera component.
                                // The CameraFrame manages its own internal offscreen render
                                // targets (scene color, depth, TAA history). Persisting it across
                                // frames avoids reallocating ~56MB of GPU textures per frame and
                                // preserves TAA history for correct temporal accumulation.
                                auto cameraFramePass = renderAction->camera->cameraFrame();
                                if (!cameraFramePass) {
                                    cameraFramePass = std::make_shared<RenderPassCameraFrame>(
                                        _device, layerComposition, _scene.get(), this, blockActions, renderAction->camera, nullptr);
                                    renderAction->camera->setCameraFrame(cameraFramePass);
                                } else {
                                    cameraFramePass->updateSourceActions(
                                        blockActions, layerComposition, _scene.get(), this, nullptr);
                                }
                                frameGraph->addRenderPass(cameraFramePass);
                            }
                        } else {
                            addMainRenderPass(frameGraph, layerComposition, renderTarget, startIndex, i);
                        }
                    }

                    // depth layer triggers grab passes if enabled
                    if (isDepthLayer && !useCameraFrame) {
                        if (renderAction->camera->renderSceneColorMap()) {
                            const auto colorGrabPass = renderAction->camera->camera()->renderPassColorGrab();
                            if (colorGrabPass) {
                                colorGrabPass->setSource(renderAction->renderTarget);
                                frameGraph->addRenderPass(colorGrabPass);
                            }
                        }

                        if (renderAction->camera->renderSceneDepthMap()) {
                            const auto depthGrabPass = renderAction->camera->camera()->renderPassDepthGrab();
                            if (depthGrabPass) {
                                frameGraph->addRenderPass(depthGrabPass);
                            }
                        }
                    }

                    // postprocessing
                    if (!useCameraFrame && renderAction->triggerPostprocess && renderAction->camera &&
                        renderAction->camera->onPostprocessing()) {
                        auto renderPass = std::make_shared<RenderPassPostprocessing>(_device, this, renderAction);
                        frameGraph->addRenderPass(renderPass);
                    }

                    newStart = true;
                }
            }
        }
    }

    void ForwardRenderer::addMainRenderPass(FrameGraph* frameGraph, LayerComposition* layerComposition,
        RenderTarget* renderTarget, int startIndex, int endIndex)
    {
        if (!frameGraph || !layerComposition) {
            return;
        }

        const auto& renderActions = layerComposition->renderActions();
        if (renderActions.empty() || startIndex < 0 || endIndex < startIndex ||
            static_cast<size_t>(endIndex) >= renderActions.size()) {
            return;
        }

        auto* firstRenderAction = renderActions[startIndex];
        if (!firstRenderAction || !firstRenderAction->camera) {
            return;
        }

        std::shared_ptr<RenderTarget> passTarget = firstRenderAction->renderTarget;
        if (!passTarget && renderTarget != nullptr) {
            // Intentional fallback to preserve API shape until render actions are the single source of truth.
            passTarget = firstRenderAction->camera->camera() ? firstRenderAction->camera->camera()->renderTarget() : nullptr;
        }

        auto mainPass = std::make_shared<RenderPassForward>(
            _device, layerComposition, _scene.get(), this
        );
        mainPass->init(passTarget);

        for (int i = startIndex; i <= endIndex; ++i) {
            auto* ra = renderActions[i];
            if (!ra) {
                continue;
            }
            const auto* cameraForAction = ra->camera;
            bool hasPreviousForCamera = false;
            bool hasNextForCamera = false;

            for (int j = startIndex; j < i; ++j) {
                if (renderActions[j] && renderActions[j]->camera == cameraForAction) {
                    hasPreviousForCamera = true;
                    break;
                }
            }
            for (int j = i + 1; j <= endIndex; ++j) {
                if (renderActions[j] && renderActions[j]->camera == cameraForAction) {
                    hasNextForCamera = true;
                    break;
                }
            }

            ra->firstCameraUse = !hasPreviousForCamera;
            ra->lastCameraUse = !hasNextForCamera;
            mainPass->addRenderAction(ra);
        }

        frameGraph->addRenderPass(mainPass);
    }
}
