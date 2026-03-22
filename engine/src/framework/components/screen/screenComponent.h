// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <vector>

#include "core/math/vector2.h"
#include "framework/components/component.h"

namespace visutwin::canvas
{
    class ScreenComponent : public Component
    {
    public:
        ScreenComponent(IComponentSystem* system, Entity* entity);
        ~ScreenComponent() override;

        void initializeComponentData() override {}

        static const std::vector<ScreenComponent*>& instances() { return _instances; }

        const Vector2& referenceResolution() const { return _referenceResolution; }
        void setReferenceResolution(const Vector2& value) { _referenceResolution = value; }

        const Vector2& resolution() const { return _resolution; }

        bool screenSpace() const { return _screenSpace; }
        void setScreenSpace(const bool value) { _screenSpace = value; }

        float scale() const { return _scale; }

        void updateScaleFromWindow(int windowWidth, int windowHeight);

    private:
        inline static std::vector<ScreenComponent*> _instances;

        Vector2 _referenceResolution = Vector2(1280.0f, 720.0f);
        Vector2 _resolution = Vector2(1280.0f, 720.0f);
        bool _screenSpace = true;
        float _scale = 1.0f;
    };
}
