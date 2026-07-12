// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.07.2026.
//
#pragma once

#include <array>
#include <optional>

#include "core/eventHandler.h"
#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;
    class StandardMaterial;

    /**
     * @brief Screen-corner orientation gizmo (port of upstream extras `ViewCube`).
     * @ingroup group_framework
     *
     * Shows the world axes as a small gnomon anchored to the top-right corner of the
     * view: three colored axis rods with filled handle spheres on the positive ends and
     * outlined handles on the negative ends. Clicking a handle fires EVENT_CAMERAALIGN
     * with the corresponding world axis so the app can snap its camera.
     * DEVIATION: upstream renders SVG circles into the DOM; this port renders unlit 3D
     * meshes on the Immediate layer (depth test off, drawn last) and picks handles with
     * a ray-sphere test.
     */
    class ViewCube : public EventHandler
    {
    public:
        /// Fired with a `const Vector3&` world axis when a handle is clicked.
        static constexpr const char* EVENT_CAMERAALIGN = "camera:align";

        explicit ViewCube(Engine* engine);
        ~ViewCube() override;

        /// Anchor the gnomon to the camera's top-right corner. Call once per frame.
        void update(Entity* cameraEntity);

        /// Handle a click at window coordinates; fires EVENT_CAMERAALIGN when a handle
        /// is hit and returns the world axis.
        std::optional<Vector3> onClick(float x, float y, float screenWidth, float screenHeight,
            Entity* cameraEntity);

        void setColors(const Color& x, const Color& y, const Color& z, const Color& negative);

        /// Fraction of the vertical field of view the gnomon occupies (default 0.22).
        void setScreenSize(const float value) { _screenSize = value; }

    private:
        Entity* makeHandle(const Color& color, float radius);
        Entity* makeRod(const Color& color);

        Engine* _engine = nullptr;
        Entity* _root = nullptr;

        // Handles in axis order +X, +Y, +Z, -X, -Y, -Z; rods along +X, +Y, +Z.
        std::array<Entity*, 6> _handles{};
        std::array<Entity*, 3> _rods{};
        std::array<std::shared_ptr<StandardMaterial>, 9> _materials{};

        float _screenSize = 0.22f;
        float _lineLength = 1.0f;   // gnomon-local units; scaled per frame
        float _handleRadius = 0.28f;
    };
}
