// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#include "immediate.h"

namespace visutwin::canvas
{
    Immediate::OverlayCommand* Immediate::addOverlayLine(const Vector3& start, const Vector3& end, const Color& color, const float thickness)
    {
        auto* command = _overlayPool.allocate();
        command->start = start;
        command->end = end;
        command->color = color;
        command->thickness = thickness;
        _overlayCommands.push_back(command);
        return command;
    }

    void Immediate::onPostRender()
    {
        _overlayCommands.clear();
        _overlayPool.freeAll();
    }
}
