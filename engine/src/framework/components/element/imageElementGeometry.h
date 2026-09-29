// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The geometry an image element draws (upstream image-element.js `_updateMesh`), in the
// element's LOCAL space: the origin at its pivot, x right, y up, in screen units.
//
// SIMPLE: one quad over the element's rectangle — after the fit mode, as upstream, which
// shrinks the quad about the pivot rather than centring it in the box — with UVs from the
// element's rect or the sprite frame.
//
// SLICED: the 4x4-vertex grid upstream builds in its vertex shader, built here on the CPU
// from the same formulas (DEVIATION, see scene/sprite.h). The borders keep the size of
// their pixels over pixelsPerUnit and the centre stretches; an element narrower than
// TWICE its LEFT border (upstream compares against the left inset only, on x, and the
// bottom on y) shrinks the whole grid to fit instead of letting the borders overlap.
//
// UVs follow this engine's texture convention, v = 0 at the image's TOP row, and a frame
// rect is measured from the image's BOTTOM (TextureAtlasFrame), so a point `y` pixels up
// from the bottom samples v = 1 - y / textureHeight.
//
// Pure functions of their inputs, so the layout can be tested without a device.
//
#pragma once

#include <cstdint>
#include <vector>

#include "core/math/vector2.h"
#include "core/math/vector4.h"

namespace visutwin::canvas
{
    struct TextureAtlasFrame;

    /// Upstream FITMODE_STRETCH / CONTAIN / COVER.
    enum class ElementFitMode
    {
        Stretch,
        Contain,
        Cover
    };

    struct ImageVertex
    {
        float x = 0.0f;
        float y = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
    };

    struct ImageGeometry
    {
        std::vector<ImageVertex> vertices;
        std::vector<uint32_t> indices;
    };

    /// The drawn size for an element of `width` x `height` whose image has `aspect`
    /// (width / height; <= 0 means unknown, which stretches), under `fitMode`.
    Vector2 fitImageSize(float width, float height, float aspect, ElementFitMode fitMode);

    /// One quad of `size` about `pivot`. `uvRect` is x, y (from the BOTTOM), width, height
    /// as fractions of the texture — an element's `rect`, or a frame rect over the texture
    /// size.
    ImageGeometry buildSimpleImageGeometry(const Vector2& size, const Vector2& pivot, const Vector4& uvRect);

    /// The 9-slice grid of `frame` (pixels) from a texture of `textureWidth` x
    /// `textureHeight`, drawn at `size` about `pivot` with `pixelsPerUnit`.
    ImageGeometry buildSlicedImageGeometry(const Vector2& size, const Vector2& pivot, const TextureAtlasFrame& frame,
                                           float textureWidth, float textureHeight, float pixelsPerUnit);
}
