// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <vector>

#include "framework/components/component.h"

namespace visutwin::canvas
{
    class Entity;

    class ButtonComponent : public Component
    {
    public:
        ButtonComponent(IComponentSystem* system, Entity* entity);
        ~ButtonComponent() override;

        void initializeComponentData() override {}

        static const std::vector<ButtonComponent*>& instances() { return _instances; }

        Entity* imageEntity() const { return _imageEntity; }
        void setImageEntity(Entity* entity) { _imageEntity = entity; }

    private:
        inline static std::vector<ButtonComponent*> _instances;
        Entity* _imageEntity = nullptr;
    };
}
