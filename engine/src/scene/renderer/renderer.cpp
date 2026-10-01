// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#include "renderer.h"
#include "cullModeResolve.h"
#include "scene/renderer/sortDistance.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstring>
#include <numbers>
#include <set>

#include "core/scopedTimer.h"
#include "core/objectPool.h"
#include "lightCamera.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/light/lightComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/instanceCuller.h"
#include "scene/frustumUtils.h"
#include "scene/renderer/sortKey.h"
#include "scene/light.h"
#include "scene/morph.h"
#include "scene/morphInstance.h"
#include "scene/skinInstance.h"
#include "scene/gsplat/gsplatInstance.h"
#include "scene/particles/particleEmitter.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/material.h"
#include "scene/graphNode.h"
#include "scene/shader-lib/programLibrary.h"
#include "scene/lighting/worldClusters.h"
#include "framework/batching/skinBatchInstance.h"
#include "lightingValidation.h"
#include "renderPassUpdateClustered.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr std::array<std::array<float, 2>, 16> haltonSequence = {{
            {0.5f, 0.333333f},
            {0.25f, 0.666667f},
            {0.75f, 0.111111f},
            {0.125f, 0.444444f},
            {0.625f, 0.777778f},
            {0.375f, 0.222222f},
            {0.875f, 0.555556f},
            {0.0625f, 0.888889f},
            {0.5625f, 0.037037f},
            {0.3125f, 0.370370f},
            {0.8125f, 0.703704f},
            {0.1875f, 0.148148f},
            {0.6875f, 0.481481f},
            {0.4375f, 0.814815f},
            {0.9375f, 0.259259f},
            {0.03125f, 0.592593f}
        }};

        struct ForwardDrawEntry
        {
            MeshInstance* meshInstance = nullptr;
            Material* material = nullptr;
            // Raw pointers — buffers are kept alive by the Mesh objects (which are kept alive
            // by MeshInstances in the same draw entry). Using raw pointers avoids atomic
            // refcount increments/decrements per draw entry (~200-2000 ops/frame savings).
            std::shared_ptr<VertexBuffer> vertexBuffer;
            std::shared_ptr<IndexBuffer> indexBuffer;
            Primitive primitive;
            uint64_t sortKey = 0;
            // Signed depth along the camera forward vector (see sortDistance.h), NOT a
            // radial distance: transparent draws sort back-to-front on this.
            float sortDistance = 0.0f;
        };

        struct LightDispatchEntry
        {
            GpuLightData light;
            uint32_t mask = MASK_AFFECT_DYNAMIC;
            Light* sceneLight = nullptr;  // for clustered spot-shadow atlas lookup
            // Fraction of the viewport height this light's bounds cover, for THIS
            // camera. Ranks local lights when more are visible than the shader has
            // slots. 1 for a directional light, which covers everything.
            float screenSize = 1.0f;
        };

        struct PixelRect
        {
            int x = 0;
            int y = 0;
            int w = 1;
            int h = 1;
        };

        /// A normalized camera or scissor rect in pixels of a target. Upstream rect
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

        /// Thin wrapper: the layout itself lives in sortKey.h so a test can hold it.
        uint64_t makeOpaqueSortKey(const MeshInstance* meshInstance)
        {
            const auto* material = meshInstance ? meshInstance->material() : nullptr;
            return makeForwardSortKey(
                meshInstance ? meshInstance->drawBucket() : 0u,
                material && material->alphaMode() == AlphaMode::MASK,
                // No material means the default one, and they all sort together.
                material ? material->id() : 0x7FFFFFu,
                // The mesh's id, never its address: see sortKey.h.
                meshInstance && meshInstance->mesh() ? meshInstance->mesh()->id() : 0u);
        }

    }

    // Material's base cull mode — reads the parameter map (unordered_map lookup).
    // This is the expensive part that should be cached per material. Declared in
    // cullModeResolve.h: the depth-only draw (shadows, prepass) needs the same
    // answer as the forward pass.
    CullMode resolveMaterialCullMode(const Material* material)
        {
            auto readIntParameter = [](const Material::ParameterValue* value, int& out) -> bool {
                if (!value) {
                    return false;
                }
                if (const auto* v = std::get_if<int32_t>(value)) {
                    out = static_cast<int>(*v);
                    return true;
                }
                if (const auto* v = std::get_if<uint32_t>(value)) {
                    out = static_cast<int>(*v);
                    return true;
                }
                if (const auto* v = std::get_if<float>(value)) {
                    out = static_cast<int>(*v);
                    return true;
                }
                if (const auto* v = std::get_if<bool>(value)) {
                    out = *v ? 1 : 0;
                    return true;
                }
                return false;
            };

            CullMode mode = material ? material->cullMode() : CullMode::CULLFACE_BACK;
            if (material) {
                int cullModeValue = static_cast<int>(mode);
                const auto* cullModeParam = material->parameter("material_cullMode");
                if (!cullModeParam) {
                    cullModeParam = material->parameter("cullMode");
                }
                if (readIntParameter(cullModeParam, cullModeValue)) {
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
        // DEVIATION: startup self-test guards recent attenuation/falloff regressions in this port.
        runLightingValidationSelfTest();

        _lightTextureAtlas = std::make_unique<LightTextureAtlas>(device);

        _shadowRenderer = std::make_unique<ShadowRenderer>();

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

    void Renderer::resetLightVisibility()
    {
        for (auto* lightComponent : LightComponent::instances()) {
            if (lightComponent) {
                if (Light* sceneLight = lightComponent->light()) {
                    sceneLight->setVisibleThisFrame(false);
                    sceneLight->setMaxScreenSize(0.0f);
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

        for (auto* lightComponent : LightComponent::instances()) {
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
            // is the maximum over every camera (upstream's culler updates it per camera),
            // and skipping it let only the first camera rank it for an atlas slot.
            const BoundingSphere bounds = sceneLight->boundingSphere();
            if (frustum.checkSphere(bounds.center(), bounds.radius())) {
                sceneLight->setVisibleThisFrame(true);
                // The union over cameras of the viewport fraction the light covers,
                // which ranks it for an atlas slot (upstream maxScreenSize).
                sceneLight->setMaxScreenSize(std::max(sceneLight->maxScreenSize(), camera->screenSize(bounds)));
                continue;
            }
            if (sceneLight->visibleThisFrame()) {
                continue;   // another camera reached it; this one does not see it
            }

            // Upstream's one exception, and it is about allocation rather than
            // visibility: outside clustered lighting the shadow passes still read a
            // culled light's map, so a caster that has never had one allocated is
            // marked visible to get one. A light that already has its map stays
            // culled and simply does not re-render it.
            if (!clusteredEnabled && sceneLight->castShadows() && !sceneLight->shadowMap()) {
                sceneLight->setVisibleThisFrame(true);
            }
        }
    }

    namespace
    {
        // Exact comparison on purpose: the two frusta are built from the same matrices
        // by the same code, so they are bit-identical unless the camera actually
        // changed. A tolerance here would hide the case this guard exists for.
        bool frustumsEqual(const Frustum& a, const Frustum& b)
        {
            for (int i = 0; i < 6; ++i) {
                if (a.planes[i] != b.planes[i]) {
                    return false;
                }
            }
            return true;
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
        // cells twice. That is upstream's rule and the reason for the hash.
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
        uint64_t hash = 1469598103934665603ull;   // FNV-1a offset basis
        for (const void* member : members) {
            uint64_t value = reinterpret_cast<uintptr_t>(member);
            for (int byte = 0; byte < 8; ++byte) {
                hash ^= (value & 0xFFull);
                hash *= 1099511628211ull;
                value >>= 8;
            }
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
        // An entry culled last frame keeps its vectors' storage for this one: a large
        // scene's lists are the same size every frame, and clearing the map made each
        // grow from nothing again. One that was NOT culled last frame belongs to a
        // camera or layer that has gone, and is dropped — which also keeps the raw
        // pointer keys from outliving what they point at by more than a frame.
        for (auto it = _culledInstances.begin(); it != _culledInstances.end();) {
            if (!it->second.valid) {
                it = _culledInstances.erase(it);
                continue;
            }
            it->second.valid = false;
            it->second.opaque.clear();
            it->second.transparent.clear();
            ++it;
        }
        _cullCameras.clear();
        _cullRequests.clear();
    }

    void Renderer::requestMeshInstanceCull(Camera* camera, Layer* layer)
    {
        if (!camera || !layer) {
            return;
        }
        auto& layers = _cullRequests[camera];
        if (layers.empty()) {
            _cullCameras.push_back(camera);
        }
        // De-duplicated, because a camera asks for the same layer twice — once for
        // its opaque sublayer and once for its transparent one — and culling it
        // twice is the waste this whole cache exists to remove.
        if (std::find(layers.begin(), layers.end(), layer) == layers.end()) {
            layers.push_back(layer);
        }
    }

    void Renderer::executeMeshInstanceCull()
    {
        _device->frameCounters().camerasRendered += static_cast<int>(_cullCameras.size());
        for (Camera* camera : _cullCameras) {
            // Before the frustum is built, so a listener may still move the camera.
            // Upstream passes the owning camera COMPONENT, or nothing for an internal
            // camera (shadow, reflection, picker); this port has no back pointer from
            // Camera to its component, so it passes the camera.
            if (_scene) {
                _scene->fire("precull", camera);
            }
            // Every layer this camera asked for, in ONE sweep of the scene.
            GraphNode* cameraNode = camera ? camera->node() : nullptr;
            const std::vector<Layer*>& layers = _cullRequests[camera];
            static thread_local std::vector<CulledInstances*> outs;
            outs.clear();
            for (Layer* layer : layers) {
                outs.push_back(&_culledInstances[{camera, layer}]);
            }
            cullMeshInstances(camera, cameraNode, layers.data(), outs.data(), layers.size());
            if (_scene) {
                _scene->fire("postcull", camera);
            }
        }
        _cullCameras.clear();
        _cullRequests.clear();
    }

    const Renderer::CulledInstances& Renderer::culledInstances(Camera* camera,
        GraphNode* cameraNode, Layer* layer)
    {
        const auto key = std::make_pair(camera, layer);
        CulledInstances& entry = _culledInstances[key];

        // A hit only counts if it was culled against the frustum this camera has NOW.
        // On the first frame the aspect ratio can change between the batch and the
        // pass, so the cached set would be the answer for a differently shaped view.
        const bool hasCameraFrustum = camera && cameraNode;
        const Frustum current = hasCameraFrustum
            ? buildCameraFrustum(camera, cameraNode) : Frustum{};
        if (entry.valid && frustumsEqual(entry.frustum, current)) {
            return entry;
        }

        // Either the frame graph never asked for this pair — an app-appended pass
        // reaches here with a camera the composition never saw, and rendering nothing
        // would be the wrong answer — or the camera has changed since it did.
        cullMeshInstancesInto(camera, cameraNode, layer, entry);
        return entry;
    }

    void Renderer::cullMeshInstancesInto(Camera* camera, GraphNode* cameraNode, Layer* layer,
        CulledInstances& out)
    {
        CulledInstances* outs[1] = {&out};
        cullMeshInstances(camera, cameraNode, &layer, outs, 1);
    }

    void Renderer::cullMeshInstances(Camera* camera, GraphNode* cameraNode, Layer* const* layers,
        CulledInstances* const* outs, const size_t layerCount)
    {
        // A component's layers are matched against the requested ones through a bit per
        // request, so the sweep below handles at most this many at once; a camera asking
        // for more (none does) is served in slices.
        constexpr size_t kMaxLayersPerSweep = 64;
        if (layerCount > kMaxLayersPerSweep) {
            cullMeshInstances(camera, cameraNode, layers, outs, kMaxLayersPerSweep);
            cullMeshInstances(camera, cameraNode, layers + kMaxLayersPerSweep, outs + kMaxLayersPerSweep,
                layerCount - kMaxLayersPerSweep);
            return;
        }

        const ScopedMilliseconds cullTimer(_device->frameCounters().cullTime);

        const bool hasCameraFrustum = camera && cameraNode;
        const Frustum cameraFrustum = hasCameraFrustum
            ? buildCameraFrustum(camera, cameraNode) : Frustum{};
        const uint32_t cullingMask = camera ? camera->cullingMask() : 0xFFFFFFFFu;

        std::array<int, kMaxLayersPerSweep> layerIds{};
        uint64_t requested = 0;
        for (size_t i = 0; i < layerCount; ++i) {
            CulledInstances& out = *outs[i];
            out.opaque.clear();
            out.transparent.clear();
            if (!layers[i]) {
                continue;
            }
            out.frustum = cameraFrustum;
            out.valid = true;
            layerIds[i] = layers[i]->id();
            requested |= uint64_t{1} << i;
        }
        if (requested == 0) {
            return;
        }

        // The same for every instance of the sweep.
        const auto fallbackMaterial = getDefaultMaterial(_device);

        enum class Bucket { None, Opaque, Transparent };
        // Whether the camera draws this instance, and in which sublayer. Decided ONCE per
        // instance: an instance on several requested layers goes into each of their
        // buckets on this one answer.
        const auto classify = [&](MeshInstance* meshInstance) {
            if (!meshInstance || !meshInstance->visible() || !meshInstance->mesh()) {
                return Bucket::None;
            }
            if (!meshInstance->mesh()->hasVertexBuffer()) {
                return Bucket::None;
            }
            // Upstream's Camera.cullingMask against MeshInstance.mask: a camera that
            // wants a subset of the scene says so here rather than by juggling layers.
            if ((meshInstance->mask() & cullingMask) == 0u) {
                return Bucket::None;
            }

            const Material* material = meshInstance->material()
                ? meshInstance->material() : fallbackMaterial.get();
            if (!material) {
                return Bucket::None;
            }

            // A skybox is drawn around the camera and has no meaningful bounds, so it
            // is never culled.
            if (!material->isSkybox() && hasCameraFrustum && meshInstance->cull() &&
                !isVisibleInFrustum(cameraFrustum, meshInstance->aabb())) {
                return Bucket::None;
            }

            // Split here, ONCE, so each sublayer reads only its own bucket.
            return material->transparent() ? Bucket::Transparent : Bucket::Opaque;
        };
        const auto push = [&](const size_t i, const Bucket bucket, MeshInstance* meshInstance) {
            (bucket == Bucket::Transparent ? outs[i]->transparent : outs[i]->opaque).push_back(meshInstance);
        };

        // ONE sweep of the components for all of the camera's layers, rather than one
        // per layer — five for a default camera (World, Depth, Skybox, UI, Immediate),
        // four of which find nearly nothing. The order inside each bucket is what a
        // per-layer sweep would produce: components in creation order, then the layer's
        // own instances.
        for (auto* renderComponent : RenderComponent::instances()) {
            // active() covers both halves: the component's own flag and the owning
            // entity's hierarchy state.
            if (!renderComponent || !renderComponent->active()) {
                continue;
            }
            uint64_t onLayers = 0;
            for (const int layerId : renderComponent->layers()) {
                for (size_t i = 0; i < layerCount; ++i) {
                    if (layerIds[i] == layerId) {
                        onLayers |= uint64_t{1} << i;
                    }
                }
            }
            onLayers &= requested;
            if (onLayers == 0) {
                continue;
            }
            for (auto* meshInstance : renderComponent->meshInstances()) {
                const Bucket bucket = classify(meshInstance);
                if (bucket == Bucket::None) {
                    continue;
                }
                for (uint64_t remaining = onLayers; remaining != 0; remaining &= remaining - 1) {
                    push(static_cast<size_t>(std::countr_zero(remaining)), bucket, meshInstance);
                }
            }
        }

        // Instances added to a layer directly rather than through a component.
        for (size_t i = 0; i < layerCount; ++i) {
            if (!layers[i]) {
                continue;
            }
            for (auto* meshInstance : layers[i]->meshInstances()) {
                if (const Bucket bucket = classify(meshInstance); bucket != Bucket::None) {
                    push(i, bucket, meshInstance);
                }
            }
        }
    }

    void Renderer::consumeOneShotShadows()
    {
        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();
        auto consume = [&](Light* light) {
            if (!light || !_shadowRenderer || !_shadowRenderer->needsShadowRendering(light)) {
                return;
            }
            _device->frameCounters().shadowMapUpdates += light->numShadowFaces();
            if (light->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_THISFRAME) {
                light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);
            }
        };

        // Local lights. Under clustered lighting a spot also needs an atlas slot to
        // have been allocated, or no pass was built for it and its request stands.
        for (auto* lightComponent : LightComponent::instances()) {
            if (!lightComponent || !lightComponent->active() ||
                lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                continue;
            }
            Light* sceneLight = lightComponent->light();
            if (clusteredEnabled && sceneLight && !sceneLight->atlasViewportAllocated()) {
                continue;
            }
            consume(sceneLight);
        }

        // A directional light's shadow is fit and rendered per camera, so its lights
        // are reached through the per-camera map rather than the component list.
        for (const auto& [cullCamera, cameraLights] : _cameraDirShadowLights) {
            (void)cullCamera;
            for (Light* light : cameraLights) {
                consume(light);
            }
        }
    }

    void Renderer::cullShadowmaps(Camera* camera)
    {
        // Refresh only this camera's entry — buildFrameGraph calls this once per
        // unique camera, and clearing the whole map here would wipe the entries
        // of previously culled cameras (breaking directional shadows whenever
        // more than one camera renders).
        _cameraDirShadowLights.erase(camera);

        if (!camera || !_shadowRendererDirectional) {
            return;
        }

        std::vector<Light*> dirShadowLights;

        for (auto* lightComponent : LightComponent::instances()) {
            // active(), not enabled(): a light on a disabled entity must stop lighting.
            if (!lightComponent || !lightComponent->active()) {
                continue;
            }
            if (lightComponent->type() != LightType::LIGHTTYPE_DIRECTIONAL || !lightComponent->castShadows()) {
                continue;
            }

            Light* sceneLight = lightComponent->light();
            if (!sceneLight) {
                continue;
            }

            // Allocate shadow map if not yet created; the light owns it.
            if (!sceneLight->shadowMap()) {
                sceneLight->setShadowMap(ShadowMap::create(_device.get(), sceneLight));
            }

            if (!sceneLight->shadowMap()) {
                continue;
            }

            // Set up the shadow camera (position, projection, snap) — unless the light
            // is at SHADOWUPDATE_NONE, whose map was rendered once and will not be
            // again. The fit follows the CAMERA (the cascades are sliced from its
            // frustum), and this port writes the sampling matrices — the palette and
            // the per-cascade fit — during the cull, then reads them at bind time. So a
            // re-fit here would move every sampling matrix with the view while the
            // texture stayed put, and a one-shot shadow came out wrong the moment the
            // camera moved. Upstream can afford to re-fit every frame because it writes
            // its shadowMatrix inside the shadow pass, so under NONE the matrix keeps
            // the fit the texture was rendered with; skipping the fit is the same
            // invariant reached from the other side. The light still joins the list:
            // the forward pass reads its matrices from it, and the pass builder and the
            // one-shot consume both gate on needsShadowRendering themselves.
            if (sceneLight->shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_NONE) {
                _shadowRendererDirectional->cull(sceneLight, camera);
            }

            dirShadowLights.push_back(sceneLight);
        }

        if (!dirShadowLights.empty()) {
            _cameraDirShadowLights[camera] = std::move(dirShadowLights);
        }
    }

    void Renderer::resolveAutoAspectRatio(Camera* camera) const
    {
        if (!camera || camera->aspectRatioMode() != AspectRatioMode::ASPECT_AUTO || !_device) {
            return;
        }
        const auto* target = camera->renderTarget().get();
        const int targetWidth = std::max(target ? target->width() : _device->size().first, 1);
        const int targetHeight = std::max(target ? target->height() : _device->size().second, 1);
        const PixelRect viewport = normalizedRectToPixels(camera->rect(), targetWidth, targetHeight);
        camera->setAspectRatio(static_cast<float>(viewport.w) / static_cast<float>(viewport.h));
    }

    void Renderer::dispatchGpuInstanceCulling(Camera* camera)
    {
        if (!_device || (camera && !camera->node())) {
            return;
        }
        // Nothing to cull: skip the sweep below, which visits every component in the
        // scene to find the few instances that use this.
        if (MeshInstance::gpuCulledInstanceCount() == 0) {
            return;
        }

        // Compute view-projection: view = inverse(camera world), proj = camera proj.
        // The frustum plane extraction expects a column-major float[16] layout,
        // which matches Matrix4's in-memory representation (64 bytes, SIMD-safe).
        // Without a camera every plane is (0, 0, 0, +big): every instance is inside.
        float planes[6][4];
        if (camera) {
            const Matrix4 view = camera->node()->worldTransform().inverse();
            const Matrix4 vp = camera->projectionMatrix() * view;
            InstanceCuller::extractFrustumPlanes(reinterpret_cast<const float*>(&vp), planes);
        } else {
            for (auto& plane : planes) {
                plane[0] = plane[1] = plane[2] = 0.0f;
                plane[3] = std::numeric_limits<float>::max();
            }
        }

        // Batch all cull dispatches into one backend command buffer with a
        // single sync at the end — begun lazily so frames with no GPU-culled
        // instances create no command buffer at all.
        bool cullBatchStarted = false;

        for (auto* rc : RenderComponent::instances()) {
            // active(): a disabled entity's instances are not drawn, so culling them
            // is wasted GPU work.
            if (!rc || !rc->active()) {
                continue;
            }
            for (auto* mi : rc->meshInstances()) {
                if (!mi || !mi->gpuCullingEnabled()) {
                    continue;
                }
                auto* culler = mi->instanceCuller();
                if (!culler) {
                    continue;
                }
                const auto& srcData = mi->instancingData();
                if (!srcData.vertexBuffer || srcData.count <= 0) {
                    continue;
                }
                auto* srcMesh = mi->mesh();
                if (!srcMesh) {
                    continue;
                }

                // The instances are in the NODE's space (the vertex stage composes the
                // node's world matrix), so the world planes are carried into it: for
                // p_world = M p_local a plane (n, d) becomes M^T (n, d), whose value at a
                // local point is still the WORLD signed distance — the kernel's test is
                // unchanged — and the local sphere radius is scaled to world by M's
                // largest axis. An identity node leaves both exactly as they were.
                InstanceCullParams params{};
                float m[16];
                (mi->node() ? mi->node()->worldTransform() : Matrix4::identity()).store(m);   // column-major
                static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                if (std::equal(std::begin(m), std::end(m), std::begin(kIdentity))) {
                    std::memcpy(params.frustumPlanes, planes, sizeof(planes));
                    params.boundingSphereRadius = mi->instanceCullRadius();
                } else {
                    for (int p = 0; p < 6; ++p) {
                        for (int c = 0; c < 4; ++c) {
                            params.frustumPlanes[p][c] = m[4 * c] * planes[p][0] + m[4 * c + 1] * planes[p][1] +
                                m[4 * c + 2] * planes[p][2] + m[4 * c + 3] * planes[p][3];
                        }
                    }
                    const float maxScale = std::sqrt(std::max({
                        m[0] * m[0] + m[1] * m[1] + m[2] * m[2],
                        m[4] * m[4] + m[5] * m[5] + m[6] * m[6],
                        m[8] * m[8] + m[9] * m[9] + m[10] * m[10]}));
                    params.boundingSphereRadius = mi->instanceCullRadius() * maxScale;
                }
                params.instanceCount = static_cast<uint32_t>(srcData.count);

                const auto prim = srcMesh->getPrimitive();
                params.indexCount  = static_cast<uint32_t>(prim.count);
                params.indexStart  = static_cast<uint32_t>(prim.base);
                params.baseVertex  = static_cast<int32_t>(prim.baseVertex);
                params.baseInstance = 0u;

                if (!cullBatchStarted) {
                    _device->beginGpuCullBatch();
                    cullBatchStarted = true;
                }
                culler->cull(srcData.vertexBuffer.get(), params);
            }
        }

        if (cullBatchStarted) {
            _device->endGpuCullBatch();
        }
    }

    // ---------------------------------------------------------------------------
    // Forward layer rendering. renderForwardLayer runs the stages below in order;
    // each stage is a function of its own so the frame can be read top to bottom.
    // ---------------------------------------------------------------------------

    namespace
    {
        constexpr int kMaxMainLights = 8;

        // Light cookie slot pools: two 2D (spot) and two cubemap (omni) slots,
        // matching the local-shadow slot count.
        constexpr int kMaxCookies = 2;

        float toRadians(const float degrees)
        {
            return degrees * (std::numbers::pi_v<float> / 180.0f);
        }

        float toDegrees(const float radians)
        {
            return radians * (180.0f / std::numbers::pi_v<float>);
        }

        // A sky drawn as a finite dome sits where its node puts it; an infinite or an
        // atmosphere sky follows the camera.
        bool skyIsDome(const Scene* scene)
        {
            return scene && scene->sky() && scene->sky()->type() != SKYTYPE_INFINITE &&
                scene->sky()->type() != SKYTYPE_ATMOSPHERE;
        }

        Matrix4 nodeWorldTransform(const MeshInstance* meshInstance)
        {
            return meshInstance && meshInstance->node()
                ? meshInstance->node()->worldTransform() : Matrix4::identity();
        }

        Matrix4 forwardModelMatrix(const MeshInstance* meshInstance, const Material* material,
            const Scene* scene, const Vector3& cameraPosition)
        {
            if (material && material->isSkybox() && !skyIsDome(scene)) {
                return Matrix4::translation(cameraPosition);
            }
            return nodeWorldTransform(meshInstance);
        }

        /// Restores the device's viewport and scissor when the scope ends, so a
        /// camera's rect never outlives its camera-layer pass.
        class ViewportScissorScope
        {
        public:
            explicit ViewportScissorScope(GraphicsDevice& device)
                : _device(device),
                  _vx(device.vx()), _vy(device.vy()), _vw(device.vw()), _vh(device.vh()),
                  _sx(device.sx()), _sy(device.sy()), _sw(device.sw()), _sh(device.sh()) {}

            ~ViewportScissorScope()
            {
                _device.setViewport(_vx, _vy, _vw, _vh);
                _device.setScissor(_sx, _sy, _sw, _sh);
            }

            ViewportScissorScope(const ViewportScissorScope&) = delete;
            ViewportScissorScope& operator=(const ViewportScissorScope&) = delete;

        private:
            GraphicsDevice& _device;
            float _vx, _vy, _vw, _vh;
            int _sx, _sy, _sw, _sh;
        };

        /// What the forward pass knows about the camera it renders for.
        struct ForwardView
        {
            Camera* camera = nullptr;
            GraphNode* cameraNode = nullptr;
            const CameraComponent* cameraComponent = nullptr;
            RenderTarget* activeTarget = nullptr;
            Vector3 position;
            Vector3 forward{0.0f, 0.0f, -1.0f};
            Matrix4 viewMatrix = Matrix4::identity();
            Matrix4 projMatrix = Matrix4::identity();
            Matrix4 viewProjection = Matrix4::identity();
            PixelRect viewport;
            PixelRect scissor;
            int toneMapping = TONEMAP_LINEAR;
            // Built only for the LIGHT cull — mesh instances are culled in the batch
            // (Renderer::cullMeshInstancesInto) and never touch it.
            bool hasFrustum = false;
            Frustum frustum{};
        };

        ForwardView setupForwardView(GraphicsDevice& device, const Scene* scene, Camera& camera,
            RenderTarget* renderTarget)
        {
            ForwardView view;
            view.camera = &camera;
            view.cameraNode = camera.node();
            if (view.cameraNode) {
                view.position = view.cameraNode->position();
                view.forward = cameraForwardOf(view.cameraNode->worldTransform());
                view.viewMatrix = view.cameraNode->worldTransform().inverse();
            }
            view.activeTarget = renderTarget ? renderTarget : camera.renderTarget().get();
            const int targetWidth = std::max(view.activeTarget ? view.activeTarget->width() : device.size().first, 1);
            const int targetHeight = std::max(view.activeTarget ? view.activeTarget->height() : device.size().second, 1);
            view.viewport = normalizedRectToPixels(camera.rect(), targetWidth, targetHeight);
            view.scissor = normalizedRectToPixels(camera.scissorRect(), targetWidth, targetHeight);

            // ASPECT_AUTO cameras use viewport size, not full target size.
            if (camera.aspectRatioMode() == AspectRatioMode::ASPECT_AUTO) {
                camera.setAspectRatio(static_cast<float>(view.viewport.w) / static_cast<float>(view.viewport.h));
            }

            for (const auto* candidate : CameraComponent::instances()) {
                if (candidate && candidate->camera() == &camera) {
                    view.cameraComponent = candidate;
                    break;
                }
            }

            view.projMatrix = camera.projectionMatrix();
            float jitterX = 0.0f;
            float jitterY = 0.0f;
            const float jitter = std::max(camera.jitter(), 0.0f);
            if (jitter > 0.0f) {
                const auto& offset = haltonSequence[static_cast<size_t>(device.renderVersion() % haltonSequence.size())];
                jitterX = jitter * (offset[0] * 2.0f - 1.0f) / static_cast<float>(view.viewport.w);
                jitterY = jitter * (offset[1] * 2.0f - 1.0f) / static_cast<float>(view.viewport.h);

                // Accumulate, do not assign: these are the same two elements that carry an
                // off-center projection offset (Camera::setProjectionOffset), so overwriting them
                // here would silently cancel the shift lens whenever TAA is enabled.
                view.projMatrix.setElement(2, 0, view.projMatrix.getElement(2, 0) + jitterX);
                view.projMatrix.setElement(2, 1, view.projMatrix.getElement(2, 1) + jitterY);
            }
            view.viewProjection = view.projMatrix * view.viewMatrix;
            camera.storeShaderMatrices(view.viewProjection, jitterX, jitterY, device.renderVersion());

            // Per-camera tone mapping (upstream CameraComponent::toneMapping) overrides the
            // scene-wide value; TONEMAP_INHERIT keeps the scene's.
            const int sceneToneMapping = scene ? scene->toneMapping() : TONEMAP_LINEAR;
            view.toneMapping = camera.toneMapping() != TONEMAP_INHERIT ? camera.toneMapping() : sceneToneMapping;
            return view;
        }

        /// Which light-dependent shader features any active light needs this frame.
        struct LightFeatureUse
        {
            bool localShadows = false;
            bool omniShadows = false;
            bool cookie2D = false;
            bool cookieCube = false;
            bool vsmShadows = false;
            bool pcssShadows = false;
            bool areaLights = false;
        };

        LightFeatureUse scanLightFeatureUse()
        {
            LightFeatureUse use;
            for (const auto* lc : LightComponent::instances()) {
                if (!lc || !lc->active()) {
                    continue;
                }
                const LightType type = lc->type();
                Light* sceneLight = lc->light();

                // Local shadows count only when the map is actually allocated, so the
                // shader never compiles with depth2d / depthcube parameters that would
                // be nil at runtime.
                if (lc->castShadows() && type != LightType::LIGHTTYPE_DIRECTIONAL &&
                    sceneLight && sceneLight->shadowMap()) {
                    if (type == LightType::LIGHTTYPE_OMNI) {
                        use.omniShadows = true;
                    } else {
                        use.localShadows = true;
                    }
                }

                // The cookie's shape has to match the light: a spot projects a 2D
                // texture, an omni samples a cubemap by direction. A mismatch is
                // ignored, as upstream does.
                if (lc->cookie()) {
                    if (type == LightType::LIGHTTYPE_SPOT && !lc->cookie()->isCubemap()) {
                        use.cookie2D = true;
                    } else if ((type == LightType::LIGHTTYPE_OMNI || type == LightType::LIGHTTYPE_POINT) &&
                               lc->cookie()->isCubemap()) {
                        use.cookieCube = true;
                    }
                }

                // Directional EVSM_16F and PCSS: both the shadow program and the
                // forward program need the matching variant.
                if (lc->castShadows() && type == LightType::LIGHTTYPE_DIRECTIONAL && sceneLight) {
                    use.vsmShadows |= sceneLight->shadowType() == SHADOW_VSM_16F;
                    use.pcssShadows |= sceneLight->shadowType() == SHADOW_PCSS_32F;
                }

                use.areaLights |= type == LightType::LIGHTTYPE_AREA_RECT;
            }
            return use;
        }

        void collectDrawEntries(const std::vector<MeshInstance*>& bucket, const ForwardView& view,
            Material* defaultMaterial, ObjectPool<ForwardDrawEntry>& pool, std::vector<ForwardDrawEntry*>& out)
        {
            for (auto* meshInstance : bucket) {
                auto* mesh = meshInstance ? meshInstance->mesh() : nullptr;
                if (!mesh) {
                    continue;
                }
                auto vertexBuffer = mesh->getVertexBuffer();
                if (!vertexBuffer) {
                    continue;
                }

                auto* entry = pool.allocate();
                entry->meshInstance = meshInstance;
                entry->material = meshInstance->material() ? meshInstance->material() : defaultMaterial;
                if (!entry->material) {
                    continue;
                }
                entry->vertexBuffer = vertexBuffer;
                entry->indexBuffer = mesh->getIndexBuffer();
                entry->primitive = mesh->getPrimitive();
                entry->sortKey = makeOpaqueSortKey(meshInstance);

                const auto worldBounds = meshInstance->aabb();
                if (meshInstance->node() && !entry->material->isSkybox()) {
                    // Signed view-axis depth, as upstream's _calculateSortDistances. A
                    // radial distance would rank an off-axis transparent surface behind a
                    // centred one at the same depth.
                    const auto& customDistance = meshInstance->calculateSortDistance();
                    entry->sortDistance = customDistance
                        ? customDistance(*meshInstance, view.position, view.forward)
                        : forwardSortDistance(worldBounds.center(), view.position, view.forward);
                } else {
                    entry->sortDistance = 0.0f;
                }
                out.push_back(entry);
            }
        }

        // Upstream's Layer.sortVisible: the mode is a per-layer, per-sublayer
        // property, because the two sublayers want opposite things — the opaque pass
        // wants the fewest state changes, the transparent pass has to composite
        // back-to-front whatever that costs. The defaults are MATERIALMESH and
        // BACK2FRONT respectively.
        void sortDrawEntries(std::vector<ForwardDrawEntry*>& entries, const Layer& layer, const bool transparent)
        {
            const SortMode sortMode = transparent ? layer.transparentSortMode() : layer.opaqueSortMode();

            // A custom comparator may read MeshInstance::sortDistance, so publish what
            // was computed per draw before calling one.
            if (sortMode == SortMode::SORTMODE_CUSTOM) {
                for (auto* entry : entries) {
                    if (entry->meshInstance) {
                        entry->meshInstance->setSortDistance(entry->sortDistance);
                    }
                }
            }

            switch (sortMode) {
            case SortMode::SORTMODE_NONE:
                // Collection order. Nothing to do, and deliberately not a fallthrough to
                // a default: a pass that asked for no sorting must not get one.
                break;

            case SortMode::SORTMODE_MANUAL:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        const double orderA = a->meshInstance ? a->meshInstance->drawOrder() : 0.0;
                        const double orderB = b->meshInstance ? b->meshInstance->drawOrder() : 0.0;
                        return orderA < orderB;
                    });
                break;

            case SortMode::SORTMODE_BACK2FRONT:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        if (a->sortDistance == b->sortDistance) {
                            return a->sortKey < b->sortKey;
                        }
                        return a->sortDistance > b->sortDistance;
                    });
                break;

            case SortMode::SORTMODE_FRONT2BACK:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        if (a->sortDistance == b->sortDistance) {
                            return a->sortKey < b->sortKey;
                        }
                        return a->sortDistance < b->sortDistance;
                    });
                break;

            case SortMode::SORTMODE_CUSTOM:
                // A null callback leaves the order alone rather than silently falling
                // back to a mode the caller did not ask for.
                if (const auto& callback = layer.customSortCallback()) {
                    std::stable_sort(entries.begin(), entries.end(),
                        [&callback](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                            return callback(a->meshInstance, b->meshInstance);
                        });
                }
                break;

            case SortMode::SORTMODE_MATERIALMESH:
            default:
                std::stable_sort(entries.begin(), entries.end(),
                    [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        if (a->sortKey != b->sortKey) {
                            return a->sortKey < b->sortKey;
                        }
                        return a->sortDistance < b->sortDistance;
                    });
                break;
            }
        }

        GpuLightData makeGpuLight(const LightComponent& lightComponent, const bool physicalUnits)
        {
            GpuLightData lightData{};
            switch (lightComponent.type()) {
                case LightType::LIGHTTYPE_DIRECTIONAL:
                    lightData.type = GpuLightType::Directional;
                    break;
                case LightType::LIGHTTYPE_SPOT:
                    lightData.type = GpuLightType::Spot;
                    break;
                case LightType::LIGHTTYPE_AREA_RECT:
                    lightData.type = GpuLightType::AreaRect;
                    lightData.areaHalfWidth = lightComponent.areaWidth() * 0.5f;
                    lightData.areaHalfHeight = lightComponent.areaHeight() * 0.5f;
                    lightData.areaShape = static_cast<uint32_t>(lightComponent.areaShape());
                    {
                        // Right vector: the light's world X AXIS, which is column 0 — the same
                        // column upstream's LTC width axis comes from (it transforms (-0.5, 0, 0)
                        // by the world matrix; the sign is immaterial here because the shader
                        // derives up as cross(direction, right) and the quad is symmetric).
                        // (Row 0 would be the X component of all three axes: right only for an
                        // unrotated light, a vector outside the light's own plane otherwise.)
                        const auto& wt = lightComponent.entity()->worldTransform();
                        Vector3 right(wt.getColumn(0));
                        if (right.lengthSquared() > 1e-8f) {
                            lightData.areaRight = right.normalized();
                        }
                    }
                    break;
                case LightType::LIGHTTYPE_OMNI:
                case LightType::LIGHTTYPE_POINT:
                default:
                    lightData.type = GpuLightType::Point;
                    break;
            }

            lightData.position = lightComponent.position();
            lightData.direction = lightComponent.direction();
            if (lightData.direction.lengthSquared() > 1e-8f) {
                lightData.direction = lightData.direction.normalized();
            } else {
                lightData.direction = Vector3(0.0f, -1.0f, 0.0f);
            }
            lightData.color = lightComponent.color();
            lightData.intensity = std::max(lightComponent.renderIntensity(physicalUnits), 0.0f);
            lightData.range = std::max(lightComponent.range(), 1e-4f);
            // inner/outerConeAngle are HALF-angles in degrees (upstream Light:
            // `cos(angle * DEG_TO_RAD)`, and its spot shadow/cookie cameras use
            // `fov = outerConeAngle * 2`). Halving them here made every spot cone
            // half as wide as the shadow and cookie frustum fitted to the same
            // light — visible as a beam covering only the middle of its cookie.
            lightData.innerConeCos = std::cos(toRadians(std::max(lightComponent.innerConeAngle(), 0.0f)));
            lightData.outerConeCos = std::cos(toRadians(std::max(lightComponent.outerConeAngle(), 0.0f)));
            if (lightData.innerConeCos < lightData.outerConeCos) {
                lightData.innerConeCos = lightData.outerConeCos;
            }
            lightData.falloffModeLinear = lightComponent.falloffMode() == LightFalloff::LIGHTFALLOFF_LINEAR;
            lightData.castShadows = lightComponent.castShadows();
            return lightData;
        }

        // DEVIATION: up to kMaxDirectionalShadows directional shadows per layer,
        // where upstream samples every directional caster's map. Each shadowed
        // light takes a slot (its uniform block and texture) and carries the slot
        // index as its shadowMapIndex; the shaders gate the cascade lookup on the
        // light's own flag and index, so a light without a slot must be told it
        // casts nothing or it would be darkened by ANOTHER light's shadow — a
        // shadowless fill light carrying the key light's shadows. The filter (PCF, PCSS, VSM) is chosen per
        // shader variant, so every slot must share the first slot's shadow type.
        //
        // `fitCamera` is the camera the cascades were fitted for (see
        // Renderer::directionalShadowFitCamera).
        void assignDirectionalShadowSlot(const LightComponent& lightComponent, Camera* fitCamera,
            GpuLightData& lightData, ShadowParams& shadowParams)
        {
            Light* sceneLight = lightComponent.light();
            const bool vsm = sceneLight && sceneLight->shadowType() == SHADOW_VSM_16F;
            const bool pcss = sceneLight && sceneLight->shadowType() == SHADOW_PCSS_32F;
            const int slot = shadowParams.directionalCount;
            const char* refusal = nullptr;
            if (!sceneLight || !sceneLight->shadowMap()) {
                refusal = "";   // no map yet (first frame): unshadowed, silently
            } else if (slot >= ShadowParams::kMaxDirectionalShadows) {
                refusal = "more directional lights cast shadows on one layer than there are "
                          "directional shadow slots; the extra ones are unshadowed";
            } else if (slot > 0 && (vsm != shadowParams.vsm || pcss != shadowParams.pcss)) {
                refusal = "a directional light's shadow type differs from the first shadowed "
                          "directional light's on the same layer; it is unshadowed";
            }
            if (refusal) {
                lightData.castShadows = false;
                static bool warned = false;
                if (*refusal && !warned) {
                    warned = true;
                    spdlog::warn("Renderer: {}", refusal);
                }
                return;
            }

            lightData.shadowMapIndex = slot;
            shadowParams.directionalCount = slot + 1;
            shadowParams.enabled = true;
            shadowParams.vsm = vsm;
            shadowParams.pcss = pcss;
            auto& dir = shadowParams.directional[slot];
            dir.shadowMap = sceneLight->shadowMap()->shadowTexture();
            dir.normalBias = lightComponent.shadowNormalBias();
            dir.strength = lightComponent.shadowStrength();

            // CSM: copy the full matrix palette and cascade distances.
            dir.numCascades = sceneLight->numCascades();
            dir.cascadeBlend = sceneLight->cascadeBlend();
            std::memcpy(dir.shadowMatrixPalette, sceneLight->shadowMatrixPalette().data(),
                        sizeof(dir.shadowMatrixPalette));
            std::memcpy(dir.shadowCascadeDistances, sceneLight->shadowCascadeDistances().data(),
                        sizeof(dir.shadowCascadeDistances));

            // For PCF: a fixed small shader-side depth bias; the real bias work
            // is hardware polygon offset (depthBias) in the shadow pass, which is
            // slope-aware. For VSM_16F the slot carries the vsmBias instead: it
            // sets the minVariance floor in chebyshevUpperBound, which decides how
            // aggressively low-variance (noisy) samples are clamped to lit. Too
            // small flickers; too large detaches contact shadows. Upstream's
            // default is 0.0025.
            dir.bias = vsm ? sceneLight->vsmBias() : 0.0001f;
            dir.penumbraSize = sceneLight->penumbraSize();
            dir.penumbraFalloff = sceneLight->penumbraFalloff();

            LightRenderData* rd = sceneLight->getRenderData(fitCamera, 0);
            if (rd && rd->shadowCamera && rd->shadowCamera->node()) {
                dir.viewProjection = rd->shadowCamera->projectionMatrix()
                    * rd->shadowCamera->node()->worldTransform().inverse();
            }
            if (pcss) {
                for (int cascade = 0; cascade < dir.numCascades && cascade < 4; ++cascade) {
                    LightRenderData* cascadeData = sceneLight->getRenderData(fitCamera, cascade);
                    if (cascadeData && cascadeData->shadowCamera) {
                        dir.pcssCascadeRadii[cascade] =
                            std::max(cascadeData->shadowCamera->orthoHeight(), 1e-4f);
                        dir.pcssCascadeDepthRanges[cascade] = std::max(
                            cascadeData->shadowCamera->farClip() - cascadeData->shadowCamera->nearClip(), 1e-4f);
                    }
                }
            }
        }

        // Wire local light shadow data (spot/point) for the NON-clustered path.
        // Omni lights use cubemap depth textures; spot lights use 2D textures.
        // Under clustered lighting every local light takes its shadow from the
        // LightTextureAtlas through the cluster grid, so none may consume one of
        // the two main-array slots nor be stripped of castShadows when they run
        // out — leaving spots in this branch once capped the whole feature at
        // kMaxLocalShadows, and leaving omnis in it capped them at two while
        // the clustered-lighting default put every scene on this path.
        void assignLocalShadowSlot(const LightComponent& lightComponent, GpuLightData& lightData,
            ShadowParams& shadowParams)
        {
            Light* sceneLight = lightComponent.light();
            if (!sceneLight || !sceneLight->shadowMap() ||
                shadowParams.localShadowCount >= ShadowParams::kMaxLocalShadows) {
                // No shadow slot available — clear castShadows so the shader
                // doesn't attempt to sample a non-existent shadow map.
                lightData.castShadows = false;
                return;
            }

            const int shadowIdx = shadowParams.localShadowCount;
            lightData.shadowMapIndex = shadowIdx;

            const bool isOmni = (sceneLight->type() == LightType::LIGHTTYPE_OMNI);
            auto& ls = shadowParams.localShadows[shadowIdx];
            ls.shadowMap = sceneLight->shadowMap()->shadowTexture();
            ls.isOmni = isOmni;

            if (isOmni) {
                // For omni lights, pack the far clip (range) into VP[0][0] so the
                // uniform binder can extract it for the cubemap depth comparison.
                Matrix4 rangePack = Matrix4::identity();
                rangePack.setElement(0, 0, sceneLight->range());
                ls.viewProjection = rangePack;
            } else {
                ls.viewProjection = sceneLight->shadowViewProjection();
            }
            // Our local-shadow shader subtracts this from the receiver depth,
            // so it needs a POSITIVE value while Light::shadowBias() is upstream's
            // negative internal one — hence the negation. The per-type scaling
            // mirrors upstream Light::_getUniformBiasValues (spot x20, omni raw).
            ls.bias = isOmni ? -sceneLight->shadowBias()
                             : -sceneLight->shadowBias() * 20.0f;
            ls.normalBias = sceneLight->normalBias();
            ls.intensity = sceneLight->shadowIntensity();
            ls.nearClip = 0.01f;
            ls.farClip = std::max(sceneLight->range(), 0.1f);

            // PCSS local shadows (upstream shadowPCSS.js): blocker-search
            // area in shadow-map UV. Spot scales by the shadow camera's
            // FOV ratio; omni uses the raw penumbra/resolution ratio.
            if (sceneLight->shadowType() == SHADOW_PCSS_32F) {
                const float res = std::max(static_cast<float>(sceneLight->shadowResolution()), 1.0f);
                if (isOmni) {
                    ls.pcssSearchArea = sceneLight->penumbraSize() / res;
                } else {
                    const float fovRad = toRadians(std::min(sceneLight->outerConeAngle() * 2.0f, 179.0f));
                    const float fovRatio = 1.0f / std::max(std::tan(fovRad * 0.5f), 1e-4f);
                    ls.pcssSearchArea = sceneLight->penumbraSize() / res * fovRatio;
                }
            }

            shadowParams.localShadowCount++;
        }

        struct CookieSlots
        {
            int used2D = 0;
            int usedCube = 0;
        };

        // Light cookies (upstream Light.cookie): the projected texture masking
        // the light's color. Spot cookies need a world → cookie-UV matrix —
        // identical to the spot shadow VP, so shadow casters reuse theirs and
        // cookie-only lights evaluate it here. Omni cookies are sampled by
        // direction and carry the light's world transform instead.
        // DEVIATION: two slots per kind (like local shadows), not one per light.
        void assignCookieSlot(const LightComponent& lightComponent, GpuLightData& lightData, CookieSlots& slots)
        {
            Light* sceneLight = lightComponent.light();
            if (!sceneLight || !sceneLight->cookie() ||
                lightData.type == GpuLightType::Directional || lightData.type == GpuLightType::AreaRect) {
                return;
            }

            const bool isOmniCookie = (sceneLight->type() == LightType::LIGHTTYPE_OMNI ||
                                       sceneLight->type() == LightType::LIGHTTYPE_POINT);
            const bool shapeMatches = (isOmniCookie == sceneLight->cookie()->isCubemap());
            int& poolCount = isOmniCookie ? slots.usedCube : slots.used2D;
            if (!shapeMatches || poolCount >= kMaxCookies) {
                return;
            }

            lightData.cookie = sceneLight->cookie();
            lightData.cookieIndex = poolCount++;
            lightData.cookieIntensity = sceneLight->cookieIntensity();
            lightData.cookieChannel = static_cast<uint32_t>(sceneLight->cookieChannel());
            lightData.cookieFalloff = sceneLight->cookieFalloff();
            lightData.cookieMatrix = isOmniCookie
                ? lightComponent.entity()->worldTransform()
                : (lightData.castShadows ? sceneLight->shadowViewProjection()
                                         : LightCamera::evalSpotCookieMatrix(*sceneLight));
        }

        /// This (camera, layer)'s lights, split as the shaders consume them.
        struct ForwardLights
        {
            std::vector<LightDispatchEntry> directional;
            // Sorted by apparent size, largest first.
            std::vector<LightDispatchEntry> local;
            ShadowParams shadowParams{};

            void reset()
            {
                directional.clear();
                local.clear();
                directional.reserve(4);
                local.reserve(8);
                shadowParams = ShadowParams{};
            }
        };

        void gatherForwardLights(const ForwardView& view, const Layer& layer, const Scene* scene,
            const bool clusteredEnabled, Camera* directionalFitCamera, ForwardLights& out)
        {
            const bool physicalUnits = scene && scene->physicalUnits();
            CookieSlots cookieSlots;

            for (const auto* lightComponent : LightComponent::instances()) {
                if (!lightComponent || !lightComponent->active()) {
                    continue;
                }
                if (!lightComponent->rendersLayer(layer.id())) {
                    continue;
                }
                if (view.cameraComponent && !view.cameraComponent->rendersLayer(layer.id())) {
                    continue;
                }

                GpuLightData lightData = makeGpuLight(*lightComponent, physicalUnits);
                if (lightData.intensity <= 0.0f) {
                    continue;
                }

                if (lightData.castShadows) {
                    if (lightData.type == GpuLightType::Directional) {
                        assignDirectionalShadowSlot(*lightComponent, directionalFitCamera, lightData, out.shadowParams);
                    } else if (!clusteredEnabled) {
                        assignLocalShadowSlot(*lightComponent, lightData, out.shadowParams);
                    }
                }
                assignCookieSlot(*lightComponent, lightData, cookieSlots);

                LightDispatchEntry dispatchEntry{};
                dispatchEntry.light = lightData;
                dispatchEntry.mask = lightComponent->mask();
                dispatchEntry.sceneLight = lightComponent->light();

                if (dispatchEntry.light.type == GpuLightType::Directional) {
                    out.directional.push_back(dispatchEntry);
                    continue;
                }

                // Cull the light against THIS camera. Light::visibleThisFrame is a
                // union over cameras and answers a different question — whether the
                // light's shadow map and cookie are worth rendering at all — so it
                // cannot stand in for this test: a light visible only to a reflection
                // camera would otherwise light the main view from off screen.
                //
                // Both the main array and the cluster grid are fed from this list, so
                // one test covers the two of them.
                if (view.hasFrustum && dispatchEntry.sceneLight) {
                    const BoundingSphere bounds = dispatchEntry.sceneLight->boundingSphere();
                    if (!view.frustum.checkSphere(bounds.center(), bounds.radius())) {
                        continue;
                    }
                    dispatchEntry.screenSize = view.camera->screenSize(bounds);
                }
                out.local.push_back(dispatchEntry);
            }

            // The main light array holds eight. Rank by apparent size so that when a
            // scene has more visible local lights than slots, the ones covering most of
            // the picture get them — authoring order decides nothing about that.
            // Stable, so an exact tie keeps authoring order and the choice stays
            // reproducible frame to frame.
            std::stable_sort(out.local.begin(), out.local.end(),
                [](const LightDispatchEntry& a, const LightDispatchEntry& b) {
                    return a.screenSize > b.screenSize;
                });
        }

        // Environment uniforms are constant across the entire layer (depend only on
        // the scene, not on per-draw state), so they are set once per layer rather
        // than per draw.
        void bindLayerEnvironment(GraphicsDevice& device, const Scene* scene, const ForwardView& view)
        {
            const bool isDome = skyIsDome(scene);
            const Vector3 skyDomeCenter = isDome ? scene->sky()->centerWorldPos() : Vector3(0, 0, 0);
            device.setEnvironmentUniforms(scene ? scene->envAtlas() : nullptr,
                scene ? scene->skyboxIntensity() : 1.0f,
                static_cast<float>(scene ? scene->skyboxMip() : 0),
                skyDomeCenter, isDome,
                scene ? scene->skybox() : nullptr);

            // A dynamic probe captures the scene into the very cubemap the scene
            // samples for reflections, so during its own face passes the probe
            // texture is simultaneously the render target and a bound sampler —
            // feedback. Skip the binding for those passes: the cube's other faces
            // hold last frame's capture, which is not worth a read-write hazard.
            // Vulkan reports this as a layout conflict (the face is in
            // COLOR_ATTACHMENT_OPTIMAL while the descriptor wants SHADER_READ_ONLY);
            // Metal reads it silently, which is undefined rather than correct.
            if (scene && scene->reflectionProbe()) {
                Texture* probe = scene->reflectionProbe();
                const bool probeIsRenderTarget =
                    view.activeTarget && view.activeTarget->colorBuffer() == probe;
                // Unbind rather than skip: the binding is device state that would
                // otherwise persist from the previous pass, which is exactly the
                // texture we must not sample here. Zero intensity also neutralizes
                // the shader term, since VT_FEATURE_REFLECTION_PROBE stays enabled
                // for the frame.
                if (probeIsRenderTarget) {
                    device.setReflectionProbeUniforms(nullptr,
                        scene->reflectionProbeBoxMin(), scene->reflectionProbeBoxMax(),
                        scene->reflectionProbeBoxProjection(), 0.0f, 0.0f);
                } else {
                    // maxLod = highest mip index (roughness → LOD in the shader).
                    const int levels = std::max(1, static_cast<int>(probe->getNumLevels()));
                    device.setReflectionProbeUniforms(probe,
                        scene->reflectionProbeBoxMin(), scene->reflectionProbeBoxMax(),
                        scene->reflectionProbeBoxProjection(),
                        scene->reflectionProbeIntensity(),
                        static_cast<float>(levels - 1));
                }
            }

            // Camera clip planes for SSR depth-grab linearization.
            device.setCameraClipPlanes(view.camera->nearClip(), view.camera->farClip());
            device.setDebugShaderPass(static_cast<uint32_t>(view.camera->debugShaderPass()));
        }

        /// The local lights in WorldClusters' input format, plus the identity of the
        /// set, order-independent: the dispatch list is sorted by apparent size, which
        /// differs per camera, and two layers seeing the same lights must still hash alike.
        void buildClusterLightInput(const std::vector<LightDispatchEntry>& localLights,
            std::vector<ClusterLightData>& lights, std::vector<const void*>& members)
        {
            lights.clear();
            lights.reserve(localLights.size());
            members.clear();

            for (const auto& dispatchEntry : localLights) {
                const auto& ld = dispatchEntry.light;
                // Area rect lights are not clustered — they go through the main 8-light array.
                // Every spot and omni light is in the grid, shadowed from the atlas
                // when it holds a slot for it and unshadowed otherwise, as upstream.
                if (ld.type == GpuLightType::AreaRect) continue;
                ClusterLightData lcd;
                lcd.position = ld.position;
                lcd.direction = ld.direction;
                lcd.color = ld.color;
                lcd.intensity = ld.intensity;
                lcd.range = ld.range;
                // Back to the half-angle in degrees that WorldClusters re-cosines.
                lcd.innerConeAngle = toDegrees(std::acos(std::clamp(ld.innerConeCos, -1.0f, 1.0f)));
                lcd.outerConeAngle = toDegrees(std::acos(std::clamp(ld.outerConeCos, -1.0f, 1.0f)));
                lcd.isSpot = (ld.type == GpuLightType::Spot);
                lcd.falloffModeLinear = ld.falloffModeLinear;

                // Clustered shadow: the atlas rect assigned by LightTextureAtlas::update
                // this frame, and for a spot the VP into it computed by cullLocalLights.
                if (ld.castShadows && dispatchEntry.sceneLight &&
                    dispatchEntry.sceneLight->atlasViewportAllocated()) {
                    Light* atlasLight = dispatchEntry.sceneLight;
                    lcd.castShadows = true;
                    lcd.atlasViewport = atlasLight->atlasViewport();
                    lcd.shadowMatrix = atlasLight->shadowViewProjection();
                    // An omni face stores perspective depth over the light's range,
                    // with the same relative bias the cubemap path applies before the
                    // projection (see the omni block of forward-fragment-lights).
                    lcd.shadowNear = 0.01f;
                    lcd.shadowFar = std::max(atlasLight->range(), 0.1f);
                    lcd.shadowRelativeBias = -atlasLight->shadowBias();
                    // NOT the non-clustered path's depth bias. A clustered spot's
                    // depth is crushed against 1.0 by a near clip of 0.01 against a
                    // range of 150, so the whole scene spans ~0.001 of depth while
                    // that bias is 0.08 — it lit every fragment and erased the
                    // feature. Upstream biases these on render (hardware polygon
                    // offset, which the atlas pass already applies) and offsets the
                    // receiver along its normal in the shader instead.
                    lcd.shadowNormalBias = atlasLight->normalBias();
                    lcd.shadowIntensity = atlasLight->shadowIntensity();
                }
                lights.push_back(lcd);
                members.push_back(dispatchEntry.sceneLight);
            }
        }

        // The main light array for one light mask: directional lights first, then
        // area lights, then — outside clustered lighting only — the other local
        // lights, up to kMaxMainLights in all. Under clustered lighting every spot and
        // omni light is in the cluster grid, shadowed from the atlas, so none of them
        // enters the main array; it holds only the directional and area lights then.
        void filterLightsForMask(const ForwardLights& lights, const uint32_t mask, const bool clusteredEnabled,
            std::vector<GpuLightData>& out)
        {
            out.clear();
            for (const auto& dispatchEntry : lights.directional) {
                if ((dispatchEntry.mask & mask) == 0u) continue;
                out.push_back(dispatchEntry.light);
                if (out.size() >= kMaxMainLights) break;
            }
            for (const auto& dispatchEntry : lights.local) {
                if (out.size() >= kMaxMainLights) break;
                if (dispatchEntry.light.type != GpuLightType::AreaRect) continue;
                if ((dispatchEntry.mask & mask) == 0u) continue;
                out.push_back(dispatchEntry.light);
            }
            if (clusteredEnabled) {
                return;
            }
            for (const auto& dispatchEntry : lights.local) {
                if (out.size() >= kMaxMainLights) break;
                if ((dispatchEntry.mask & mask) == 0u) continue;
                if (dispatchEntry.light.type == GpuLightType::AreaRect) continue;  // already added above
                out.push_back(dispatchEntry.light);
            }
        }

        /// Everything the draw loop reads that is the same for every draw of the layer.
        struct ForwardDrawContext
        {
            const std::shared_ptr<GraphicsDevice>& device;
            const Scene* scene;
            ProgramLibrary& programLibrary;
            const ForwardView& view;
            const ForwardLights& lights;
            const Layer& layer;
            Material* defaultMaterial;
            bool transparent;
            bool clusteredEnabled;
        };

        /// The inputs ProgramLibrary::bindMaterial resolves a variant from. The
        /// resolution is pure in them, and the states it sets are sticky on the encoder
        /// for the duration of the pass, so consecutive draws that agree on all of them
        /// — which dominate after sort-key ordering — skip it.
        struct ShaderVariantInputs
        {
            const Material* material = nullptr;
            bool dynamicBatch = false;
            bool skinned = false;
            bool morphed = false;
            bool instanced = false;
            bool instanceColor = false;
            bool instanceLightMap = false;
            bool screenSpace = false;

            bool operator==(const ShaderVariantInputs&) const = default;
        };

        /// What a draw contributes to its shader variant, read off its mesh instance.
        ShaderVariantInputs shaderVariantInputs(const MeshInstance* meshInstance, const Material* boundMaterial)
        {
            ShaderVariantInputs variant;
            variant.material = boundMaterial;
            if (!meshInstance) {
                return variant;
            }
            // Hardware instancing is a property of the draw, not the material: a mesh instance
            // that carries a per-instance buffer gets the instanced vertex stage, and the buffer's
            // stride decides whether that stage also reads a per-instance base color.
            const auto& drawInstancing = meshInstance->instancingData();
            const auto& instanceBuffer = drawInstancing.compactedVertexBuffer
                ? drawInstancing.compactedVertexBuffer : drawInstancing.vertexBuffer;
            variant.dynamicBatch = meshInstance->isDynamicBatch();
            variant.skinned = meshInstance->skinInstance() != nullptr;
            variant.morphed = meshInstance->morphInstance() != nullptr;
            variant.instanced = instanceBuffer != nullptr && drawInstancing.count > 0;
            variant.instanceColor = variant.instanced && instanceBuffer->format() &&
                instanceBuffer->format()->hasInstanceColor();
            // A lightmap the mesh instance owns (a lightmapper's bake) selects the
            // lightmap variant whatever the material says.
            variant.instanceLightMap = meshInstance->lightMap() != nullptr;
            variant.screenSpace = meshInstance->screenSpace();
            return variant;
        }

        /// Resolves the forward shader of every draw whose material the library has not
        /// resolved as it is now — a new material, an edited one, or one last drawn under
        /// other frame switches — BEFORE anything draws with them. A backend that
        /// compiles a shader when it is created (Metal: MetalShader) then compiles the
        /// new variants side by side instead of the draw loop stopping at each for a
        /// compile of its own. With nothing new this is one pass over the draws that
        /// reads what the loop is about to read anyway.
        ///
        /// `meshInstanceOf` and `materialOf` read a draw; the frame switches must
        /// already be set on the library (configureForwardShaderFeatures).
        template <typename Draws, typename MeshInstanceOf, typename MaterialOf>
        void resolveNewForwardShaders(ProgramLibrary& programLibrary, const Material* defaultMaterial,
            const bool transparent, const Draws& draws, MeshInstanceOf&& meshInstanceOf, MaterialOf&& materialOf)
        {
            const uint64_t frameBits = programLibrary.forwardFrameBits();
            const Material* lastMaterial = nullptr;
            for (const auto& draw : draws) {
                const Material* drawMaterial = materialOf(draw);
                const Material* material = drawMaterial ? drawMaterial : defaultMaterial;
                if (material == lastMaterial) {
                    continue;
                }
                lastMaterial = material;
                // A shader override is used as it is; a resolved material needs nothing.
                if (!material || material->shaderOverride() ||
                    programLibrary.forwardShaderResolved(material, frameBits)) {
                    continue;
                }
                const ShaderVariantInputs variant = shaderVariantInputs(meshInstanceOf(draw), material);
                (void)programLibrary.getForwardShader(material, transparent, variant.dynamicBatch,
                    variant.skinned, variant.morphed, variant.instanced, variant.instanceColor,
                    variant.instanceLightMap, variant.screenSpace);
            }
        }

        void submitGSplatDraw(const ForwardDrawContext& ctx, const ForwardDrawEntry& entry, GSplatInstance& gsplat)
        {
            GraphicsDevice& device = *ctx.device;
            const ForwardView& view = ctx.view;
            const Matrix4 modelMatrix = nodeWorldTransform(entry.meshInstance);
            gsplat.update(view.position, view.forward, modelMatrix, view.viewMatrix, view.projMatrix,
                static_cast<float>(view.viewport.w), static_cast<float>(view.viewport.h));
            if (gsplat.visibleCount() == 0) {
                return;
            }
            device.frameCounters().gsplats += static_cast<int>(gsplat.visibleCount());
#ifndef NDEBUG
            // Splats blend into EVERY sample of a multisampled target, which makes
            // them several times more expensive (upstream #9532, also Debug only).
            if (view.activeTarget && view.activeTarget->samples() > 1) {
                static std::set<std::string> warnedLayers;
                if (warnedLayers.insert(ctx.layer.name()).second) {
                    spdlog::warn("Gaussian splats on layer '{}' are rendered into a multisampled "
                        "target ({} samples), which makes them several times more expensive to "
                        "render. Render them into a single-sampled target: set the camera's "
                        "RenderingSettings::samples to 1.", ctx.layer.name(), view.activeTarget->samples());
                }
            }
#endif
            // The output stage: a splat carries a GAMMA-space colour, so it owes
            // the target the fog, exposure, tone mapping and encode the forward
            // tail applies to lit colour (upstream gsplatOutput). Decoding to linear
            // and stopping is right only under a camera frame: on a gamma target the
            // splats would be written linear, untonemapped and unfogged beside
            // tonemapped meshes.
            const FogParams fogParams = ctx.scene ? ctx.scene->fog() : FogParams{};
            GpuGSplatParams splatParams = gsplat.gpuParams();
            splatParams.cameraOrtho = view.camera->projection() == ProjectionType::Orthographic ? 1u : 0u;
            Color fogLinear;
            fogLinear.linear(&fogParams.color);
            splatParams.fogColor[0] = fogLinear.r;
            splatParams.fogColor[1] = fogLinear.g;
            splatParams.fogColor[2] = fogLinear.b;
            splatParams.fogParams[0] = fogParams.start;
            splatParams.fogParams[1] = fogParams.end;
            splatParams.fogParams[2] = fogParams.density;
            splatParams.fogParams[3] = fogParams.enabled ? static_cast<float>(fogParams.type) : 0.0f;
            splatParams.output[0] = ctx.scene ? ctx.scene->exposure() : 1.0f;
            splatParams.output[1] = static_cast<float>(view.toneMapping);
            splatParams.output[2] = device.hdrPass() ? 1.0f : 0.0f;
            for (const auto* buffer : {&gsplat.resource()->splatBuffer(), &gsplat.orderBuffer(),
                                       &gsplat.resource()->shBuffer()}) {
                if (*buffer) {
                    (*buffer)->markStorageUse();   // counts as vram.sb
                }
            }
            device.setGSplatState(gsplat.resource()->splatBuffer(), gsplat.orderBuffer(),
                gsplat.resource()->shBuffer(), &splatParams, sizeof(GpuGSplatParams));
            device.setTransformUniforms(view.viewProjection, modelMatrix);
            device.draw(entry.primitive, entry.indexBuffer,
                static_cast<int>(gsplat.visibleCount()), -1, true, true);
        }

        /// Binds the draw's geometry-kind state and issues the draw: GPU-culled or
        /// direct instancing, a dynamic batch, a storage draw, particles, splats, or
        /// an ordinary (possibly skinned or morphed) mesh.
        void submitForwardDraw(const ForwardDrawContext& ctx, const ForwardDrawEntry& entry,
            const ShaderVariantInputs& variant, const MeshInstance::InstancingData& instData)
        {
            GraphicsDevice& device = *ctx.device;
            FrameCounters& counters = device.frameCounters();
            const ForwardView& view = ctx.view;
            MeshInstance* meshInstance = entry.meshInstance;

            if (instData.indirectArgsBuffer && instData.indirectSlot >= 0 && instData.compactedVertexBuffer) {
                // GPU-culled indirect instancing:
                // Bind the compacted buffer (visible instances only) at slot 5.
                // Instance count comes from the GPU via indirect draw arguments.
                device.setVertexBuffer(instData.compactedVertexBuffer, 5);
                device.setIndirectDrawBuffer(instData.indirectArgsBuffer);
                // Each instance is placed in its NODE's space (upstream
                // transformInstancing: matrix_model * instance), so the node's world
                // matrix goes up as the model matrix and the shaders compose the two.
                device.setTransformUniforms(view.viewProjection, nodeWorldTransform(meshInstance));
                device.draw(entry.primitive, entry.indexBuffer, 0, instData.indirectSlot, true, true);
            } else if (instData.vertexBuffer && instData.count > 0) {
                // Direct instancing: all instances drawn, CPU-provided count. The
                // node's world matrix: instances live in its space (see above).
                device.setVertexBuffer(instData.vertexBuffer, 5);
                device.setTransformUniforms(view.viewProjection,
                    forwardModelMatrix(meshInstance, variant.material, ctx.scene, view.position));
                device.draw(entry.primitive, entry.indexBuffer, instData.count, -1, true, true);
            } else if (variant.dynamicBatch) {
                // Dynamic batch draw: dynamic batches use SkinBatchInstance with a
                // per-frame matrix palette, which the vertex shader indexes with a
                // per-vertex bone index; the model matrix is identity.
                if (auto* sbi = meshInstance->skinBatchInstance()) {
                    device.setDynamicBatchPalette(sbi->paletteData(), sbi->paletteSizeBytes(),
                        sbi->paletteVersion());
                }
                device.setTransformUniforms(view.viewProjection, Matrix4::identity());
                device.draw(entry.primitive, entry.indexBuffer, 1, -1, true, true);
            } else if (meshInstance && meshInstance->storageDrawCount() > 0) {
                // App-driven storage draw: a custom shader expands one instance per
                // record in an app-owned buffer (typically filled by a compute shader).
                // Same binding slots as the emitter/splat paths, no engine-side simulation.
                const auto& storageParams = meshInstance->storageParams();
                if (meshInstance->storageBuffer()) {
                    meshInstance->storageBuffer()->markStorageUse();   // counts as vram.sb
                }
                device.setStorageDrawState(meshInstance->storageBuffer(),
                    storageParams.data(), storageParams.size());
                device.setTransformUniforms(view.viewProjection, nodeWorldTransform(meshInstance));
                device.draw(entry.primitive, entry.indexBuffer, meshInstance->storageDrawCount(), -1, true, true);
            } else if (auto* particles = meshInstance ? meshInstance->particleEmitter() : nullptr) {
                // GPU particles: one instanced billboard quad per particle over the
                // compute-simulated pool (dead particles emit clipped vertices).
                const Matrix4 modelMatrix = nodeWorldTransform(meshInstance);
                particles->prepareRender(view.viewMatrix, view.projMatrix, modelMatrix,
                    static_cast<float>(view.viewport.w), static_cast<float>(view.viewport.h));
                // Upstream particle_end: tone map and gamma-encode on a gamma target, leave
                // both to compose on a camera frame's linear HDR scene.
                particles->setOutput(ctx.scene ? ctx.scene->exposure() : 1.0f, view.toneMapping, device.hdrPass());
                if (particles->particleBuffer()) {
                    particles->particleBuffer()->markStorageUse();   // counts as vram.sb
                }
                device.setParticleState(particles->particleBuffer(),
                    &particles->renderParams(), sizeof(GpuParticleRenderParams));
                device.setTransformUniforms(view.viewProjection, modelMatrix);
                device.draw(entry.primitive, entry.indexBuffer,
                    static_cast<int>(particles->numParticles()), -1, true, true);
            } else if (auto* gsplat = meshInstance ? meshInstance->gsplatInstance() : nullptr) {
                // Gaussian splats: one instanced quad per splat, back-to-front via the
                // per-instance order buffer filled by the background depth sorter.
                submitGSplatDraw(ctx, entry, *gsplat);
            } else {
                // GPU skinning: refresh the bone palette (no-op if already updated this
                // frame) and bind it at slot 6. The palette is node-relative; the model
                // matrix stays the node's world transform (they cancel in the shader).
                if (variant.skinned) {
                    auto* si = meshInstance->skinInstance();
                    {
                        const ScopedMilliseconds skinTimer(counters.skinTime);
                        si->updateMatrixPalette(meshInstance->node());
                    }
                    device.setDynamicBatchPalette(si->paletteData(), si->paletteSizeBytes(),
                        si->paletteVersion());
                    counters.skinDrawCalls++;
                }
                // Morph targets: bind the shared delta buffer + current weights.
                if (variant.morphed) {
                    auto* mi = meshInstance->morphInstance();
                    if (mi->morph() && mi->morph()->deltaBuffer()) {
                        const MorphInstance::GpuMorphParams* params = nullptr;
                        {
                            const ScopedMilliseconds morphTimer(counters.morphTime);
                            params = &mi->gpuParams();
                        }
                        device.setMorphState(mi->morph()->deltaBuffer(), params, sizeof(*params));
                    }
                }

                device.setTransformUniforms(view.viewProjection,
                    forwardModelMatrix(meshInstance, variant.material, ctx.scene, view.position));
                device.draw(entry.primitive, entry.indexBuffer, 1, -1, true, true);
            }
        }

        void drawForwardEntries(const ForwardDrawContext& ctx, const std::vector<ForwardDrawEntry*>& entries)
        {
            GraphicsDevice& device = *ctx.device;
            FrameCounters& counters = device.frameCounters();
            const Scene* scene = ctx.scene;
            Camera& camera = *ctx.view.camera;

            const auto ambientColor = scene ? scene->ambientLight() : Color(0.0f, 0.0f, 0.0f, 1.0f);
            const auto fogParams = scene ? scene->fog() : FogParams{};
            const float exposure = scene ? scene->exposure() : 1.0f;
            const Vector3* ambientSH = (scene && scene->hasAmbientSH()) ? scene->ambientSH().data() : nullptr;
            ShadowParams noShadow;
            noShadow.enabled = false;

            // The main light array for the light mask of the draw. 95%+ of mesh
            // instances use MASK_AFFECT_DYNAMIC (the default), so it is built for that
            // mask up front and rebuilt only when a draw's mask differs.
            static thread_local std::vector<GpuLightData> maskedLights;
            maskedLights.reserve(kMaxMainLights);
            uint32_t maskedLightsMask = MASK_AFFECT_DYNAMIC;
            filterLightsForMask(ctx.lights, maskedLightsMask, ctx.clusteredEnabled, maskedLights);

            // New variants start compiling before the loop needs the first of them. The
            // frame graph did this for the whole frame before any pass ran
            // (prepareForwardShaders); this is the exact one, with the frame switches as
            // they are now, for whatever that could not foresee.
            resolveNewForwardShaders(ctx.programLibrary, ctx.defaultMaterial, ctx.transparent, entries,
                [](const ForwardDrawEntry* entry) { return entry->meshInstance; },
                [](const ForwardDrawEntry* entry) { return entry->material; });

            ShaderVariantInputs boundVariant;

            // The lighting block depends on the DRAW only through its light mask and
            // receiveShadow; everything else in it is the layer's. Set it on the layer's
            // first draw and when either changes, not per draw: each set repacks ~2.8 KB
            // on Metal and makes Vulkan allocate and upload a fresh copy
            // (setLightingUniforms flags an upload).
            bool lightingSet = false;
            uint32_t lightingSetMask = 0;
            bool lightingSetReceivesShadow = true;

            // The material's base cull mode reads its parameter map, so it is cached
            // per material; the node-scale flip is applied per draw.
            const Material* lastCullMaterial = nullptr;
            CullMode cachedCullMode = CullMode::CULLFACE_BACK;

            for (const auto* entry : entries) {
                MeshInstance* meshInstance = entry->meshInstance;
                const Material* boundMaterial = entry->material ? entry->material : ctx.defaultMaterial;

                // A lightmap the mesh instance owns (a lightmapper's bake) overrides the
                // material's, so it both selects the lightmap variant and is bound by the
                // device over the material's lightmap slot.
                Texture* instanceLightMap = meshInstance ? meshInstance->lightMap().get() : nullptr;

                const ShaderVariantInputs variant = shaderVariantInputs(meshInstance, boundMaterial);

                device.setInstanceLightMap(instanceLightMap);
                // Upstream's per-draw stencil (UI masks); null leaves the stencil off.
                if (meshInstance) {
                    device.setStencilState(meshInstance->stencilFront(), meshInstance->stencilBack());
                } else {
                    device.setStencilState();
                }

                if (variant != boundVariant) {
                    ctx.programLibrary.bindMaterial(ctx.device, boundMaterial, ctx.transparent,
                        variant.dynamicBatch, variant.skinned, variant.morphed, variant.instanced,
                        variant.instanceColor, variant.instanceLightMap, variant.screenSpace);
                    boundVariant = variant;
                }

                const uint32_t drawLightMask = meshInstance ? meshInstance->mask() : MASK_AFFECT_DYNAMIC;
                if (drawLightMask != maskedLightsMask) {
                    filterLightsForMask(ctx.lights, drawLightMask, ctx.clusteredEnabled, maskedLights);
                    maskedLightsMask = drawLightMask;
                }

                // A mesh instance with receiveShadow=false gets no shadow params (SHADERDEF_NOSHADOW).
                const bool drawReceivesShadow = !meshInstance || meshInstance->receiveShadow();
                if (!lightingSet || drawLightMask != lightingSetMask ||
                    drawReceivesShadow != lightingSetReceivesShadow) {
                    device.setLightingUniforms(ambientColor, maskedLights, ctx.view.position, true, exposure,
                        fogParams, drawReceivesShadow ? ctx.lights.shadowParams : noShadow,
                        ctx.view.toneMapping, ambientSH, &ctx.view.viewProjection);
                    lightingSet = true;
                    lightingSetMask = drawLightMask;
                    lightingSetReceivesShadow = drawReceivesShadow;
                }

                if (boundMaterial != lastCullMaterial) {
                    counters.materialSwitches++;   // stats.frame.materials, upstream's prevMaterial test
                    cachedCullMode = resolveMaterialCullMode(boundMaterial);
                    lastCullMaterial = boundMaterial;
                }
                auto cullMode = applyNodeScaleFlip(cachedCullMode, meshInstance ? meshInstance->node() : nullptr);
                if (camera.lightmapBakeAccumulate()) {
                    // Ambient-occlusion virtual lights: each pass adds its own contribution
                    // to the lightmap already in the target (upstream sums the virtual lights
                    // the same way), so the draw blends additively and the camera does not
                    // clear between passes.
                    static const auto additive = std::make_shared<BlendState>(BlendState::additiveBlend());
                    device.setBlendState(additive);
                }
                if (camera.lightmapBakePass()) {
                    // UV-space bake: triangle winding follows the unwrap, not the surface —
                    // mirrored charts (and the v-flip that puts UV origin at the top) make
                    // triangles face either way, so face culling would drop them. Upstream's
                    // lightmapper likewise renders the bake double-sided.
                    cullMode = CullMode::CULLFACE_NONE;
                }
                device.setCullMode(cullMode);

                device.setVertexBuffer(entry->vertexBuffer, 0);
                submitForwardDraw(ctx, *entry, variant,
                    meshInstance ? meshInstance->instancingData() : MeshInstance::InstancingData{});
                counters.forwardDrawCalls++;
            }

            // No other pass may inherit the last draw's lightmap.
            device.setInstanceLightMap(nullptr);
            device.setStencilState();
        }
    }

    void Renderer::configureForwardShaderFeatures(ProgramLibrary& programLibrary, const Camera& camera,
        const bool clusteredEnabled)
    {
        // tell ProgramLibrary whether a skybox cubemap is available
        // so skybox shaders compile with VT_FEATURE_SKY_CUBEMAP.
        programLibrary.setSkyCubemapAvailable(_scene && _scene->skybox() != nullptr);

        // when camera is in depth pass mode, compile shaders
        // with VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS to override fragment output
        // with distance-from-reflection-plane (setShaderPass).
        programLibrary.setPlanarReflectionDepthPass(camera.planarReflectionDepthPass());
        programLibrary.setLightmapBakePass(camera.lightmapBakePass());
        programLibrary.setLightmapBakeAccumulate(camera.lightmapBakeAccumulate());

        // Debug surface-quantity output (setDebugShaderPass). One variant covers every mode;
        // the mode itself rides in a uniform, uploaded with the environment.
        programLibrary.setDebugPassEnabled(camera.debugShaderPass() != DebugShaderPass::DEBUGPASS_NONE);

        // Light-dependent variants are enabled only when a light actually needs
        // them: an unbound shadow map or cookie parameter would be nil at draw time.
        const LightFeatureUse lightFeatures = scanLightFeatureUse();
        programLibrary.setLocalShadowsEnabled(lightFeatures.localShadows);
        programLibrary.setOmniShadowsEnabled(lightFeatures.omniShadows);
        programLibrary.setCookie2DEnabled(lightFeatures.cookie2D);
        programLibrary.setCookieCubeEnabled(lightFeatures.cookieCube);
        programLibrary.setVsmShadowsEnabled(lightFeatures.vsmShadows);
        programLibrary.setPcssShadowsEnabled(lightFeatures.pcssShadows);
        programLibrary.setAreaLightsEnabled(lightFeatures.areaLights);

        // LTC lookup tables: created lazily the first time an area light is
        // seen, then bound at fragment slots 20/21 every frame they're needed.
        if (lightFeatures.areaLights && !_areaLightLuts.lut1) {
            _areaLightLuts = AreaLightLuts::create(_device.get());
        }
        _device->setAreaLightLuts(
            lightFeatures.areaLights ? _areaLightLuts.lut1.get() : nullptr,
            lightFeatures.areaLights ? _areaLightLuts.lut2.get() : nullptr);

        // Clustered lighting: compile forward shaders with VT_FEATURE_LIGHT_CLUSTERING
        // so the fragment shader samples the cluster grid.
        programLibrary.setClusteredLightingEnabled(clusteredEnabled);

        // SSAO per-material: when the device has a forward SSAO texture, compile
        // forward shaders with VT_FEATURE_SSAO so the fragment shader modulates
        // ambient occlusion by sampling the SSAO texture at screen-space UV.
        programLibrary.setSsaoEnabled(_device->ssaoForwardTexture() != nullptr);
        programLibrary.setLightProbesEnabled(_scene && _scene->hasAmbientSH());
        programLibrary.setEnvAtlasEnabled(_scene && _scene->envAtlas() != nullptr);
        programLibrary.setReflectionProbeEnabled(_scene && _scene->reflectionProbe() != nullptr);

        // Atmosphere scattering: when enabled on the scene, compile skybox shaders
        // with VT_FEATURE_ATMOSPHERE and push atmosphere uniforms to the device.
        const bool atmosphereEnabled = _scene && _scene->atmosphereEnabled();
        programLibrary.setAtmosphereEnabled(atmosphereEnabled);
        _device->setAtmosphereEnabled(atmosphereEnabled);
        if (atmosphereEnabled) {
            _device->setAtmosphereUniforms(_scene->atmosphereUniformData(), _scene->atmosphereUniformSize());
        }
    }

    void Renderer::resolveClusterConfig()
    {
        // Resolve the grid shape the scene asked for, once. Every pooled grid is
        // built with it; the grids themselves are created on demand, one per distinct
        // light set (clustersForLightSet). The shadow atlas, by contrast, is
        // configured every frame where it updates (ForwardRenderer::buildFrameGraph),
        // since a scene may change shadowAtlasResolution at any time.
        if (_clusterConfigResolved || !_scene) {
            return;
        }
        _clusterConfigResolved = true;
        const auto& lightingParams = _scene->lighting();
        _clusterConfig.cellsX = std::max(1, lightingParams.cellsX);
        _clusterConfig.cellsY = std::max(1, lightingParams.cellsY);
        _clusterConfig.cellsZ = std::max(1, lightingParams.cellsZ);
        _clusterConfig.maxLightsPerCell = std::max(1, lightingParams.maxLightsPerCell);
    }

    Camera* Renderer::directionalShadowFitCamera(Camera* camera) const
    {
        // Cascades are fit for a single designated camera per frame (see
        // ForwardRenderer::buildFrameGraph) — use its render data whichever
        // camera's pass is being encoded, so the matrices match the map.
        if (!_cameraDirShadowLights.contains(camera) && !_cameraDirShadowLights.empty()) {
            return _cameraDirShadowLights.begin()->first;
        }
        return camera;
    }

    void Renderer::bindLayerClusterLights(const std::vector<ClusterLightData>& lights,
        const std::vector<const void*>& lightSetMembers)
    {
        // The light list is per (camera, layer) — the gather filters on
        // LightComponent::rendersLayer — so the grid has to be too. One grid for the
        // frame would light a layer whose lights differ by another layer's cells, and
        // leave a layer with no clustered lights lit by the previous layer's buffers.
        //
        // Grids are keyed on the light set, so two layers that see the same lights
        // still share one and it is built once. The grid is sized from the lights
        // alone (see WorldClusters::computeGridBounds).
        WorldClusters* clusters = clustersForLightSet(lightSetHash(lightSetMembers), lights);

        // Bind the clustered shadow atlas for this frame.
        if (_lightTextureAtlas) {
            _device->setClusterShadowAtlas(_lightTextureAtlas->shadowAtlasTexture());
        }

        bindLayerClusters(clusters);
    }

    bool Renderer::forwardShaderPreparationDue()
    {
        if (!_device) {
            return false;
        }
        // Decided once a frame: the first frame, and any frame after one that built a
        // variant (new content tends to arrive over several frames).
        const int frame = _device->renderVersion();
        if (frame != _shaderPreparationFrame) {
            _shaderPreparationFrame = frame;
            const auto programLibrary = getProgramLibrary(_device);
            const uint64_t created = programLibrary ? programLibrary->forwardVariantsCreated() : 0;
            _shaderPreparationDue = !_shaderPreparationEver || created != _shaderPreparationVariantsSeen;
            _shaderPreparationVariantsSeen = created;
            _shaderPreparationEver = true;
        }
        return _shaderPreparationDue;
    }

    void Renderer::prepareForwardShaders(Camera* camera, Layer* layer, const bool transparent)
    {
        if (!camera || !layer || !_device || !forwardShaderPreparationDue()) {
            return;
        }
        const auto programLibrary = getProgramLibrary(_device);
        if (!programLibrary) {
            return;
        }
        // The frame switches as renderForwardLayer will set them for this camera. One
        // of them can still change before it runs (the lighting-mode SSAO texture is
        // published by a pass of this frame); renderForwardLayer resolves again, with
        // the switches as they then are.
        configureForwardShaderFeatures(*programLibrary, *camera, _scene && _scene->clusteredLightingEnabled());
        const CulledInstances& visible = culledInstances(camera, camera->node(), layer);
        const auto defaultMaterial = getDefaultMaterial(_device);
        resolveNewForwardShaders(*programLibrary, defaultMaterial.get(), transparent,
            transparent ? visible.transparent : visible.opaque,
            [](const MeshInstance* meshInstance) { return meshInstance; },
            [](const MeshInstance* meshInstance) { return meshInstance ? meshInstance->material() : nullptr; });
    }

    void Renderer::renderForwardLayer(Camera* camera, RenderTarget* renderTarget, Layer* layer, bool transparent)
    {
        if (!camera || !layer || !_device) {
            return;
        }

        FrameCounters& counters = _device->frameCounters();
        const ScopedMilliseconds forwardTimer(counters.forwardTime);

        auto programLibrary = getProgramLibrary(_device);
        if (!programLibrary) {
            spdlog::error("ProgramLibrary is not initialized. Forward rendering requires ProgramLibrary.");
            return;
        }

        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();
        configureForwardShaderFeatures(*programLibrary, *camera, clusteredEnabled);
        if (clusteredEnabled) {
            resolveClusterConfig();
        }

        const auto defaultMaterial = getDefaultMaterial(_device);

        ForwardView view = setupForwardView(*_device, _scene.get(), *camera, renderTarget);

        // Apply the camera's rect on the active render target for this pass only.
        const ViewportScissorScope viewportScope(*_device);
        _device->setViewport(
            static_cast<float>(view.viewport.x),
            static_cast<float>(view.viewport.y),
            static_cast<float>(view.viewport.w),
            static_cast<float>(view.viewport.h));
        _device->setScissor(view.scissor.x, view.scissor.y, view.scissor.w, view.scissor.h);

        // This function runs once per camera x layer x (opaque|transparent), so several
        // times a frame: the entries and the vectors holding them are reused across
        // calls rather than allocated each time. thread_local for the same reason.
        // DEVIATION: pooled frame-local query objects reduce allocator churn in this native port.
        static thread_local ObjectPool<ForwardDrawEntry> drawEntryPool(256);
        static thread_local std::vector<ForwardDrawEntry*> drawEntries;
        static thread_local ForwardLights lights;
        drawEntryPool.freeAll();
        drawEntries.clear();
        drawEntries.reserve(256);

        // Culling has already happened, once for this (camera, layer) pair — see
        // Renderer::cullMeshInstancesInto. Each sublayer reads its own bucket.
        const CulledInstances& visible = culledInstances(camera, view.cameraNode, layer);
        view.hasFrustum = view.cameraNode != nullptr;
        view.frustum = view.hasFrustum ? buildCameraFrustum(camera, view.cameraNode) : Frustum{};

        collectDrawEntries(transparent ? visible.transparent : visible.opaque, view,
            defaultMaterial.get(), drawEntryPool, drawEntries);
        {
            const ScopedMilliseconds sortTimer(counters.sortTime);
            sortDrawEntries(drawEntries, *layer, transparent);
        }

        lights.reset();
        gatherForwardLights(view, *layer, _scene.get(), clusteredEnabled,
            directionalShadowFitCamera(camera), lights);

        bindLayerEnvironment(*_device, _scene.get(), view);

        if (clusteredEnabled) {
            static thread_local std::vector<ClusterLightData> clusterLights;
            static thread_local std::vector<const void*> lightSetMembers;
            buildClusterLightInput(lights.local, clusterLights, lightSetMembers);
            bindLayerClusterLights(clusterLights, lightSetMembers);
        }

        drawForwardEntries(ForwardDrawContext{
            .device = _device,
            .scene = _scene.get(),
            .programLibrary = *programLibrary,
            .view = view,
            .lights = lights,
            .layer = *layer,
            .defaultMaterial = defaultMaterial.get(),
            .transparent = transparent,
            .clusteredEnabled = clusteredEnabled,
        }, drawEntries);
    }
}
