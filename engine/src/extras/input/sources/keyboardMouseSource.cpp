// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "keyboardMouseSource.h"

#include "framework/engine.h"
#include "platform/input/keyboard.h"
#include "platform/input/mouse.h"

namespace visutwin::canvas
{
    namespace
    {
        // A browser reports a wheel notch as 100 pixels of deltaY, positive toward the
        // user; SDL reports notches, positive away from the user.
        constexpr float kWheelPixelsPerNotch = 100.0f;

        int buttonIndex(const MouseButton button)
        {
            switch (button) {
            case MouseButton::Left: return 0;
            case MouseButton::Middle: return 1;
            case MouseButton::Right: return 2;
            default: return -1;
            }
        }

        int keyCodeOf(const int scancode)
        {
            using KC = KeyboardMouseSource;
            const Key key = static_cast<Key>(scancode);
            if (scancode >= static_cast<int>(Key::A) && scancode <= static_cast<int>(Key::Z)) {
                return KC::A + (scancode - static_cast<int>(Key::A));
            }
            // SDL orders the digit row 1..9 then 0.
            if (key == Key::Digit0) {
                return KC::Digit0;
            }
            if (scancode >= static_cast<int>(Key::Digit1) && scancode <= static_cast<int>(Key::Digit9)) {
                return KC::Digit0 + 1 + (scancode - static_cast<int>(Key::Digit1));
            }
            switch (key) {
            case Key::Up: return KC::UP;
            case Key::Down: return KC::DOWN;
            case Key::Left: return KC::LEFT;
            case Key::Right: return KC::RIGHT;
            case Key::Space: return KC::SPACE;
            case Key::LeftShift:
            case Key::RightShift: return KC::SHIFT;
            case Key::LeftControl:
            case Key::RightControl: return KC::CTRL;
            default: return -1;
            }
        }
    }

    KeyboardMouseSource::KeyboardMouseSource()
        : InputSource({{"key", KEY_COUNT}, {"button", 3}, {"mouse", 2}, {"wheel", 1}})
    {
    }

    KeyboardMouseSource::~KeyboardMouseSource()
    {
        detach();
    }

    void KeyboardMouseSource::clearButtons()
    {
        for (float& b : _button) {
            b = b == 1.0f ? -1.0f : 0.0f;
        }
    }

    void KeyboardMouseSource::setKey(const int scancode, const float value)
    {
        const int code = keyCodeOf(scancode);
        if (code >= 0) {
            _keyNow[code] = value;
        }
    }

    void KeyboardMouseSource::attach(Engine* engine)
    {
        InputSource::attach(engine);
        if (!engine) {
            return;
        }

        if (Mouse* mouse = engine->mouse()) {
            _handles.push_back(mouse->on("mousewheel", [this](const MouseEvent& e) {
                if (e.fromTouch || _pointerBlocked) {
                    return;
                }
                deltas.at("wheel").append({-e.wheelDelta * kWheelPixelsPerNotch});
            }));
            _handles.push_back(mouse->on("mousedown", [this](const MouseEvent& e) {
                const int index = buttonIndex(e.button);
                if (e.fromTouch || index < 0 || _pointerBlocked) {
                    return;
                }
                clearButtons();
                _button[index] = 1.0f;
                deltas.at("button").append({_button.begin(), _button.end()});
                _pointerDown = true;
            }));
            _handles.push_back(mouse->on("mousemove", [this](const MouseEvent& e) {
                // The pointer is captured on a press, so movement counts only
                // between a press and its release.
                if (e.fromTouch || !_pointerDown) {
                    return;
                }
                deltas.at("mouse").append({e.dx, e.dy});
            }));
            _handles.push_back(mouse->on("mouseup", [this](const MouseEvent& e) {
                if (e.fromTouch || !_pointerDown) {
                    return;
                }
                clearButtons();
                deltas.at("button").append({_button.begin(), _button.end()});
                _pointerDown = false;
            }));
        }

        if (Keyboard* keyboard = engine->keyboard()) {
            _handles.push_back(keyboard->on("keydown", [this](const KeyboardEvent& e) {
                setKey(static_cast<int>(e.key), 1.0f);
            }));
            _handles.push_back(keyboard->on("keyup", [this](const KeyboardEvent& e) {
                setKey(static_cast<int>(e.key), 0.0f);
            }));
        }
    }

    void KeyboardMouseSource::detach()
    {
        if (!_engine) {
            return;
        }
        for (const auto& handle : _handles) {
            handle->off();
        }
        _handles.clear();
        _keyNow.fill(0.0f);
        _keyPrev.fill(0.0f);
        _pointerDown = false;
        InputSource::detach();
    }

    InputValues KeyboardMouseSource::read()
    {
        std::vector<float> keys(KEY_COUNT);
        for (size_t i = 0; i < KEY_COUNT; ++i) {
            keys[i] = _keyNow[i] - _keyPrev[i];
            _keyPrev[i] = _keyNow[i];
        }
        deltas.at("key").append(keys);
        return InputSource::read();
    }
}
