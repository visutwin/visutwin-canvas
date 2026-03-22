// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.07.2025.
//

#pragma once
#include <unordered_map>

#include "core/math/vector2.h"
#include "SDL3/SDL_events.h"

namespace visutwin::canvas
{
    enum class Key {
        W, A, S, D, Q, E
    };

    enum class MouseButton {
        Left,
        Right,
        Middle
    };

    class Input {
    public:
        Input(const int windowWidth, const int windowHeight): windowSize(windowWidth, windowHeight) {}

        void handleEvent(const SDL_Event& event);
        void newFrame(); // Call at the start of each frame

        [[nodiscard]] bool isKeyDown(Key key) const;
        [[nodiscard]] bool isKeyPressed(Key key) const; // Key just pressed this frame
        [[nodiscard]] bool isMouseButtonDown(MouseButton button) const;

        [[nodiscard]] const Vector2& getMouseDelta() const {
            return mouseDelta;
        }

        [[nodiscard]] bool mouseMoved() const {
            return mouseDelta.length() > 0.0f;
        }

        Vector2u getWindowSize() const
        {
            return windowSize;
        }

    private:
        Vector2 mousePosition{0.0f};
        Vector2 prevMousePosition{0.0f};
        Vector2 mouseDelta{0.0f};

        Vector2u windowSize{0};

        std::unordered_map<SDL_Scancode, bool> currentKeys;
        std::unordered_map<SDL_Scancode, bool> previousKeys;

        uint32_t mouseButtonMask = 0;
    };
}

