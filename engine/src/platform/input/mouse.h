// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include <array>

#include <SDL3/SDL_events.h>

#include "core/eventHandler.h"
#include "inputConstants.h"

namespace visutwin::canvas
{
    /**
     * Manages mouse input by tracking button states and dispatching events.
     *
     * State (`isPressed`) and edges (`wasPressed`, `wasReleased`) split the same way
     * as on Keyboard, and record transitions for the same reason: a click that
     * begins and ends inside one frame reports both edges rather than neither.
     *
     * Events are `mousedown`, `mouseup`, `mousemove` and `mousewheel`, each carrying
     * a MouseEvent. Movement deltas live on the EVENT rather than being accumulated
     * here, because a frame can contain several move events and a drag wants each
     * one: summing them into a per-frame delta is the caller's choice, not the
     * device's. `deltaX()` and `deltaY()` offer that sum for callers that want it.
     */
    class Mouse : public EventHandler
    {
    public:
        /// Feed one SDL event. Ignores everything that is not a mouse event.
        void handleEvent(const SDL_Event& event);

        /// Stop tracking and release every held button, for the reason Keyboard
        /// gives: a button held while focus moves away sends no button-up.
        void detach();

        /// Ends the frame: clears the recorded transitions and the accumulated
        /// movement and wheel deltas. Engine calls it.
        void update();

        [[nodiscard]] bool isPressed(MouseButton button) const;
        [[nodiscard]] bool wasPressed(MouseButton button) const;
        [[nodiscard]] bool wasReleased(MouseButton button) const;

        /// Cursor position in window pixels, top-left origin.
        [[nodiscard]] float x() const { return _x; }
        [[nodiscard]] float y() const { return _y; }

        /// Movement summed over this frame so far, and wheel notches likewise.
        [[nodiscard]] float deltaX() const { return _deltaX; }
        [[nodiscard]] float deltaY() const { return _deltaY; }
        [[nodiscard]] float wheelDelta() const { return _wheelDelta; }

        /// Relative mouse mode: the cursor is hidden and locked in place while
        /// movement keeps arriving as deltas. Upstream's pointer lock. Needs the
        /// window, which the application owns.
        void setRelativeMode(SDL_Window* window, bool enabled);
        [[nodiscard]] bool relativeMode() const { return _relativeMode; }

        [[nodiscard]] bool attached() const { return _attached; }

    private:
        static size_t index(MouseButton button);

        std::array<bool, 4> _buttons{};
        std::array<bool, 4> _pressedThisFrame{};
        std::array<bool, 4> _releasedThisFrame{};

        float _x = 0.0f;
        float _y = 0.0f;
        float _deltaX = 0.0f;
        float _deltaY = 0.0f;
        float _wheelDelta = 0.0f;
        bool _relativeMode = false;
        bool _attached = false;
    };
}
