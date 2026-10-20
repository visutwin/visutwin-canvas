// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.10.2026
//
// A filled arc on the gizmo layer: the rotate gizmo's swept angle, alpha blended, not writing
// depth, both faces drawn. A fan of `segments` triangles around the centre, whose positions are
// rewritten each time it is drawn.
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;
    class GizmoMaterial;
    class Mesh;
    class MeshInstance;

    class MeshSector
    {
    public:
        /// The sector's entity is added to the engine root, drawn on `layerId`.
        MeshSector(Engine* engine, int layerId, int segments = 64);
        ~MeshSector();

        MeshSector(const MeshSector&) = delete;
        MeshSector& operator=(const MeshSector&) = delete;

        Entity* entity() const { return _entity; }
        int segments() const { return _segments; }

        /// A filled arc around the normalized `axis` from `center`, starting `from` degrees past
        /// `start` (the offset from the centre to the arc's start) and sweeping `sweep` degrees,
        /// scaled by `scale`.
        void draw(const Vector3& center, const Vector3& start, const Vector3& axis, float from, float sweep,
            float scale, const Color& color);

        /// The fan's vertices for those arguments: the centre, then `segments + 1` points along
        /// the arc.
        static std::vector<Vector3> fanPositions(const Vector3& center, const Vector3& start, const Vector3& axis,
            float from, float sweep, float scale, int segments);

        /// The engine is going away and frees the entity with its root.
        void forgetEntity() { _entity = nullptr; }

    private:
        int _segments = 64;
        std::shared_ptr<GizmoMaterial> _material;
        Entity* _entity = nullptr;
        // A mesh per frame in flight: the positions are written into memory the GPU reads on
        // Metal, so a mesh a frame still in flight draws is not rewritten. The one written this
        // frame is the one shown.
        std::vector<std::shared_ptr<Mesh>> _meshes;
        std::vector<MeshInstance*> _instances;
        std::vector<float> _vertices;
        size_t _slot = 0;
        int _writtenVersion = -1;
    };
}
