// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "extras/input/input.h"
#include "virtualJoystick.h"

namespace visutwin::canvas
{
    /**
     * Splits the canvas into a left and a right half, each read as a joystick or as a
     * touch pad by `layout` ("joystick-touch" by default; any of joystick|touch on
     * each side). Fills `leftInput` and `rightInput` (a joystick's value every read, a
     * pad's movement in canvas points) and `doubleTap`. Fires
     * `joystick:position:left` / `:right` with a std::array<float, 4> of base and stick
     * positions for a UI to draw.
     *
     * DEVIATION: listens to the engine's TouchDevice rather than DOM pointer events.
     */
    class DualGestureSource : public InputSource
    {
    public:
        explicit DualGestureSource(const std::string& layout = "joystick-touch");
        ~DualGestureSource() override;

        void setLayout(const std::string& layout);
        [[nodiscard]] const std::string& layout() const { return _layout; }

        VirtualJoystick& leftJoystick() { return _leftJoystick; }
        VirtualJoystick& rightJoystick() { return _rightJoystick; }

        void attach(Engine* engine) override;
        void detach() override;
        InputValues read() override;

    private:
        struct Pointer
        {
            int64_t id = 0;
            float x = 0.0f;
            float y = 0.0f;
            bool left = false;
        };

        [[nodiscard]] bool leftIsJoystick() const { return _layout.starts_with("joystick"); }
        [[nodiscard]] bool rightIsJoystick() const { return _layout.ends_with("joystick"); }

        void onDown(int64_t id, float x, float y);
        void onMove(int64_t id, float x, float y);
        void onUp(int64_t id);

        std::string _layout = "joystick-touch";
        std::vector<Pointer> _pointers;
        float _lastX = 0.0f;
        float _lastY = 0.0f;
        std::chrono::steady_clock::time_point _lastTime{};
        VirtualJoystick _leftJoystick;
        VirtualJoystick _rightJoystick;
        std::vector<EventHandlePtr> _handles;
    };
}
