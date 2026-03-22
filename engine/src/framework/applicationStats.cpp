// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#include "applicationStats.h"

namespace visutwin::canvas
{
    void ApplicationStats::setFrameStats(double now, float dt, float ms)
    {
        _frame.dt = dt;
        _frame.ms = ms;
        if (now > _frame.timeToCountFrames) {
            _frame.fps = _frame.fpsAccum;
            _frame.fpsAccum = 0;
            _frame.timeToCountFrames = now + 1000;
        } else {
            _frame.fpsAccum++;
        }
    }
}