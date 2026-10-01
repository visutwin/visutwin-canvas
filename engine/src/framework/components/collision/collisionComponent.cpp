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
        _instanceList.add(this);
    }

    CollisionComponent::~CollisionComponent()
    {
        _instanceList.remove(this);
    }

    BoundingSphere CollisionComponent::worldBounds() const
    {
        if (!entity()) {
            return BoundingSphere(Vector3(0.0f, 0.0f, 0.0f), _radius);
        }

        // A conservative sphere for the CPU raycast fallback, used only for shape types it
        // cannot intersect analytically (anything but box, sphere and capsule); the physics
        // world builds its own shapes. Taken from the render bounds where there are some.
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

    void CollisionComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const CollisionComponent*>(source);
        if (!src) {
            return;
        }
        _type = src->_type;
        _halfExtents = src->_halfExtents;
        _radius = src->_radius;
        _height = src->_height;
    }
}
