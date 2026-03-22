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
     * Manages mouse input by tracking button states and dispatching events
     */
    class Mouse : public EventHandler
    {
    public:
        // Remove mouse events from the element that it is attached to
        void detach();

        void update() {}
    };
}
