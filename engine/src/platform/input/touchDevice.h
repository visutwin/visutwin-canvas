// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once
#include "core/eventHandler.h"

namespace visutwin::canvas
{
    /**
     * Manages touch input by handling and dispatching touch events
     */
    class TouchDevice : public EventHandler
    {
    public:
        void detach();
    };
}
