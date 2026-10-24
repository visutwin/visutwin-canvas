// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The unlit shader and material every gizmo shape and mesh line draws with.
//
// The Slang program `gizmo`: position only, a flat colour, discard below 1/255 alpha, clip z
// clamped into [-w, w] so a shape is never cut by the near or far plane, and — when `depth`
// is 0 or more — the fragment depth replaced by that constant: the plane handles draw at
// depth 1, behind every other shape of the gizmo, and the rotate gizmo's angle guide lines
// at depth 0, in front of all of them. With a texture (an icon glyph) the alpha is scaled by
// its coverage. The colour is gamma encoded, and decoded per draw for a linear HDR target.
//
// Every material shares one library per device (two variants: plain and textured).
//
#pragma once

#include <memory>

#include "core/math/color.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    class GizmoMaterial final : public Material
    {
    public:
        explicit GizmoMaterial(const std::shared_ptr<GraphicsDevice>& device);

        const Color& color() const { return _color; }
        void setColor(const Color& color);

        /// -1 keeps the interpolated depth; 0..1 writes that depth for every fragment.
        float depth() const { return _depth; }
        void setDepth(float depth);

        /// Whether the material tests and writes the depth buffer (both on by default).
        void setDepthTest(bool value);
        void setDepthWrite(bool value);
        bool depthTest() const { return _depthTest; }
        bool depthWrite() const { return _depthWrite; }

        /// Polygon offset in depth-buffer units, constant and slope alike: positive values
        /// push the shape away from the camera, so it does not fight a surface it lies on.
        void setDepthBias(float value);
        float depthBias() const { return _depthBias; }

        /// A texture whose alpha scales the colour's (an icon glyph), or null for none.
        void setCoverageTexture(Texture* texture);

        const void* customUniformData(size_t& outSize) const override;

    private:
        void updateDepthState();
        void updateShader();

        struct alignas(16) Block
        {
            float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float depth[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
        };

        std::shared_ptr<GraphicsDevice> _device;
        Color _color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float _depth = -1.0f;
        bool _depthTest = true;
        bool _depthWrite = true;
        float _depthBias = 0.0f;
        bool _textured = false;
        Block _block;
        mutable Block _drawn;
    };
}
