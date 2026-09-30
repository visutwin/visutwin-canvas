// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "scrollViewComponent.h"
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
            // Upstream updates every enabled scroll view; the bindings are refreshed here too,
            // since nothing fires upstream's `element:add` or `scrollbar:add`.
            if (engine && engine->systems()) {
                engine->systems()->on("update", [engine](float) {
                    const auto& views = ScrollViewComponent::instances();
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
