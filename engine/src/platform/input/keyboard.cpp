// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025
//
#include "keyboard.h"

#include "platform/input/sdlInputMapping.h"

namespace visutwin::canvas
{

    void Keyboard::handleEvent(const SDL_Event& event)
    {
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN: {
            _attached = true;
            const int scancode = static_cast<int>(event.key.scancode);
            // Auto-repeat sends key-down again while the key is held; it is the same
            // press, so only a key that was actually up records the edge.
            if (!_keys[scancode]) {
                _pressedThisFrame[scancode] = true;
            }
            _keys[scancode] = true;
            fire("keydown", KeyboardEvent{
                .key = static_cast<Key>(scancode),
                // SDL keycodes below 0x40000000 are the character the key produces;
                // above it they are named keys (arrows, function keys) with none.
                .character = event.key.key < SDLK_SCANCODE_MASK ? event.key.key : 0u,
                .repeat = event.key.repeat,
                .modifiers = keyModifiersFromSdl(event.key.mod),
            });
            break;
        }
        case SDL_EVENT_KEY_UP: {
            _attached = true;
            const int scancode = static_cast<int>(event.key.scancode);
            _releasedThisFrame[scancode] = true;
            _keys[scancode] = false;
            fire("keyup", KeyboardEvent{
                .key = static_cast<Key>(scancode),
                .character = event.key.key < SDLK_SCANCODE_MASK ? event.key.key : 0u,
                .repeat = false,
                .modifiers = keyModifiersFromSdl(event.key.mod),
            });
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Keys held across a focus change never send their key-up, so they would
            // read as held for as long as the application runs.
            detach();
            break;
        default:
            break;
        }
    }

    void Keyboard::detach()
    {
        // Every held key is released, so a caller watching for the release edge is
        // told about it rather than left waiting for a key-up that will never come.
        for (const auto& [scancode, isDown] : _keys) {
            if (isDown) {
                _releasedThisFrame[scancode] = true;
            }
        }
        _keys.clear();
        _attached = false;
    }

    void Keyboard::update()
    {
        _pressedThisFrame.clear();
        _releasedThisFrame.clear();
    }

    namespace
    {
        bool contains(const std::unordered_map<int, bool>& map, const Key key)
        {
            const auto it = map.find(static_cast<int>(key));
            return it != map.end() && it->second;
        }
    }

    bool Keyboard::isPressed(const Key key) const
    {
        return contains(_keys, key);
    }

    bool Keyboard::wasPressed(const Key key) const
    {
        return contains(_pressedThisFrame, key);
    }

    bool Keyboard::wasReleased(const Key key) const
    {
        return contains(_releasedThisFrame, key);
    }

    bool Keyboard::shift() const
    {
        return isPressed(Key::LeftShift) || isPressed(Key::RightShift);
    }

    bool Keyboard::control() const
    {
        return isPressed(Key::LeftControl) || isPressed(Key::RightControl);
    }

    bool Keyboard::alt() const
    {
        return isPressed(Key::LeftAlt) || isPressed(Key::RightAlt);
    }

    bool Keyboard::meta() const
    {
        return isPressed(Key::LeftMeta) || isPressed(Key::RightMeta);
    }
}
