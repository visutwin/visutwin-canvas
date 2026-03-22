// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/math/vector3.h"
#include "core/shape/boundingSphere.h"
#include "framework/components/component.h"

namespace visutwin::canvas
{
    class RenderComponent;

    class CollisionComponent : public Component
    {
    public:
        CollisionComponent(IComponentSystem* system, Entity* entity);
        ~CollisionComponent() override;

        void initializeComponentData() override {}

        static const std::vector<CollisionComponent*>& instances() { return _instances; }

        const std::string& type() const { return _type; }
        void setType(const std::string& type) { _type = type; }

        const Vector3& halfExtents() const { return _halfExtents; }
        void setHalfExtents(const Vector3& value) { _halfExtents = value; }

        float radius() const { return _radius; }
        void setRadius(const float value) { _radius = std::max(value, 0.001f); }

        float height() const { return _height; }
        void setHeight(const float value) { _height = std::max(value, 0.001f); }

        BoundingSphere worldBounds() const;

    private:
        inline static std::vector<CollisionComponent*> _instances;

        std::string _type = "box";
        Vector3 _halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        float _radius = 0.5f;
        float _height = 1.0f;
    };
}
