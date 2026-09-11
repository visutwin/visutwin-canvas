// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassPrepass.h"

#include "framework/batching/batchManager.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"
#include "scene/frustumUtils.h"
#include "scene/graphNode.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/renderer/depthOnlyDraw.h"
#include "scene/renderer/shadowCasterFiltering.h"
#include "scene/shader-lib/programLibrary.h"

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
        /**
         * Whether this mesh instance contributes to the depth the prepass produces.
         * Upstream's test is `material.depthWrite`; the equivalent here is a material
         * that is not blended, since a transparent surface writes colour over what is
         * behind it without replacing its depth. An alpha-tested (MASK) material DOES
         * write depth, and its cut-out holes come from the opacity frontend the shadow
         * shader runs — which is why the draw binds such a material.
         *
         * Deliberately NOT the shadow-caster test: a mesh with castShadow off still
         * occludes in screen space, and one that casts a shadow while being blended
         * (the dithered-shadow case) must not write depth here.
         */
        bool writesPrepassDepth(MeshInstance* meshInstance)
        {
            if (!meshInstance || !meshInstance->visible() || !meshInstance->mesh()) {
                return false;
            }
            if (meshInstance->node() && !meshInstance->node()->enabled()) {
                return false;
            }
            if (!meshInstance->mesh()->getVertexBuffer()) {
                return false;
            }
            const Material* material = meshInstance->material();
            return material && !material->transparent();
        }
    }

    RenderPassPrepass::RenderPassPrepass(const std::shared_ptr<GraphicsDevice>& device, Scene* scene, Renderer* renderer,
        CameraComponent* cameraComponent, Texture* sceneDepthTexture, const std::shared_ptr<RenderPassOptions>& options)
        : RenderPass(device), _scene(scene), _renderer(renderer), _cameraComponent(cameraComponent),
          _sceneDepthTexture(sceneDepthTexture)
    {
        _name = "RenderPassPrepass";
        _requiresCubemaps = false;
        (void)_scene;
        (void)_renderer;
        setOptions(options);
    }

    void RenderPassPrepass::execute()
    {
        const auto gd = device();
        if (!gd || !_cameraComponent) {
            return;
        }
        auto* camera = _cameraComponent->camera();
        if (!camera || !camera->node()) {
            return;
        }

        const auto programLibrary = getProgramLibrary(gd);
        if (!programLibrary) {
            return;
        }

        // The prepass writes the same depth the scene pass is about to write, from the
        // same camera, using the depth-only (shadow) programs — this port has no
        // separate SHADER_PREPASS. The scene pass then clears and re-renders that
        // depth for real; what the prepass exists for is the window BETWEEN them, in
        // which lighting-mode SSAO has a populated depth buffer to read. Without it
        // that pass has to run after the scene, and the forward shaders sample an
        // occlusion texture that describes the PREVIOUS frame.
        DepthOnlyShaders shaders;
        shaders.plain = programLibrary->getShadowShader(nullptr, false);
        shaders.dynamicBatch = programLibrary->getShadowShader(nullptr, true);
        if (!shaders.plain) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("No depth-only shader for this device — the scene depth "
                    "prepass is disabled, and lighting-mode SSAO with it");
            }
            return;
        }

        // This pass bypasses materials for the common caster, so it owns the state the
        // forward pass would otherwise set per material. No depth bias: unlike a shadow
        // map this depth is read from the same viewpoint that wrote it.
        gd->setMaterial(nullptr);
        gd->setShader(shaders.plain);
        static auto prepassBlendState = std::make_shared<BlendState>();
        static auto prepassDepthState = std::make_shared<DepthState>();
        gd->setBlendState(prepassBlendState);
        gd->setDepthState(prepassDepthState);
        gd->setDepthBias(0.0f, 0.0f, 0.0f);

        const Matrix4 viewProjection = camera->projectionMatrix() * camera->node()->worldTransform().inverse();
        const Frustum frustum = buildCameraFrustum(camera, camera->node());

        // Collected with the CAMERA, unlike collectShadowCasters, which sweeps the
        // whole scene: depth this camera will not draw must not be in the buffer its
        // own SSAO reads. Batched mesh instances belong to no RenderComponent and are
        // added separately, exactly as the shadow passes do.
        std::vector<MeshInstance*> meshInstances;
        for (auto* renderComponent : RenderComponent::instances()) {
            if (!shouldRenderShadowRenderComponent(renderComponent, camera)) {
                continue;
            }
            for (auto* meshInstance : renderComponent->meshInstances()) {
                meshInstances.push_back(meshInstance);
            }
        }
        for (auto* meshInstance : BatchManager::batchMeshInstances()) {
            meshInstances.push_back(meshInstance);
        }

        for (auto* meshInstance : meshInstances) {
            if (!writesPrepassDepth(meshInstance)) {
                continue;
            }
            if (meshInstance->cull() && !isVisibleInFrustum(frustum, meshInstance->aabb())) {
                continue;
            }
            drawDepthOnly(gd.get(), programLibrary.get(), meshInstance, viewProjection, shaders);
        }
    }

    void RenderPassPrepass::after()
    {
        RenderPass::after();
        if (const auto gd = device(); gd && _sceneDepthTexture) {
            gd->setSceneDepthMap(_sceneDepthTexture);
        }
    }
}
