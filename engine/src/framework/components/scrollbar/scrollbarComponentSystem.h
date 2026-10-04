// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
#pragma once

#include "scrollbarComponent.h"

#include "framework/components/componentRegistry.h"
#include "scrollbarComponentData.h"
#include "framework/components/componentSystem.h"
#include "framework/engine.h"

namespace visutwin::canvas
{
    class ScrollbarComponentSystem : public ComponentSystem<ScrollbarComponent, ScrollbarComponentData>
    {
    public:
        explicit ScrollbarComponentSystem(Engine* engine) : ComponentSystem(engine, "scrollbar")
        {
            // Nothing fires an `element:add` event, so the bindings are refreshed here.
            if (engine && engine->systems()) {
                engine->systems()->on("update", [engine](float) {
                    const auto& scrollbars = engine->components().instances<ScrollbarComponent>();
                    for (size_t i = 0; i < scrollbars.size(); ++i) {
                        ScrollbarComponent* scrollbar = scrollbars[i];
                        if (scrollbar && scrollbar->entity() && scrollbar->entity()->engine() == engine) {
                            scrollbar->refreshBindings();
                        }
                    }
                }, this);
            }
        }

        ~ScrollbarComponentSystem() override
        {
            if (_engine && _engine->systems()) {
                _engine->systems()->off("update", HandleEventCallback(), this);
            }
        }
    };
}
