// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#pragma once

#include <cstdint>
#include <utility>

#include "extras/input/input.h"

namespace visutwin::canvas
{
    /**
     * Fills `touch` (one finger's movement, or with two or more the movement of the
     * first two fingers' midpoint), `count` (+1 per finger down, -1 per finger up)
     * and `pinch` (the decrease in the first two fingers' distance), in canvas points.
     *
     * DEVIATION: listens to the engine's TouchDevice rather than DOM pointer events.
     */
    class MultiTouchSource : public InputSource
    {
    public:
        MultiTouchSource();
        ~MultiTouchSource() override;

        void attach(Engine* engine) override;
        void detach() override;

    private:
        struct Pointer
        {
            int64_t id = 0;
            float x = 0.0f;
            float y = 0.0f;
        };

        Pointer* find(int64_t id);
        std::pair<float, float> midPoint() const;
        float pinchDist() const;

        void onDown(int64_t id, float x, float y);
        void onMove(int64_t id, float x, float y);
        void onUp(int64_t id);

        std::vector<Pointer> _pointers;     // in touch-down order
        float _pointerX = 0.0f;
        float _pointerY = 0.0f;
        float _pinchDist = -1.0f;
        std::vector<EventHandlePtr> _handles;
    };
}
