// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers  on 18.10.2025.
//
#include "controller.h"

#include <algorithm>
#include <cmath>

#include "keyboard.h"
#include "mouse.h"

namespace visutwin::canvas
{
    void Controller::registerKeys(const std::string& action, const std::vector<Key>& keys)
    {
        auto& binding = _actions[action];
        binding.keys.insert(binding.keys.end(), keys.begin(), keys.end());
    }

    void Controller::registerMouse(const std::string& action, const MouseButton button)
    {
        _actions[action].mouseButtons.push_back(button);
    }

    void Controller::registerPadButton(const std::string& action, const PadButton button,
        const size_t padIndex)
    {
        _actions[action].padButtons.emplace_back(padIndex, button);
    }

    void Controller::registerKeyAxis(const std::string& axis, const Key positive, const Key negative)
    {
        _axes[axis].keyPairs.emplace_back(positive, negative);
    }

    void Controller::registerPadAxis(const std::string& axis, const PadAxis padAxis,
        const size_t padIndex)
    {
        _axes[axis].padAxes.emplace_back(padIndex, padAxis);
    }

    template<typename KeyTest, typename MouseTest, typename PadTest>
    bool Controller::testAction(const std::string& action, KeyTest keyTest,
        MouseTest mouseTest, PadTest padTest) const
    {
        const auto it = _actions.find(action);
        if (it == _actions.end()) {
            return false;
        }
        const Binding& binding = it->second;
        if (_keyboard) {
            for (const Key key : binding.keys) {
                if (keyTest(*_keyboard, key)) {
                    return true;
                }
            }
        }
        if (_mouse) {
            for (const MouseButton button : binding.mouseButtons) {
                if (mouseTest(*_mouse, button)) {
                    return true;
                }
            }
        }
        if (_gamepads) {
            for (const auto& [padIndex, button] : binding.padButtons) {
                if (padTest(*_gamepads, padIndex, button)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool Controller::isPressed(const std::string& action) const
    {
        return testAction(action,
            [](const Keyboard& k, const Key key) { return k.isPressed(key); },
            [](const Mouse& m, const MouseButton b) { return m.isPressed(b); },
            [](const GamePads& g, const size_t i, const PadButton b) { return g.isPressed(i, b); });
    }

    bool Controller::wasPressed(const std::string& action) const
    {
        return testAction(action,
            [](const Keyboard& k, const Key key) { return k.wasPressed(key); },
            [](const Mouse& m, const MouseButton b) { return m.wasPressed(b); },
            [](const GamePads& g, const size_t i, const PadButton b) { return g.wasPressed(i, b); });
    }

    bool Controller::wasReleased(const std::string& action) const
    {
        return testAction(action,
            [](const Keyboard& k, const Key key) { return k.wasReleased(key); },
            [](const Mouse& m, const MouseButton b) { return m.wasReleased(b); },
            [](const GamePads& g, const size_t i, const PadButton b) { return g.wasReleased(i, b); });
    }

    float Controller::axis(const std::string& name) const
    {
        const auto it = _axes.find(name);
        if (it == _axes.end()) {
            return 0.0f;
        }
        const AxisBinding& binding = it->second;

        float value = 0.0f;
        const auto take = [&value](const float candidate) {
            if (std::abs(candidate) > std::abs(value)) {
                value = candidate;
            }
        };

        if (_keyboard) {
            for (const auto& [positive, negative] : binding.keyPairs) {
                // Both held cancel to zero rather than one winning by declaration
                // order: holding left and right at once means neither.
                const float keyValue = (_keyboard->isPressed(positive) ? 1.0f : 0.0f) -
                    (_keyboard->isPressed(negative) ? 1.0f : 0.0f);
                take(keyValue);
            }
        }
        if (_gamepads) {
            for (const auto& [padIndex, padAxis] : binding.padAxes) {
                take(_gamepads->axis(padIndex, padAxis));
            }
        }
        return std::clamp(value, -1.0f, 1.0f);
    }
}
