// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#pragma once

#include "framework/components/componentSystem.h"

#include "framework/components/componentRegistry.h"
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
            // Nothing here fires a device `resizecanvas` event, so the canvas size is
            // polled once an update and a change is handed to every screen-space screen.
            // The queued draw-order syncs are resolved here too.
            if (engine && engine->systems()) {
                engine->systems()->on("update", [engine](const float /*dt*/) {
                    const auto [w, h] = engine->canvasSize();
                    for (auto* screen : engine->components().instances<ScreenComponent>()) {
                        if (!screen || !screen->entity() || screen->entity()->engine() != engine) {
                            continue;
                        }
                        if (w > 0 && h > 0) {
                            screen->onCanvasResize(w, h);
                        }
                        if (screen->drawOrderDirty()) {
                            screen->processDrawOrderSync();
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
