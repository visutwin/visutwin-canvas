// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#include "renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

#include "core/objectPool.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/light/lightComponent.h"
#include "platform/graphics/instanceCuller.h"
#include "scene/frustumUtils.h"
#include "scene/light.h"
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
            float distanceToCameraSq = 0.0f;
        };

        struct LightDispatchEntry
        {
            GpuLightData light;
            uint32_t mask = MASK_AFFECT_DYNAMIC;
        };

        uint64_t makeOpaqueSortKey(const MeshInstance* meshInstance)
        {
            //uses material / shader variants in opaque sort keys.
            const auto material = meshInstance ? meshInstance->material() : nullptr;
            const auto materialKey = material ? material->sortKey()
                : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(material)) >> 4;
            const auto meshKey = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(meshInstance ? meshInstance->mesh() : nullptr)) >> 4;
            return (materialKey << 32) ^ (meshKey & 0xffffffffu);
        }

        // Material's base cull mode — reads the parameter map (unordered_map lookup).
        // This is the expensive part that should be cached per material.
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
    }

    Renderer::Renderer(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Scene>& scene) : _device(device), _scene(scene)
    {
        // DEVIATION: startup self-test guards recent attenuation/falloff regressions in this port.
        runLightingValidationSelfTest();

        _lightTextureAtlas = std::make_unique<LightTextureAtlas>(device);

        _shadowRenderer = std::make_unique<ShadowRenderer>(this, _lightTextureAtlas.get());

        _shadowRendererLocal = std::make_unique<ShadowRendererLocal>(this, _shadowRenderer.get());
        _shadowRendererDirectional = std::make_unique<ShadowRendererDirectional>(device, this, _shadowRenderer.get());

        if (scene->clusteredLightingEnabled())
        {
            _renderPassUpdateClustered = std::make_unique<RenderPassUpdateClustered>(
                device, this, _shadowRenderer.get(), _shadowRendererLocal.get(), _lightTextureAtlas.get()
            );
        }

    }

    void Renderer::cullShadowmaps(Camera* camera)
    {
        _cameraDirShadowLights.clear();

        if (!camera || !_shadowRendererDirectional) {
            return;
        }

        std::vector<Light*> dirShadowLights;

        for (auto* lightComponent : LightComponent::instances()) {
            if (!lightComponent || !lightComponent->enabled()) {
                continue;
            }
            if (lightComponent->type() != LightType::LIGHTTYPE_DIRECTIONAL || !lightComponent->castShadows()) {
                continue;
            }

            Light* sceneLight = lightComponent->light();
            if (!sceneLight) {
                continue;
            }

            // Allocate shadow map if not yet created.
            if (!sceneLight->shadowMap()) {
                auto shadowMap = ShadowMap::create(_device.get(), sceneLight);
                if (shadowMap) {
                    // Store the owned ShadowMap on the Renderer (keep alive), set raw pointer on Light.
                    sceneLight->setShadowMap(shadowMap.get());
                    _ownedShadowMaps.push_back(std::move(shadowMap));
                }
            }

            if (!sceneLight->shadowMap()) {
                continue;
            }

            // Set up the shadow camera (position, projection, snap).
            _shadowRendererDirectional->cull(sceneLight, camera);

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

                culler->cull(srcData.vertexBuffer.get(), params);
            }
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

        // Tell ProgramLibrary whether any local light casts shadows AND has
        // an allocated shadow map. Only enable VT_FEATURE_LOCAL_SHADOWS / VT_FEATURE_OMNI_SHADOWS
        // when actual shadow textures exist, so the shader doesn't compile with
        // depth2d / depthcube parameters that would be nil at runtime.
        {
            bool hasLocalShadows = false;
            bool hasOmniShadows = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->enabled() && lc->castShadows() &&
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

            // Directional EVSM_16F: enable when any shadow-casting directional
            // light is configured for SHADOW_VSM_16F. Both the shadow program
            // (writes moments) and the forward program (samples via Chebyshev)
            // need the matching VT_FEATURE_VSM_SHADOWS variant.
            bool hasVsmShadows = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->enabled() && lc->castShadows() &&
                    lc->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                    Light* sceneLight = lc->light();
                    if (sceneLight && sceneLight->shadowType() == SHADOW_VSM_16F) {
                        hasVsmShadows = true;
                        break;
                    }
                }
            }
            programLibrary->setVsmShadowsEnabled(hasVsmShadows);
        }

        // Area lights: enable when any LightComponent has LIGHTTYPE_AREA_RECT.
        {
            bool hasAreaLights = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->enabled() && lc->type() == LightType::LIGHTTYPE_AREA_RECT) {
                    hasAreaLights = true;
                    break;
                }
            }
            programLibrary->setAreaLightsEnabled(hasAreaLights);
        }

        // Clustered lighting: when enabled on the scene, compile forward shaders
        // with VT_FEATURE_LIGHT_CLUSTERING so the fragment shader samples the cluster grid.
        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();
        programLibrary->setClusteredLightingEnabled(clusteredEnabled);

        // SSAO per-material: when the device has a forward SSAO texture, compile
        // forward shaders with VT_FEATURE_SSAO so the fragment shader modulates
        // ambient occlusion by sampling the SSAO texture at screen-space UV.
        programLibrary->setSsaoEnabled(_device->ssaoForwardTexture() != nullptr);

        // Atmosphere scattering: when enabled on the scene, compile skybox shaders
        // with VT_FEATURE_ATMOSPHERE and push atmosphere uniforms to the device.
        const bool atmosphereEnabled = _scene && _scene->atmosphereEnabled();
        programLibrary->setAtmosphereEnabled(atmosphereEnabled);
        _device->setAtmosphereEnabled(atmosphereEnabled);
        if (atmosphereEnabled) {
            _device->setAtmosphereUniforms(_scene->atmosphereUniformData(), _scene->atmosphereUniformSize());
        }

        // Lazily create WorldClusters when clustering is first enabled.
        if (clusteredEnabled && !_worldClusters) {
            _worldClusters = std::make_unique<WorldClusters>();
        }

        const auto defaultMaterial = getDefaultMaterial(_device);

        auto* cameraNode = camera->node().get();
        const auto cameraPosition = cameraNode ? cameraNode->position() : Vector3{};
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
            projMatrix.setElement(2, 0, jitterX);
            projMatrix.setElement(2, 1, jitterY);
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
        std::vector<ForwardDrawEntry*> drawEntries;
        drawEntries.reserve(256);

        const auto appendMeshInstance = [&](MeshInstance* meshInstance) {
            if (!meshInstance || !meshInstance->visible()) {
                return;
            }

            auto* mesh = meshInstance->mesh();
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
            if (entry->material->transparent() != transparent) {
                return;
            }

            const bool isSkyboxMaterial = entry->material->isSkybox();
            const auto worldBounds = meshInstance->aabb();
            if (!isSkyboxMaterial && !isVisibleInCameraFrustum(camera, cameraNode, worldBounds)) {
                _numDrawCallsCulled++;
                return;
            }

            entry->vertexBuffer = vertexBuffer;
            entry->indexBuffer = mesh->getIndexBuffer();
            entry->primitive = mesh->getPrimitive();
            entry->sortKey = makeOpaqueSortKey(meshInstance);

            auto* node = meshInstance->node();
            if (node && !isSkyboxMaterial) {
                const auto delta = worldBounds.center() - cameraPosition;
                entry->distanceToCameraSq = delta.lengthSquared();
            } else {
                entry->distanceToCameraSq = 0.0f;
            }

            drawEntries.push_back(entry);
        };

        for (auto* renderComponent : RenderComponent::instances()) {
            if (!renderComponent || !renderComponent->enabled()) {
                continue;
            }

            // Also check that the owning entity (and its entire hierarchy) is enabled.
            // Component::enabled() only returns the component's own flag — it does not
            // reflect the parent entity's setEnabled(false) state.
            if (renderComponent->entity() && !renderComponent->entity()->enabled()) {
                continue;
            }

            const auto& componentLayers = renderComponent->layers();
            if (std::find(componentLayers.begin(), componentLayers.end(), layer->id()) == componentLayers.end()) {
                continue;
            }

            for (auto* meshInstance : renderComponent->meshInstances()) {
                appendMeshInstance(meshInstance);
            }
        }

        for (auto* meshInstance : layer->meshInstances()) {
            appendMeshInstance(meshInstance);
        }

        if (transparent) {
            // transparent sublayer is sorted back-to-front.
            std::stable_sort(drawEntries.begin(), drawEntries.end(),
                [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                    if (a->distanceToCameraSq == b->distanceToCameraSq) {
                        return a->sortKey < b->sortKey;
                    }
                    return a->distanceToCameraSq > b->distanceToCameraSq;
                });
        } else {
            // opaque sublayer prioritizes material/mesh sort, then front-to-back.
            std::stable_sort(drawEntries.begin(), drawEntries.end(),
                [](const ForwardDrawEntry* a, const ForwardDrawEntry* b) {
                    if (a->sortKey != b->sortKey) {
                        return a->sortKey < b->sortKey;
                    }
                    return a->distanceToCameraSq < b->distanceToCameraSq;
                });
        }

        const auto sortEnd = std::chrono::high_resolution_clock::now();
        _sortTime += static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(sortEnd - sortStart).count());

        // Intentional temporary deviation from JS:
        // this path now binds core material uniforms/textures (including Material::setParameter overrides)
        // and forward shader variants, while full parameter scope is still being ported.
        const auto ambientColor = _scene ? _scene->ambientLight() : Color(0.0f, 0.0f, 0.0f, 1.0f);
        const auto fogParams = _scene ? _scene->fog() : FogParams{};
        std::vector<LightDispatchEntry> directionalLights;
        std::vector<LightDispatchEntry> localLights;
        directionalLights.reserve(4);
        localLights.reserve(8);
        ShadowParams shadowParams{};

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
                    {
                        // Right vector from entity's world transform X axis.
                        const auto& wt = lightComponent->entity()->worldTransform();
                        Vector3 right(wt.getElement(0, 0), wt.getElement(1, 0), wt.getElement(2, 0));
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
            lightData.innerConeCos = std::cos(toRadians(std::max(lightComponent->innerConeAngle(), 0.0f) * 0.5f));
            lightData.outerConeCos = std::cos(toRadians(std::max(lightComponent->outerConeAngle(), 0.0f) * 0.5f));
            if (lightData.innerConeCos < lightData.outerConeCos) {
                lightData.innerConeCos = lightData.outerConeCos;
            }
            lightData.falloffModeLinear = lightComponent->falloffMode() == LightFalloff::LIGHTFALLOFF_LINEAR;
            lightData.castShadows = lightComponent->castShadows();
        };

        for (const auto* lightComponent : LightComponent::instances()) {
            if (!lightComponent || !lightComponent->enabled()) {
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

            if (lightData.castShadows && !shadowParams.enabled &&
                lightData.type == GpuLightType::Directional) {
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
                    LightRenderData* rd = sceneLight->getRenderData(camera, 0);
                    if (rd && rd->shadowCamera && rd->shadowCamera->node()) {
                        shadowParams.viewProjection = rd->shadowCamera->projectionMatrix()
                            * rd->shadowCamera->node()->worldTransform().inverse();

                        // fixed small shader-side depth bias for directional
                        // shadow sampling. The real bias work is done by hardware polygon offset
                        // (depthBias) during the shadow render pass, which is slope-aware.
                        //"saturate(z) - 0.0001".
                        shadowParams.bias = 0.0001f;
                    }
                }
            }

            // Wire local light shadow data (spot/point).
            // Assign shadow map index and populate ShadowParams.localShadows.
            // Omni lights use cubemap depth textures; spot lights use 2D textures.
            if (lightData.castShadows && lightData.type != GpuLightType::Directional) {
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
                    ls.bias = sceneLight->shadowBias();
                    ls.normalBias = sceneLight->normalBias();
                    ls.intensity = sceneLight->shadowIntensity();

                    shadowParams.localShadowCount++;
                } else {
                    // No shadow slot available — clear castShadows so the shader
                    // doesn't attempt to sample a non-existent shadow map.
                    lightData.castShadows = false;
                }
            }

            LightDispatchEntry dispatchEntry{};
            dispatchEntry.light = lightData;
            dispatchEntry.mask = lightComponent->mask();

            if (dispatchEntry.light.type == GpuLightType::Directional) {
                directionalLights.push_back(dispatchEntry);
            } else {
                localLights.push_back(dispatchEntry);
            }
        }

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
        }

        // --- Clustered lighting: feed local lights into WorldClusters ---
        if (clusteredEnabled && _worldClusters) {
            // Convert local light dispatch entries to WorldClusters input format.
            std::vector<ClusterLightData> clusterLocalLights;
            clusterLocalLights.reserve(localLights.size());

            for (const auto& dispatchEntry : localLights) {
                const auto& ld = dispatchEntry.light;
                // Area rect lights are not clustered — they go through the main 8-light array.
                if (ld.type == GpuLightType::AreaRect) continue;
                ClusterLightData lcd;
                lcd.position = ld.position;
                lcd.direction = ld.direction;
                lcd.color = ld.color;
                lcd.intensity = ld.intensity;
                lcd.range = ld.range;
                lcd.innerConeAngle = std::acos(std::clamp(ld.innerConeCos, -1.0f, 1.0f))
                    * (360.0f / std::numbers::pi_v<float>);  // radians half-angle → degrees full-angle
                lcd.outerConeAngle = std::acos(std::clamp(ld.outerConeCos, -1.0f, 1.0f))
                    * (360.0f / std::numbers::pi_v<float>);
                lcd.isSpot = (ld.type == GpuLightType::Spot);
                lcd.falloffModeLinear = ld.falloffModeLinear;
                clusterLocalLights.push_back(lcd);
            }

            // Compute camera frustum AABB for cluster grid bounds.
            // Use camera position ± reasonable range as a simple approximation.
            // A full frustum AABB would require unprojecting corners, but camera
            // position ± max light range is sufficient for the grid to cover all lights.
            BoundingBox cameraBounds(cameraPosition, Vector3(50.0f, 50.0f, 50.0f));

            _worldClusters->update(clusterLocalLights, cameraBounds);

            // Bind cluster GPU buffers.
            if (_worldClusters->lightCount() > 0) {
                _device->setClusterBuffers(
                    _worldClusters->lightData(), _worldClusters->lightDataSize(),
                    _worldClusters->cellData(), _worldClusters->cellDataSize());

                // Pack cluster grid params into LightingUniforms.
                const auto& bMin = _worldClusters->boundsMin();
                const auto bRange = _worldClusters->boundsRange();
                const auto cellsBySize = _worldClusters->cellsCountByBoundsSize();
                const auto& cfg = _worldClusters->config();

                const float boundsMinArr[3] = {bMin.getX(), bMin.getY(), bMin.getZ()};
                const float boundsRangeArr[3] = {bRange.getX(), bRange.getY(), bRange.getZ()};
                const float cellsBySizeArr[3] = {cellsBySize.getX(), cellsBySize.getY(), cellsBySize.getZ()};

                _device->setClusterGridParams(boundsMinArr, boundsRangeArr, cellsBySizeArr,
                    cfg.cellsX, cfg.cellsY, cfg.cellsZ, cfg.maxLightsPerCell,
                    _worldClusters->lightCount());
            }
        }

        // --- Phase 4: pre-compute filtered light list for the common mask ---
        // 95%+ of mesh instances use MASK_AFFECT_DYNAMIC (default). Pre-filter the
        // light list for this mask once, then reuse it across all draws with the
        // same mask. Only re-filter when a draw has a different mask.
        std::vector<GpuLightData> cachedGpuLights;
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
            // When clustering is enabled, non-area local lights are handled by the
            // cluster grid (fragment shader samples buffer slots 7/8).
            if (!clusteredEnabled) {
                for (const auto& dispatchEntry : localLights) {
                    if (out.size() >= 8) break;
                    if ((dispatchEntry.mask & mask) == 0u) continue;
                    if (dispatchEntry.light.type == GpuLightType::AreaRect) continue;  // already added above
                    out.push_back(dispatchEntry.light);
                }
            }
        };

        // Pre-build for the default mask (covers 95%+ of draws).
        buildFilteredLights(MASK_AFFECT_DYNAMIC, cachedGpuLights);

        // Phase 4: cull mode cache — skip material parameter map lookups for same material.
        const Material* lastCullMaterial = nullptr;
        CullMode cachedCullMode = CullMode::CULLFACE_BACK;

        for (const auto* entry : drawEntries) {
            const Material* boundMaterial = entry->material ? entry->material : defaultMaterial.get();
            const bool isDynBatch = entry->meshInstance && entry->meshInstance->isDynamicBatch();
            programLibrary->bindMaterial(_device, boundMaterial, transparent, isDynBatch);

            // Phase 4: reuse cached light list when mask matches (zero allocation per draw).
            const uint32_t drawLightMask = (entry->meshInstance ? entry->meshInstance->mask() : MASK_AFFECT_DYNAMIC);
            if (drawLightMask != cachedLightMask) {
                buildFilteredLights(drawLightMask, cachedGpuLights);
                cachedLightMask = drawLightMask;
            }

            //controls SHADERDEF_NOSHADOW.
            // When a mesh instance has receiveShadow=false, suppress shadow params for this draw.
            const bool drawReceivesShadow = (!entry->meshInstance || entry->meshInstance->receiveShadow());
            if (drawReceivesShadow) {
                _device->setLightingUniforms(ambientColor, cachedGpuLights, cameraPosition, true,
                    (_scene ? _scene->exposure() : 1.0f), fogParams, shadowParams,
                    (_scene ? _scene->toneMapping() : 0));
            } else {
                ShadowParams noShadow;
                noShadow.enabled = false;
                _device->setLightingUniforms(ambientColor, cachedGpuLights, cameraPosition, true,
                    (_scene ? _scene->exposure() : 1.0f), fogParams, noShadow,
                    (_scene ? _scene->toneMapping() : 0));
            }

            // Phase 4: cache material's base cull mode (skip parameter map lookups),
            // then apply node-scale flip per draw (trivial float check).
            if (boundMaterial != lastCullMaterial) {
                cachedCullMode = resolveMaterialCullMode(boundMaterial);
                lastCullMaterial = boundMaterial;
            }
            const auto cullMode = applyNodeScaleFlip(cachedCullMode,
                entry->meshInstance ? entry->meshInstance->node() : nullptr);
            _device->setCullMode(cullMode);

            _device->setVertexBuffer(entry->vertexBuffer, 0);

            // Hardware instancing: bind instance buffer at slot 5, pass instanceCount to draw.
            //checks meshInstance.instancingData before draw.
            const auto& instData = entry->meshInstance ? entry->meshInstance->instancingData() : MeshInstance::InstancingData{};

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
                        modelMatrix = Matrix4::translation(cameraPosition.getX(), cameraPosition.getY(), cameraPosition.getZ());
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
            } else {
                // Non-instanced draw.
                Matrix4 modelMatrix;
                if (boundMaterial && boundMaterial->isSkybox()) {
                    if (_scene && _scene->sky() && _scene->sky()->type() != SKYTYPE_INFINITE && _scene->sky()->type() != SKYTYPE_ATMOSPHERE) {
                        modelMatrix = entry->meshInstance && entry->meshInstance->node()
                            ? entry->meshInstance->node()->worldTransform()
                            : Matrix4::identity();
                    } else {
                        modelMatrix = Matrix4::translation(cameraPosition.getX(), cameraPosition.getY(), cameraPosition.getZ());
                    }
                } else {
                    modelMatrix = (entry->meshInstance && entry->meshInstance->node())
                        ? entry->meshInstance->node()->worldTransform()
                        : Matrix4::identity();
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
