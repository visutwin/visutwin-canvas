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
#include "scene/constants.h"
#include "scene/skinInstance.h"

namespace visutwin::canvas
{
    void ForwardRenderer::buildFrameGraph(FrameGraph* frameGraph, LayerComposition* layerComposition)
    {
        frameGraph->reset();

        // New frame for GPU skinning: bone palettes recompute lazily at first use
        // (shadow or forward pass) and are shared for the rest of the frame.
        SkinInstance::beginFrame();

        // ── Mesh-instance culling, once per (camera, layer) for the whole frame ──
        // The frame graph asks for the pairs it will actually render, and the batch
        // fills a cache both sublayer passes then read. Before this, each call of
        // renderForwardLayer swept every RenderComponent in the scene and ran the
        // frustum test itself — so a layer with an opaque and a transparent sublayer
        // paid for the whole scene twice and threw half of each pass away.
        //
        // Culling at BUILD time is safe for the same reason the light and shadow
        // culls above and below it are: camera transforms are final for the frame by
        // the time the graph is assembled. It is also what gives the precull and
        // postcull events upstream's contract, once per camera rather than once per
        // layer.
        {
            // ASPECT_AUTO before anything reads a camera's projection: the cull below,
            // and the shadow fit after it. It used to be resolved only at DRAW time, so
            // the cull saw last frame's aspect (the frustum-compare re-cull in the cull
            // cache caught the first frame); renderForwardLayer still sets it from the
            // target it actually draws to, which normally agrees.
            for (const auto* action : layerComposition->renderActions()) {
                if (action && action->camera) {
                    if (Camera* cam = action->camera->camera()) {
                        resolveAutoAspectRatio(cam);
                    }
                }
            }

            resetCulledInstances();
            for (const auto* action : layerComposition->renderActions()) {
                if (action && action->camera && action->layer) {
                    if (Camera* cam = action->camera->camera()) {
                        requestMeshInstanceCull(cam, action->layer);
                    }
                }
            }
            executeMeshInstanceCull();
        }

        // ── Light visibility, before anything is built from it ───────────────────
        // Every shadow and cookie pass below is created only for a light some camera
        // can reach, so this has to come first. It was tempting to fold it into the
        // per-camera loop further down that already culls shadow maps and dispatches
        // GPU instance culling — but that loop runs AFTER the local shadow passes are
        // built, so the passes would have been built from the PREVIOUS frame's
        // visibility. A frame-late cull is worse than none: it renders the shadow of
        // a light that has just left the view and skips one that has just entered.
        //
        // The flag is a union over cameras, so the reset is outside the loop.
        {
            resetLightVisibility();
            std::unordered_set<Camera*> lightCulledCameras;
            for (const auto* action : layerComposition->renderActions()) {
                if (action && action->camera) {
                    if (Camera* cam = action->camera->camera();
                        cam && lightCulledCameras.insert(cam).second) {
                        cullLights(cam);
                    }
                }
            }
        }

        // Cull + render local-light shadow maps for shadow-casting spot/point lights.
        // Runs in BOTH clustered and non-clustered modes. Routing under clustering:
        // every shadow-casting spot AND omni light renders into the shared
        // LightTextureAtlas — one packed 2D depth texture, a slot per light — and is
        // sampled by the clustered fragment shader, arbitrarily many and none of them
        // through the bounded main light array. A light the atlas has no slot left
        // for casts no shadow this frame, as upstream. Non-clustered mode: every
        // shadow-casting local owns its own map (a cubemap for an omni light).
        const bool clusteredMode = _scene->clusteredLightingEnabled();
        std::vector<Light*> atlasLights;             // clustered: shadow-casting spots and omnis
        {
            std::vector<Light*> localShadowLights;   // non-clustered: own maps, per-face passes
            for (auto* lightComponent : LightComponent::instances()) {
                // active(), not enabled(): a light on a disabled entity casts no shadow.
                if (!lightComponent || !lightComponent->active()) {
                    continue;
                }
                if (lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                    continue;
                }
                if (!lightComponent->castShadows()) {
                    continue;
                }
                Light* sceneLight = lightComponent->light();
                if (!sceneLight) {
                    continue;
                }
                if (clusteredMode) {
                    atlasLights.push_back(sceneLight);
                } else {
                    // A light that was atlased while clustering was on must not keep
                    // its slot: the cull would widen an omni's faces for a tile it no
                    // longer renders into, and the pass would skip it.
                    sceneLight->setAtlasViewportAllocated(false);
                    localShadowLights.push_back(sceneLight);
                }
            }
            // Assign atlas slots BEFORE culling: cullLocalLights reads each light's
            // slot to widen an omni face and to fold a spot's rect into its VP, and
            // it targets the shadow cameras at the atlas through the ShadowMap
            // wrapper the atlas installs.
            if (clusteredMode && _lightTextureAtlas) {
                // Configured right before it updates, so the first update creates the
                // atlas at the SCENE's resolution and a later change is applied on the
                // next frame (configure only records; update resizes). Configured
                // anywhere later in the frame, the first frame allocated the 2048
                // default and resized it a frame later.
                if (_scene) {
                    const auto& lightingParams = _scene->lighting();
                    _lightTextureAtlas->configure(lightingParams.shadowAtlasResolution, lightingParams.atlasSplit);
                }
                _lightTextureAtlas->update(atlasLights);
            }
            const auto& cullList = clusteredMode ? atlasLights : localShadowLights;
            if (!cullList.empty()) {
                _shadowRendererLocal->cullLocalLights(cullList, _device);
            }
            // Per-face passes for the lights that own their maps (non-clustered mode
            // only; the list is empty under clustering).
            _shadowRendererLocal->buildNonClusteredRenderPasses(frameGraph, localShadowLights);
        }

        if (clusteredMode)
        {
            const auto lighting = _scene->lighting();
            // The clustered atlas pass renders every atlased light's faces into the
            // atlas in ONE pass (RenderPassShadowLocalClustered), fed the atlas
            // lights; it takes only those with a slot and a pending render.
            // NOTE the cookie list is EMPTY, and deliberately so for now. It feeds the
            // clustered cookie ATLAS pass, which renders each clustered spot's cookie
            // into the light texture atlas — and nothing samples that atlas: `grep
            // cookie` over forward-fragment-clustered.{metal,glsl} returns nothing on
            // either backend. Filling the list would render cookies into a texture no
            // shader reads, which is worse than leaving it visibly unwired. The pass
            // itself is a faithful port and stays for when the clustered shader gains
            // cookie sampling; the list is what to fill then.
            _renderPassUpdateClustered->update(frameGraph, lighting.shadowsEnabled, lighting.cookiesEnabled,
                _lights, atlasLights);
            frameGraph->addRenderPass(_renderPassUpdateClustered);
        }

        // Cull directional shadow maps for each unique camera in the layer composition.
        // This positions shadow cameras and populates _cameraDirShadowLights.
        // Also dispatches per-camera GPU instance culling for any MeshInstances
        // that opted in via enableGpuInstanceCulling().
        // Drop the frame's grid assignments; the pooled grids themselves survive.
        // A grid is built per DISTINCT light set now, not once for the whole frame.
        resetClusters();

        {
            // Drop last frame's per-camera entries so destroyed cameras don't
            // linger as stale (dangling) keys.
            _cameraDirShadowLights.clear();

            const auto& actions = layerComposition->renderActions();

            // Directional cascades are fit for ONE designated camera per frame.
            // Each light owns a single shadow atlas and matrix palette, so
            // fitting per camera makes the fits overwrite each other and every
            // camera except the last-fitted one samples the atlas with
            // mismatched matrices — shadows swim/flicker as the fits diverge
            // (planar-reflection cameras are mirrored below the ground and
            // produce wildly different fits). Designate the presentation
            // camera: the last unique camera that renders to the backbuffer
            // (null render target); fall back to the last camera seen.
            Camera* shadowFitCamera = nullptr;
            Camera* lastCamera = nullptr;
            Camera* lightmapBakeCamera = nullptr;
            for (const auto* action : actions) {
                if (action && action->camera) {
                    if (Camera* cam = action->camera->camera()) {
                        lastCamera = cam;
                        if (cam->lightmapBakePass()) {
                            lightmapBakeCamera = cam;
                        }
                        if (!cam->renderTarget()) {
                            shadowFitCamera = cam;
                        }
                    }
                }
            }
            if (!shadowFitCamera) {
                shadowFitCamera = lastCamera;
            }
            // A lightmap bake takes priority for the frame it runs in: its cameras always
            // have a render target, so the rule above would fit the cascades to the
            // presentation camera and bake whatever shadows happen to suit the current
            // view. Fitting to the bake camera instead is what puts real shadows into the
            // lightmap; the presentation camera re-fits on the next (non-bake) frame.
            if (lightmapBakeCamera) {
                shadowFitCamera = lightmapBakeCamera;
            }

            std::unordered_set<Camera*> culledCameras;
            Camera* onlyCamera = nullptr;
            for (const auto* action : actions) {
                if (action && action->camera) {
                    Camera* cam = action->camera->camera();
                    if (cam && culledCameras.insert(cam).second) {
                        if (cam == shadowFitCamera) {
                            cullShadowmaps(cam);
                        }
                        onlyCamera = cam;
                    }
                }
            }
            // GPU instance culling has one output per mesh, filled before any pass
            // draws: cull to the frustum only when one camera draws this frame. It used
            // to run per camera, so with two or more every view drew the LAST camera's
            // set and instances vanished from the others.
            if (!culledCameras.empty()) {
                dispatchGpuInstanceCulling(culledCameras.size() == 1 ? onlyCamera : nullptr);
            }
        }

        if (auto* directionalShadowRenderer = shadowRendererDirectional()) {
            directionalShadowRenderer->buildNonClusteredRenderPasses(frameGraph, _cameraDirShadowLights);
        }

        int startIndex = 0;
        bool newStart = true;
        RenderTarget* renderTarget = nullptr;
        const auto& renderActions = layerComposition->renderActions();

        for (int i = startIndex; i < static_cast<int>(renderActions.size()); i++) {
            if (auto* renderAction = renderActions[i]; renderAction->useCameraPasses)  {
                // schedule render passes from the camera
                for (auto renderPass : renderAction->camera->renderPasses()) {
                    if (renderPass) {
                        frameGraph->addRenderPass(renderPass);
                    }
                };
            } else {
                const auto isDepthLayer = renderAction->layer->id() == LAYERID_DEPTH;
                // A camera that renders through a frame does its own grabbing, from the
                // offscreen scene target rather than the back buffer, so the depth layer
                // must NOT break its block: the camera frame has to receive every render
                // action of the camera. Splitting there handed it only the actions after
                // the grab - the opaque world and the sky went straight to the back buffer
                // and compose then overwrote them with a target holding just the
                // transparent tail, which is a black frame for any camera that asked for a
                // grab and for any post-processing at the same time.
                // The camera frame owns BOTH grabs: the scene colour copy and, since
                // 2026-09-19, the post-opaque depth copy SSR marches against
                // (CameraFrameOptions::sceneDepthMap, from requestSceneDepthMap), plus the
                // scene depth publication from its own attachment or prepass texture.
                const auto cameraOwnsGrabs = renderAction->camera &&
                    renderAction->camera->onPostprocessing() != nullptr;
                const auto  isGrabPass = isDepthLayer && !cameraOwnsGrabs &&
                    (renderAction->camera->renderSceneColorMap() || renderAction->camera->renderSceneDepthMap());

                // start of block of render actions rendering to the same render target
                if (newStart) {
                    newStart = false;
                    startIndex = i;
                    renderTarget = renderAction->renderTarget.get();
                }

                // info about the next render action
                auto* nextRenderAction = (i + 1 < static_cast<int>(renderActions.size())) ? renderActions[i + 1] : nullptr;
                const auto isNextLayerDepth = nextRenderAction ? (!nextRenderAction->useCameraPasses && nextRenderAction->layer->id() == LAYERID_DEPTH) : false;
                const auto isNextLayerGrabPass = isNextLayerDepth && !cameraOwnsGrabs &&
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

                    newStart = true;
                }
            }
        }
        // App-injected passes (Renderer::addAppendPass) run last, before frame end.
        for (const auto& appendPass : _appendPasses) {
            if (appendPass) {
                frameGraph->addRenderPass(appendPass);
            }
        }

        // Every shadow pass this frame will have has now been added, so a one-shot
        // request can be retired — and only now. Retiring it while the passes were
        // still being decided is what made the predicate impure, and a culled light
        // would have lost the shadow it asked for.
        consumeOneShotShadows();
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

        // The actions keep the COMPOSITION's firstCameraUse / lastCameraUse: whether
        // this is the camera's first / last action of the whole frame, which is what
        // prerender / postrender and the directional-shadow split mean (upstream copies
        // them into a render step and never mutates the action). They used to be
        // rewritten here per block, so a camera split into blocks fired its events once
        // per block and the next frame's split read the rewritten flag.
        for (int i = startIndex; i <= endIndex; ++i) {
            auto* ra = renderActions[i];
            if (!ra) {
                continue;
            }
            mainPass->addRenderAction(ra);
        }

        frameGraph->addRenderPass(mainPass);
    }
}
