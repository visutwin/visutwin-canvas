// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
#pragma once

#include "scrollViewComponent.h"

#include "framework/components/componentRegistry.h"
#include "scrollViewComponentData.h"
#include "framework/components/componentSystem.h"
#include "framework/engine.h"

namespace visutwin::canvas
{
    class ScrollViewComponentSystem : public ComponentSystem<ScrollViewComponent, ScrollViewComponentData>
    {
    public:
        explicit ScrollViewComponentSystem(Engine* engine) : ComponentSystem(engine, "scrollview")
        {
            // Every enabled scroll view is updated; the bindings are refreshed here too,
            // since nothing fires `element:add` or `scrollbar:add`.
            if (engine && engine->systems()) {
                engine->systems()->on("update", [engine](float) {
                    const auto& views = engine->components().instances<ScrollViewComponent>();
                    for (size_t i = 0; i < views.size(); ++i) {
                        ScrollViewComponent* view = views[i];
                        if (!view || !view->entity() || view->entity()->engine() != engine) {
                            continue;
                        }
                        view->refreshBindings();
                        if (view->active()) {
                            view->update();
                        }
                    }
                }, this);
            }
        }

        ~ScrollViewComponentSystem() override
        {
            if (_engine && _engine->systems()) {
                _engine->systems()->off("update", HandleEventCallback(), this);
            }
        }
    };
}
