// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Icons that mark the attached entities with nothing else to see or click — a light, a camera, a
// particle system, an animation or a script, on an entity that draws no mesh — as camera-facing
// glyphs of a constant screen size over a clickable backdrop. Clicking one fires `icon:select`
// (GraphNode*).
//
// DEVIATION: there are no audio listener or sound components here, so their glyphs are never
// shown.
//
#pragma once

#include <memory>
#include <optional>

#include "core/math/color.h"
#include "framework/gizmo/gizmo.h"
#include "framework/gizmo/overlay/iconGlyphs.h"

namespace visutwin::canvas
{
    class Entity;
    class OverlayRenderer;

    struct IconGizmoTheme
    {
        /// Where the icons are visible.
        Color base;
        /// Where the scene hides them.
        Color occluded;
        /// The disc behind each icon.
        Color backdrop;
    };

    struct IconGizmoThemePartial
    {
        std::optional<Color> base, occluded, backdrop;
    };

    class IconGizmo final : public Gizmo
    {
    public:
        static constexpr const char* EVENT_ICONSELECT = "icon:select";

        IconGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer);
        ~IconGizmo() override;

        /// Draws a light's icon in the light's colour instead of the theme's.
        bool useLightColor = false;

        /// The icon size in points (24).
        float iconSize() const;
        void setIconSize(float value);

        const IconGizmoTheme& theme() const { return _theme; }
        void setTheme(const IconGizmoThemePartial& partial);

        /// The node whose icon was picked by a mesh instance, or null.
        GraphNode* getIconNode(const MeshInstance* meshInstance) const;

        /// The icon an entity shows, if any.
        std::optional<OverlayIcon> iconOf(Entity* entity) const;

        const OverlayRenderer& renderer() const { return *_renderer; }

        void prerender() override;
        void destroy() override;

    protected:
        void updateRotation() override {}
        void updateScale() override {}

    private:
        IconGizmoTheme _theme;
        std::unique_ptr<OverlayRenderer> _renderer;
    };
}
