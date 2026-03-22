// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "shadowCatcher.h"

#include "spdlog/spdlog.h"

#include "framework/entity.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"

namespace visutwin::canvas
{
    void ShadowCatcher::setPlaneScale(float value)
    {
        _planeScale = value;
        if (_planeEntity) {
            _planeEntity->setLocalScale(_planeScale, 1.0f, _planeScale);
        }
    }

    void ShadowCatcher::setYOffset(float value)
    {
        _yOffset = value;
        if (_planeEntity) {
            _planeEntity->setLocalPosition(0.0f, _yOffset, 0.0f);
        }
    }

    void ShadowCatcher::initialize()
    {
        auto* owner = entity();
        if (!owner || !owner->engine()) {
            spdlog::error("ShadowCatcher::initialize — no owner entity or engine");
            return;
        }

        // Create the shadow-catching plane entity as a child of the owner
        _planeEntity = new Entity();
        _planeEntity->setEngine(owner->engine());

        // Create the shadow catcher material
        // black diffuse, black specular, no emissive, no skybox,
        // multiplicative blend, shadow catcher flag, no depth write
        _material = new StandardMaterial();
        _material->setName("shadow-catcher");
        _material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _material->setSpecular(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _material->setEmissive(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _material->setUseSkybox(false);
        _material->setUseFog(false);
        _material->setShadowCatcher(true);

        // Set multiplicative blend state: output * framebuffer
        auto blendState = std::make_shared<BlendState>(BlendState::multiplicativeBlend());
        _material->setBlendState(blendState);
        _material->setTransparent(true);

        // disable depth write so the shadow plane doesn't
        // occlude objects behind/below it in the depth buffer.
        auto depthState = std::make_shared<DepthState>(DepthState::noWrite());
        _material->setDepthState(depthState);

        // Add RenderComponent with plane type and the shadow catcher material
        auto* renderComp = static_cast<RenderComponent*>(_planeEntity->addComponent<RenderComponent>());
        if (renderComp) {
            renderComp->setMaterial(_material);
            renderComp->setType("plane");

            // Disable shadow casting on the plane's mesh instances
            for (auto* mi : renderComp->meshInstances()) {
                mi->setCastShadow(false);
            }
            spdlog::info("ShadowCatcher: RenderComponent created, {} mesh instances",
                renderComp->meshInstances().size());
        } else {
            spdlog::error("ShadowCatcher: failed to add RenderComponent to plane entity");
        }

        // Position the plane: centered below entity, scaled to desired size
        _planeEntity->setLocalPosition(0.0f, _yOffset, 0.0f);
        _planeEntity->setLocalScale(Vector3(_planeScale, 1.0f, _planeScale));

        // Add as child of the owner entity
        owner->addChild(_planeEntity);

        spdlog::info("ShadowCatcher: created {}x{} shadow plane at y={:.1f}, transparent={}, shadowCatcher={}",
            _planeScale, _planeScale, _yOffset, _material->transparent(), _material->shadowCatcher());
    }
}
