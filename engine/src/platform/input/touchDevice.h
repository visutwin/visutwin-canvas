// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025
//
#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_touch.h>

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
     * Positions are window POINTS, the space Mouse and the UI's element input report in
     * (not the back buffer's pixels). SDL reports touches in normalized [0,1] window
     * coordinates, so the device has to be told the window size to convert; without it
     * the positions stay normalized, which is wrong rather than merely unscaled, so
     * setWindowSize is not optional for a touch application. Engine sets it from
     * `canvasSize()` and keeps it current on resize.
     *
     * Only a touch SCREEN's fingers are touches. A trackpad's contacts arrive as SDL
     * finger events too, but they move the pointer, which is reported as mouse events;
     * counted here as well, every trackpad gesture would also be a touch at the window
     * position the trackpad's coordinates happen to map to.
     */
    class TouchDevice : public EventHandler
    {
    public:
        void handleEvent(const SDL_Event& event);

        /// Drop every tracked finger. A touch in progress when the window loses
        /// focus sends no touch-end, and would otherwise stay in the list forever.
        void detach();

        /// The window size in points.
        void setWindowSize(int width, int height);

        /// Which devices' fingers are touches: by default the direct (touch screen) ones,
        /// `isDirectTouchDevice`. A test, which cannot register an SDL touch device, passes
        /// its own; null restores the default.
        void setDeviceFilter(std::function<bool(SDL_TouchID)> filter) { _deviceFilter = std::move(filter); }

        /// Fingers currently in contact, in the order they touched down.
        [[nodiscard]] const std::vector<Touch>& touches() const { return _touches; }

        [[nodiscard]] bool attached() const { return _attached; }

    private:
        std::vector<Touch>::iterator find(int64_t id);

        bool acceptsDevice(SDL_TouchID touchId) const;

        std::vector<Touch> _touches;
        std::function<bool(SDL_TouchID)> _deviceFilter;
        float _width = 1.0f;
        float _height = 1.0f;
        bool _attached = false;
    };
}
