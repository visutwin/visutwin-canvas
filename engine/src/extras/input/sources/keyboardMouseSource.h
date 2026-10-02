// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <array>

#include "extras/input/input.h"

namespace visutwin::canvas
{
    /**
     * Fills `key` (one lane per KeyCode: +1 the frame a key went down, -1 the frame it
     * went up), `button` (left, middle, right: +1 on press, -1 on release), `mouse`
     * (movement in canvas points while a button is held) and `wheel` (in browser
     * wheel pixels, positive toward the user).
     *
     * DEVIATION: listens to the engine's Mouse and Keyboard rather than DOM pointer
     * events. A mouse event SDL synthesized from a touch is ignored, as upstream ignores
     * a pointer whose type is not 'mouse'; the wheel arrives in notches and is scaled
     * to a browser's 100 pixels a notch. There is no pointer lock.
     */
    class KeyboardMouseSource : public InputSource
    {
    public:
        enum KeyCode : int
        {
            A = 0, Z = 25,
            Digit0 = 26, Digit9 = 35,
            UP = 36, DOWN = 37, LEFT = 38, RIGHT = 39,
            SPACE = 40, SHIFT = 41, CTRL = 42,
            KEY_COUNT = 43
        };

        /// A..Z, 0..9 codes by letter or digit.
        static constexpr int letter(const char c) { return A + (c - 'A'); }

        KeyboardMouseSource();
        ~KeyboardMouseSource() override;

        void attach(Engine* engine) override;
        void detach() override;
        InputValues read() override;

        /// While set, presses and the wheel are ignored, as if the pointer were over
        /// something drawn on top of the canvas (a HUD). Drags already under way go on.
        void setPointerBlocked(const bool blocked) { _pointerBlocked = blocked; }

    private:
        void clearButtons();
        void setKey(int scancode, float value);

        std::array<float, KEY_COUNT> _keyPrev{};
        std::array<float, KEY_COUNT> _keyNow{};
        std::array<float, 3> _button{};
        bool _pointerDown = false;
        bool _pointerBlocked = false;
        std::vector<EventHandlePtr> _handles;
    };
}
