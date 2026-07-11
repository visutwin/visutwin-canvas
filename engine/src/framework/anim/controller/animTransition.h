// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <string>
#include <vector>

#include "animConstants.h"

namespace visutwin::canvas
{
    /**
     * AnimTransitions represent connections in the controller's state graph between AnimStates.
     * Each frame the controller tests whether any transition has the current state as its source
     * and its conditions met; if so it transitions to the destination state.
     */
    class AnimTransition
    {
    public:
        AnimTransition() = default;
        AnimTransition(std::string from, std::string to, const float time = 0.0f,
                       const int priority = 0, std::vector<AnimCondition> conditions = {},
                       const float exitTime = -1.0f, const float transitionOffset = -1.0f,
                       const AnimInterruption interruptionSource = AnimInterruption::NONE)
            : _from(std::move(from)), _to(std::move(to)), _time(time), _priority(priority),
              _conditions(std::move(conditions)), _exitTime(exitTime),
              _transitionOffset(transitionOffset), _interruptionSource(interruptionSource)
        {
        }

        const std::string& from() const { return _from; }
        const std::string& to() const { return _to; }
        void setTo(const std::string& value) { _to = value; }

        float time() const { return _time; }
        int priority() const { return _priority; }
        const std::vector<AnimCondition>& conditions() const { return _conditions; }

        /** Normalized exit time; negative = none (upstream uses null). */
        float exitTime() const { return _exitTime; }
        bool hasExitTime() const { return _exitTime > 0.0f; }

        /** Normalized start offset in the destination state; negative = none. */
        float transitionOffset() const { return _transitionOffset; }

        AnimInterruption interruptionSource() const { return _interruptionSource; }

    private:
        std::string _from;
        std::string _to;
        float _time = 0.0f;
        int _priority = 0;
        std::vector<AnimCondition> _conditions;
        float _exitTime = -1.0f;
        float _transitionOffset = -1.0f;
        AnimInterruption _interruptionSource = AnimInterruption::NONE;
    };
}
