// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.11.2026
//
// Color::toString / fromString: '#RRGGBB[AA]' with two hex digits per component whatever the
// value, so the string always reads back. An alpha that rounds to 0x10 or above keeps two
// digits (and one below keeps its leading zero); a negative component or an alpha above 1 is
// clamped to the byte range; an r, g or b above 1 (HDR) comes out as comma-separated numbers.

#include <string>

#include "core/math/color.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    bool roundTrips(const Color& color)
    {
        Color back;
        back.fromString(color.toString(true));
        return back.toString(true) == color.toString(true);
    }
}

int main()
{
    check(Color(1.0f, 0.5f, 0.0f, 1.0f).toString() == "#ff8000", "rgb without alpha: " +
        Color(1.0f, 0.5f, 0.0f, 1.0f).toString());
    check(Color(0.1f, 0.2f, 0.3f, 1.0f).toString(true) == "#1a334dff", "rgb with alpha");

    // Alpha just below 16/255 rounds UP to 0x10: two digits, not a padded three.
    const Color roundsUp(0.1f, 0.2f, 0.3f, 15.6f / 255.0f);
    check(roundsUp.toString(true) == "#1a334d10", "alpha rounding up to 0x10: " + roundsUp.toString(true));
    check(roundTrips(roundsUp), "alpha rounding up to 0x10 reads back");

    // Alpha below 16/255 keeps its leading zero.
    const Color small(0.1f, 0.2f, 0.3f, 5.0f / 255.0f);
    check(small.toString(true) == "#1a334d05", "small alpha keeps a leading zero: " + small.toString(true));

    // Out of range: clamped, never more or fewer than two digits.
    const Color overAlpha(0.1f, 0.2f, 0.3f, 1.5f);
    check(overAlpha.toString(true) == "#1a334dff", "alpha above 1 clamps: " + overAlpha.toString(true));
    const Color negative(-0.25f, 0.2f, 0.3f, -0.5f);
    check(negative.toString(true) == "#00334d00", "negative components clamp: " + negative.toString(true));
    check(roundTrips(overAlpha) && roundTrips(negative), "clamped colours read back");

    // HDR: numbers, not hex.
    check(Color(2.0f, 0.5f, 0.0f, 1.0f).toString().front() != '#', "an HDR colour is not written as hex");

    return finish("color-string");
}
