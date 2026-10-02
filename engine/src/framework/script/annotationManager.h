// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// AnnotationManager: clickable hotspots with tooltips on scene entities.
//
// Each annotation is drawn as a camera-facing quad of the hotspot
// texture (a dark disc, a light ring and the label), twice — once depth-tested in a layer
// after World's opaque sublayer, once on top of everything at `behindOpacity` in a layer
// after World's transparent one — sized every frame to `hotspotSize` pixels.
//
// DEVIATIONS, all where upstream uses the browser:
// - the clickable hotspots and the tooltip are UI ELEMENTS on a screen-space screen this
//   manager creates, where upstream uses DOM elements and a stylesheet. So the application
//   must register the Screen and Element component systems and supply an ElementInput;
//   without them the hotspots still draw but cannot be clicked. The tooltip copies the
//   stylesheet: 8 px padding, black at 0.8, 4 px corners, 14 px text, a 200 px maximum
//   content width, a bold title 4 px above the text, an 8 px arrow on its left edge, 25 px
//   right of the hotspot and centred on it, faded in and out over 0.2 s.
// - text needs FONTS (setFonts): the hotspot label is rasterised on the CPU from the bold
//   font's MSDF atlas, where upstream draws "bold 32px Arial" on a 2D canvas, and the
//   tooltip uses both, where upstream uses the system UI font.
// - a press anywhere that no hotspot took hides the tooltip, as upstream's document-level
//   pointerdown does: it is read from the engine's Mouse and TouchDevice, which a press a
//   hotspot stopped never reaches.
// - the quads hang under a child node of the annotation's entity, which takes the camera
//   rotation and the per-frame scale; upstream turns and scales the annotation entity
//   itself, so anything else parented to it turned and scaled too.
// - no near/far depth clamp of the quad's vertices (upstream does it in a vertex chunk):
//   the forward vertex stage has no chunked form on Vulkan. A hotspot nearer than the near
//   plane is clipped.
//
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "script.h"
#include "scriptRegistry.h"
#include "core/math/color.h"

namespace visutwin::canvas
{
    class Annotation;
    class CameraComponent;
    class ElementComponent;
    class Entity;
    class Layer;
    class Sprite;
    class StandardMaterial;
    class Texture;
    class TextureAtlas;
    struct FontResource;

    /**
     * A manager script that handles global configuration and shared resources for all
     * annotations in a scene. Add this script to a single entity to configure annotation
     * appearance.
     *
     * The manager listens for engine-level events to automatically register annotations:
     * - `annotation:add` — fired when an Annotation script post-initializes
     * - `annotation:remove` — fired when an Annotation script is destroyed
     *
     * The manager handles:
     * - Global hotspot size, colors, and opacity settings
     * - Shared rendering resources (layers, the tooltip)
     * - Per-annotation rendering resources (entities, materials, the hotspot element)
     * - Hover and click interactions
     */
    class AnnotationManager : public Script
    {
    public:
        SCRIPT_NAME("annotationManager")

        ~AnnotationManager() override;

        /// The size of hotspots in screen pixels (points). Default 25.
        float hotspotSize() const { return _hotspotSize; }
        void setHotspotSize(float value);

        /// The default color of hotspots. Default (0.8, 0.8, 0.8).
        const Color& hotspotColor() const { return _hotspotColor; }
        void setHotspotColor(const Color& value);

        /// The color of hotspots when hovered. Default (1, 0.4, 0).
        const Color& hoverColor() const { return _hoverColor; }
        void setHoverColor(const Color& value);

        /// The opacity of hotspots when visible (not occluded). Default 1.
        float opacity() const { return _opacity; }
        void setOpacity(const float value) { _opacity = value; }

        /// The opacity of hotspots when behind geometry. Default 0.25.
        float behindOpacity() const { return _behindOpacity; }
        void setBehindOpacity(const float value) { _behindOpacity = value; }

        /// DEVIATION: the fonts the tooltip text and the title (and hotspot label) are
        /// drawn with; upstream uses the browser's. Set before the engine starts, or at
        /// least before the first annotation registers. Borrowed: they must outlive this.
        void setFonts(FontResource* regular, FontResource* bold);

        Annotation* activeAnnotation() const { return _activeAnnotation; }
        Annotation* hoverAnnotation() const { return _hoverAnnotation; }

        void initialize() override;

        /// Runs the tooltip's fade and the 200 ms hide timeout.
        void update(float dt) override;

    private:
        struct Resources
        {
            Annotation* annotation = nullptr;
            // Child of the annotation entity: the camera-facing, screen-sized node.
            Entity* holder = nullptr;
            Entity* hotspot = nullptr;   // the clickable element on the screen
            std::unique_ptr<Texture> texture;
            std::shared_ptr<StandardMaterial> materials[2];   // base, overlay
            float appliedOpacity[2] = {-1.0f, -1.0f};
            std::vector<EventHandlePtr> eventHandles;
        };

        Resources* resourcesFor(const Annotation* annotation);

        std::unique_ptr<Texture> createHotspotTexture(const std::string& label) const;
        std::shared_ptr<StandardMaterial> createHotspotMaterial(Texture* texture, float opacity, bool depth) const;

        void registerAnnotation(Annotation* annotation);
        void unregisterAnnotation(Annotation* annotation);

        void setAnnotationHover(Annotation* annotation, bool hover);
        void showTooltip(Annotation* annotation);
        void hideTooltip(Annotation* annotation);
        void hideAnnotationElements(Annotation* annotation, Resources& resources);
        void layoutTooltip();

        void onLabelChange(Annotation* annotation);
        void onAnnotationEnable(Annotation* annotation);
        void onAnnotationDisable(Annotation* annotation);

        void createTooltip();
        void prerender();
        void teardown();
        void clearScreen();

        float _hotspotSize = 25.0f;
        Color _hotspotColor = Color(0.8f, 0.8f, 0.8f, 1.0f);
        Color _hoverColor = Color(1.0f, 0.4f, 0.0f, 1.0f);
        float _opacity = 1.0f;
        float _behindOpacity = 0.25f;

        FontResource* _regularFont = nullptr;
        FontResource* _boldFont = nullptr;

        Entity* _camera = nullptr;
        std::shared_ptr<Layer> _layers[2];

        // The screen holding the hotspot elements and the tooltip, under the root.
        Entity* _screen = nullptr;
        ElementComponent* _tooltip = nullptr;
        ElementComponent* _arrow = nullptr;
        ElementComponent* _title = nullptr;
        ElementComponent* _text = nullptr;
        std::unique_ptr<Texture> _panelTexture;
        std::unique_ptr<Texture> _arrowTexture;
        std::shared_ptr<TextureAtlas> _panelAtlas;
        std::shared_ptr<Sprite> _panelSprite;

        // Tooltip fade: `_fade` runs 0..1 toward the target over 0.2 s; the opacity is
        // its ease-in-out. `_hideTimer` > 0 counts down the 200 ms hide delay.
        bool _tooltipShown = false;
        float _fade = 0.0f;
        float _appliedFade = -1.0f;
        float _hideTimer = 0.0f;
        Annotation* _hidePending = nullptr;

        // In registration order.
        std::vector<std::unique_ptr<Resources>> _annotationResources;

        Annotation* _activeAnnotation = nullptr;
        Annotation* _hoverAnnotation = nullptr;

        bool _tornDown = false;
        std::vector<EventHandlePtr> _handles;
    };
}

REGISTER_SCRIPT(visutwin::canvas::AnnotationManager, "annotationManager")
