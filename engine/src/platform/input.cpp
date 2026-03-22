// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.07.2025.
//
#include "input.h"

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    static SDL_Scancode mapKey(const Key key)
    {
        switch (key)
        {
        case Key::W: return SDL_SCANCODE_W;
        case Key::A: return SDL_SCANCODE_A;
        case Key::S: return SDL_SCANCODE_S;
        case Key::D: return SDL_SCANCODE_D;
        case Key::Q: return SDL_SCANCODE_Q;
        case Key::E: return SDL_SCANCODE_E;
        default: return SDL_SCANCODE_UNKNOWN;
        }
    }

    static uint32_t mapMouseButton(const MouseButton btn)
    {
        switch (btn)
        {
        case MouseButton::Left: return SDL_BUTTON_LMASK;
        case MouseButton::Right: return SDL_BUTTON_RMASK;
        case MouseButton::Middle: return SDL_BUTTON_MMASK;
        default: return 0;
        }
    }

    void Input::handleEvent(const SDL_Event& event)
    {
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
            currentKeys[event.key.scancode] = true;
            break;
        case SDL_EVENT_KEY_UP:
            currentKeys[event.key.scancode] = false;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            mousePosition = Vector2(event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            mouseButtonMask |= event.button.button;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            mouseButtonMask &= ~event.button.button;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            windowSize = Vector2u(event.window.data1, event.window.data2);
            break;
        default:
            spdlog::warn("Unhandled input event {}", event.type);
        }
    }

    void Input::newFrame()
    {
        previousKeys = currentKeys;
        mouseDelta = mousePosition - prevMousePosition;
        prevMousePosition = mousePosition;
    }

    bool Input::isKeyDown(const Key key) const
    {
        const auto scanCode = mapKey(key);
        return currentKeys.contains(scanCode) && currentKeys.at(scanCode);
    }

    bool Input::isKeyPressed(const Key key) const
    {
        const auto scanCode = mapKey(key);
        return currentKeys.at(scanCode) && !previousKeys.at(scanCode);
    }

    bool Input::isMouseButtonDown(const MouseButton button) const
    {
        return (mouseButtonMask & mapMouseButton(button)) != 0;
    }
}
