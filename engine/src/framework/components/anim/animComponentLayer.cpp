// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animComponentLayer.h"

#include <algorithm>

#include "animComponent.h"
#include "framework/anim/binder/defaultAnimBinder.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    AnimComponentLayer::AnimComponentLayer(std::string name, AnimComponent* component,
        const std::vector<AnimStateDesc>& states, const std::vector<AnimTransitionDesc>& transitions,
        const float weight, const bool activate)
        : _name(std::move(name)), _component(component), _weight(weight)
    {
        // Ensure the control states exist — the controller starts in START and the
        // ANY/END pseudo-states participate in transition lookup (upstream state-graph
        // data always includes them).
        auto allStates = states;
        for (const auto& controlState : {ANIM_STATE_START, ANIM_STATE_ANY, ANIM_STATE_END}) {
            const bool present = std::any_of(allStates.begin(), allStates.end(),
                [&controlState](const AnimStateDesc& state) { return state.name == controlState; });
            if (!present) {
                allStates.push_back(AnimStateDesc{controlState});
            }
        }

        _evaluator = std::make_unique<AnimEvaluator>(
            std::make_unique<DefaultAnimBinder>(component ? component->entity() : nullptr));
        _controller = std::make_unique<AnimController>(
            _evaluator.get(), allStates, transitions, activate,
            [component](const std::string& parameterName) -> AnimParameter* {
                return component ? component->findParameter(parameterName) : nullptr;
            },
            [component](const std::string& parameterName) {
                if (component) {
                    component->consumeTrigger(parameterName);
                }
            });
    }
}
