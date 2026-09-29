// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include <SDL3/SDL_rect.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "framework/components/element/elementComponent.h"

namespace visutwin::canvas
{
    class Engine;
    class ElementComponent;
    class Entity;
    class Mesh;
    class MeshInstance;
    class RenderComponent;
    class Sprite;
    class StandardMaterial;
    class Texture;

    /**
     * Draws UI elements and hit-tests them for mouse input. Each text or image element gets
     * a visual: a child entity with a render component, rebuilt when what it depends on
     * changes. Engine::render syncs the visuals before drawing (upstream's element system
     * keeps its meshes current on its own), so an application does not call it.
     */
    class ElementInput
    {
    public:
        /// The colour, outline and shadow one run of text draws in.
        struct TextStyle
        {
            Color color;
            Color outlineColor;
            float outlineThickness = 0.0f;
            Color shadowColor;
            Vector2 shadowOffset;
        };

        void setEngine(const std::shared_ptr<Engine>& engine) { _engine = engine; }

        void detach();
        bool handleMouseButtonDown(float x, float y);
        /// Create, update and retire the visuals of every text and image element.
        void syncElements();

    private:
        /// One mesh instance of a visual: an image has one, text one per atlas page it
        /// uses (each page is its own texture, so its own material).
        struct VisualPart
        {
            std::shared_ptr<Mesh> mesh;
            std::shared_ptr<StandardMaterial> material;
            MeshInstance* meshInstance = nullptr;
            Texture* texture = nullptr;
            /// Index into ElementVisual::styles; 0 is the element's own, read live.
            int style = 0;
            // What the material was last given, so an unchanged element repacks nothing.
            bool styled = false;
            Color color;
            float opacity = 1.0f;
            Color outlineColor;
            float outlineThickness = 0.0f;
            Color shadowColor;
            Vector2 shadowOffset;
        };

        struct ElementVisual
        {
            Entity* entity = nullptr;
            RenderComponent* render = nullptr;
            std::vector<VisualPart> parts;
            /// Text: the styles its parts draw in (resolved at the last rebuild).
            std::vector<TextStyle> styles;
            /// Text with markup tags: its shadow offsets take upstream's PER-VERTEX
            /// convention, which differs from the uniform one (see msdfShadowUvOffset).
            bool markupStyles = false;
            ElementType type = ElementType::Group;
            bool activeFrame = false;
            // Decided once, when the visual is created: an element on a screen-space
            // screen is drawn in screen space (depth test off, over whatever layer it is
            // on); any other is world geometry that simply follows its entity.
            bool worldSpace = false;
            std::vector<int> layers;
            EventHandlePtr destroyHandle;

            // What the mesh was built from; a difference rebuilds it.
            float cachedWidth = 0.0f;
            float cachedHeight = 0.0f;
            Vector2 cachedPivot = Vector2(0.5f, 0.5f);
            // image
            uint64_t cachedImageVersion = 0;
            const Sprite* cachedSprite = nullptr;
            uint64_t cachedSpriteVersion = 0;
            uint64_t cachedAtlasVersion = 0;
        };

        bool computeElementRect(const ElementComponent* element, SDL_FRect& outRect) const;
        ElementVisual& visualFor(ElementComponent* element);
        void releaseVisual(ElementVisual& visual);

        std::shared_ptr<Engine> _engine;
        std::unordered_map<ElementComponent*, ElementVisual> _visuals;
    };
}
