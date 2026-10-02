// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// A real Engine for a unit test, and the entity helpers the UI tests build scenes with.
//
// makeTestEngine<Systems...>(device, configure) initialises an engine on `device` with
// the component systems registered in the order given; `configure` sets anything else
// on the AppOptions (an ElementInput, a mouse, a physics world) before they are.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "framework/appOptions.h"
#include "framework/components/element/elementComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas::test
{
    template <typename... Systems>
    std::shared_ptr<Engine> makeTestEngine(std::shared_ptr<GraphicsDevice> device,
        const std::function<void(AppOptions&)>& configure = {})
    {
        auto engine = std::make_shared<Engine>(nullptr);
        AppOptions options;
        options.graphicsDevice = std::move(device);
        if (configure) {
            configure(options);
        }
        (options.registerComponentSystem<Systems>(), ...);
        engine->init(options);
        return engine;
    }

    /// A named entity on `engine`, not yet in the hierarchy; addTo() gives it a parent,
    /// which then owns it.
    inline Entity* newEntity(Engine* engine, const std::string& name = "e")
    {
        auto* e = new Entity();
        e->setEngine(engine);
        e->setName(name);
        return e;
    }

    inline Entity* addTo(GraphNode* parent, Entity* child)
    {
        parent->addChild(std::unique_ptr<GraphNode>(child));
        return child;
    }

    /// An element component on `e`, set up from `desc`.
    inline ElementComponent* addElement(Entity* e, const ElementDesc& desc = {})
    {
        auto* element = static_cast<ElementComponent*>(e->addComponent<ElementComponent>());
        element->setup(desc);
        return element;
    }
}
