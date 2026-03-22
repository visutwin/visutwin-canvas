// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#pragma once

#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/objectPool.h"

namespace visutwin::canvas
{
    /**
     * Immediate rendering helper class for debug drawing and immediate mode rendering
     */
    class Immediate
    {
    public:
        struct OverlayCommand
        {
            Vector3 start = Vector3(0.0f);
            Vector3 end = Vector3(0.0f);
            Color color = Color(1.0f, 1.0f, 1.0f, 1.0f);
            float thickness = 1.0f;
        };

        // DEVIATION: this C++ port exposes a pooled overlay command API for debug instrumentation workflows.
        OverlayCommand* addOverlayLine(const Vector3& start, const Vector3& end, const Color& color, float thickness = 1.0f);

        const std::vector<OverlayCommand*>& overlays() const { return _overlayCommands; }

        // Called after the frame was rendered, clears data
        void onPostRender();

    private:
        ObjectPool<OverlayCommand> _overlayPool = ObjectPool<OverlayCommand>(64);
        std::vector<OverlayCommand*> _overlayCommands;
    };
}
