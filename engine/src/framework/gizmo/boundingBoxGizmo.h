// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Corner brackets around the world-space bounds of the attached nodes and their
// descendants: three arms per corner, each a box sized in world units along its edge and in
// screen pixels across it. Every arm draws twice, unlit and alpha blended, first without a
// depth test in the theme's `occluded` colour and then depth tested in its `base` colour, so
// the parts the scene hides show faintly. That needs a layer that keeps the scene's depth:
// one from Gizmo::createLayer clears it, so turn that off (`setClearDepthBuffer(false)`).
//
// The bounds are gathered every frame, before rendering, from the visible mesh instances of
// every enabled render component under the attached nodes (a splat cloud's instance
// included, which carries its extent); nothing selects or drags.
//
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/shape/boundingBox.h"
#include "framework/gizmo/gizmo.h"

namespace visutwin::canvas
{
    class GizmoMaterial;
    class Mesh;

    struct BoundingBoxGizmoTheme
    {
        /// Where the box is visible.
        Color base = Color(1.0f, 1.0f, 1.0f, 1.0f);
        /// Where the scene hides it.
        Color occluded = Color(1.0f, 1.0f, 1.0f, 0.2f);
    };

    /// The colours setTheme changes; one left empty keeps its value.
    struct BoundingBoxGizmoThemePartial
    {
        std::optional<Color> base;
        std::optional<Color> occluded;
    };

    class BoundingBoxGizmo final : public Gizmo
    {
    public:
        static constexpr int kArmCount = 24;

        /// One arm's place under the gizmo root, and its scale (a unit box).
        struct Arm
        {
            Vector3 position;
            Vector3 scale;
        };

        /// What places the arms: the padded bounds, the root position and, for the thickness, the
        /// world size of a screen pixel (`pixel`, per unit of depth in perspective) and the camera.
        struct ArmLayout
        {
            Vector3 center;
            Vector3 halfExtents;
            Vector3 rootPosition;
            float armLength = 0.3f;
            float armThickness = 2.0f;
            float pixel = 0.0f;
            bool perspective = true;
            Vector3 cameraPosition;
            Vector3 cameraForward = Vector3(0.0f, 0.0f, -1.0f);
            float nearClip = 0.1f;
        };

        /// The 24 arms, corner by corner (x, then y, then z, each -1 then +1), three per corner
        /// along x, y and z: each runs `armLength` of its half edge in from the corner and overhangs
        /// it by half a thickness, so the three meet; the thickness is `armThickness` pixels at the
        /// corner's depth along the camera's forward (never nearer than the near plane).
        static std::array<Arm, kArmCount> armTransforms(const ArmLayout& layout);

        BoundingBoxGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);
        ~BoundingBoxGizmo() override;

        const BoundingBoxGizmoTheme& theme() const { return _theme; }
        void setTheme(const BoundingBoxGizmoThemePartial& partial);

        /// The arm length as a fraction of the half edge; 1 draws full edges.
        float armLength = 0.3f;
        /// The world-space padding on each side.
        float padding = 0.01f;
        /// The arm thickness in canvas points.
        float armThickness = 2.0f;

        /// The bounds gathered for the last frame, empty when nothing was found.
        const std::optional<BoundingBox>& bounds() const { return _bounds; }

        void prerender() override;

    protected:
        // The box is world aligned and sized in world units.
        void updateRotation() override {}
        void updateScale() override {}

    private:
        void addNode(GraphNode* node);

        BoundingBoxGizmoTheme _theme;
        std::optional<BoundingBox> _bounds;
        std::shared_ptr<Mesh> _mesh;
        std::shared_ptr<GizmoMaterial> _occludedMaterial;
        std::shared_ptr<GizmoMaterial> _material;
        Entity* _box = nullptr;
        std::array<Entity*, kArmCount> _arms{};
    };
}
