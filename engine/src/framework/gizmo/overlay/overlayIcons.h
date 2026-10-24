// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Camera-facing icons of a constant screen size, drawn with an OverlayRenderer and picked by
// their backdrops: a glyph over a disc in the backdrop colour.
//
#pragma once

#include "core/math/vector3.h"
#include "framework/gizmo/overlay/iconGlyphs.h"
#include "framework/gizmo/pool.h"

namespace visutwin::canvas
{
    class Gizmo;
    class GraphNode;
    class MeshInstance;
    class OverlayRenderer;
    class Shape;

    class OverlayIcons
    {
    public:
        OverlayIcons(OverlayRenderer* renderer, Gizmo* gizmo);

        /// Starts a frame.
        void begin() { _placed.count = 0; }

        /// An icon at a world position, marking `node`.
        void add(OverlayIcon kind, const Vector3& position, GraphNode* node);

        /// The node whose icon, placed this frame, was picked by its backdrop or glyph, or null.
        GraphNode* pick(const MeshInstance* meshInstance) const;

        /// The number of icons placed this frame; for tests.
        size_t count() const { return _placed.count; }

    private:
        struct Placed
        {
            Shape* shape = nullptr;
            Shape* glyph = nullptr;
            GraphNode* node = nullptr;
        };

        OverlayRenderer* _renderer = nullptr;
        Gizmo* _gizmo = nullptr;
        Pool<Placed> _placed{[]() { return std::make_unique<Placed>(); }};
    };
}
