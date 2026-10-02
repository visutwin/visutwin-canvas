// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#include "forwardRenderer.h"

#include <algorithm>
#include <unordered_set>

#include "framework/components/light/lightComponent.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/light.h"
#include "renderPassForward.h"
#include "scene/constants.h"
#include "scene/skinInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        /// Every camera the composition renders, once each, in order of first use.
        std::vector<Camera*> uniqueCameras(const std::vector<RenderAction*>& actions)
        {
            std::vector<Camera*> cameras;
            std::unordered_set<Camera*> seen;
            for (const auto* action : actions) {
                if (action && action->camera) {
                    if (Camera* cam = action->camera->camera(); cam && seen.insert(cam).second) {
                        cameras.push_back(cam);
                    }
                }
            }
            return cameras;
        }

        // Directional cascades are fit for ONE designated camera per frame.
        // Each light owns a single shadow atlas and matrix palette, so
        // fitting per camera makes the fits overwrite each other and every
        // camera except the last-fitted one samples the atlas with
        // mismatched matrices — shadows swim/flicker as the fits diverge
        // (planar-reflection cameras are mirrored below the ground and
        // produce wildly different fits). Designate the presentation
        // camera: the last camera that renders to the backbuffer
        // (null render target); fall back to the last camera seen.
        //
        // A lightmap bake takes priority for the frame it runs in: its cameras always
        // have a render target, so the rule above would fit the cascades to the
        // presentation camera and bake whatever shadows happen to suit the current
        // view. Fitting to the bake camera instead is what puts real shadows into the
        // lightmap; the presentation camera re-fits on the next (non-bake) frame.
        Camera* selectShadowFitCamera(const std::vector<RenderAction*>& actions)
        {
            Camera* presentationCamera = nullptr;
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
                            presentationCamera = cam;
                        }
                    }
                }
            }
            if (lightmapBakeCamera) {
                return lightmapBakeCamera;
            }
            return presentationCamera ? presentationCamera : lastCamera;
        }

        bool isDepthLayerAction(const RenderAction& action)
        {
            return action.layer->id() == LAYERID_DEPTH;
        }

        // A camera that renders through a frame does its own grabbing, from the
        // offscreen scene target rather than the back buffer: the camera frame owns
        // BOTH grabs, the scene colour copy and the post-opaque
        // depth copy SSR marches against (CameraFrameOptions::sceneDepthMap, from
        // requestSceneDepthMap), plus the scene depth publication from its own
        // attachment or prepass texture. Any other camera that asked for a grab gets
        // standalone grab passes at its depth layer.
        bool usesStandaloneGrabs(const RenderAction& action)
        {
            const bool cameraOwnsGrabs = action.camera && action.camera->onPostprocessing() != nullptr;
            return !cameraOwnsGrabs &&
                (action.camera->renderSceneColorMap() || action.camera->renderSceneDepthMap());
        }

        bool rendersThroughCameraFrame(const RenderAction& action)
        {
            return action.triggerPostprocess && action.camera && action.camera->onPostprocessing() != nullptr;
        }
    }

    void ForwardRenderer::buildFrameGraph(FrameGraph* frameGraph, LayerComposition* layerComposition)
    {
        frameGraph->reset();

        // New frame for GPU skinning: bone palettes recompute lazily at first use
        // (shadow or forward pass) and are shared for the rest of the frame.
        SkinInstance::beginFrame();

        // Culling happens at BUILD time, and in this order: camera transforms are
        // final for the frame by the time the graph is assembled, and every shadow
        // and cookie pass below is built from the visibility worked out here.
        cullMeshInstancesForFrame(*layerComposition);
        cullLightsForFrame(*layerComposition);

        const std::vector<Light*> atlasLights = addLocalShadowPasses(frameGraph);
        if (_scene->clusteredLightingEnabled()) {
            addClusteredLightingPass(frameGraph, atlasLights);
        }

        // Drop the frame's grid assignments; the pooled grids themselves survive.
        // A grid is built per DISTINCT light set, not once for the whole frame.
        resetClusters();

        cullDirectionalShadowsAndInstances(*layerComposition);
        if (auto* directionalShadowRenderer = shadowRendererDirectional()) {
            directionalShadowRenderer->buildNonClusteredRenderPasses(frameGraph, _cameraDirShadowLights);
        }

        addRenderActionPasses(frameGraph, layerComposition);

        // App-injected passes (Renderer::addAppendPass) run last, before frame end.
        for (const auto& appendPass : _appendPasses) {
            if (appendPass) {
                frameGraph->addRenderPass(appendPass);
            }
        }

        // Every shadow pass this frame will have has now been added, so a one-shot
        // request can be retired — and only now. Retiring it while the passes are
        // still being decided would make the predicate impure, and a culled light
        // would lose the shadow it asked for.
        consumeOneShotShadows();
    }

    void ForwardRenderer::cullMeshInstancesForFrame(LayerComposition& layerComposition)
    {
        // Mesh-instance culling, once per (camera, layer) for the whole frame.
        // The frame graph asks for the pairs it will actually render, and the batch
        // fills a cache both sublayer passes then read, rather than each sublayer
        // sweeping the whole scene and throwing away the half that belongs to the
        // other. Culling per batch is also what gives the precull and postcull events upstream's
        // contract, once per camera rather than once per layer.
        const auto& actions = layerComposition.renderActions();

        // ASPECT_AUTO before anything reads a camera's projection: the cull below,
        // and the shadow fit after it; resolved only at DRAW time, the cull would see
        // last frame's aspect. renderForwardLayer still sets it from the target it
        // actually draws to, which normally agrees.
        for (Camera* cam : uniqueCameras(actions)) {
            resolveAutoAspectRatio(cam);
        }

        resetCulledInstances();
        for (const auto* action : actions) {
            if (action && action->camera && action->layer) {
                if (Camera* cam = action->camera->camera()) {
                    requestMeshInstanceCull(cam, action->layer);
                }
            }
        }
        executeMeshInstanceCull();
    }

    void ForwardRenderer::cullLightsForFrame(LayerComposition& layerComposition)
    {
        // Light visibility, before anything is built from it. Every shadow and cookie
        // pass is created only for a light some camera can reach, so this has to come
        // first. Do not fold it into the per-camera loop that culls shadow maps and
        // dispatches GPU instance culling: that loop runs AFTER the local shadow passes
        // are built, so the passes would be built from the PREVIOUS frame's visibility. A frame-late cull is worse than none: it
        // renders the shadow of a light that has just left the view and skips one
        // that has just entered.
        //
        // The flag is a union over cameras, so the reset is outside the loop.
        resetLightVisibility();
        for (Camera* cam : uniqueCameras(layerComposition.renderActions())) {
            cullLights(cam);
        }
    }

    std::vector<Light*> ForwardRenderer::addLocalShadowPasses(FrameGraph* frameGraph)
    {
        _atlasSlotLights.clear();
        // Cull + render local-light shadow maps for shadow-casting spot/point lights.
        // Runs in BOTH clustered and non-clustered modes. Routing under clustering:
        // every shadow-casting spot AND omni light renders into the shared
        // LightTextureAtlas — one packed 2D depth texture, a slot per light — and is
        // sampled by the clustered fragment shader, arbitrarily many and none of them
        // through the bounded main light array. A light the atlas has no slot left
        // for casts no shadow this frame, as upstream. Non-clustered mode: every
        // shadow-casting local owns its own map (a cubemap for an omni light).
        const bool clusteredMode = _scene->clusteredLightingEnabled();
        std::vector<Light*> atlasLights;         // clustered: shadow-casting spots and omnis
        std::vector<Light*> localShadowLights;   // non-clustered: own maps, per-face passes
        for (auto* lightComponent : LightComponent::instances()) {
            // active(), not enabled(): a light on a disabled entity casts no shadow.
            if (!lightComponent || !lightComponent->active() ||
                lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL ||
                !lightComponent->castShadows()) {
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
            //
            // A light with a cookie needs a slot too, shadowed or not (upstream's
            // collectLights): its cookie is copied into the same rect of the cookie
            // atlas. Only the shadow casters are culled and rendered below.
            std::vector<Light*>& slotLights = _atlasSlotLights;
            slotLights = atlasLights;
            bool anyCookieLight = false;
            if (_scene && _scene->lighting().cookiesEnabled) {
                for (auto* lightComponent : LightComponent::instances()) {
                    if (!lightComponent || !lightComponent->active() || !lightComponent->cookie() ||
                        lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                        continue;
                    }
                    Light* sceneLight = lightComponent->light();
                    if (!sceneLight || !sceneLight->visibleThisFrame()) {
                        continue;
                    }
                    // The cookie's shape has to match the light, as on the main path.
                    const bool omni = lightComponent->type() != LightType::LIGHTTYPE_SPOT;
                    if (omni != lightComponent->cookie()->isCubemap()) {
                        continue;
                    }
                    anyCookieLight = true;
                    if (std::find(slotLights.begin(), slotLights.end(), sceneLight) == slotLights.end()) {
                        slotLights.push_back(sceneLight);
                    }
                }
            }
            if (_scene) {
                const auto& lightingParams = _scene->lighting();
                _lightTextureAtlas->configure(lightingParams.shadowAtlasResolution, lightingParams.atlasSplit);
                _lightTextureAtlas->configureCookies(lightingParams.cookieAtlasResolution, anyCookieLight);
            }
            _lightTextureAtlas->update(slotLights);
        }
        const auto& cullList = clusteredMode ? atlasLights : localShadowLights;
        if (!cullList.empty()) {
            _shadowRendererLocal->cullLocalLights(cullList, _device);
        }
        // Per-face passes for the lights that own their maps (non-clustered mode
        // only; the list is empty under clustering).
        _shadowRendererLocal->buildNonClusteredRenderPasses(frameGraph, localShadowLights);
        return atlasLights;
    }

    void ForwardRenderer::addClusteredLightingPass(FrameGraph* frameGraph, const std::vector<Light*>& atlasLights)
    {
        const auto lighting = _scene->lighting();
        // The clustered atlas pass renders every atlased light's faces into the
        // atlas in ONE pass (RenderPassShadowLocalClustered), fed the atlas
        // lights; it takes only those with a slot and a pending render.
        // The cookie pass copies the cookies of the scene's lights that hold an atlas
        // slot into the cookie atlas (see RenderPassCookieRenderer); the cluster loop
        // samples it.
        _renderPassUpdateClustered->update(frameGraph, lighting.shadowsEnabled, lighting.cookiesEnabled,
            _atlasSlotLights, atlasLights);
        frameGraph->addRenderPass(_renderPassUpdateClustered);
    }

    void ForwardRenderer::cullDirectionalShadowsAndInstances(LayerComposition& layerComposition)
    {
        // Positions the directional shadow cameras and populates
        // _cameraDirShadowLights. Last frame's entries are dropped first so destroyed
        // cameras don't linger as stale (dangling) keys.
        _cameraDirShadowLights.clear();

        const auto& actions = layerComposition.renderActions();
        Camera* shadowFitCamera = selectShadowFitCamera(actions);
        const std::vector<Camera*> cameras = uniqueCameras(actions);
        for (Camera* cam : cameras) {
            if (cam == shadowFitCamera) {
                cullShadowmaps(cam);
            }
        }

        // GPU instance culling has one output per mesh, filled before any pass
        // draws: cull to the frustum only when one camera draws this frame. It used
        // to run per camera, so with two or more every view drew the LAST camera's
        // set and instances vanished from the others.
        if (!cameras.empty()) {
            dispatchGpuInstanceCulling(cameras.size() == 1 ? cameras.front() : nullptr);
        }
    }

    void ForwardRenderer::addRenderActionPasses(FrameGraph* frameGraph, LayerComposition* layerComposition)
    {
        // Consecutive render actions are grouped into BLOCKS, each rendered by one
        // pass (a forward pass, or a camera frame for a post-processed camera).
        const auto& renderActions = layerComposition->renderActions();
        const int actionCount = static_cast<int>(renderActions.size());
        int startIndex = 0;
        bool newStart = true;
        RenderTarget* renderTarget = nullptr;

        for (int i = 0; i < actionCount; i++) {
            auto* renderAction = renderActions[i];
            if (renderAction->useCameraPasses) {
                // schedule render passes from the camera
                for (const auto& renderPass : renderAction->camera->renderPasses()) {
                    if (renderPass) {
                        frameGraph->addRenderPass(renderPass);
                    }
                }
                continue;
            }

            // start of block of render actions rendering to the same render target
            if (newStart) {
                newStart = false;
                startIndex = i;
                renderTarget = renderAction->renderTarget.get();
            }

            const RenderAction* nextRenderAction = i + 1 < actionCount ? renderActions[i + 1] : nullptr;
            if (endsRenderActionBlock(*renderAction, nextRenderAction, renderTarget)) {
                addRenderActionBlock(frameGraph, layerComposition, renderTarget, startIndex, i);
                newStart = true;
            }
        }
    }

    bool ForwardRenderer::endsRenderActionBlock(const RenderAction& action, const RenderAction* next,
        const RenderTarget* blockTarget) const
    {
        // The last action ends its block.
        if (!next) {
            return true;
        }

        // A camera that grabs through standalone passes ends a block at its depth
        // layer, and the block before it, so the grab runs between the two. A camera
        // frame must NOT be split there: it has to receive every render action of the
        // camera. Splitting there would hand it only the actions after the grab - the
        // opaque world and the sky would go straight to the back buffer and compose
        // would overwrite them with a target holding just the transparent tail, which is
        // a black frame for any camera that asked for a grab and for any
        // post-processing at the same time.
        const bool isGrabPass = isDepthLayerAction(action) && usesStandaloneGrabs(action);
        const bool isNextLayerDepth = !next->useCameraPasses && isDepthLayerAction(*next);
        const bool isNextLayerGrabPass = isNextLayerDepth && usesStandaloneGrabs(action);
        if (isGrabPass || isNextLayerGrabPass) {
            return true;
        }

        // The depth layer uses a null render target (separate from the camera's target).
        // When it is NOT a grab pass it will be skipped as depth-only, so it should not
        // break the current block — otherwise the camera-frame postprocessing pass ends
        // up missing the opaque world layer that precedes the depth layer, causing the
        // scene to render black when TAA/DOF is enabled.
        const bool nextIsNonGrabDepth = isNextLayerDepth && !isNextLayerGrabPass;
        if (next->renderTarget.get() != blockTarget && !nextIsNonGrabDepth) {
            return true;
        }

        // A camera's first action needs its directional shadows rendered before it.
        Camera* nextCamera = next->camera ? next->camera->camera() : nullptr;
        return next->firstCameraUse && _cameraDirShadowLights.contains(nextCamera);
    }

    void ForwardRenderer::addRenderActionBlock(FrameGraph* frameGraph, LayerComposition* layerComposition,
        RenderTarget* renderTarget, const int startIndex, const int endIndex)
    {
        RenderAction* lastAction = layerComposition->renderActions()[endIndex];
        const bool isDepthLayer = isDepthLayerAction(*lastAction);
        const bool useCameraFrame = rendersThroughCameraFrame(*lastAction);

        // A block holding nothing but the depth layer renders nothing itself.
        if (const bool isDepthOnly = isDepthLayer && startIndex == endIndex; !isDepthOnly) {
            if (useCameraFrame) {
                addCameraFramePass(frameGraph, layerComposition, startIndex, endIndex);
            } else {
                addMainRenderPass(frameGraph, layerComposition, renderTarget, startIndex, endIndex);
            }
        }

        // depth layer triggers grab passes if enabled
        if (isDepthLayer && !useCameraFrame) {
            addGrabPasses(frameGraph, *lastAction);
        }
    }

    void ForwardRenderer::addCameraFramePass(FrameGraph* frameGraph, LayerComposition* layerComposition,
        const int startIndex, const int endIndex)
    {
        const auto& renderActions = layerComposition->renderActions();
        std::vector<RenderAction*> blockActions;
        blockActions.reserve(static_cast<size_t>(endIndex - startIndex + 1));
        for (int actionIndex = startIndex; actionIndex <= endIndex; ++actionIndex) {
            if (auto* blockAction = renderActions[actionIndex]) {
                blockActions.push_back(blockAction);
            }
        }
        if (blockActions.empty()) {
            return;
        }

        // Get or create persistent CameraFrame on the camera component.
        // The CameraFrame manages its own internal offscreen render
        // targets (scene color, depth, TAA history). Persisting it across
        // frames avoids reallocating ~56MB of GPU textures per frame and
        // preserves TAA history for correct temporal accumulation.
        CameraComponent* cameraComponent = renderActions[endIndex]->camera;
        auto cameraFramePass = cameraComponent->cameraFrame();
        if (!cameraFramePass) {
            cameraFramePass = std::make_shared<RenderPassCameraFrame>(
                _device, layerComposition, _scene.get(), this, blockActions, cameraComponent, nullptr);
            cameraComponent->setCameraFrame(cameraFramePass);
        } else {
            cameraFramePass->updateSourceActions(
                blockActions, layerComposition, _scene.get(), this, nullptr);
        }
        frameGraph->addRenderPass(cameraFramePass);
    }

    void ForwardRenderer::addGrabPasses(FrameGraph* frameGraph, const RenderAction& depthLayerAction)
    {
        CameraComponent* cameraComponent = depthLayerAction.camera;
        if (cameraComponent->renderSceneColorMap()) {
            if (const auto colorGrabPass = cameraComponent->camera()->renderPassColorGrab()) {
                colorGrabPass->setSource(depthLayerAction.renderTarget);
                frameGraph->addRenderPass(colorGrabPass);
            }
        }
        if (cameraComponent->renderSceneDepthMap()) {
            if (const auto depthGrabPass = cameraComponent->camera()->renderPassDepthGrab()) {
                frameGraph->addRenderPass(depthGrabPass);
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

        // The actions keep the COMPOSITION's firstCameraUse / lastCameraUse: whether
        // this is the camera's first / last action of the whole frame, which is what
        // prerender / postrender and the directional-shadow split mean (upstream copies
        // them into a render step and never mutates the action). Do not rewrite them
        // here per block: a camera split into blocks would fire its events once per
        // block, and the next frame's split would read the rewritten flag.
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
