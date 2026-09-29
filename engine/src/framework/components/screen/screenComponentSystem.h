// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include "framework/components/componentSystem.h"
#include "framework/engine.h"
#include "screenComponent.h"
#include "screenComponentData.h"

namespace visutwin::canvas
{
    class ScreenComponentSystem : public ComponentSystem<ScreenComponent, ScreenComponentData>
    {
    public:
        explicit ScreenComponentSystem(Engine* engine) : ComponentSystem(engine, "screen")
        {
            // Upstream screens listen to the device's `resizecanvas`; nothing here fires
            // one, so the canvas size is polled once an update and a change is handed to
            // every screen-space screen.
            if (engine && engine->systems()) {
                engine->systems()->on("update", [engine](const float /*dt*/) {
                    const auto [w, h] = engine->canvasSize();
                    if (w <= 0 || h <= 0) {
                        return;
                    }
                    for (auto* screen : ScreenComponent::instances()) {
                        if (screen && screen->entity() && screen->entity()->engine() == engine) {
                            screen->onCanvasResize(w, h);
                        }
                    }
                }, this);
            }
        }

        ~ScreenComponentSystem() override
        {
            if (_engine && _engine->systems()) {
                _engine->systems()->off("update", HandleEventCallback(), this);
            }
        }
    };
}
