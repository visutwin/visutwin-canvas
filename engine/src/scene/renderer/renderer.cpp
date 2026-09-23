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
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

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

        /// Thin wrapper: the layout itself lives in sortKey.h so a test can hold it.
        uint64_t makeOpaqueSortKey(const MeshInstance* meshInstance)
        {
            const auto* material = meshInstance ? meshInstance->material() : nullptr;
            return makeForwardSortKey(
                meshInstance ? meshInstance->drawBucket() : 0u,
                material && material->alphaMode() == AlphaMode::MASK,
                // No material means the default one, and they all sort together.
                material ? material->id() : 0x7FFFFFu,
                reinterpret_cast<uintptr_t>(meshInstance ? meshInstance->mesh() : nullptr));
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

        _shadowRenderer = std::make_unique<ShadowRenderer>(this, _lightTextureAtlas.get());

        _shadowRendererLocal = std::make_unique<ShadowRendererLocal>(this, _shadowRenderer.get());
        _shadowRendererDirectional = std::make_unique<ShadowRendererDirectional>(device, this, _shadowRenderer.get());

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
            if (!sceneLight || sceneLight->visibleThisFrame()) {
                continue;   // another camera already reached it
            }

            // A directional light has no position and no range, so there is nothing
            // to test: it lights whatever the camera can see, by construction.
            if (lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                sceneLight->setVisibleThisFrame(true);
                continue;
            }

            const BoundingSphere bounds = sceneLight->boundingSphere();
            if (frustum.checkSphere(bounds.center(), bounds.radius())) {
                sceneLight->setVisibleThisFrame(true);
                // The union over cameras of the viewport fraction the light covers,
                // which ranks it for an atlas slot (upstream maxScreenSize).
                sceneLight->setMaxScreenSize(std::max(sceneLight->maxScreenSize(), camera->screenSize(bounds)));
                continue;
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
        clusters->update(lights);
        _clustersByLightSet[lightSetHash] = clusters;
        return clusters;
    }

    void Renderer::resetCulledInstances()
    {
        _culledInstances.clear();
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
        for (Camera* camera : _cullCameras) {
            // Before the frustum is built, so a listener may still move the camera.
            // Upstream passes the owning camera COMPONENT, or nothing for an internal
            // camera (shadow, reflection, picker); this port has no back pointer from
            // Camera to its component, so it passes the camera.
            if (_scene) {
                _scene->fire("precull", camera);
            }
            GraphNode* cameraNode = camera ? camera->node() : nullptr;
            for (Layer* layer : _cullRequests[camera]) {
                cullMeshInstancesInto(camera, cameraNode, layer,
                    _culledInstances[{camera, layer}]);
            }
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
        out.opaque.clear();
        out.transparent.clear();
        if (!layer) {
            return;
        }

        const bool hasCameraFrustum = camera && cameraNode;
        const Frustum cameraFrustum = hasCameraFrustum
            ? buildCameraFrustum(camera, cameraNode) : Frustum{};
        out.frustum = cameraFrustum;
        out.valid = true;
        const uint32_t cullingMask = camera ? camera->cullingMask() : 0xFFFFFFFFu;

        const auto consider = [&](MeshInstance* meshInstance) {
            if (!meshInstance || !meshInstance->visible() || !meshInstance->mesh()) {
                return;
            }
            if (!meshInstance->mesh()->getVertexBuffer()) {
                return;
            }
            // Upstream's Camera.cullingMask against MeshInstance.mask: a camera that
            // wants a subset of the scene says so here rather than by juggling layers.
            if ((meshInstance->mask() & cullingMask) == 0u) {
                return;
            }

            const auto fallbackMaterial = getDefaultMaterial(_device);
            const Material* material = meshInstance->material()
                ? meshInstance->material() : fallbackMaterial.get();
            if (!material) {
                return;
            }

            // A skybox is drawn around the camera and has no meaningful bounds, so it
            // is never culled — the same exception the per-draw path used to make.
            if (!material->isSkybox() && hasCameraFrustum && meshInstance->cull() &&
                !isVisibleInFrustum(cameraFrustum, meshInstance->aabb())) {
                _numDrawCallsCulled++;
                return;
            }

            // Split here, ONCE, so each sublayer reads only its own bucket.
            (material->transparent() ? out.transparent : out.opaque).push_back(meshInstance);
        };

        for (auto* renderComponent : RenderComponent::instances()) {
            // active() covers both halves: the component's own flag and the owning
            // entity's hierarchy state.
            if (!renderComponent || !renderComponent->active()) {
                continue;
            }
            const auto& componentLayers = renderComponent->layers();
            if (std::find(componentLayers.begin(), componentLayers.end(), layer->id())
                    == componentLayers.end()) {
                continue;
            }
            for (auto* meshInstance : renderComponent->meshInstances()) {
                consider(meshInstance);
            }
        }

        // Instances added to the layer directly rather than through a component.
        for (auto* meshInstance : layer->meshInstances()) {
            consider(meshInstance);
        }
    }

    void Renderer::consumeOneShotShadows()
    {
        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();
        auto consume = [&](Light* light) {
            if (!light || !_shadowRenderer || !_shadowRenderer->needsShadowRendering(light)) {
                return;
            }
            _shadowMapUpdates += light->numShadowFaces();
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

    void Renderer::dispatchGpuInstanceCulling(Camera* camera)
    {
        if (!camera || !camera->node() || !_device) {
            return;
        }

        // Compute view-projection: view = inverse(camera world), proj = camera proj.
        // The frustum plane extraction expects a column-major float[16] layout,
        // which matches Matrix4's in-memory representation (64 bytes, SIMD-safe).
        const Matrix4 view = camera->node()->worldTransform().inverse();
        const Matrix4 vp = camera->projectionMatrix() * view;

        float planes[6][4];
        InstanceCuller::extractFrustumPlanes(reinterpret_cast<const float*>(&vp), planes);

        // Batch all cull dispatches into one backend command buffer with a
        // single sync at the end — begun lazily so frames with no GPU-culled
        // instances create no command buffer at all.
        bool cullBatchStarted = false;

        for (auto* rc : RenderComponent::instances()) {
            if (!rc) {
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

                InstanceCullParams params{};
                std::memcpy(params.frustumPlanes, planes, sizeof(planes));
                params.boundingSphereRadius = mi->instanceCullRadius();
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

    void Renderer::renderForwardLayer(Camera* camera, RenderTarget* renderTarget, Layer* layer, bool transparent)
    {
        if (!camera || !layer || !_device) {
            return;
        }

        const auto sortStart = std::chrono::high_resolution_clock::now();

        auto programLibrary = getProgramLibrary(_device);
        if (!programLibrary) {
            spdlog::error("ProgramLibrary is not initialized. Forward rendering requires ProgramLibrary.");
            return;
        }

        // tell ProgramLibrary whether a skybox cubemap is available
        // so skybox shaders compile with VT_FEATURE_SKY_CUBEMAP.
        programLibrary->setSkyCubemapAvailable(_scene && _scene->skybox() != nullptr);

        // when camera is in depth pass mode, compile shaders
        // with VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS to override fragment output
        // with distance-from-reflection-plane (setShaderPass).
        programLibrary->setPlanarReflectionDepthPass(camera && camera->planarReflectionDepthPass());
        programLibrary->setLightmapBakePass(camera && camera->lightmapBakePass());
        programLibrary->setLightmapBakeAccumulate(camera && camera->lightmapBakeAccumulate());

        // Debug surface-quantity output (setDebugShaderPass). One variant covers every mode;
        // the mode itself rides in a uniform, uploaded below.
        programLibrary->setDebugPassEnabled(
            camera && camera->debugShaderPass() != DebugShaderPass::DEBUGPASS_NONE);

        // Tell ProgramLibrary whether any local light casts shadows AND has
        // an allocated shadow map. Only enable VT_FEATURE_LOCAL_SHADOWS / VT_FEATURE_OMNI_SHADOWS
        // when actual shadow textures exist, so the shader doesn't compile with
        // depth2d / depthcube parameters that would be nil at runtime.
        {
            bool hasLocalShadows = false;
            bool hasOmniShadows = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->active() && lc->castShadows() &&
                    lc->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
                    Light* sceneLight = lc->light();
                    if (sceneLight && sceneLight->shadowMap()) {
                        if (lc->type() == LightType::LIGHTTYPE_OMNI) {
                            hasOmniShadows = true;
                        } else {
                            hasLocalShadows = true;
                        }
                    }
                }
            }
            programLibrary->setLocalShadowsEnabled(hasLocalShadows);
            programLibrary->setOmniShadowsEnabled(hasOmniShadows);

            // Light cookies: separate variants for the 2D (spot) and cubemap (omni)
            // samplers, enabled only when a light actually carries a cookie of that
            // kind — same reason as the shadow features, an unbound texture
            // parameter would be nil at draw time.
            bool hasCookie2D = false;
            bool hasCookieCube = false;
            for (const auto* lc : LightComponent::instances()) {
                if (!lc || !lc->active() || !lc->cookie()) {
                    continue;
                }
                // The cookie's shape has to match the light: a spot projects a 2D
                // texture, an omni samples a cubemap by direction. A mismatch is
                // ignored, as upstream does.
                if (lc->type() == LightType::LIGHTTYPE_SPOT && !lc->cookie()->isCubemap()) {
                    hasCookie2D = true;
                } else if ((lc->type() == LightType::LIGHTTYPE_OMNI ||
                            lc->type() == LightType::LIGHTTYPE_POINT) &&
                           lc->cookie()->isCubemap()) {
                    hasCookieCube = true;
                }
            }
            programLibrary->setCookie2DEnabled(hasCookie2D);
            programLibrary->setCookieCubeEnabled(hasCookieCube);

            // Directional EVSM_16F: enable when any shadow-casting directional
            // light is configured for SHADOW_VSM_16F. Both the shadow program
            // (writes moments) and the forward program (samples via Chebyshev)
            // need the matching VT_FEATURE_VSM_SHADOWS variant.
            bool hasVsmShadows = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->active() && lc->castShadows() &&
                    lc->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                    Light* sceneLight = lc->light();
                    if (sceneLight && sceneLight->shadowType() == SHADOW_VSM_16F) {
                        hasVsmShadows = true;
                        break;
                    }
                }
            }
            programLibrary->setVsmShadowsEnabled(hasVsmShadows);

            // Directional PCSS: contact-hardening soft shadows. The shadow map
            // stays the standard depth texture (only the sampling differs).
            bool hasPcssShadows = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->active() && lc->castShadows() &&
                    lc->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                    Light* sceneLight = lc->light();
                    if (sceneLight && sceneLight->shadowType() == SHADOW_PCSS_32F) {
                        hasPcssShadows = true;
                        break;
                    }
                }
            }
            programLibrary->setPcssShadowsEnabled(hasPcssShadows);
        }

        // Area lights: enable when any LightComponent has LIGHTTYPE_AREA_RECT.
        {
            bool hasAreaLights = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->active() && lc->type() == LightType::LIGHTTYPE_AREA_RECT) {
                    hasAreaLights = true;
                    break;
                }
            }
            programLibrary->setAreaLightsEnabled(hasAreaLights);

            // LTC lookup tables: created lazily the first time an area light is
            // seen, then bound at fragment slots 20/21 every frame they're needed.
            if (hasAreaLights && !_areaLightLuts.lut1) {
                _areaLightLuts = AreaLightLuts::create(_device.get());
            }
            _device->setAreaLightLuts(
                hasAreaLights ? _areaLightLuts.lut1.get() : nullptr,
                hasAreaLights ? _areaLightLuts.lut2.get() : nullptr);
        }

        // Clustered lighting: when enabled on the scene, compile forward shaders
        // with VT_FEATURE_LIGHT_CLUSTERING so the fragment shader samples the cluster grid.
        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();
        programLibrary->setClusteredLightingEnabled(clusteredEnabled);

        // SSAO per-material: when the device has a forward SSAO texture, compile
        // forward shaders with VT_FEATURE_SSAO so the fragment shader modulates
        // ambient occlusion by sampling the SSAO texture at screen-space UV.
        programLibrary->setSsaoEnabled(_device->ssaoForwardTexture() != nullptr);
        programLibrary->setLightProbesEnabled(_scene && _scene->hasAmbientSH());
        programLibrary->setEnvAtlasEnabled(_scene && _scene->envAtlas() != nullptr);
        programLibrary->setReflectionProbeEnabled(_scene && _scene->reflectionProbe() != nullptr);

        // Atmosphere scattering: when enabled on the scene, compile skybox shaders
        // with VT_FEATURE_ATMOSPHERE and push atmosphere uniforms to the device.
        const bool atmosphereEnabled = _scene && _scene->atmosphereEnabled();
        programLibrary->setAtmosphereEnabled(atmosphereEnabled);
        _device->setAtmosphereEnabled(atmosphereEnabled);
        if (atmosphereEnabled) {
            _device->setAtmosphereUniforms(_scene->atmosphereUniformData(), _scene->atmosphereUniformSize());
        }

        // Resolve the grid shape the scene asked for, once. Every pooled grid is
        // built with it; the grids themselves are created on demand, one per distinct
        // light set (clustersForLightSet).
        if (clusteredEnabled && !_clusterConfigResolved) {
            _clusterConfigResolved = true;
            const auto& lightingParams = _scene->lighting();
            _clusterConfig.cellsX = std::max(1, lightingParams.cellsX);
            _clusterConfig.cellsY = std::max(1, lightingParams.cellsY);
            _clusterConfig.cellsZ = std::max(1, lightingParams.cellsZ);
            _clusterConfig.maxLightsPerCell = std::max(1, lightingParams.maxLightsPerCell);
        }
        // The atlas is configured where it updates (ForwardRenderer::buildFrameGraph),
        // every frame: a scene may change shadowAtlasResolution at any time. It used to
        // be configured once, here, so a later change was ignored without a word.

        const auto defaultMaterial = getDefaultMaterial(_device);

        auto* cameraNode = camera->node();
        const auto cameraPosition = cameraNode ? cameraNode->position() : Vector3{};
        const auto cameraForward = cameraNode ? cameraForwardOf(cameraNode->worldTransform())
                                              : Vector3(0.0f, 0.0f, -1.0f);
        const auto viewMatrix = cameraNode ? cameraNode->worldTransform().inverse() : Matrix4::identity();
        const auto activeTarget = renderTarget ? renderTarget : camera->renderTarget().get();
        const int targetWidth = std::max(activeTarget ? activeTarget->width() : _device->size().first, 1);
        const int targetHeight = std::max(activeTarget ? activeTarget->height() : _device->size().second, 1);

        const auto clamp01 = [](const float v) {
            return std::clamp(v, 0.0f, 1.0f);
        };
        const Vector4 rect = camera->rect();
        const float rectXNorm = clamp01(rect.getX());
        const float rectYNorm = clamp01(rect.getY());
        const float rectWNorm = clamp01(rect.getZ());
        const float rectHNorm = clamp01(rect.getW());
        const float rectTopNorm = clamp01(rectYNorm + rectHNorm);

        int viewportX = static_cast<int>(rectXNorm * static_cast<float>(targetWidth));
        // Upstream rect origin is bottom-left. Metal viewport/scissor origin is top-left.
        int viewportY = targetHeight - static_cast<int>(rectTopNorm * static_cast<float>(targetHeight));
        viewportX = std::clamp(viewportX, 0, std::max(targetWidth - 1, 0));
        viewportY = std::clamp(viewportY, 0, std::max(targetHeight - 1, 0));
        int viewportW = std::max(1, static_cast<int>(rectWNorm * static_cast<float>(targetWidth)));
        int viewportH = std::max(1, static_cast<int>(rectHNorm * static_cast<float>(targetHeight)));
        viewportW = std::clamp(viewportW, 1, targetWidth - viewportX);
        viewportH = std::clamp(viewportH, 1, targetHeight - viewportY);

        const Vector4 scissorRect = camera->scissorRect();
        const float sxNorm = clamp01(scissorRect.getX());
        const float syNorm = clamp01(scissorRect.getY());
        const float swNorm = clamp01(scissorRect.getZ());
        const float shNorm = clamp01(scissorRect.getW());
        const float sTopNorm = clamp01(syNorm + shNorm);
        int scissorX = static_cast<int>(sxNorm * static_cast<float>(targetWidth));
        int scissorY = targetHeight - static_cast<int>(sTopNorm * static_cast<float>(targetHeight));
        scissorX = std::clamp(scissorX, 0, std::max(targetWidth - 1, 0));
        scissorY = std::clamp(scissorY, 0, std::max(targetHeight - 1, 0));
        int scissorW = std::max(1, static_cast<int>(swNorm * static_cast<float>(targetWidth)));
        int scissorH = std::max(1, static_cast<int>(shNorm * static_cast<float>(targetHeight)));
        scissorW = std::clamp(scissorW, 1, targetWidth - scissorX);
        scissorH = std::clamp(scissorH, 1, targetHeight - scissorY);

        // ASPECT_AUTO cameras use viewport size, not full target size.
        if (camera->aspectRatioMode() == AspectRatioMode::ASPECT_AUTO) {
            camera->setAspectRatio(static_cast<float>(viewportW) / static_cast<float>(viewportH));
        }

        const CameraComponent* cameraComponent = nullptr;
        for (const auto* candidate : CameraComponent::instances()) {
            if (candidate && candidate->camera() == camera) {
                cameraComponent = candidate;
                break;
            }
        }

        Matrix4 projMatrix = camera->projectionMatrix();
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        const float jitter = std::max(camera->jitter(), 0.0f);
        if (jitter > 0.0f) {
            const auto& offset = haltonSequence[static_cast<size_t>(_device->renderVersion() % haltonSequence.size())];
            jitterX = jitter * (offset[0] * 2.0f - 1.0f) / static_cast<float>(viewportW);
            jitterY = jitter * (offset[1] * 2.0f - 1.0f) / static_cast<float>(viewportH);

            // Accumulate, do not assign: these are the same two elements that carry an
            // off-center projection offset (Camera::setProjectionOffset), so overwriting them
            // here would silently cancel the shift lens whenever TAA is enabled.
            projMatrix.setElement(2, 0, projMatrix.getElement(2, 0) + jitterX);
            projMatrix.setElement(2, 1, projMatrix.getElement(2, 1) + jitterY);
        }
        const auto viewProjection = projMatrix * viewMatrix;
        camera->storeShaderMatrices(viewProjection, jitterX, jitterY, _device->renderVersion());

        // apply per-camera rect on active render target.
        const float oldVx = _device->vx();
        const float oldVy = _device->vy();
        const float oldVw = _device->vw();
        const float oldVh = _device->vh();
        const int oldSx = _device->sx();
        const int oldSy = _device->sy();
        const int oldSw = _device->sw();
        const int oldSh = _device->sh();

        _device->setViewport(
            static_cast<float>(viewportX),
            static_cast<float>(viewportY),
            static_cast<float>(viewportW),
            static_cast<float>(viewportH)
        );
        _device->setScissor(scissorX, scissorY, scissorW, scissorH);
        // DEVIATION: pooled frame-local query objects reduce allocator churn in this native port.
        static thread_local ObjectPool<ForwardDrawEntry> drawEntryPool(256);
        drawEntryPool.freeAll();
        // Reused across calls, like the pool above: this function runs once per
        // camera x layer x (opaque|transparent), so several times per frame, and a
        // fresh vector here meant a heap allocation on each of them. clear() keeps
        // the capacity. thread_local for the same reason the pool is.
        static thread_local std::vector<ForwardDrawEntry*> drawEntries;
        drawEntries.clear();
        drawEntries.reserve(256);

        // Culling has already happened, once for this (camera, layer) pair — see
        // Renderer::cullMeshInstancesInto. This used to sweep every RenderComponent
        // in the scene and run the frustum test here, for the OPAQUE sublayer and
        // then again for the TRANSPARENT one, each discarding the half that belonged
        // to the other. Now each sublayer reads its own bucket.
        const CulledInstances& visible = culledInstances(camera, cameraNode, layer);
        const std::vector<MeshInstance*>& bucket =
            transparent ? visible.transparent : visible.opaque;

        // Still built here, but only for the LIGHT cull further down — mesh
        // instances are culled in the batch above and never touch it.
        const bool hasCameraFrustum = camera && cameraNode;
        const Frustum cameraFrustum = hasCameraFrustum
            ? buildCameraFrustum(camera, cameraNode) : Frustum{};

        const auto appendMeshInstance = [&](MeshInstance* meshInstance) {
            auto* mesh = meshInstance ? meshInstance->mesh() : nullptr;
            if (!mesh) {
                return;
            }

            auto vertexBuffer = mesh->getVertexBuffer();
            if (!vertexBuffer) {
                return;
            }

            auto* entry = drawEntryPool.allocate();
            entry->meshInstance = meshInstance;
            entry->material = meshInstance->material() ? meshInstance->material() : defaultMaterial.get();
            if (!entry->material) {
                return;
            }

            const bool isSkyboxMaterial = entry->material->isSkybox();
            const auto worldBounds = meshInstance->aabb();

            entry->vertexBuffer = vertexBuffer;
            entry->indexBuffer = mesh->getIndexBuffer();
            entry->primitive = mesh->getPrimitive();
            entry->sortKey = makeOpaqueSortKey(meshInstance);

            auto* node = meshInstance->node();
            if (node && !isSkyboxMaterial) {
                // Signed view-axis depth, as upstream's _calculateSortDistances. This
                // used to be the squared radial distance, which ranked an off-axis
                // transparent surface behind a centred one at the same depth.
                const auto& customDistance = meshInstance->calculateSortDistance();
                entry->sortDistance = customDistance
                    ? customDistance(*meshInstance, cameraPosition, cameraForward)
                    : forwardSortDistance(worldBounds.center(), cameraPosition, cameraForward);
            } else {
                entry->sortDistance = 0.0f;
            }

            drawEntries.push_back(entry);
        };

        for (auto* meshInstance : bucket) {
            appendMeshInstance(meshInstance);
        }

        // Upstream's Layer.sortVisible: the mode is a per-layer, per-sublayer
        // property, because the two sublayers want opposite things — the opaque pass
        // wants the fewest state changes, the transparent pass has to composite
        // back-to-front whatever that costs. The defaults reproduce exactly what this
        // renderer did before the modes existed.
        const SortMode sortMode = transparent
            ? layer->transparentSortMode() : layer->opaqueSortMode();

        // A custom comparator may read MeshInstance::sortDistance, so publish what
        // was computed per draw above before calling one.
        if (sortMode == SortMode::SORTMODE_CUSTOM) {
            for (auto* entry : drawEntries) {
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
            std::stable_sort(drawEntries.begin(), drawEntries.end(),
                [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                    const int orderA = a->meshInstance ? a->meshInstance->drawOrder() : 0;
                    const int orderB = b->meshInstance ? b->meshInstance->drawOrder() : 0;
                    return orderA < orderB;
                });
            break;

        case SortMode::SORTMODE_BACK2FRONT:
            std::stable_sort(drawEntries.begin(), drawEntries.end(),
                [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                    if (a->sortDistance == b->sortDistance) {
                        return a->sortKey < b->sortKey;
                    }
                    return a->sortDistance > b->sortDistance;
                });
            break;

        case SortMode::SORTMODE_FRONT2BACK:
            std::stable_sort(drawEntries.begin(), drawEntries.end(),
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
            if (const auto& callback = layer->customSortCallback()) {
                std::stable_sort(drawEntries.begin(), drawEntries.end(),
                    [&callback](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                        return callback(a->meshInstance, b->meshInstance);
                    });
            }
            break;

        case SortMode::SORTMODE_MATERIALMESH:
        default:
            std::stable_sort(drawEntries.begin(), drawEntries.end(),
                [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                    if (a->sortKey != b->sortKey) {
                        return a->sortKey < b->sortKey;
                    }
                    return a->sortDistance < b->sortDistance;
                });
            break;
        }

        const auto sortEnd = std::chrono::high_resolution_clock::now();
        _sortTime += static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(sortEnd - sortStart).count());

        // Intentional temporary deviation from JS:
        // this path now binds core material uniforms/textures (including Material::setParameter overrides)
        // and forward shader variants, while full parameter scope is still being ported.
        const auto ambientColor = _scene ? _scene->ambientLight() : Color(0.0f, 0.0f, 0.0f, 1.0f);
        const auto fogParams = _scene ? _scene->fog() : FogParams{};
        static thread_local std::vector<LightDispatchEntry> directionalLights;
        static thread_local std::vector<LightDispatchEntry> localLights;
        directionalLights.clear();
        localLights.clear();
        directionalLights.reserve(4);
        localLights.reserve(8);
        ShadowParams shadowParams{};

        // Light cookie slot pools: two 2D (spot) and two cubemap (omni) slots,
        // matching the local-shadow slot count.
        constexpr int kMaxCookies = 2;
        int cookie2DCount = 0;
        int cookieCubeCount = 0;

        auto toRadians = [](const float degrees) {
            return degrees * (std::numbers::pi_v<float> / 180.0f);
        };

        auto makeGpuLight = [&](const LightComponent* lightComponent, GpuLightData& lightData) {
            if (!lightComponent) {
                return;
            }

            switch (lightComponent->type()) {
                case LightType::LIGHTTYPE_DIRECTIONAL:
                    lightData.type = GpuLightType::Directional;
                    break;
                case LightType::LIGHTTYPE_SPOT:
                    lightData.type = GpuLightType::Spot;
                    break;
                case LightType::LIGHTTYPE_AREA_RECT:
                    lightData.type = GpuLightType::AreaRect;
                    lightData.areaHalfWidth = lightComponent->areaWidth() * 0.5f;
                    lightData.areaHalfHeight = lightComponent->areaHeight() * 0.5f;
                    lightData.areaShape = static_cast<uint32_t>(lightComponent->areaShape());
                    {
                        // Right vector: the light's world X AXIS, which is column 0 — the same
                        // column upstream's LTC width axis comes from (it transforms (-0.5, 0, 0)
                        // by the world matrix; the sign is immaterial here because the shader
                        // derives up as cross(direction, right) and the quad is symmetric).
                        // This read ROW 0 until 2026-09-22, which is the X component of all
                        // three axes: right for an unrotated light, a vector outside the light's
                        // own plane for any rotated one.
                        const auto& wt = lightComponent->entity()->worldTransform();
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

            lightData.position = lightComponent->position();
            lightData.direction = lightComponent->direction();
            if (lightData.direction.lengthSquared() > 1e-8f) {
                lightData.direction = lightData.direction.normalized();
            } else {
                lightData.direction = Vector3(0.0f, -1.0f, 0.0f);
            }
            lightData.color = lightComponent->color();
            lightData.intensity = std::max(lightComponent->intensity(), 0.0f);
            lightData.range = std::max(lightComponent->range(), 1e-4f);
            // inner/outerConeAngle are HALF-angles in degrees (upstream Light:
            // `cos(angle * DEG_TO_RAD)`, and its spot shadow/cookie cameras use
            // `fov = outerConeAngle * 2`). Halving them here made every spot cone
            // half as wide as the shadow and cookie frustum fitted to the same
            // light — visible as a beam covering only the middle of its cookie.
            lightData.innerConeCos = std::cos(toRadians(std::max(lightComponent->innerConeAngle(), 0.0f)));
            lightData.outerConeCos = std::cos(toRadians(std::max(lightComponent->outerConeAngle(), 0.0f)));
            if (lightData.innerConeCos < lightData.outerConeCos) {
                lightData.innerConeCos = lightData.outerConeCos;
            }
            lightData.falloffModeLinear = lightComponent->falloffMode() == LightFalloff::LIGHTFALLOFF_LINEAR;
            lightData.castShadows = lightComponent->castShadows();
        };

        for (const auto* lightComponent : LightComponent::instances()) {
            if (!lightComponent || !lightComponent->active()) {
                continue;
            }
            if (layer && !lightComponent->rendersLayer(layer->id())) {
                continue;
            }
            if (cameraComponent && layer && !cameraComponent->rendersLayer(layer->id())) {
                continue;
            }

            GpuLightData lightData{};
            makeGpuLight(lightComponent, lightData);
            if (lightData.intensity <= 0.0f) {
                continue;
            }

            // DEVIATION: ONE directional shadow per layer, where upstream samples
            // every directional caster's map. The lighting block has a single
            // cascade palette and a single directional shadow texture slot, so the
            // first directional caster owns it, and every other directional light
            // must be told it casts nothing: the shaders gate the cascade lookup on
            // the light's own flag, and a light that kept it would be darkened by
            // the OWNER's shadow — a shadowless fill light carrying the key light's
            // shadows, which is what Vulkan did until 2026-09-23.
            if (lightData.castShadows && shadowParams.enabled &&
                lightData.type == GpuLightType::Directional) {
                lightData.castShadows = false;
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    spdlog::warn("Renderer: more than one directional light casts shadows on one layer; "
                                 "only the first is shadowed");
                }
            }
            if (lightData.castShadows && !shadowParams.enabled &&
                lightData.type == GpuLightType::Directional) {
                // Slot 0 marks this light as the owner of the directional shadow;
                // the Vulkan chunk reads it from the same component the local
                // shadows use, so a directional light without it stays unshadowed.
                lightData.shadowMapIndex = 0;
                shadowParams.enabled = true;
                shadowParams.normalBias = lightComponent->shadowNormalBias();
                shadowParams.strength = lightComponent->shadowStrength();

                // Wire actual shadow map and cascade data from scene Light object.
                Light* sceneLight = lightComponent->light();
                if (sceneLight && sceneLight->shadowMap()) {
                    shadowParams.shadowMap = sceneLight->shadowMap()->shadowTexture();

                    // CSM: copy the full matrix palette and cascade distances.
                    shadowParams.numCascades = sceneLight->numCascades();
                    shadowParams.cascadeBlend = sceneLight->cascadeBlend();
                    std::memcpy(shadowParams.shadowMatrixPalette,
                                sceneLight->shadowMatrixPalette().data(),
                                sizeof(shadowParams.shadowMatrixPalette));
                    std::memcpy(shadowParams.shadowCascadeDistances,
                                sceneLight->shadowCascadeDistances().data(),
                                sizeof(shadowParams.shadowCascadeDistances));

                    // Keep single VP matrix for cascade 0 (backward compat).
                    // Cascades are fit for a single designated camera per
                    // frame (see ForwardRenderer::buildFrameGraph) — use its
                    // render data regardless of which camera's pass is being
                    // encoded, so the matrix always matches the atlas.
                    Camera* fitCamera = camera;
                    if (!_cameraDirShadowLights.contains(fitCamera) && !_cameraDirShadowLights.empty()) {
                        fitCamera = _cameraDirShadowLights.begin()->first;
                    }
                    LightRenderData* rd = sceneLight->getRenderData(fitCamera, 0);
                    if (rd && rd->shadowCamera && rd->shadowCamera->node()) {
                        shadowParams.viewProjection = rd->shadowCamera->projectionMatrix()
                            * rd->shadowCamera->node()->worldTransform().inverse();

                        // For PCF: fixed small shader-side depth bias. The real
                        // bias work is done by hardware polygon offset
                        // (depthBias) during the shadow render pass, which is
                        // slope-aware.
                        //
                        // For VSM_16F: use the dedicated vsmBias instead. It
                        // controls the minVariance floor in chebyshevUpperBound,
                        // which determines how aggressively low-variance
                        // (high-noise) samples are clamped to "lit". Too small
                        // → noisy / flicker; too large → soft / detached
                        // contact shadows. Upstream default is 0.0025.
                        shadowParams.vsm = (sceneLight->shadowType() == SHADOW_VSM_16F);
                        shadowParams.bias = shadowParams.vsm
                            ? sceneLight->vsmBias()
                            : 0.0001f;

                        // PCSS: penumbra parameters + per-cascade shadow-camera
                        // extents (the world-space penumbra math needs the ortho
                        // radius and caster depth range of each cascade).
                        shadowParams.pcss = (sceneLight->shadowType() == SHADOW_PCSS_32F);
                        if (shadowParams.pcss) {
                            shadowParams.penumbraSize = sceneLight->penumbraSize();
                            shadowParams.penumbraFalloff = sceneLight->penumbraFalloff();
                            for (int cascade = 0; cascade < shadowParams.numCascades && cascade < 4; ++cascade) {
                                LightRenderData* cascadeData = sceneLight->getRenderData(fitCamera, cascade);
                                if (cascadeData && cascadeData->shadowCamera) {
                                    shadowParams.pcssCascadeRadii[cascade] =
                                        std::max(cascadeData->shadowCamera->orthoHeight(), 1e-4f);
                                    shadowParams.pcssCascadeDepthRanges[cascade] = std::max(
                                        cascadeData->shadowCamera->farClip() - cascadeData->shadowCamera->nearClip(), 1e-4f);
                                }
                            }
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
            if (lightData.castShadows && !clusteredEnabled &&
                lightData.type != GpuLightType::Directional) {
                Light* sceneLight = lightComponent->light();
                if (sceneLight && sceneLight->shadowMap() &&
                    shadowParams.localShadowCount < ShadowParams::kMaxLocalShadows) {

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
                } else {
                    // No shadow slot available — clear castShadows so the shader
                    // doesn't attempt to sample a non-existent shadow map.
                    lightData.castShadows = false;
                }
            }

            // Light cookies (upstream Light.cookie): the projected texture masking
            // the light's color. Spot cookies need a world → cookie-UV matrix —
            // identical to the spot shadow VP, so shadow casters reuse theirs and
            // cookie-only lights evaluate it here. Omni cookies are sampled by
            // direction and carry the light's world transform instead.
            // DEVIATION: two slots per kind (like local shadows), not one per light.
            if (Light* sceneLight = lightComponent->light();
                sceneLight && sceneLight->cookie() &&
                lightData.type != GpuLightType::Directional &&
                lightData.type != GpuLightType::AreaRect) {

                const bool isOmniCookie = (sceneLight->type() == LightType::LIGHTTYPE_OMNI ||
                                           sceneLight->type() == LightType::LIGHTTYPE_POINT);
                const bool shapeMatches = (isOmniCookie == sceneLight->cookie()->isCubemap());
                int& poolCount = isOmniCookie ? cookieCubeCount : cookie2DCount;

                if (shapeMatches && poolCount < kMaxCookies) {
                    lightData.cookie = sceneLight->cookie();
                    lightData.cookieIndex = poolCount++;
                    lightData.cookieIntensity = sceneLight->cookieIntensity();
                    lightData.cookieChannel = static_cast<uint32_t>(sceneLight->cookieChannel());
                    lightData.cookieFalloff = sceneLight->cookieFalloff();
                    lightData.cookieMatrix = isOmniCookie
                        ? lightComponent->entity()->worldTransform()
                        : (lightData.castShadows ? sceneLight->shadowViewProjection()
                                                 : LightCamera::evalSpotCookieMatrix(*sceneLight));
                }
            }

            LightDispatchEntry dispatchEntry{};
            dispatchEntry.light = lightData;
            dispatchEntry.mask = lightComponent->mask();
            dispatchEntry.sceneLight = lightComponent->light();

            if (dispatchEntry.light.type == GpuLightType::Directional) {
                directionalLights.push_back(dispatchEntry);
            } else {
                // Cull the light against THIS camera. Light::visibleThisFrame is a
                // union over cameras and answers a different question — whether the
                // light's shadow map and cookie are worth rendering at all — so it
                // cannot stand in for this test: a light visible only to a reflection
                // camera would otherwise light the main view from off screen.
                //
                // Both the main array and the cluster grid are fed from this list, so
                // one test covers the two of them.
                if (hasCameraFrustum && dispatchEntry.sceneLight) {
                    const BoundingSphere bounds = dispatchEntry.sceneLight->boundingSphere();
                    if (!cameraFrustum.checkSphere(bounds.center(), bounds.radius())) {
                        continue;
                    }
                    dispatchEntry.screenSize = camera ? camera->screenSize(bounds) : 1.0f;
                }
                localLights.push_back(dispatchEntry);
            }
        }

        // The main light array holds eight, and until now the eight were whichever
        // components happened to come first. Rank by apparent size so that when a
        // scene has more visible local lights than slots, the ones covering most of
        // the picture get them — authoring order decides nothing about that.
        // Stable, so an exact tie keeps authoring order and the choice stays
        // reproducible frame to frame.
        std::stable_sort(localLights.begin(), localLights.end(),
            [](const LightDispatchEntry& a, const LightDispatchEntry& b) {
                return a.screenSize > b.screenSize;
            });

        // Environment uniforms are constant across the entire layer (depend only on
        // _scene, not on per-draw state). Hoisted out of the per-draw loop to avoid
        // redundant calls — setEnvironmentUniforms writes to _lightingUniforms fields
        // and sets texture pointers that are the same for every draw in the layer.
        {
            Vector3 skyDomeCenter(0, 0, 0);
            bool isDome = false;
            if (_scene && _scene->sky() && _scene->sky()->type() != SKYTYPE_INFINITE && _scene->sky()->type() != SKYTYPE_ATMOSPHERE) {
                skyDomeCenter = _scene->sky()->centerWorldPos();
                isDome = true;
            }
            _device->setEnvironmentUniforms(_scene ? _scene->envAtlas() : nullptr,
                _scene ? _scene->skyboxIntensity() : 1.0f,
                static_cast<float>(_scene ? _scene->skyboxMip() : 0),
                skyDomeCenter, isDome,
                _scene ? _scene->skybox() : nullptr);

            // A dynamic probe captures the scene into the very cubemap the scene
            // samples for reflections, so during its own face passes the probe
            // texture is simultaneously the render target and a bound sampler —
            // feedback. Skip the binding for those passes: the cube's other faces
            // hold last frame's capture, which is not worth a read-write hazard.
            // Vulkan reports this as a layout conflict (the face is in
            // COLOR_ATTACHMENT_OPTIMAL while the descriptor wants SHADER_READ_ONLY);
            // Metal reads it silently, which is undefined rather than correct.
            if (_scene && _scene->reflectionProbe()) {
                Texture* probe = _scene->reflectionProbe();
                const bool probeIsRenderTarget =
                    activeTarget && activeTarget->colorBuffer() == probe;
                // Unbind rather than skip: the binding is device state that would
                // otherwise persist from the previous pass, which is exactly the
                // texture we must not sample here. Zero intensity also neutralizes
                // the shader term, since VT_FEATURE_REFLECTION_PROBE stays enabled
                // for the frame.
                if (probeIsRenderTarget) {
                    _device->setReflectionProbeUniforms(nullptr,
                        _scene->reflectionProbeBoxMin(), _scene->reflectionProbeBoxMax(),
                        _scene->reflectionProbeBoxProjection(), 0.0f, 0.0f);
                } else {
                    // maxLod = highest mip index (roughness → LOD in the shader).
                    const int levels = std::max(1, static_cast<int>(probe->getNumLevels()));
                    _device->setReflectionProbeUniforms(probe,
                        _scene->reflectionProbeBoxMin(), _scene->reflectionProbeBoxMax(),
                        _scene->reflectionProbeBoxProjection(),
                        _scene->reflectionProbeIntensity(),
                        static_cast<float>(levels - 1));
                }
            }

            // Camera clip planes for SSR depth-grab linearization.
            if (camera) {
                _device->setCameraClipPlanes(camera->nearClip(), camera->farClip());
                _device->setDebugShaderPass(static_cast<uint32_t>(camera->debugShaderPass()));
            }
        }

        // --- Clustered lighting: feed THIS layer's local lights into a grid ---
        // The light list is per (camera, layer) — the gather filters on
        // LightComponent::rendersLayer — so the grid has to be too. This used to
        // build ONE grid from whichever layer rendered first and bind it for every
        // layer after: a layer whose lights differed was lit by another layer's
        // cells, and a layer with no clustered lights at all kept the previous
        // layer's buffers bound and stayed lit by them.
        //
        // Grids are keyed on the light set, so two layers that see the same lights
        // still share one and it is still built once.
        if (clusteredEnabled) {
            // Convert local light dispatch entries to WorldClusters input format.
            static thread_local std::vector<ClusterLightData> clusterLocalLights;
            clusterLocalLights.clear();
            clusterLocalLights.reserve(localLights.size());

            // Identity of the set, order-independent: the dispatch list is sorted by
            // apparent size, which differs per camera, and two layers seeing the same
            // lights must still hash alike.
            static thread_local std::vector<const void*> lightSetMembers;
            lightSetMembers.clear();

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
                lcd.innerConeAngle = std::acos(std::clamp(ld.innerConeCos, -1.0f, 1.0f))
                    * (180.0f / std::numbers::pi_v<float>);
                lcd.outerConeAngle = std::acos(std::clamp(ld.outerConeCos, -1.0f, 1.0f))
                    * (180.0f / std::numbers::pi_v<float>);
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
                    lcd.shadowNormalBias = dispatchEntry.sceneLight->normalBias();
                    lcd.shadowIntensity = dispatchEntry.sceneLight->shadowIntensity();
                }
                clusterLocalLights.push_back(lcd);
                lightSetMembers.push_back(dispatchEntry.sceneLight);
            }

            // Order-independent hash of the member set: sort, then fold. XOR alone
            // would be order-independent too and would collide on any pair repeated.
            std::sort(lightSetMembers.begin(), lightSetMembers.end());
            uint64_t lightSetHash = 1469598103934665603ull;   // FNV-1a offset basis
            for (const void* member : lightSetMembers) {
                uint64_t value = reinterpret_cast<uintptr_t>(member);
                for (int byte = 0; byte < 8; ++byte) {
                    lightSetHash ^= (value & 0xFFull);
                    lightSetHash *= 1099511628211ull;
                    value >>= 8;
                }
            }

            // The grid for this set: built once per frame per distinct set, and
            // sized from the lights alone (it used to be unioned with the camera
            // padded by 50 units, a 100-unit cube however small the lit region was).
            WorldClusters* clusters = clustersForLightSet(lightSetHash, clusterLocalLights);

            // Bind the clustered shadow atlas for this frame.
            if (_lightTextureAtlas) {
                _device->setClusterShadowAtlas(_lightTextureAtlas->shadowAtlasTexture());
            }

            // Bind cluster GPU buffers. EVERY layer binds, because every layer may be
            // on a different grid — the old code bound only on the frame's first
            // layer and left the rest reading whatever was still bound.
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

        // --- Phase 4: pre-compute filtered light list for the common mask ---
        // 95%+ of mesh instances use MASK_AFFECT_DYNAMIC (default). Pre-filter the
        // light list for this mask once, then reuse it across all draws with the
        // same mask. Only re-filter when a draw has a different mask.
        static thread_local std::vector<GpuLightData> cachedGpuLights;
        cachedGpuLights.clear();
        cachedGpuLights.reserve(8);
        uint32_t cachedLightMask = MASK_AFFECT_DYNAMIC;

        auto buildFilteredLights = [&](uint32_t mask, std::vector<GpuLightData>& out) {
            out.clear();
            // Directional lights always go into LightingData.lights[].
            for (const auto& dispatchEntry : directionalLights) {
                if ((dispatchEntry.mask & mask) == 0u) continue;
                out.push_back(dispatchEntry.light);
                if (out.size() >= 8) break;
            }
            // Area rect lights always go into the main 8-light array (not clustered).
            // They must be added before the clustering guard so they're always present.
            for (const auto& dispatchEntry : localLights) {
                if (out.size() >= 8) break;
                if (dispatchEntry.light.type != GpuLightType::AreaRect) continue;
                if ((dispatchEntry.mask & mask) == 0u) continue;
                out.push_back(dispatchEntry.light);
            }
            // Non-area local lights. In non-clustered mode all of them go into the
            // main array. When clustering is enabled, the many *unshadowed* local
            // lights are handled by the cluster grid (fragment shader samples buffer
            // slots 7/8). Under clustered lighting every spot and omni light is in the
            // cluster grid, shadowed from the atlas, so none of them enters the main
            // array; it holds only the directional and area lights then.
            for (const auto& dispatchEntry : localLights) {
                if (out.size() >= 8) break;
                if ((dispatchEntry.mask & mask) == 0u) continue;
                if (dispatchEntry.light.type == GpuLightType::AreaRect) continue;  // already added above
                if (clusteredEnabled) continue;
                out.push_back(dispatchEntry.light);
            }
        };

        // Pre-build for the default mask (covers 95%+ of draws).
        buildFilteredLights(MASK_AFFECT_DYNAMIC, cachedGpuLights);

        // Per-camera tone mapping (upstream CameraComponent::toneMapping) overrides the
        // scene-wide value; TONEMAP_INHERIT keeps the scene's.
        const int sceneToneMapping = _scene ? _scene->toneMapping() : TONEMAP_LINEAR;
        const int cameraToneMapping = camera ? camera->toneMapping() : TONEMAP_INHERIT;
        const int toneMapping = (cameraToneMapping != TONEMAP_INHERIT) ? cameraToneMapping : sceneToneMapping;

        // Phase 4: cull mode cache — skip material parameter map lookups for same material.
        const Material* lastCullMaterial = nullptr;
        CullMode cachedCullMode = CullMode::CULLFACE_BACK;

        // bindMaterial cache: variant resolution (dynamic_cast + string-keyed
        // parameter probes + key hashing) is pure in (material, transparent,
        // dynamicBatch), and the states it sets are sticky on the encoder for
        // the duration of this pass — skip it for consecutive same-material
        // draws, which dominate after sort-key ordering.
        const Material* lastShaderMaterial = nullptr;
        bool lastShaderDynBatch = false;
        bool lastShaderSkinned = false;
        bool lastShaderMorphed = false;
        bool lastShaderInstanced = false;
        bool lastShaderInstanceColor = false;

        for (const auto* entry : drawEntries) {
            const Material* boundMaterial = entry->material ? entry->material : defaultMaterial.get();
            const bool isDynBatch = entry->meshInstance && entry->meshInstance->isDynamicBatch();
            const bool isSkinned = entry->meshInstance && entry->meshInstance->skinInstance() != nullptr;
            const bool isMorphed = entry->meshInstance && entry->meshInstance->morphInstance() != nullptr;

            // Hardware instancing is a property of the draw, not the material: a mesh instance
            // that carries a per-instance buffer gets the instanced vertex stage, and the buffer's
            // stride decides whether that stage also reads a per-instance base color.
            const auto& drawInstancing = entry->meshInstance
                ? entry->meshInstance->instancingData() : MeshInstance::InstancingData{};
            const auto& instanceBuffer = drawInstancing.compactedVertexBuffer
                ? drawInstancing.compactedVertexBuffer : drawInstancing.vertexBuffer;
            const bool isInstanced = instanceBuffer != nullptr && drawInstancing.count > 0;
            const bool hasInstanceColor = isInstanced && instanceBuffer->format() &&
                instanceBuffer->format()->hasInstanceColor();

            if (boundMaterial != lastShaderMaterial || isDynBatch != lastShaderDynBatch ||
                isSkinned != lastShaderSkinned || isMorphed != lastShaderMorphed ||
                isInstanced != lastShaderInstanced || hasInstanceColor != lastShaderInstanceColor) {
                programLibrary->bindMaterial(_device, boundMaterial, transparent, isDynBatch, isSkinned, isMorphed,
                    isInstanced, hasInstanceColor);
                lastShaderMaterial = boundMaterial;
                lastShaderDynBatch = isDynBatch;
                lastShaderSkinned = isSkinned;
                lastShaderMorphed = isMorphed;
                lastShaderInstanced = isInstanced;
                lastShaderInstanceColor = hasInstanceColor;
            }

            // Phase 4: reuse cached light list when mask matches (zero allocation per draw).
            const uint32_t drawLightMask = (entry->meshInstance ? entry->meshInstance->mask() : MASK_AFFECT_DYNAMIC);
            if (drawLightMask != cachedLightMask) {
                buildFilteredLights(drawLightMask, cachedGpuLights);
                cachedLightMask = drawLightMask;
            }

            //controls SHADERDEF_NOSHADOW.
            // When a mesh instance has receiveShadow=false, suppress shadow params for this draw.
            const bool drawReceivesShadow = (!entry->meshInstance || entry->meshInstance->receiveShadow());
            const Vector3* ambientSH = (_scene && _scene->hasAmbientSH())
                ? _scene->ambientSH().data() : nullptr;
            if (drawReceivesShadow) {
                _device->setLightingUniforms(ambientColor, cachedGpuLights, cameraPosition, true,
                    (_scene ? _scene->exposure() : 1.0f), fogParams, shadowParams,
                    toneMapping, ambientSH, &viewProjection);
            } else {
                ShadowParams noShadow;
                noShadow.enabled = false;
                _device->setLightingUniforms(ambientColor, cachedGpuLights, cameraPosition, true,
                    (_scene ? _scene->exposure() : 1.0f), fogParams, noShadow,
                    toneMapping, ambientSH, &viewProjection);
            }

            // Phase 4: cache material's base cull mode (skip parameter map lookups),
            // then apply node-scale flip per draw (trivial float check).
            if (boundMaterial != lastCullMaterial) {
                cachedCullMode = resolveMaterialCullMode(boundMaterial);
                lastCullMaterial = boundMaterial;
            }
            auto cullMode = applyNodeScaleFlip(cachedCullMode,
                entry->meshInstance ? entry->meshInstance->node() : nullptr);
            if (camera && camera->lightmapBakeAccumulate()) {
                // Ambient-occlusion virtual lights: each pass adds its own contribution
                // to the lightmap already in the target (upstream sums the virtual lights
                // the same way), so the draw blends additively and the camera does not
                // clear between passes.
                static const auto additive = std::make_shared<BlendState>(BlendState::additiveBlend());
                _device->setBlendState(additive);
            }
            if (camera && camera->lightmapBakePass()) {
                // UV-space bake: triangle winding follows the unwrap, not the surface —
                // mirrored charts (and the v-flip that puts UV origin at the top) make
                // triangles face either way, so face culling would drop them. Upstream's
                // lightmapper likewise renders the bake double-sided.
                cullMode = CullMode::CULLFACE_NONE;
            }
            _device->setCullMode(cullMode);

            _device->setVertexBuffer(entry->vertexBuffer, 0);

            // Hardware instancing: bind instance buffer at slot 5, pass instanceCount to draw.
            //checks meshInstance.instancingData before draw.
            const auto& instData = drawInstancing;

            if (instData.indirectArgsBuffer && instData.indirectSlot >= 0 && instData.compactedVertexBuffer) {
                // GPU-culled indirect instancing (Phase 3):
                // Bind the compacted buffer (visible instances only) at slot 5.
                // Instance count comes from the GPU via indirect draw arguments.
                _device->setVertexBuffer(instData.compactedVertexBuffer, 5);
                _device->setIndirectDrawBuffer(instData.indirectArgsBuffer);
                // Identity model matrix — each instance carries its own transform via stage_in.
                _device->setTransformUniforms(viewProjection, Matrix4::identity());
                _device->draw(entry->primitive, entry->indexBuffer, 0, instData.indirectSlot, true, true);
            } else if (instData.vertexBuffer && instData.count > 0) {
                // Direct instancing (Phase 2): all instances drawn, CPU-provided count.
                _device->setVertexBuffer(instData.vertexBuffer, 5);

                Matrix4 modelMatrix;
                if (boundMaterial && boundMaterial->isSkybox()) {
                    if (_scene && _scene->sky() && _scene->sky()->type() != SKYTYPE_INFINITE && _scene->sky()->type() != SKYTYPE_ATMOSPHERE) {
                        modelMatrix = entry->meshInstance && entry->meshInstance->node()
                            ? entry->meshInstance->node()->worldTransform()
                            : Matrix4::identity();
                    } else {
                        modelMatrix = Matrix4::translation(cameraPosition);
                    }
                } else {
                    modelMatrix = Matrix4::identity();
                }
                _device->setTransformUniforms(viewProjection, modelMatrix);
                _device->draw(entry->primitive, entry->indexBuffer, instData.count, -1, true, true);
            } else if (isDynBatch) {
                // Dynamic batch draw: bind palette, use identity model matrix.
                //— dynamic batches use SkinBatchInstance
                // with a per-frame matrix palette. The vertex shader indexes into the palette
                // using a per-vertex bone index.
                auto* sbi = entry->meshInstance->skinBatchInstance();
                if (sbi) {
                    _device->setDynamicBatchPalette(sbi->paletteData(), sbi->paletteSizeBytes());
                }
                _device->setTransformUniforms(viewProjection, Matrix4::identity());
                _device->draw(entry->primitive, entry->indexBuffer, 1, -1, true, true);
            } else if (entry->meshInstance && entry->meshInstance->storageDrawCount() > 0) {
                // App-driven storage draw: a custom shader expands one instance per
                // record in an app-owned buffer (typically filled by a compute shader).
                // Same binding slots as the emitter/splat paths, no engine-side simulation.
                const Matrix4 modelMatrix = entry->meshInstance->node()
                    ? entry->meshInstance->node()->worldTransform() : Matrix4::identity();
                const auto& storageParams = entry->meshInstance->storageParams();
                _device->setStorageDrawState(entry->meshInstance->storageBuffer(),
                    storageParams.data(), storageParams.size());
                _device->setTransformUniforms(viewProjection, modelMatrix);
                _device->draw(entry->primitive, entry->indexBuffer,
                    entry->meshInstance->storageDrawCount(), -1, true, true);
            } else if (auto* particles = entry->meshInstance ? entry->meshInstance->particleEmitter() : nullptr) {
                // GPU particles: one instanced billboard quad per particle over the
                // compute-simulated pool (dead particles emit clipped vertices).
                const Matrix4 modelMatrix = entry->meshInstance->node()
                    ? entry->meshInstance->node()->worldTransform() : Matrix4::identity();
                particles->prepareRender(viewMatrix, projMatrix, modelMatrix);
                _device->setParticleState(particles->particleBuffer(),
                    &particles->renderParams(), sizeof(GpuParticleRenderParams));
                _device->setTransformUniforms(viewProjection, modelMatrix);
                _device->draw(entry->primitive, entry->indexBuffer,
                    static_cast<int>(particles->numParticles()), -1, true, true);
            } else if (auto* gsplat = entry->meshInstance ? entry->meshInstance->gsplatInstance() : nullptr) {
                // Gaussian splats: one instanced quad per splat, back-to-front via the
                // per-instance order buffer filled by the background depth sorter.
                const Matrix4 modelMatrix = entry->meshInstance->node()
                    ? entry->meshInstance->node()->worldTransform() : Matrix4::identity();
                gsplat->update(cameraPosition, cameraForward, modelMatrix, viewMatrix, projMatrix,
                    static_cast<float>(viewportW), static_cast<float>(viewportH));
                if (gsplat->visibleCount() > 0) {
                    _device->setGSplatState(gsplat->resource()->splatBuffer(), gsplat->orderBuffer(),
                        gsplat->resource()->shBuffer(), &gsplat->gpuParams(), sizeof(GpuGSplatParams));
                    _device->setTransformUniforms(viewProjection, modelMatrix);
                    _device->draw(entry->primitive, entry->indexBuffer,
                        static_cast<int>(gsplat->visibleCount()), -1, true, true);
                }
            } else {
                // Non-instanced draw.
                Matrix4 modelMatrix;
                if (boundMaterial && boundMaterial->isSkybox()) {
                    if (_scene && _scene->sky() && _scene->sky()->type() != SKYTYPE_INFINITE && _scene->sky()->type() != SKYTYPE_ATMOSPHERE) {
                        modelMatrix = entry->meshInstance && entry->meshInstance->node()
                            ? entry->meshInstance->node()->worldTransform()
                            : Matrix4::identity();
                    } else {
                        modelMatrix = Matrix4::translation(cameraPosition);
                    }
                } else {
                    modelMatrix = (entry->meshInstance && entry->meshInstance->node())
                        ? entry->meshInstance->node()->worldTransform()
                        : Matrix4::identity();
                }

                // GPU skinning: refresh the bone palette (no-op if already updated this
                // frame) and bind it at slot 6. The palette is node-relative; the model
                // matrix stays the node's world transform (they cancel in the shader).
                if (isSkinned) {
                    auto* si = entry->meshInstance->skinInstance();
                    si->updateMatrixPalette(entry->meshInstance->node());
                    _device->setDynamicBatchPalette(si->paletteData(), si->paletteSizeBytes());
                    _skinDrawCalls++;
                }
                // Morph targets: bind the shared delta buffer + current weights.
                if (isMorphed) {
                    auto* mi = entry->meshInstance->morphInstance();
                    if (mi->morph() && mi->morph()->deltaBuffer()) {
                        const auto& params = mi->gpuParams();
                        _device->setMorphState(mi->morph()->deltaBuffer(), &params, sizeof(params));
                    }
                }

                _device->setTransformUniforms(viewProjection, modelMatrix);
                _device->draw(entry->primitive, entry->indexBuffer, 1, -1, true, true);
            }
            _forwardDrawCalls++;
        }

        // Restore global viewport/scissor after this camera-layer pass.
        _device->setViewport(oldVx, oldVy, oldVw, oldVh);
        _device->setScissor(oldSx, oldSy, oldSw, oldSh);

    }
}
