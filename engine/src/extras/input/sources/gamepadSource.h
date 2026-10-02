// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#pragma once

#include <array>

#include "extras/input/input.h"

namespace visutwin::canvas
{
    /**
     * Polls every connected pad on read(): `buttons` (one lane per ButtonCode, +1 the
     * read a button went down, -1 the read it came up), `leftStick` and `rightStick`
     * (summed over pads, y down).
     *
     * DEVIATION: reads the engine's GamePads, which SDL maps to one layout, so there is
     * no 'standard mapping' check. The triggers are analogue axes there and count as
     * pressed past half travel, the browser's threshold for a standard pad.
     */
    class GamepadSource : public InputSource
    {
    public:
        enum ButtonCode : int
        {
            A = 0, B, X, Y, LB, RB, LT, RT, SELECT, START, LEFT_STICK, RIGHT_STICK,
            BUTTON_COUNT
        };

        GamepadSource();

        InputValues read() override;

    private:
        std::array<float, BUTTON_COUNT> _buttonPrev{};
    };
}
