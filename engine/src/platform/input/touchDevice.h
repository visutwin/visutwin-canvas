// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include <vector>

#include <SDL3/SDL_events.h>

#include "core/eventHandler.h"
#include "inputConstants.h"

namespace visutwin::canvas
{
    /**
     * Manages touch input by handling and dispatching touch events.
     *
     * Fires `touchstart`, `touchmove`, `touchend` and `touchcancel`, each with a
     * TouchEvent carrying every finger in contact and the subset this event is
     * about. A gesture needs both lists: a pinch is defined by two fingers, only one
     * of which moved.
     *
     * Positions are window pixels, like Mouse. SDL reports touches in normalized
     * [0,1] window coordinates, so the device has to be told the window size to
     * convert; without it the positions stay normalized, which is wrong rather than
     * merely unscaled, so setWindowSize is not optional for a touch application.
     */
    class TouchDevice : public EventHandler
    {
    public:
        void handleEvent(const SDL_Event& event);

        /// Drop every tracked finger. A touch in progress when the window loses
        /// focus sends no touch-end, and would otherwise stay in the list forever.
        void detach();

        void setWindowSize(int width, int height);

        /// Fingers currently in contact, in the order they touched down.
        [[nodiscard]] const std::vector<Touch>& touches() const { return _touches; }

        [[nodiscard]] bool attached() const { return _attached; }

    private:
        std::vector<Touch>::iterator find(int64_t id);

        std::vector<Touch> _touches;
        float _width = 1.0f;
        float _height = 1.0f;
        bool _attached = false;
    };
}
