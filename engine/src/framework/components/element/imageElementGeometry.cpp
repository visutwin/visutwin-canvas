// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "imageElementGeometry.h"

#include <algorithm>
#include <array>

#include "scene/textureAtlas.h"

namespace visutwin::canvas
{
    Vector2 fitImageSize(const float width, const float height, const float aspect, const ElementFitMode fitMode)
    {
        float w = width;
        float h = height;
        if (fitMode != ElementFitMode::Stretch && aspect > 0.0f && height != 0.0f) {
            const float actual = width / height;
            // Which side must change to keep the image's aspect (upstream _updateMesh).
            if ((fitMode == ElementFitMode::Contain && actual > aspect) ||
                (fitMode == ElementFitMode::Cover && actual < aspect)) {
                w = height * aspect;
            } else {
                h = width / aspect;
            }
        }
        return Vector2(w, h);
    }

    ImageGeometry buildSimpleImageGeometry(const Vector2& size, const Vector2& pivot, const Vector4& uvRect)
    {
        const float x0 = -pivot.x * size.x;
        const float x1 = (1.0f - pivot.x) * size.x;
        const float y0 = -pivot.y * size.y;
        const float y1 = (1.0f - pivot.y) * size.y;

        const float u0 = uvRect.getX();
        const float u1 = uvRect.getX() + uvRect.getZ();
        const float vBottom = 1.0f - uvRect.getY();
        const float vTop = 1.0f - (uvRect.getY() + uvRect.getW());

        ImageGeometry geometry;
        // bottom left, bottom right, top right, top left
        geometry.vertices = {
            {x0, y0, u0, vBottom},
            {x1, y0, u1, vBottom},
            {x1, y1, u1, vTop},
            {x0, y1, u0, vTop},
        };
        geometry.indices = {0, 1, 2, 0, 2, 3};
        return geometry;
    }

    ImageGeometry buildSlicedImageGeometry(const Vector2& size, const Vector2& pivot, const TextureAtlasFrame& frame,
                                           const float textureWidth, const float textureHeight, const float pixelsPerUnit)
    {
        const float ppu = pixelsPerUnit > 0.0f ? pixelsPerUnit : 1.0f;
        const float texW = textureWidth > 0.0f ? textureWidth : 1.0f;
        const float texH = textureHeight > 0.0f ? textureHeight : 1.0f;
        const float frameX = frame.rect.getX();
        const float frameY = frame.rect.getY();
        const float frameW = frame.rect.getZ();
        const float frameH = frame.rect.getW();
        const float left = frame.border.getX();
        const float bottom = frame.border.getY();
        const float right = frame.border.getZ();
        const float top = frame.border.getW();

        // Upstream's vertex shader, solved for the final positions: the outer edges sit at
        // the element's rectangle; each inner edge sits a border's width (pixels over ppu)
        // inside it. When the rectangle is narrower than twice the left border (upstream's
        // outerScale max with innerOffset.x), the grid is laid out at that minimum width
        // and scaled down by width / minimum (upstream's node scale clamp), which keeps
        // the outer edges on the rectangle and pulls the inner ones in proportionally.
        const auto axis = [](const float extent, const float nearBorder, const float farBorder) {
            const float minimum = 2.0f * nearBorder;
            const float laidOut = std::max(extent, minimum);
            const float shrink = minimum > 0.0f ? std::clamp(extent / minimum, 0.0001f, 1.0f) : 1.0f;
            // positions from the rectangle's low edge
            return std::array<float, 4>{
                0.0f,
                shrink * nearBorder,
                shrink * (laidOut - farBorder),
                extent
            };
        };
        const std::array<float, 4> xs = axis(size.x, left / ppu, right / ppu);
        const std::array<float, 4> ys = axis(size.y, bottom / ppu, top / ppu);

        // The texture coordinates of the same four lines, in frame pixels.
        const std::array<float, 4> pxU = {frameX, frameX + left, frameX + frameW - right, frameX + frameW};
        const std::array<float, 4> pxV = {frameY, frameY + bottom, frameY + frameH - top, frameY + frameH};

        const float originX = -pivot.x * size.x;
        const float originY = -pivot.y * size.y;

        ImageGeometry geometry;
        geometry.vertices.reserve(16);
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                geometry.vertices.push_back({
                    originX + xs[col], originY + ys[row],
                    pxU[col] / texW, 1.0f - pxV[row] / texH
                });
            }
        }
        geometry.indices.reserve(54);
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t col = 0; col < 3; ++col) {
                const uint32_t i = row * 4 + col;
                geometry.indices.insert(geometry.indices.end(), {i, i + 1, i + 5, i, i + 5, i + 4});
            }
        }
        return geometry;
    }
}
