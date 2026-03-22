// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "collisionComponent.h"

#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    CollisionComponent::CollisionComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    CollisionComponent::~CollisionComponent()
    {
        std::erase(_instances, this);
    }

    BoundingSphere CollisionComponent::worldBounds() const
    {
        if (!entity()) {
            return BoundingSphere(Vector3(0.0f, 0.0f, 0.0f), _radius);
        }

        // DEVIATION: collision shape primitives are approximated from render bounds until dedicated
        // Ammo/Bullet collision shape generation is ported.
        if (const auto* render = entity()->findComponent<RenderComponent>(); render && !render->meshInstances().empty()) {
            bool hasBounds = false;
            BoundingBox merged;
            for (auto* meshInstance : render->meshInstances()) {
                if (!meshInstance) {
                    continue;
                }
                const BoundingBox worldAabb = meshInstance->aabb();
                if (!hasBounds) {
                    merged = worldAabb;
                    hasBounds = true;
                } else {
                    merged.add(worldAabb);
                }
            }

            if (hasBounds) {
                return BoundingSphere(merged.center(), std::max(merged.halfExtents().length(), 0.001f));
            }
        }

        return BoundingSphere(entity()->position(), std::max(_halfExtents.length(), _radius));
    }
}
