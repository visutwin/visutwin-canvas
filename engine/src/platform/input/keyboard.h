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
     * Manages keyboard input by tracking key states and dispatching events
     */
    class Keyboard : public EventHandler
    {
    public:
        void detach();

        void update() {}
    };
}
