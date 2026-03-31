// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassDofBlur.h"

#include <algorithm>
#include <cmath>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        // Concentric sample kernel equivalent to Kernel.concentric usage in the upstream engine.
        std::vector<float> makeConcentricKernel(const int rings, const int pointsPerRing)
        {
            std::vector<float> out;
            out.reserve(static_cast<size_t>(rings * pointsPerRing * 2));
            constexpr float twoPi = 6.28318530718f;
            for (int r = 1; r <= rings; ++r) {
                const float radius = static_cast<float>(r) / static_cast<float>(std::max(rings, 1));
                const int points = std::max(pointsPerRing * r, 1);
                for (int i = 0; i < points; ++i) {
                    const float angle = (static_cast<float>(i) / static_cast<float>(points)) * twoPi;
                    out.push_back(std::cos(angle) * radius);
                    out.push_back(std::sin(angle) * radius);
                }
            }
            if (out.empty()) {
                out.push_back(0.0f);
                out.push_back(0.0f);
            }
            return out;
        }
    }

    RenderPassDofBlur::RenderPassDofBlur(const std::shared_ptr<GraphicsDevice>& device, Texture* nearTexture,
        Texture* farTexture, Texture* cocTexture)
        : RenderPassShaderQuad(device), _nearTexture(nearTexture), _farTexture(farTexture), _cocTexture(cocTexture)
    {
        rebuildKernel();
    }

    void RenderPassDofBlur::setBlurRings(const int value)
    {
        const int clamped = std::max(value, 1);
        if (_blurRings != clamped) {
            _blurRings = clamped;
            rebuildKernel();
        }
    }

    void RenderPassDofBlur::setBlurRingPoints(const int value)
    {
        const int clamped = std::max(value, 1);
        if (_blurRingPoints != clamped) {
            _blurRingPoints = clamped;
            rebuildKernel();
        }
    }

    void RenderPassDofBlur::execute()
    {
        if (_kernel.empty()) {
            rebuildKernel();
        }

        const auto gd = device();
        if (!gd) {
            RenderPassShaderQuad::execute();
            return;
        }

        const auto rt = renderTarget();
        if (!rt || !rt->colorBuffer()) {
            RenderPassShaderQuad::execute();
            return;
        }

        const auto width = static_cast<float>(rt->colorBuffer()->width());
        const auto height = static_cast<float>(rt->colorBuffer()->height());
        if (width <= 0.0f || height <= 0.0f) {
            RenderPassShaderQuad::execute();
            return;
        }

        DofBlurPassParams params;
        params.nearTexture = _nearTexture;
        params.farTexture = _farTexture;
        params.cocTexture = _cocTexture;
        params.blurRadiusNear = blurRadiusNear;
        params.blurRadiusFar = blurRadiusFar;
        params.blurRings = _blurRings;
        params.blurRingPoints = _blurRingPoints;
        params.invResolutionX = 1.0f / width;
        params.invResolutionY = 1.0f / height;
        gd->executeDofBlurPass(params);
    }

    void RenderPassDofBlur::rebuildKernel()
    {
        _kernel = makeConcentricKernel(_blurRings, _blurRingPoints);
    }
}

