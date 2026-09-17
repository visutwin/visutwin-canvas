// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include "platform/graphics/constants.h"

namespace visutwin::canvas
{
    class GraphNode;
    class Material;

    /// The material's own cull mode, honouring a `material_cullMode` / `cullMode`
    /// parameter override. Reads the parameter map, so cache it per bound material
    /// in a hot loop.
    CullMode resolveMaterialCullMode(const Material* material);

    /// Swaps front and back culling for a node whose world transform is mirrored
    /// (negative determinant), as the glTF specification asks of a renderer.
    CullMode applyNodeScaleFlip(CullMode mode, GraphNode* node);

    /// Both of the above: the cull mode one draw of `material` on `node` needs.
    /// Every pass that rasterises a mesh instance owes this — the forward pass and
    /// the depth-only passes (shadows, prepass) alike. A pass that leaves the
    /// device's cull mode alone inherits whatever the previous draw set, which is a
    /// different answer on the first frame (the device default) than on every
    /// frame after (the last quad of the previous frame).
    CullMode resolveCullMode(const Material* material, GraphNode* node);
}
