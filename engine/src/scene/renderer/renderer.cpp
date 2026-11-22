// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025
//
// Renderer: construction, light visibility and the cluster grids. Culling, shadow-map culling
// and the forward layer are in renderer<Part>.cpp, sharing rendererInternal.h.
#include "rendererInternal.h"

namespace visutwin::canvas
{
    using namespace renderer_internal;

    namespace renderer_internal
    {
        /// A normalized camera or scissor rect in pixels of a target. The rect's
        /// origin is bottom-left; the viewport and scissor origin is top-left.
        PixelRect normalizedRectToPixels(const Vector4& rect, const int targetWidth, const int targetHeight)
        {
            const auto clamp01 = [](const float v) { return std::clamp(v, 0.0f, 1.0f); };
            const float xNorm = clamp01(rect.getX());
            const float yNorm = clamp01(rect.getY());
            const float wNorm = clamp01(rect.getZ());
            const float hNorm = clamp01(rect.getW());
            const float topNorm = clamp01(yNorm + hNorm);

            PixelRect pixels;
            pixels.x = std::clamp(static_cast<int>(xNorm * static_cast<float>(targetWidth)),
                0, std::max(targetWidth - 1, 0));
            pixels.y = std::clamp(targetHeight - static_cast<int>(topNorm * static_cast<float>(targetHeight)),
                0, std::max(targetHeight - 1, 0));
            pixels.w = std::clamp(std::max(1, static_cast<int>(wNorm * static_cast<float>(targetWidth))),
                1, targetWidth - pixels.x);
            pixels.h = std::clamp(std::max(1, static_cast<int>(hNorm * static_cast<float>(targetHeight))),
                1, targetHeight - pixels.y);
            return pixels;
        }
    }

    // Material's base cull mode — reads the parameter map (unordered_map lookup).
    // This is the expensive part that should be cached per material. Declared in
    // cullModeResolve.h: the depth-only draw (shadows, prepass) needs the same
    // answer as the forward pass.
    CullMode resolveMaterialCullMode(const Material* material)
        {
            CullMode mode = material ? material->cullMode() : CullMode::CULLFACE_BACK;
            if (material) {
                int cullModeValue = static_cast<int>(mode);
                if (readParameterInt(findMaterialParameter(material, {"material_cullMode", "cullMode"}), cullModeValue)) {
                    if (cullModeValue >= static_cast<int>(CullMode::CULLFACE_NONE) &&
                        cullModeValue <= static_cast<int>(CullMode::CULLFACE_FRONTANDBACK)) {
                        mode = static_cast<CullMode>(cullModeValue);
                    }
                }
            }
            return mode;
        }

    // Node-scale flip — trivial per-draw float check, not worth caching.
    CullMode applyNodeScaleFlip(const CullMode mode, GraphNode* node)
        {
            if ((mode == CullMode::CULLFACE_BACK || mode == CullMode::CULLFACE_FRONT) && node) {
                if (node->worldScaleSign() < 0.0f) {
                    return (mode == CullMode::CULLFACE_FRONT) ? CullMode::CULLFACE_BACK : CullMode::CULLFACE_FRONT;
                }
            }
            return mode;
        }

    // Combined convenience wrapper (used where caching is not needed).
    CullMode resolveCullMode(const Material* material, GraphNode* node)
    {
        return applyNodeScaleFlip(resolveMaterialCullMode(material), node);
    }

    Renderer::Renderer(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Scene>& scene) : _device(device), _scene(scene)
    {
        // Startup self-test: guards the attenuation and falloff curves against regressions.
        runLightingValidationSelfTest();

        _lightTextureAtlas = std::make_unique<LightTextureAtlas>(device);

        _shadowRenderer = std::make_unique<ShadowRenderer>();

        // VISUTWIN_CULL_CHECK=1: prove every reused cull set and draw order against the
        // sweep and the sort, in this process (see cullCheckMismatches).
        if (const char* value = std::getenv("VISUTWIN_CULL_CHECK"); value && *value && *value != '0') {
            _cullCheck = true;
        }
        // VISUTWIN_CULL_REUSE=0: cull every set every frame, the A/B control.
        if (const char* value = std::getenv("VISUTWIN_CULL_REUSE"); value && *value == '0') {
            _cullReuse = false;
        }

        _shadowRendererLocal = std::make_unique<ShadowRendererLocal>(_shadowRenderer.get());
        _shadowRendererDirectional = std::make_unique<ShadowRendererDirectional>(device, _shadowRenderer.get());

        // Always construct the clustered update pass so clustered lighting can be
        // toggled on the scene at any time (it is only added to the frame graph by
        // ForwardRenderer when clusteredLightingEnabled() is true). Constructing it
        // unconditionally avoids a null deref when clustering is enabled after init.
        _renderPassUpdateClustered = std::make_unique<RenderPassUpdateClustered>(
            device, this, _shadowRenderer.get(), _shadowRendererLocal.get(), _lightTextureAtlas.get()
        );

    }

    ComponentRegistry* Renderer::componentRegistry() const
    {
        return _scene ? _scene->componentRegistry() : nullptr;
    }

    void Renderer::syncShadowComponentRegistry()
    {
        _shadowRenderer->setComponentRegistry(componentRegistry());
    }

    void Renderer::resetLightVisibility()
    {
        for (auto* lightComponent : instancesOf<LightComponent>(componentRegistry())) {
            if (lightComponent) {
                if (Light* sceneLight = lightComponent->light()) {
                    sceneLight->setVisibleThisFrame(false);
                    sceneLight->setMaxScreenSize(0.0f);
                    // An atlas slot is this frame's too. The atlas only touches the
                    // lights it is handed, which are the visible ones, so a light
                    // culled this frame would otherwise keep last frame's slot — a rect
                    // another light may now own — for the cluster data and the cookie
                    // pass to read.
                    sceneLight->setAtlasViewportAllocated(false);
                    sceneLight->setAtlasSlotUpdated(false);
                }
            }
        }
    }

    void Renderer::cullLights(Camera* camera)
    {
        const ScopedMilliseconds cullTimer(_device->frameCounters().cullTime);
        GraphNode* cameraNode = camera ? camera->node() : nullptr;
        if (!camera || !cameraNode) {
            return;
        }
        const Frustum frustum = buildCameraFrustum(camera, cameraNode);
        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();

        for (auto* lightComponent : instancesOf<LightComponent>(componentRegistry())) {
            // active(), not enabled(): a light on a disabled entity lights nothing,
            // so it has no shadow map or cookie to render either.
            if (!lightComponent || !lightComponent->active()) {
                continue;
            }
            Light* sceneLight = lightComponent->light();
            if (!sceneLight) {
                continue;
            }

            // A directional light has no position and no range, so there is nothing
            // to test: it lights whatever the camera can see, by construction.
            if (lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                sceneLight->setVisibleThisFrame(true);
                continue;
            }

            // A light an earlier camera already reached is still TESTED: its screen size
            // is the maximum over every camera,
            // and skipping it let only the first camera rank it for an atlas slot.
            const BoundingSphere bounds = sceneLight->boundingSphere();
            if (frustum.checkSphere(bounds.center(), bounds.radius())) {
                sceneLight->setVisibleThisFrame(true);
                // The union over cameras of the viewport fraction the light covers,
                // which ranks it for an atlas slot.
                sceneLight->setMaxScreenSize(std::max(sceneLight->maxScreenSize(), camera->screenSize(bounds)));
                continue;
            }
            if (sceneLight->visibleThisFrame()) {
                continue;   // another camera reached it; this one does not see it
            }

            // The one exception, and it is about allocation rather than
            // visibility: outside clustered lighting the shadow passes still read a
            // culled light's map, so a caster that has never had one allocated is
            // marked visible to get one. A light that already has its map stays
            // culled and simply does not re-render it.
            if (!clusteredEnabled && sceneLight->castShadows() && !sceneLight->shadowMap()) {
                sceneLight->setVisibleThisFrame(true);
            }
        }
    }

    void Renderer::resetClusters()
    {
        _clustersByLightSet.clear();
        _clustersUsedThisFrame = 0;
    }

    WorldClusters* Renderer::clustersForLightSet(const uint64_t lightSetHash,
        const std::vector<ClusterLightData>& lights)
    {
        // Two layers that see the same lights share a grid — the content depends on
        // the lights and nothing else, so building it twice would produce the same
        // cells twice. That is the reason for the hash.
        if (const auto found = _clustersByLightSet.find(lightSetHash);
            found != _clustersByLightSet.end()) {
            return found->second;
        }

        // A grid the pool already owns, or a new one. Pooled because the cell and
        // light buffers are large enough that per-frame allocation would churn.
        if (_clustersUsedThisFrame >= _clusterPool.size()) {
            _clusterPool.push_back(std::make_unique<WorldClusters>(_clusterConfig));
        }
        WorldClusters* clusters = _clusterPool[_clustersUsedThisFrame++].get();
        {
            FrameCounters& counters = _device->frameCounters();
            const ScopedMilliseconds clusterTimer(counters.lightClustersTime);
            clusters->update(lights);
            counters.lightClusters = static_cast<int>(_clustersUsedThisFrame);
        }
        _clustersByLightSet[lightSetHash] = clusters;
        return clusters;
    }

    uint64_t Renderer::lightSetHash(std::vector<const void*> members)
    {
        // Order-independent hash of the member set: sort, then fold. XOR alone
        // would be order-independent too and would collide on any pair repeated.
        std::sort(members.begin(), members.end());
        uint64_t hash = kFnv1aOffsetBasis;
        for (const void* member : members) {
            hash = fnv1aBytesOf(hash, reinterpret_cast<uintptr_t>(member));
        }
        return hash;
    }

    void Renderer::bindLayerClusters(const WorldClusters* clusters)
    {
        // Bind cluster GPU buffers. EVERY layer binds, because every layer may be
        // on a different grid; binding only on the frame's first layer would leave
        // the rest reading whatever was still bound.
        if (clusters->lightCount() > 0) {
            _device->setClusterBuffers(
                clusters->lightData(), clusters->lightDataSize(),
                clusters->cellData(), clusters->cellDataSize());

            // Pack cluster grid params into LightingUniforms.
            const auto& bMin = clusters->boundsMin();
            const auto bRange = clusters->boundsRange();
            const auto cellsBySize = clusters->cellsCountByBoundsSize();
            const auto& cfg = clusters->config();

            float boundsMinArr[3];
            float boundsRangeArr[3];
            float cellsBySizeArr[3];
            bMin.store(boundsMinArr);
            bRange.store(boundsRangeArr);
            cellsBySize.store(cellsBySizeArr);

            _device->setClusterGridParams(boundsMinArr, boundsRangeArr, cellsBySizeArr,
                cfg.cellsX, cfg.cellsY, cfg.cellsZ, cfg.maxLightsPerCell,
                clusters->lightCount());
        } else {
            // No clustered lights for THIS layer: zero the grid params so the
            // shader's cell bounds check rejects every fragment. Otherwise
            // the previously bound cluster buffers stay live and deleted
            // lights keep illuminating the scene.
            const float zero3[3] = {0.0f, 0.0f, 0.0f};
            _device->setClusterGridParams(zero3, zero3, zero3, 0, 0, 0, 0, 0);
        }
    }

    void Renderer::resetCulledInstances()
    {
        // A set used last frame is KEPT, content and all: executeMeshInstanceCull reuses
        // it while its stamps hold, and otherwise culls into its storage, which a large
        // scene's lists fill to the same size every frame. One that was NOT used last
        // frame belongs to a camera or layer that has gone, and is dropped — which also
        // keeps the raw pointer keys from outliving what they point at by more than a
        // frame.
        for (auto it = _culledInstances.begin(); it != _culledInstances.end();) {
            if (!it->second.used) {
                it = _culledInstances.erase(it);
                continue;
            }
            it->second.used = false;
            ++it;
        }
        _cullCameras.clear();
        _cullRequests.clear();
    }
}
