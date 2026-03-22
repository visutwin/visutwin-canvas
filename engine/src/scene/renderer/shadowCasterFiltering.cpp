// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "shadowCasterFiltering.h"

#include <algorithm>

#include "framework/components/camera/cameraComponent.h"
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

        const auto* cameraComponent = findCameraComponentForCamera(camera);
        return cameraRendersRenderComponent(cameraComponent, renderComponent);
    }

    bool shouldRenderShadowMeshInstance(MeshInstance* meshInstance, Camera* shadowCamera)
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
        if (material && material->transparent() && !alphaTestCaster) {
            return false;
        }

        if (!meshInstance->mesh()->getVertexBuffer()) {
            return false;
        }

        if (meshInstance->cull()) {
            if (!shadowCamera || !shadowCamera->node()) {
                return false;
            }
            if (!isVisibleInCameraFrustum(shadowCamera, shadowCamera->node().get(), meshInstance->aabb())) {
                return false;
            }
        }

        return true;
    }
}
