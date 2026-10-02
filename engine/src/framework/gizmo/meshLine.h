// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// A gizmo line drawn as a thin cylinder (the rotate
// gizmo's angle guides), alpha blended, written at depth 0 so it shows in front of
// every shape of the gizmo layer.
//
#pragma once

#include <memory>

#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;
    class GizmoMaterial;

    class MeshLine
    {
    public:
        /// The line's entity is added to the engine root, drawn on `layerId`.
        MeshLine(Engine* engine, int layerId, float thickness = 0.02f);
        ~MeshLine();

        MeshLine(const MeshLine&) = delete;
        MeshLine& operator=(const MeshLine&) = delete;

        Entity* entity() const { return _entity; }

        float thickness() const { return _thickness; }
        void setThickness(const float value) { _thickness = value; }

        /// A cylinder from `from` along the
        /// direction to `to`, `distance * scale` long and `thickness * scale` wide.
        void draw(const Vector3& from, const Vector3& to, float scale, const Color& color);

        /// The engine is going away and frees the entity with its root.
        void forgetEntity() { _entity = nullptr; }

    private:
        float _thickness = 0.02f;
        std::shared_ptr<GizmoMaterial> _material;
        Entity* _entity = nullptr;
    };
}
