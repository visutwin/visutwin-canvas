// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <functional>
#include <vector>

#include "batching/batchManager.h"
#include "input/elementInput.h"
#include "lightmapper/lightmapper.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/input/gamePads.h"
#include "platform/input/keyboard.h"
#include "platform/input/mouse.h"
#include "platform/input/touchDevice.h"
#include "xr/xrManager.h"

namespace visutwin::canvas
{
    class Engine;
    class IComponentSystem;

    /*
     * AppOptions holds configuration settings utilized in the creation of an {@link AppBase} instance
     */
    struct AppOptions
    {
        using ComponentSystemCreator = std::function<std::unique_ptr<IComponentSystem>(Engine*)>;

        template <class ComponentSystem>
        void registerComponentSystem() {
            componentSystems.emplace_back([](Engine* engine) {
                return std::make_unique<ComponentSystem>(engine);
            });
        }

        // The component systems the app requires
        std::vector<ComponentSystemCreator> componentSystems;

        std::shared_ptr<GraphicsDevice> graphicsDevice;

        std::vector<std::string> scriptsOrder;
        std::string scriptPrefix;

        std::shared_ptr<Lightmapper> lightmapper;

        std::shared_ptr<BatchManager> batchManager;

        std::shared_ptr<Keyboard> keyboard;
        std::shared_ptr<Mouse> mouse;
        std::shared_ptr<GamePads> gamepads;
        std::shared_ptr<TouchDevice> touch;
        std::shared_ptr<ElementInput> elementInput;
        std::shared_ptr<XrManager> xr;
    };
}
