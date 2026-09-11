// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#include "mouse.h"

#include <SDL3/SDL_mouse.h>

namespace visutwin::canvas
{
    namespace
    {
        MouseButton buttonOf(const uint8_t sdlButton)
        {
            switch (sdlButton) {
            case SDL_BUTTON_LEFT:   return MouseButton::Left;
            case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
            case SDL_BUTTON_RIGHT:  return MouseButton::Right;
            default:                return MouseButton::None;
            }
        }

        KeyModifiers currentModifiers()
        {
            const SDL_Keymod mod = SDL_GetModState();
            return KeyModifiers{
                .shift = (mod & SDL_KMOD_SHIFT) != 0,
                .control = (mod & SDL_KMOD_CTRL) != 0,
                .alt = (mod & SDL_KMOD_ALT) != 0,
                .meta = (mod & SDL_KMOD_GUI) != 0,
            };
        }
    }

    size_t Mouse::index(const MouseButton button)
    {
        switch (button) {
        case MouseButton::Left:   return 1;
        case MouseButton::Middle: return 2;
        case MouseButton::Right:  return 3;
        default:                  return 0;
        }
    }

    void Mouse::handleEvent(const SDL_Event& event)
    {
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            _attached = true;
            _x = event.motion.x;
            _y = event.motion.y;
            _deltaX += event.motion.xrel;
            _deltaY += event.motion.yrel;
            fire("mousemove", MouseEvent{
                .x = event.motion.x,
                .y = event.motion.y,
                .dx = event.motion.xrel,
                .dy = event.motion.yrel,
                .button = MouseButton::None,
                .modifiers = currentModifiers(),
            });
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            _attached = true;
            const MouseButton button = buttonOf(event.button.button);
            const size_t i = index(button);
            if (!_buttons[i]) {
                _pressedThisFrame[i] = true;
            }
            _buttons[i] = true;
            _x = event.button.x;
            _y = event.button.y;
            fire("mousedown", MouseEvent{
                .x = event.button.x, .y = event.button.y,
                .button = button, .modifiers = currentModifiers(),
            });
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            _attached = true;
            const MouseButton button = buttonOf(event.button.button);
            const size_t i = index(button);
            _releasedThisFrame[i] = true;
            _buttons[i] = false;
            _x = event.button.x;
            _y = event.button.y;
            fire("mouseup", MouseEvent{
                .x = event.button.x, .y = event.button.y,
                .button = button, .modifiers = currentModifiers(),
            });
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            _attached = true;
            _wheelDelta += event.wheel.y;
            fire("mousewheel", MouseEvent{
                .x = _x, .y = _y,
                .wheelDelta = event.wheel.y,
                .button = MouseButton::None,
                .modifiers = currentModifiers(),
            });
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            detach();
            break;
        default:
            break;
        }
    }

    void Mouse::detach()
    {
        // A button held across a focus change gets its release edge here, for the
        // reason Keyboard::detach gives.
        for (size_t i = 0; i < _buttons.size(); ++i) {
            if (_buttons[i]) {
                _releasedThisFrame[i] = true;
            }
        }
        _buttons.fill(false);
        _deltaX = 0.0f;
        _deltaY = 0.0f;
        _wheelDelta = 0.0f;
        _attached = false;
    }

    void Mouse::update()
    {
        _pressedThisFrame.fill(false);
        _releasedThisFrame.fill(false);
        _deltaX = 0.0f;
        _deltaY = 0.0f;
        _wheelDelta = 0.0f;
    }

    bool Mouse::isPressed(const MouseButton button) const
    {
        return _buttons[index(button)];
    }

    bool Mouse::wasPressed(const MouseButton button) const
    {
        return _pressedThisFrame[index(button)];
    }

    bool Mouse::wasReleased(const MouseButton button) const
    {
        return _releasedThisFrame[index(button)];
    }

    void Mouse::setRelativeMode(SDL_Window* window, const bool enabled)
    {
        if (!window || _relativeMode == enabled) {
            return;
        }
        if (SDL_SetWindowRelativeMouseMode(window, enabled)) {
            _relativeMode = enabled;
        }
    }
}
