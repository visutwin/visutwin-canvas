// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#include "touchDevice.h"

#include <algorithm>

namespace visutwin::canvas
{
    std::vector<Touch>::iterator TouchDevice::find(const int64_t id)
    {
        return std::find_if(_touches.begin(), _touches.end(),
            [id](const Touch& touch) { return touch.id == id; });
    }

    void TouchDevice::setWindowSize(const int width, const int height)
    {
        _width = width > 0 ? static_cast<float>(width) : 1.0f;
        _height = height > 0 ? static_cast<float>(height) : 1.0f;
    }

    void TouchDevice::handleEvent(const SDL_Event& event)
    {
        const auto fireTouch = [this](const char* name, const Touch& changed) {
            fire(name, TouchEvent{.touches = _touches, .changed = {changed}});
        };

        switch (event.type) {
        case SDL_EVENT_FINGER_DOWN: {
            _attached = true;
            const Touch touch{
                .id = static_cast<int64_t>(event.tfinger.fingerID),
                .x = event.tfinger.x * _width,
                .y = event.tfinger.y * _height,
            };
            if (const auto it = find(touch.id); it != _touches.end()) {
                *it = touch;   // SDL reuses a finger id after release
            } else {
                _touches.push_back(touch);
            }
            fireTouch("touchstart", touch);
            break;
        }
        case SDL_EVENT_FINGER_MOTION: {
            _attached = true;
            const auto id = static_cast<int64_t>(event.tfinger.fingerID);
            const auto it = find(id);
            if (it == _touches.end()) {
                break;
            }
            it->x = event.tfinger.x * _width;
            it->y = event.tfinger.y * _height;
            fireTouch("touchmove", *it);
            break;
        }
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            const auto id = static_cast<int64_t>(event.tfinger.fingerID);
            const auto it = find(id);
            if (it == _touches.end()) {
                break;
            }
            const Touch released = *it;
            // Erased BEFORE the event: a handler asking which fingers remain must
            // not be told about the one that just left.
            _touches.erase(it);
            fireTouch(event.type == SDL_EVENT_FINGER_UP ? "touchend" : "touchcancel", released);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            detach();
            break;
        default:
            break;
        }
    }

    void TouchDevice::detach()
    {
        _touches.clear();
        _attached = false;
    }
}
