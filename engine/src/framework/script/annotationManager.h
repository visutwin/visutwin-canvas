// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
// DEVIATION: Rendering replaced with ImGui-based overlays instead of
// SDL_RenderDebugText + CPU-rasterized bitmap textures. The manager
// is now a lightweight annotation registry with screen-space hit testing.
// All visual rendering is done externally via ImGuiOverlay.
//
#pragma once

#include <string>
#include <vector>

#include "script.h"
#include "scriptRegistry.h"
#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class Annotation;
    class Entity;

    /**
     * Annotation screen-space info — computed each frame by the manager.
     * Consumers use this to render annotations via ImGui or any other overlay.
     */
    struct AnnotationScreenInfo
    {
        Annotation* annotation = nullptr;
        float screenX = 0.0f;
        float screenY = 0.0f;
        bool visible = true;   // false if behind camera or off-screen
    };

    /**
     * A manager script that handles annotation registration, screen-space projection,
     * and click interaction. All rendering is delegated to an external overlay system
     * (e.g., ImGuiOverlay) which reads the projected screen positions each frame.
     *
     * The manager listens for engine-level events to automatically register annotations:
     * - `annotation:add` — fired when an Annotation script initializes
     * - `annotation:remove` — fired when an Annotation script is destroyed
     *
     * The manager handles:
     * - Global hotspot size and color configuration
     * - Per-frame world-to-screen projection of all annotations
     * - Mouse hover detection (nearest annotation within hotspot radius)
     * - Click interaction for selecting/deselecting annotations
     *
     * All visual rendering (hotspot circles, tooltip panels, connector lines)
     * is done externally by reading annotations(), activeAnnotation(), and
     * hoveredAnnotation().
     */
    class AnnotationManager : public Script
    {
    public:
        SCRIPT_NAME("annotationManager")

        float hotspotSize() const { return _hotspotSize; }
        void setHotspotSize(float value) { _hotspotSize = value; }

        const Color& hotspotColor() const { return _hotspotColor; }
        void setHotspotColor(const Color& value) { _hotspotColor = value; }

        const Color& activeColor() const { return _activeColor; }
        void setActiveColor(const Color& value) { _activeColor = value; }

        const Color& hoverColor() const { return _hoverColor; }
        void setHoverColor(const Color& value) { _hoverColor = value; }

        float opacity() const { return _opacity; }
        void setOpacity(float value) { _opacity = value; }

        /// All registered annotations with their current screen positions.
        /// Updated each frame in update(). Use this to render annotations externally.
        const std::vector<AnnotationScreenInfo>& screenInfos() const { return _screenInfos; }

        /// The currently selected (clicked) annotation, or nullptr.
        Annotation* activeAnnotation() const { return _activeAnnotation; }

        /// The currently hovered annotation (nearest to mouse within hotspot radius), or nullptr.
        Annotation* hoveredAnnotation() const { return _hoveredAnnotation; }

        void initialize() override;
        void update(float dt) override;

        /// Handle mouse click at screen coordinates — selects/deselects the nearest annotation.
        void handleClick(float screenX, float screenY);

        /// Update hover state based on mouse position (call each frame with current mouse pos).
        void updateHover(float mouseX, float mouseY);

    private:
        void registerAnnotation(Annotation* annotation);
        void unregisterAnnotation(Annotation* annotation);

        // Find camera entity in the scene graph (lazy, called from update)
        void findCameraEntity();

        // Compute screen position from world position
        // Returns false if behind camera
        bool worldToScreen(const Vector3& worldPos, float& screenX, float& screenY) const;

        // Properties
        float _hotspotSize = 25.0f;
        Color _hotspotColor = Color(0.22f, 0.74f, 0.97f, 1.0f);  // cyan-400
        Color _activeColor = Color(0.40f, 0.91f, 0.98f, 1.0f);   // cyan-300
        Color _hoverColor = Color(1.0f, 0.4f, 0.0f, 1.0f);       // orange
        float _opacity = 1.0f;

        Entity* _camera = nullptr;

        // Registered annotations
        std::vector<Annotation*> _annotations;

        // Per-frame screen-space projection results
        std::vector<AnnotationScreenInfo> _screenInfos;

        // Interaction state
        Annotation* _activeAnnotation = nullptr;
        Annotation* _hoveredAnnotation = nullptr;
    };
}

REGISTER_SCRIPT(visutwin::canvas::AnnotationManager, "annotationManager")
