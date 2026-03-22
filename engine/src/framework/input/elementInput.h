// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include <SDL3/SDL_render.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "core/math/vector2.h"
#include "framework/components/element/elementComponent.h"

namespace visutwin::canvas
{
    class Engine;
    class ElementComponent;
    class Entity;
    class Mesh;
    class RenderComponent;
    class StandardMaterial;

    /**
     * Handles mouse and touch events for {@link ElementComponent}s. When input events occur on an
     * ElementComponent, this fires the appropriate events on the ElementComponent.
     */
    class ElementInput
    {
    public:
        void setEngine(const std::shared_ptr<Engine>& engine) { _engine = engine; }
        void setSdlRenderer(SDL_Renderer* renderer) { _sdlRenderer = renderer; }

        void detach();
        bool handleMouseButtonDown(float x, float y);
        void renderElements();
        void syncTextElements();

    private:
        struct TextVisual
        {
            Entity* entity = nullptr;
            RenderComponent* render = nullptr;
            std::shared_ptr<Mesh> mesh;
            std::shared_ptr<StandardMaterial> material;
            std::string cachedText;
            int cachedFontSize = 0;
            float cachedWidth = 0.0f;
            float cachedHeight = 0.0f;
            Vector2 cachedPivot = Vector2(0.5f, 0.5f);
            ElementHorizontalAlign cachedAlign = ElementHorizontalAlign::Center;
            bool cachedWrap = false;
            FontResource* cachedFont = nullptr;
            bool activeFrame = false;
        };

        bool computeElementRect(const ElementComponent* element, SDL_FRect& outRect) const;

        std::shared_ptr<Engine> _engine;
        SDL_Renderer* _sdlRenderer = nullptr;
        std::unordered_map<ElementComponent*, TextVisual> _textVisuals;
    };
}
