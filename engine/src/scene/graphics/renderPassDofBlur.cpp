// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
//
// Part of the multi-pass DOF pipeline (CoC -> Downsample -> Blur) that
// RenderPassCameraFrame::setupDofPass builds.
//
#include "renderPassDofBlur.h"

#include <algorithm>
#include <cmath>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        struct alignas(16) DofBlurUniforms
        {
            float radii[4];  // x=blurRadiusNear, y=blurRadiusFar (both in UV, i.e. / referenceHeight), z=near aspect (h/w, 0 = no near texture), w=far aspect
            float rings[4];  // x=blurRings, y=blurRingPoints
        };
        static_assert(sizeof(DofBlurUniforms) == 32);

        // Concentric sample kernel.
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

    void RenderPassDofBlur::prepareShaders()
    {
        if (!shader()) {
            useSlangShader("dof-blur");
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

        // Normally there already: the frame graph prepares every pass's shaders first.
        prepareShaders();
        if (!shader()) {
            return;
        }

        // The authored radius is a fraction of a 540-row
        // reference frame, so it reads the same at any resolution.
        constexpr float referenceHeight = 540.0f;
        (void)width;
        (void)height;
        DofBlurUniforms uniforms{};
        uniforms.radii[0] = _blurRadiusNear / referenceHeight;
        uniforms.radii[1] = _blurRadiusFar / referenceHeight;
        const auto aspect = [](const Texture* t) {
            return (t && t->width() > 0) ? static_cast<float>(t->height()) / static_cast<float>(t->width()) : 0.0f;
        };
        uniforms.radii[2] = aspect(_nearTexture);
        uniforms.radii[3] = _farTexture ? aspect(_farTexture) : 1.0f;
        uniforms.rings[0] = static_cast<float>(_blurRings);
        uniforms.rings[1] = static_cast<float>(_blurRingPoints);

        setQuadTextureBinding(0, _farTexture);
        setQuadTextureBinding(1, _cocTexture);
        setQuadTextureBinding(2, _nearTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }

    void RenderPassDofBlur::rebuildKernel()
    {
        _kernel = makeConcentricKernel(_blurRings, _blurRingPoints);
    }
}

