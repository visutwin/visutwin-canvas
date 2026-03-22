// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.10.2025.
//
#pragma once

namespace visutwin::canvas
{
    enum class FillMode
    {
        FILLMODE_NONE,          // When resizing the window the size of the canvas will not change
        FILLMODE_FILL_WINDOW,   // When resizing the window the size of the canvas will change to fill the window exactly
        FILLMODE_KEEP_ASPECT    // When resizing the window the size of the canvas will change to fill the window as best it can, while maintaining the same aspect ratio
    };

    enum class ResolutionMode
    {
        RESOLUTION_AUTO,    // When the canvas is resized the resolution of the canvas will change to match the size of the canvas
        RESOLUTION_FIXED    // When the canvas is resized the resolution of the canvas will remain at the same value and the output will just be scaled to fit the canva
    };
}