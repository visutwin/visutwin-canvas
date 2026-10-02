// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/vector3.h"
#include "core/shape/boundingSphere.h"
#include "framework/components/component.h"
#include "framework/components/componentInstanceList.h"

namespace visutwin::canvas
{
    class Mesh;
    class RenderComponent;

    class CollisionComponent : public Component
    {
    public:
        CollisionComponent(IComponentSystem* system, Entity* entity);
        ~CollisionComponent() override;

        void initializeComponentData() override {}
        void cloneFrom(const Component* source) override;

        static const std::vector<CollisionComponent*>& instances() { return _instanceList.items(); }

        const std::string& type() const { return _type; }
        void setType(const std::string& type) { _type = type; }

        const Vector3& halfExtents() const { return _halfExtents; }
        void setHalfExtents(const Vector3& value) { _halfExtents = value; }

        float radius() const { return _radius; }
        void setRadius(const float value) { _radius = std::max(value, 0.001f); }

        float height() const { return _height; }
        void setHeight(const float value) { _height = std::max(value, 0.001f); }

        BoundingSphere worldBounds() const;

        /// 'mesh': the meshes the collision volume is made of, in the
        /// entity's space. Left empty, the entity's own RenderComponent supplies them, each
        /// mesh through its node's transform relative to the entity.
        const std::vector<std::shared_ptr<Mesh>>& render() const { return _render; }
        void setRender(const std::vector<std::shared_ptr<Mesh>>& meshes) { _render = meshes; }

        /// 'mesh': collide with the convex hull of the vertices
        /// instead of the triangles. A DYNAMIC body always takes the hull, as Jolt simulates
        /// triangle meshes only on static and kinematic bodies (DEVIATION: upstream's
        /// Ammo gives a dynamic body the triangle mesh).
        bool convexHull() const { return _convexHull; }
        void setConvexHull(const bool value) { _convexHull = value; }

        /// The 'mesh' geometry in the entity's space, scaled by its world scale:
        /// `points`, and `indices` as triangles. Positions
        /// are read from the meshes' CPU copies; a mesh without one contributes nothing.
        void collectMeshGeometry(std::vector<Vector3>& points, std::vector<uint32_t>& indices) const;

    private:
        inline static ComponentInstanceList<CollisionComponent> _instanceList;

        std::string _type = "box";
        Vector3 _halfExtents = Vector3(0.5f, 0.5f, 0.5f);
        float _radius = 0.5f;
        float _height = 1.0f;
        std::vector<std::shared_ptr<Mesh>> _render;
        bool _convexHull = false;
    };
}
