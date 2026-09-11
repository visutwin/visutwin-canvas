// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include <array>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include "core/eventHandler.h"

namespace visutwin::canvas
{
    /// Buttons in SDL's canonical layout. A pad is described in terms of where a
    /// button SITS, not what its face says, so the same binding works across pads
    /// whose faces are lettered differently.
    enum class PadButton : int
    {
        South = SDL_GAMEPAD_BUTTON_SOUTH,
        East = SDL_GAMEPAD_BUTTON_EAST,
        West = SDL_GAMEPAD_BUTTON_WEST,
        North = SDL_GAMEPAD_BUTTON_NORTH,
        Back = SDL_GAMEPAD_BUTTON_BACK,
        Guide = SDL_GAMEPAD_BUTTON_GUIDE,
        Start = SDL_GAMEPAD_BUTTON_START,
        LeftStick = SDL_GAMEPAD_BUTTON_LEFT_STICK,
        RightStick = SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        LeftShoulder = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        RightShoulder = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        DPadUp = SDL_GAMEPAD_BUTTON_DPAD_UP,
        DPadDown = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        DPadLeft = SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        DPadRight = SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    };

    enum class PadAxis : int
    {
        LeftX = SDL_GAMEPAD_AXIS_LEFTX,
        LeftY = SDL_GAMEPAD_AXIS_LEFTY,
        RightX = SDL_GAMEPAD_AXIS_RIGHTX,
        RightY = SDL_GAMEPAD_AXIS_RIGHTY,
        LeftTrigger = SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
        RightTrigger = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    };

    /**
     * Connected game pads, addressed by index in connection order.
     *
     * Fires `gamepadconnected` and `gamepaddisconnected` with the pad's index.
     * Button state follows the same now/edge split as Keyboard and Mouse.
     *
     * DEVIATION: upstream carries mapping tables for a long list of pads because a
     * browser gamepad reports raw button indices. SDL already normalises every pad
     * it knows to one layout, so the tables have no counterpart here — PadButton IS
     * that layout. A pad SDL does not recognise is not reported, exactly as an
     * unmapped pad is unusable upstream.
     */
    class GamePads : public EventHandler
    {
    public:
        ~GamePads() override;

        /// Feed one SDL event. Connection and disconnection arrive this way; axis
        /// and button state is polled in update(), which is what SDL is built for.
        void handleEvent(const SDL_Event& event);

        /// Polls every connected pad and ends the frame for the edge queries.
        void update();

        [[nodiscard]] size_t count() const { return _pads.size(); }
        [[nodiscard]] std::string name(size_t index) const;

        [[nodiscard]] bool isPressed(size_t index, PadButton button) const;
        [[nodiscard]] bool wasPressed(size_t index, PadButton button) const;
        [[nodiscard]] bool wasReleased(size_t index, PadButton button) const;

        /// Sticks are -1..1 with Y positive DOWN, as SDL reports them; triggers are
        /// 0..1. Values inside `deadZone` of centre read as exactly zero, because a
        /// stick at rest reports small nonzero values that would otherwise drift a
        /// camera forever.
        [[nodiscard]] float axis(size_t index, PadAxis axis) const;

        void setDeadZone(const float value) { _deadZone = value; }
        [[nodiscard]] float deadZone() const { return _deadZone; }

    private:
        static constexpr size_t kButtonCount = SDL_GAMEPAD_BUTTON_COUNT;
        static constexpr size_t kAxisCount = SDL_GAMEPAD_AXIS_COUNT;

        struct Pad
        {
            SDL_Gamepad* handle = nullptr;
            SDL_JoystickID id = 0;
            std::array<bool, kButtonCount> buttons{};
            std::array<bool, kButtonCount> lastButtons{};
            std::array<float, kAxisCount> axes{};
        };

        void add(SDL_JoystickID id);
        void remove(SDL_JoystickID id);
        [[nodiscard]] const Pad* pad(size_t index) const;

        std::vector<Pad> _pads;
        float _deadZone = 0.15f;
    };
}
