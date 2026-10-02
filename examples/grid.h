// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The grid the gizmo examples stand their scene on, in place of the Grid script those
// examples share upstream. Upstream's is a pristine-grid shader on a blended plane; this
// one is drawn with a WideLineRenderer: opaque 1-pixel lines every unit over the plane's
// scaled extent in 0.7 grey, the x = 0 line in colorZ and the z = 0 line in colorX. The
// anti-aliased coverage alpha and the 0.1-unit HIGH resolution level are not reproduced,
// so the lines read brighter than upstream's.
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "framework/engine.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"

namespace visutwin::canvas
{
    class Grid
    {
    public:
        /// A grid over a plane scaled (scaleX, 1, scaleZ), drawn for a canvas of the given
        /// size in points.
        Grid(Engine* engine, const std::shared_ptr<GraphicsDevice>& device, const float canvasWidth,
             const float canvasHeight, const float scaleX, const float scaleZ)
            : _renderer(std::make_unique<WideLineRenderer>(engine, device))
        {
            _renderer->setScreenSize(canvasWidth, canvasHeight);

            const Color lineColor(0.7f, 0.7f, 0.7f, 1.0f);
            const Color colorX(1.0f, 0.3f, 0.3f, 1.0f);
            const Color colorZ(0.3f, 0.3f, 1.0f, 1.0f);
            const float hx = scaleX * 0.5f;
            const float hz = scaleZ * 0.5f;

            for (int i = static_cast<int>(-hx); i <= static_cast<int>(hx); ++i) {
                auto& line = _lines.emplace_back(std::make_unique<WideLine>());
                const auto x = static_cast<float>(i);
                line->setPoints({Vector3(x, 0.0f, -hz), Vector3(x, 0.0f, hz)}, i == 0 ? colorZ : lineColor, 1.0f);
            }
            for (int i = static_cast<int>(-hz); i <= static_cast<int>(hz); ++i) {
                auto& line = _lines.emplace_back(std::make_unique<WideLine>());
                const auto z = static_cast<float>(i);
                line->setPoints({Vector3(-hx, 0.0f, z), Vector3(hx, 0.0f, z)}, i == 0 ? colorX : lineColor, 1.0f);
            }
            for (auto& line : _lines) {
                _renderer->add(line.get());
            }
        }

        /// Call once a frame.
        void update() { _renderer->update(); }

    private:
        // Declared after the lines, so it lets go of them before they are freed.
        std::vector<std::unique_ptr<WideLine>> _lines;
        std::unique_ptr<WideLineRenderer> _renderer;
    };
}
