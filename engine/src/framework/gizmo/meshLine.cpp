// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "meshLine.h"

#include <cmath>

#include "core/math/defines.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmoMaterial.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    MeshLine::MeshLine(Engine* engine, const int layerId, const float thickness)
        : _thickness(thickness)
    {
        if (!engine || !engine->root()) {
            return;
        }
        _material = std::make_shared<GizmoMaterial>(engine->graphicsDevice());
        // Depth write on, at depth 0.
        _material->setDepth(0.0f);

        auto entity = std::make_unique<Entity>();
        _entity = entity.get();
        _entity->setEngine(engine);
        _entity->setName("mesh-line");
        engine->root()->addChild(std::move(entity));

        auto* render = static_cast<RenderComponent*>(_entity->addComponent<RenderComponent>());
        if (!render) {
            return;
        }
        render->setLayers({layerId});
        render->setCastShadows(false);
        auto instance = std::make_unique<MeshInstance>(
            createMeshFromGeometry(engine->graphicsDevice(), createCylinderGeometry()),
            std::static_pointer_cast<Material>(_material), _entity);
        instance->setCull(false);
        instance->setCastShadow(false);
        render->addMeshInstance(std::move(instance));
    }

    MeshLine::~MeshLine()
    {
        if (_entity && _entity->parent()) {
            auto owned = _entity->remove();
        }
        _entity = nullptr;
    }

    void MeshLine::draw(const Vector3& from, const Vector3& to, const float scale, const Color& color)
    {
        if (!_entity) {
            return;
        }
        _material->setColor(color);

        const Vector3 dir = (to - from).normalized();
        const float elev = std::atan2(-dir.getY(), std::sqrt(dir.getX() * dir.getX() + dir.getZ() * dir.getZ())) *
            RAD_TO_DEG;
        const float azim = std::atan2(-dir.getX(), -dir.getZ()) * RAD_TO_DEG;
        _entity->setLocalEulerAngles(-elev + 90.0f, azim, 0.0f);

        const float length = from.distance(to) * scale;
        _entity->setLocalPosition(dir * (0.5f * length) + from);
        _entity->setLocalScale(_thickness * scale, length, _thickness * scale);
    }
}
