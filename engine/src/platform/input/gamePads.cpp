// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#include "gamePads.h"

#include <algorithm>
#include <cmath>

namespace visutwin::canvas
{
    GamePads::~GamePads()
    {
        for (auto& pad : _pads) {
            if (pad.handle) {
                SDL_CloseGamepad(pad.handle);
            }
        }
    }

    void GamePads::add(const SDL_JoystickID id)
    {
        if (std::any_of(_pads.begin(), _pads.end(),
                [id](const Pad& pad) { return pad.id == id; })) {
            return;
        }
        SDL_Gamepad* handle = SDL_OpenGamepad(id);
        if (!handle) {
            // A joystick SDL has no mapping for. Reporting it as a pad would give
            // callers an index whose buttons never correspond to anything.
            return;
        }
        _pads.push_back(Pad{.handle = handle, .id = id});
        fire("gamepadconnected", _pads.size() - 1);
    }

    void GamePads::remove(const SDL_JoystickID id)
    {
        const auto it = std::find_if(_pads.begin(), _pads.end(),
            [id](const Pad& pad) { return pad.id == id; });
        if (it == _pads.end()) {
            return;
        }
        const auto index = static_cast<size_t>(std::distance(_pads.begin(), it));
        if (it->handle) {
            SDL_CloseGamepad(it->handle);
        }
        _pads.erase(it);
        // Fired AFTER the erase, so a handler that enumerates pads sees the list
        // without the one that left. Indices above it shift down, which is why the
        // event carries an index rather than a handle callers could hold.
        fire("gamepaddisconnected", index);
    }

    void GamePads::handleEvent(const SDL_Event& event)
    {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            add(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            remove(event.gdevice.which);
            break;
        default:
            break;
        }
    }

    void GamePads::update()
    {
        for (auto& pad : _pads) {
            pad.lastButtons = pad.buttons;
            if (!pad.handle) {
                continue;
            }
            for (size_t i = 0; i < kButtonCount; ++i) {
                pad.buttons[i] = SDL_GetGamepadButton(pad.handle, static_cast<SDL_GamepadButton>(i));
            }
            for (size_t i = 0; i < kAxisCount; ++i) {
                const int16_t raw = SDL_GetGamepadAxis(pad.handle, static_cast<SDL_GamepadAxis>(i));
                // 32767 rather than 32768: the negative end reaches one step
                // further, and dividing by the larger magnitude would stop a fully
                // deflected stick just short of 1.
                float value = static_cast<float>(raw) / 32767.0f;
                value = std::clamp(value, -1.0f, 1.0f);
                const bool isTrigger = i == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                    i == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                if (!isTrigger && std::abs(value) < _deadZone) {
                    value = 0.0f;
                }
                pad.axes[i] = value;
            }
        }
    }

    const GamePads::Pad* GamePads::pad(const size_t index) const
    {
        return index < _pads.size() ? &_pads[index] : nullptr;
    }

    std::string GamePads::name(const size_t index) const
    {
        const Pad* p = pad(index);
        if (!p || !p->handle) {
            return {};
        }
        const char* padName = SDL_GetGamepadName(p->handle);
        return padName ? padName : std::string{};
    }

    bool GamePads::isPressed(const size_t index, const PadButton button) const
    {
        const Pad* p = pad(index);
        return p && p->buttons[static_cast<size_t>(button)];
    }

    bool GamePads::wasPressed(const size_t index, const PadButton button) const
    {
        const Pad* p = pad(index);
        const auto i = static_cast<size_t>(button);
        return p && p->buttons[i] && !p->lastButtons[i];
    }

    bool GamePads::wasReleased(const size_t index, const PadButton button) const
    {
        const Pad* p = pad(index);
        const auto i = static_cast<size_t>(button);
        return p && !p->buttons[i] && p->lastButtons[i];
    }

    float GamePads::axis(const size_t index, const PadAxis axis) const
    {
        const Pad* p = pad(index);
        return p ? p->axes[static_cast<size_t>(axis)] : 0.0f;
    }
}
