// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "gamepadSource.h"

#include "framework/engine.h"
#include "platform/input/gamePads.h"

namespace visutwin::canvas
{
    GamepadSource::GamepadSource()
        : InputSource({{"buttons", BUTTON_COUNT}, {"leftStick", 2}, {"rightStick", 2}})
    {
    }

    InputValues GamepadSource::read()
    {
        const GamePads* pads = _engine ? _engine->gamepads() : nullptr;
        if (pads) {
            for (size_t i = 0; i < pads->count(); ++i) {
                const std::array<bool, BUTTON_COUNT> state = {
                    pads->isPressed(i, PadButton::South),
                    pads->isPressed(i, PadButton::East),
                    pads->isPressed(i, PadButton::West),
                    pads->isPressed(i, PadButton::North),
                    pads->isPressed(i, PadButton::LeftShoulder),
                    pads->isPressed(i, PadButton::RightShoulder),
                    pads->axis(i, PadAxis::LeftTrigger) > 0.5f,
                    pads->axis(i, PadAxis::RightTrigger) > 0.5f,
                    pads->isPressed(i, PadButton::Back),
                    pads->isPressed(i, PadButton::Start),
                    pads->isPressed(i, PadButton::LeftStick),
                    pads->isPressed(i, PadButton::RightStick),
                };
                InputDelta& buttons = deltas.at("buttons");
                for (size_t j = 0; j < BUTTON_COUNT; ++j) {
                    const float s = state[j] ? 1.0f : 0.0f;
                    buttons[j] = s - _buttonPrev[j];
                    _buttonPrev[j] = s;
                }
                deltas.at("leftStick").append({pads->axis(i, PadAxis::LeftX), pads->axis(i, PadAxis::LeftY)});
                deltas.at("rightStick").append({pads->axis(i, PadAxis::RightX), pads->axis(i, PadAxis::RightY)});
            }
        }
        return InputSource::read();
    }
}
