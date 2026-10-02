// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#pragma once

#include "buttonComponent.h"
#include "buttonComponentData.h"
#include "framework/components/componentSystem.h"
#include "framework/engine.h"

namespace visutwin::canvas
{
    class ButtonComponentSystem : public ComponentSystem<ButtonComponent, ButtonComponentData>
    {
    public:
        explicit ButtonComponentSystem(Engine* engine) : ComponentSystem(engine, "button")
        {
            // The system updates every enabled button (its tint fade). The element
            // bindings are refreshed here too, since nothing fires `element:add`.
            if (engine && engine->systems()) {
                engine->systems()->on("update", [engine](const float dt) {
                    // By index: a handler may add a button.
                    const auto& buttons = ButtonComponent::instances();
                    for (size_t i = 0; i < buttons.size(); ++i) {
                        ButtonComponent* button = buttons[i];
                        if (!button || !button->entity() || button->entity()->engine() != engine || !button->active()) {
                            continue;
                        }
                        button->refreshBindings();
                        button->update(dt);
                    }
                }, this);
            }
        }

        ~ButtonComponentSystem() override
        {
            if (_engine && _engine->systems()) {
                _engine->systems()->off("update", HandleEventCallback(), this);
            }
        }
    };
}
