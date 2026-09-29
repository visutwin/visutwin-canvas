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

    ImageGeometry buildTiledImageGeometry(const Vector2& size, const Vector2& pivot, const TextureAtlasFrame& frame,
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

        // One axis as segments of (position from the low edge, frame pixel) pairs: the near
        // border, the middle cut into tiles of the inner region's natural size, the far
        // border. The grid and its shrink are the sliced grid's; the tile period shrinks with
        // it, as upstream's whole mesh scales by the same clamp.
        struct Segment
        {
            float p0, p1;    // positions from the low edge
            float px0, px1;  // frame pixels
        };
        const auto axis = [](const float extent, const float nearPx, const float farPx, const float framePx,
                             const float frameStartPx, const float unitsPerPx) {
            const float nearBorder = nearPx * unitsPerPx;
            const float farBorder = farPx * unitsPerPx;
            const float minimum = 2.0f * nearBorder;
            const float laidOut = std::max(extent, minimum);
            const float shrink = minimum > 0.0f ? std::clamp(extent / minimum, 0.0001f, 1.0f) : 1.0f;
            const float innerStart = shrink * nearBorder;
            const float innerEnd = shrink * (laidOut - farBorder);
            const float innerPx = framePx - nearPx - farPx;
            std::vector<Segment> segments;
            segments.push_back({0.0f, innerStart, frameStartPx, frameStartPx + nearPx});
            const float period = shrink * innerPx * unitsPerPx;
            if (innerPx > 0.0f && period > 1e-6f && innerEnd > innerStart) {
                // Tiles start at the inner region's low edge; the last one is cut short, its
                // texture coordinates with it. Bounded, so a degenerate frame cannot explode.
                constexpr int kMaxTiles = 4096;
                float p = innerStart;
                for (int n = 0; n < kMaxTiles && p < innerEnd - 1e-6f; ++n) {
                    const float end = std::min(p + period, innerEnd);
                    const float fraction = (end - p) / period;
                    segments.push_back({p, end, frameStartPx + nearPx, frameStartPx + nearPx + fraction * innerPx});
                    p = end;
                }
            } else {
                segments.push_back({innerStart, innerEnd, frameStartPx + nearPx, frameStartPx + framePx - farPx});
            }
            segments.push_back({innerEnd, extent, frameStartPx + framePx - farPx, frameStartPx + framePx});
            return segments;
        };
        const auto columns = axis(size.x, left, right, frameW, frameX, 1.0f / ppu);
        const auto rows = axis(size.y, bottom, top, frameH, frameY, 1.0f / ppu);

        const float originX = -pivot.x * size.x;
        const float originY = -pivot.y * size.y;

        ImageGeometry geometry;
        geometry.vertices.reserve(columns.size() * rows.size() * 4);
        geometry.indices.reserve(columns.size() * rows.size() * 6);
        for (const Segment& row : rows) {
            if (row.p1 - row.p0 <= 0.0f) {
                continue;
            }
            for (const Segment& col : columns) {
                if (col.p1 - col.p0 <= 0.0f) {
                    continue;
                }
                const auto base = static_cast<uint32_t>(geometry.vertices.size());
                const float u0 = col.px0 / texW;
                const float u1 = col.px1 / texW;
                const float v0 = 1.0f - row.px0 / texH;   // bottom
                const float v1 = 1.0f - row.px1 / texH;   // top
                geometry.vertices.push_back({originX + col.p0, originY + row.p0, u0, v0});
                geometry.vertices.push_back({originX + col.p1, originY + row.p0, u1, v0});
                geometry.vertices.push_back({originX + col.p1, originY + row.p1, u1, v1});
                geometry.vertices.push_back({originX + col.p0, originY + row.p1, u0, v1});
                geometry.indices.insert(geometry.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            }
        }
        return geometry;
    }
}
