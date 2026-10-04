// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
#pragma once

#include <algorithm>
#include <vector>

#include <spdlog/spdlog.h>

#include "layoutGroupComponent.h"

#include "framework/components/componentRegistry.h"
#include "layoutGroupComponentData.h"
#include "framework/components/componentSystem.h"
#include "framework/engine.h"

namespace visutwin::canvas
{
    class LayoutGroupComponentSystem : public ComponentSystem<LayoutGroupComponent, LayoutGroupComponentData>
    {
    public:
        explicit LayoutGroupComponentSystem(Engine* engine) : ComponentSystem(engine, "layoutgroup")
        {
            if (engine && engine->systems()) {
                engine->systems()->on("postUpdate", [this](float) { processReflows(); }, this);
            }
        }

        ~LayoutGroupComponentSystem() override
        {
            if (_engine && _engine->systems()) {
                _engine->systems()->off("postUpdate", HandleEventCallback(), this);
            }
        }

        /// Reflow every active group whose inputs changed, outermost first so a nested group
        /// sees the size its parent gave it, and again until no group changes (at most
        /// 100 passes).
        void processReflows()
        {
            constexpr int kMaxIterations = 100;
            for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
                std::vector<LayoutGroupComponent*> groups;
                for (LayoutGroupComponent* group : instancesOf<LayoutGroupComponent>(componentRegistry())) {
                    if (group && group->entity() && group->entity()->engine() == _engine && group->active()) {
                        groups.push_back(group);
                    }
                }
                std::stable_sort(groups.begin(), groups.end(), [](const auto* a, const auto* b) {
                    return a->entity()->graphDepth() < b->entity()->graphDepth();
                });
                bool reflowed = false;
                for (LayoutGroupComponent* group : groups) {
                    reflowed |= group->reflowIfChanged();
                }
                if (!reflowed) {
                    return;
                }
            }
            spdlog::warn("Max reflow iterations limit reached, bailing.");
        }
    };
}
