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
        void setEngine(const std::shared_ptr<Engine>& engine) { _engine = engine; }

        void detach();
        bool handleMouseButtonDown(float x, float y);
        /// Create, update and retire the visuals of every text and image element.
        void syncElements();

    private:
        struct ElementVisual
        {
            Entity* entity = nullptr;
            RenderComponent* render = nullptr;
            MeshInstance* meshInstance = nullptr;
            std::shared_ptr<Mesh> mesh;
            std::shared_ptr<StandardMaterial> material;
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
            // text
            std::string cachedText;
            int cachedFontSize = 0;
            ElementHorizontalAlign cachedAlign = ElementHorizontalAlign::Center;
            bool cachedWrap = false;
            float cachedVerticalAlign = 1.0f;
            FontResource* cachedFont = nullptr;
            // image
            uint64_t cachedImageVersion = 0;
            const Sprite* cachedSprite = nullptr;
            uint64_t cachedSpriteVersion = 0;
            uint64_t cachedAtlasVersion = 0;
            // The texture the material currently samples.
            Texture* boundTexture = nullptr;
        };

        bool computeElementRect(const ElementComponent* element, SDL_FRect& outRect) const;
        ElementVisual& visualFor(ElementComponent* element);
        void releaseVisual(ElementVisual& visual);

        std::shared_ptr<Engine> _engine;
        std::unordered_map<ElementComponent*, ElementVisual> _visuals;
    };
}
