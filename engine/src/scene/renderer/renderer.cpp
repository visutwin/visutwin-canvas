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
            float distanceToCameraSq = 0.0f;
        };

        struct LightDispatchEntry
        {
            GpuLightData light;
            uint32_t mask = MASK_AFFECT_DYNAMIC;
            Light* sceneLight = nullptr;  // for clustered spot-shadow atlas lookup
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

        // Always construct the clustered update pass so clustered lighting can be
        // toggled on the scene at any time (it is only added to the frame graph by
        // ForwardRenderer when clusteredLightingEnabled() is true). Constructing it
        // unconditionally avoids a null deref when clustering is enabled after init.
        _renderPassUpdateClustered = std::make_unique<RenderPassUpdateClustered>(
            device, this, _shadowRenderer.get(), _shadowRendererLocal.get(), _lightTextureAtlas.get()
        );

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

            // Allocate shadow map if not yet created; the light owns it.
            if (!sceneLight->shadowMap()) {
                sceneLight->setShadowMap(ShadowMap::create(_device.get(), sceneLight));
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

            // Light cookies: separate variants for the 2D (spot) and cubemap (omni)
            // samplers, enabled only when a light actually carries a cookie of that
            // kind — same reason as the shadow features, an unbound texture
            // parameter would be nil at draw time.
            bool hasCookie2D = false;
            bool hasCookieCube = false;
            for (const auto* lc : LightComponent::instances()) {
                if (!lc || !lc->enabled() || !lc->cookie()) {
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

            // Directional PCSS: contact-hardening soft shadows. The shadow map
            // stays the standard depth texture (only the sampling differs).
            bool hasPcssShadows = false;
            for (const auto* lc : LightComponent::instances()) {
                if (lc && lc->enabled() && lc->castShadows() &&
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
                if (lc && lc->enabled() && lc->type() == LightType::LIGHTTYPE_AREA_RECT) {
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

        // Lazily create WorldClusters when clustering is first enabled, with the
        // grid the scene asked for.
        if (clusteredEnabled && !_worldClusters) {
            const auto& lightingParams = _scene->lighting();
            ClusterConfig config;
            config.cellsX = std::max(1, lightingParams.cellsX);
            config.cellsY = std::max(1, lightingParams.cellsY);
            config.cellsZ = std::max(1, lightingParams.cellsZ);
            config.maxLightsPerCell = std::max(1, lightingParams.maxLightsPerCell);
            _worldClusters = std::make_unique<WorldClusters>(config);
            if (_lightTextureAtlas) {
                _lightTextureAtlas->configure(lightingParams.shadowAtlasResolution,
                    lightingParams.shadowAtlasCapacity);
            }
        }

        const auto defaultMaterial = getDefaultMaterial(_device);

        auto* cameraNode = camera->node();
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

        // Build the camera frustum ONCE for this layer — the per-instance test
        // used to rebuild it (matrix inverse + plane extraction) on every call.
        const bool hasCameraFrustum = camera && cameraNode;
        const Frustum cameraFrustum = hasCameraFrustum
            ? buildCameraFrustum(camera, cameraNode) : Frustum{};

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
            // Respect the per-instance cull flag: skinned instances disable frustum
            // culling because the bind-pose AABB is invalid under animation.
            if (!isSkyboxMaterial && hasCameraFrustum && meshInstance->cull() &&
                !isVisibleInFrustum(cameraFrustum, worldBounds)) {
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

        // --- Clustered lighting: feed local lights into WorldClusters ---
        // This runs once per frame per camera position, not per layer/sublayer:
        // renderForwardLayer is called for every layer × sublayer × camera, but
        // the cluster inputs (light set, camera-centered bounds) only change
        // per frame/camera. The flag resets in buildFrameGraph.
        const bool clusterCameraMoved =
            cameraPosition.getX() != _lastClusterCameraPosition.getX() ||
            cameraPosition.getY() != _lastClusterCameraPosition.getY() ||
            cameraPosition.getZ() != _lastClusterCameraPosition.getZ();
        if (clusteredEnabled && _worldClusters &&
            (!_clustersUpdatedThisFrame || clusterCameraMoved)) {
            _clustersUpdatedThisFrame = true;
            _lastClusterCameraPosition = cameraPosition;

            // Convert local light dispatch entries to WorldClusters input format.
            static thread_local std::vector<ClusterLightData> clusterLocalLights;
            clusterLocalLights.clear();
            clusterLocalLights.reserve(localLights.size());

            for (const auto& dispatchEntry : localLights) {
                const auto& ld = dispatchEntry.light;
                // Area rect lights are not clustered — they go through the main 8-light array.
                if (ld.type == GpuLightType::AreaRect) continue;
                // Shadow-casting OMNI/POINT lights are evaluated via the main light array
                // (bounded). Shadow-casting SPOTS stay in the grid and get a real shadow
                // from the LightTextureAtlas (below). Unshadowed lights: grid, no shadow.
                if (ld.castShadows && ld.type != GpuLightType::Spot) continue;
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

                // Clustered spot shadow: pull the atlas slice + VP matrix computed by
                // cullLocalLights + LightTextureAtlas::allocate this frame.
                if (ld.castShadows && ld.type == GpuLightType::Spot && dispatchEntry.sceneLight &&
                    dispatchEntry.sceneLight->atlasSlice() >= 0) {
                    lcd.castShadows = true;
                    lcd.shadowMatrix = dispatchEntry.sceneLight->shadowViewProjection();
                    lcd.atlasSlice = dispatchEntry.sceneLight->atlasSlice();
                    // Same convention as the non-clustered local path above.
                    lcd.shadowBias = -dispatchEntry.sceneLight->shadowBias() * 20.0f;
                    lcd.shadowIntensity = dispatchEntry.sceneLight->shadowIntensity();
                }
                clusterLocalLights.push_back(lcd);
            }

            // Compute camera frustum AABB for cluster grid bounds.
            // Use camera position ± reasonable range as a simple approximation.
            // A full frustum AABB would require unprojecting corners, but camera
            // position ± max light range is sufficient for the grid to cover all lights.
            BoundingBox cameraBounds(cameraPosition, Vector3(50.0f, 50.0f, 50.0f));

            _worldClusters->update(clusterLocalLights, cameraBounds);

            // Bind the clustered spot-shadow atlas (depth array) for this frame.
            if (_lightTextureAtlas) {
                _device->setClusterShadowAtlas(_lightTextureAtlas->shadowArrayTexture());
            }

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
            } else {
                // No clustered lights this frame: zero the grid params so the
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
            // slots 7/8) — but *shadow-casting* local lights still go into the main
            // array so they sample their shadow maps (clustered atlas shadows are
            // not yet ported). They are correspondingly excluded from the grid feed.
            for (const auto& dispatchEntry : localLights) {
                if (out.size() >= 8) break;
                if ((dispatchEntry.mask & mask) == 0u) continue;
                if (dispatchEntry.light.type == GpuLightType::AreaRect) continue;  // already added above
                // Clustered mode: only shadow-casting OMNI/POINT lights use the main array
                // (bounded). Unshadowed lights and shadow-casting SPOTS go to the cluster
                // grid (spots get a real shadow from the LightTextureAtlas).
                if (clusteredEnabled &&
                    !(dispatchEntry.light.castShadows && dispatchEntry.light.type != GpuLightType::Spot)) {
                    continue;
                }
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
                Vector3 cameraForward(0.0f, 0.0f, -1.0f);
                if (cameraNode) {
                    const Matrix4& cameraWorld = cameraNode->worldTransform();
                    cameraForward = (cameraWorld.transformPoint(Vector3(0.0f, 0.0f, -1.0f)) -
                                     cameraWorld.transformPoint(Vector3(0.0f))).normalized();
                }
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
                        modelMatrix = Matrix4::translation(cameraPosition.getX(), cameraPosition.getY(), cameraPosition.getZ());
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
