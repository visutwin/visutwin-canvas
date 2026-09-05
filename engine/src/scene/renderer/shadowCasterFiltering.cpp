// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "shadowCasterFiltering.h"

#include "scene/materials/standardMaterial.h"

#include <algorithm>

#include "framework/components/camera/cameraComponent.h"
#include "framework/batching/batchManager.h"
#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/frustumUtils.h"
#include "scene/meshInstance.h"
#include "scene/mesh.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    namespace
    {
        CameraComponent* findCameraComponentForCamera(const Camera* camera)
        {
            if (!camera) {
                return nullptr;
            }
            for (auto* cameraComponent : CameraComponent::instances()) {
                if (cameraComponent && cameraComponent->camera() == camera) {
                    return cameraComponent;
                }
            }
            return nullptr;
        }

        bool cameraRendersRenderComponent(const CameraComponent* cameraComponent, const RenderComponent* renderComponent)
        {
            if (!renderComponent) {
                return false;
            }
            if (!cameraComponent) {
                return true;
            }

            const auto& layers = renderComponent->layers();
            if (layers.empty()) {
                return true;
            }

            return std::any_of(layers.begin(), layers.end(), [cameraComponent](const int layerId) {
                return cameraComponent->rendersLayer(layerId);
            });
        }
    }

    bool shouldRenderShadowRenderComponent(const RenderComponent* renderComponent, const Camera* camera)
    {
        if (!renderComponent || !renderComponent->enabled() || !renderComponent->entity() || !renderComponent->entity()->enabled()) {
            return false;
        }

        // A lightmap bake camera renders one mesh in UV space through its own private
        // layer, but the shadows baked into that mesh have to come from the WHOLE scene —
        // filtering casters by the camera's layers would leave the bake shadowless.
        if (camera && camera->lightmapBakePass()) {
            return true;
        }

        const auto* cameraComponent = findCameraComponentForCamera(camera);
        return cameraRendersRenderComponent(cameraComponent, renderComponent);
    }

    void collectShadowCasters(std::vector<MeshInstance*>& casters)
    {
        for (auto* renderComponent : RenderComponent::instances()) {
            if (!shouldRenderShadowRenderComponent(renderComponent, nullptr)) {
                continue;
            }
            for (auto* meshInstance : renderComponent->meshInstances()) {
                casters.push_back(meshInstance);
            }
        }
        for (auto* meshInstance : BatchManager::batchMeshInstances()) {
            casters.push_back(meshInstance);
        }
    }

    bool shouldRenderShadowMeshInstanceIgnoringVisibility(MeshInstance* meshInstance)
    {
        if (!meshInstance || !meshInstance->mesh()) {
            return false;
        }
        if (!meshInstance->castShadow()) {
            return false;
        }
        if (meshInstance->node() && !meshInstance->node()->enabled()) {
            return false;
        }

        Material* material = meshInstance->material();
        const bool alphaTestCaster = material && material->alphaMode() == AlphaMode::MASK;
        // A material that dithers its shadow is asking to cast one despite being blended
        // (upstream opacityShadowDither): the shadow pass discards the same screen-space
        // Bayer pattern, so the caster throws a thinned shadow instead of a solid one.
        const auto* standard = dynamic_cast<const StandardMaterial*>(material);
        const bool ditheredShadowCaster = standard &&
            standard->opacityShadowDitherMode() != DitherMode::DITHER_NONE;
        if (material && material->transparent() && !alphaTestCaster && !ditheredShadowCaster) {
            return false;
        }

        return meshInstance->mesh()->getVertexBuffer() != nullptr;
    }

    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera)
    {
        const Frustum frustum = (shadowCamera && shadowCamera->node())
            ? buildCameraFrustum(shadowCamera, shadowCamera->node()) : Frustum{};
        return shouldRenderShadowMeshInstance(meshInstance, shadowCamera, frustum);
    }

    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera,
        const Frustum& shadowFrustum)
    {
        if (!shouldRenderShadowMeshInstanceIgnoringVisibility(meshInstance)) {
            return false;
        }

        if (meshInstance->cull()) {
            if (!shadowCamera || !shadowCamera->node()) {
                return false;
            }
            if (!isVisibleInFrustum(shadowFrustum, meshInstance->aabb())) {
                return false;
            }
        }

        return true;
    }
}
