// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The unlit shader and material every gizmo shape and mesh line draws with.
//
// The shader, in MSL and GLSL: position only, a flat colour (`uColor`),
// discard below 1/255 alpha, clip z clamped into [-w, w] so a shape is never cut by
// the near or far plane, and — when `depth` is 0 or more — the fragment depth
// replaced by that constant (`uDepth`): the plane
// handles draw at depth 1, behind every other shape of the gizmo, and the rotate
// gizmo's angle guide lines at depth 0, in front of all of them.
//
// The colour is written as given, which is right on a gamma target, the only kind the
// gizmo layer draws to here.
// DEVIATION: under a camera frame (linear HDR scene target) upstream would write the
// decoded colour and let compose encode it; this material does not see that pass state.
//
// Every material shares ONE shader per device, so twenty shapes cost one compile.
//
#pragma once

#include <memory>

#include "core/math/color.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    class GraphicsDevice;

    class GizmoMaterial final : public Material
    {
    public:
        explicit GizmoMaterial(const std::shared_ptr<GraphicsDevice>& device);

        const Color& color() const { return _color; }
        void setColor(const Color& color);

        /// -1 keeps the interpolated depth; 0..1 writes that depth for every fragment.
        float depth() const { return _depth; }
        void setDepth(float depth);

        const void* customUniformData(size_t& outSize) const override;

    private:
        struct alignas(16) Block
        {
            float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float depth[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
        };

        Color _color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float _depth = -1.0f;
        Block _block;
    };
}
