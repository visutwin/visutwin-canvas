// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "screenComponent.h"

#include <algorithm>

namespace visutwin::canvas
{
    ScreenComponent::ScreenComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    ScreenComponent::~ScreenComponent()
    {
        std::erase(_instances, this);
    }

    void ScreenComponent::updateScaleFromWindow(const int windowWidth, const int windowHeight)
    {
        const float w = static_cast<float>(std::max(windowWidth, 1));
        const float h = static_cast<float>(std::max(windowHeight, 1));
        _resolution = Vector2(w, h);

        if (!_screenSpace) {
            _scale = 1.0f;
            return;
        }

        const float refW = std::max(_referenceResolution.x, 1.0f);
        const float refH = std::max(_referenceResolution.y, 1.0f);
        _scale = std::min(w / refW, h / refH);
        if (_scale <= 1e-6f) {
            _scale = 1.0f;
        }
    }
}
