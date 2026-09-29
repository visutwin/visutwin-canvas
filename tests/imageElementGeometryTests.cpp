// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Image element geometry (framework/components/element/imageElementGeometry.h).
//
// Upstream slices a sprite in its VERTEX SHADER (transform.js / uv0.js with innerOffset,
// outerScale and atlasRect, the renderable node scaled and offset by image-element.js
// `_updateMesh`, and the fragment stage's `nineSlicedUv = (u, 1 - v)`); the port builds
// the same grid on the CPU. The oracle here is that shader path ported LITERALLY, every
// step as upstream spells it, so the test cannot share a derivation with the code it checks.
//
// Orientation is pinned separately: a frame rect is measured from the image's BOTTOM and
// this engine samples v = 0 at the image's TOP row, so the bottom edge of a quad must
// sample 1 - rect.y / textureHeight.

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "framework/components/element/imageElementGeometry.h"
#include "scene/textureAtlas.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool near(const float a, const float b, const float eps = 1e-4f) { return std::abs(a - b) <= eps; }

    /// Upstream's sliced vertex, ported literally: the 4x4 mesh from Sprite._create9SliceMesh
    /// (x, z in {+1, -1}, texCoord0 = 1 on the inner lines), transform.js and uv0.js under
    /// NINESLICED, the renderable node's scale and position from _updateMesh, and the
    /// fragment stage's v flip. Returns element-local x, y and the sampled u, v.
    ImageVertex upstreamSlicedVertex(const int i, const int j, const Vector2& size, const Vector2& pivot,
                                     const TextureAtlasFrame& frame, const float texW, const float texH, const float ppu)
    {
        // Sprite._create9SliceMesh: ws = ls = 3, he = (1, 1)
        const float meshX = -(-1.0f + 2.0f * (i <= 1 ? 0.0f : 3.0f) / 3.0f);   // pushed as -x
        const float meshZ = -(-1.0f + 2.0f * (j <= 1 ? 0.0f : 3.0f) / 3.0f);
        const float tcX = (i == 0 || i == 3) ? 0.0f : 1.0f;
        const float tcY = (j == 0 || j == 3) ? 0.0f : 1.0f;

        // image-element.js _updateMesh (fit mode stretch: w, h are the calculated size)
        const float w = size.x;
        const float h = size.y;
        const float bws = 2.0f / frame.rect.getZ();
        const float bhs = 2.0f / frame.rect.getW();
        const float ioX = frame.border.getX() * bws;
        const float ioY = frame.border.getY() * bhs;
        const float ioZ = frame.border.getZ() * bws;
        const float ioW = frame.border.getW() * bhs;
        const float arX = frame.rect.getX() / texW;
        const float arY = frame.rect.getY() / texH;
        const float arZ = frame.rect.getZ() / texW;
        const float arW = frame.rect.getW() / texH;
        const float scaleMulX = frame.rect.getZ() / ppu;
        const float scaleMulY = frame.rect.getW() / ppu;
        const float outerX = std::max(w, ioX * scaleMulX) / scaleMulX;
        const float outerY = std::max(h, ioY * scaleMulY) / scaleMulY;
        // math.clamp(w / 0) is clamp(Infinity) = 1 in JS
        const auto clampRatio = [](const float num, const float den) {
            return den > 0.0f ? std::clamp(num / den, 0.0001f, 1.0f) : 1.0f;
        };
        const float scaleX = scaleMulX * clampRatio(w, ioX * scaleMulX);
        const float scaleY = scaleMulY * clampRatio(h, ioY * scaleMulY);
        const float nodeX = (0.5f - pivot.x) * w;
        const float nodeY = (0.5f - pivot.y) * h;

        // transform.js
        float lx = meshX * outerX;
        float lz = meshZ * outerY;
        const float posX = std::clamp(meshX, 0.0f, 1.0f);
        const float posZ = std::clamp(meshZ, 0.0f, 1.0f);
        const float negX = std::clamp(-meshX, 0.0f, 1.0f);
        const float negZ = std::clamp(-meshZ, 0.0f, 1.0f);
        lx += (-posX * ioX + negX * ioZ) * tcX;
        lz += (-posZ * ioY + negZ * ioW) * tcY;
        lx *= -0.5f;
        lz *= -0.5f;
        // localPos = localPos.xzy, then the node's scale and position
        const float x = lx * scaleX + nodeX;
        const float y = lz * scaleY + nodeY;

        // uv0.js
        float u = meshX + (-posX * ioX + negX * ioZ) * tcX;
        float v = meshZ + (-posZ * ioY + negZ * ioW) * tcY;
        u = u * -0.5f + 0.5f;
        v = v * -0.5f + 0.5f;
        u = u * arZ + arX;
        v = v * arW + arY;
        // startNineSliced.js
        v = 1.0f - v;
        return {x, y, u, v};
    }

    void compareWithUpstream(const std::string& label, const Vector2& size, const Vector2& pivot,
                             const TextureAtlasFrame& frame, const float texW, const float texH, const float ppu)
    {
        const ImageGeometry geometry = buildSlicedImageGeometry(size, pivot, frame, texW, texH, ppu);
        bool ok = geometry.vertices.size() == 16 && geometry.indices.size() == 54;
        float worst = 0.0f;
        for (int row = 0; ok && row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                const ImageVertex ours = geometry.vertices[static_cast<size_t>(row * 4 + col)];
                // Upstream's column index i runs left to right, and row j bottom to top.
                const ImageVertex theirs = upstreamSlicedVertex(col, row, size, pivot, frame, texW, texH, ppu);
                worst = std::max({worst, std::abs(ours.x - theirs.x), std::abs(ours.y - theirs.y),
                                  std::abs(ours.u - theirs.u), std::abs(ours.v - theirs.v)});
            }
        }
        check(ok && worst < 1e-4f, label + " matches upstream's shader path (worst " + std::to_string(worst) + ")");
    }
}

int main()
{
    std::cout << "simple quad\n";
    {
        const ImageGeometry g = buildSimpleImageGeometry(Vector2(100.0f, 50.0f), Vector2(0.5f, 0.5f),
                                                         Vector4(0.0f, 0.0f, 1.0f, 1.0f));
        check(g.vertices.size() == 4 && g.indices.size() == 6, "four vertices, two triangles");
        check(near(g.vertices[0].x, -50.0f) && near(g.vertices[0].y, -25.0f), "bottom left about the pivot");
        check(near(g.vertices[2].x, 50.0f) && near(g.vertices[2].y, 25.0f), "top right about the pivot");
        check(near(g.vertices[0].v, 1.0f) && near(g.vertices[2].v, 0.0f),
              "the bottom edge samples the image's bottom row (v = 1), the top edge its top (v = 0)");
    }
    {
        // Upstream's rect: x, y from the bottom, width, height.
        const ImageGeometry g = buildSimpleImageGeometry(Vector2(10.0f, 10.0f), Vector2(0.0f, 0.0f),
                                                         Vector4(0.25f, 0.5f, 0.5f, 0.25f));
        check(near(g.vertices[0].x, 0.0f) && near(g.vertices[2].x, 10.0f), "pivot (0, 0) puts the origin at the corner");
        check(near(g.vertices[0].u, 0.25f) && near(g.vertices[1].u, 0.75f), "u spans the rect");
        check(near(g.vertices[0].v, 0.5f) && near(g.vertices[3].v, 0.25f), "v = 1 - y from the bottom");
    }

    std::cout << "fit modes\n";
    {
        const Vector2 stretch = fitImageSize(200.0f, 100.0f, 1.0f, ElementFitMode::Stretch);
        const Vector2 contain = fitImageSize(200.0f, 100.0f, 1.0f, ElementFitMode::Contain);
        const Vector2 cover = fitImageSize(200.0f, 100.0f, 1.0f, ElementFitMode::Cover);
        const Vector2 unknown = fitImageSize(200.0f, 100.0f, -1.0f, ElementFitMode::Contain);
        check(near(stretch.x, 200.0f) && near(stretch.y, 100.0f), "stretch keeps the rectangle");
        check(near(contain.x, 100.0f) && near(contain.y, 100.0f), "contain fits a square inside 200x100");
        check(near(cover.x, 200.0f) && near(cover.y, 200.0f), "cover fills 200x100 with a square");
        check(near(unknown.x, 200.0f) && near(unknown.y, 100.0f), "an unknown aspect stretches");
        const Vector2 tall = fitImageSize(100.0f, 200.0f, 1.0f, ElementFitMode::Contain);
        check(near(tall.x, 100.0f) && near(tall.y, 100.0f), "contain in a tall rectangle shrinks the height");
    }

    std::cout << "sliced grid\n";
    {
        TextureAtlasFrame frame;
        frame.rect = Vector4(100.0f, 200.0f, 64.0f, 32.0f);
        frame.border = Vector4(16.0f, 8.0f, 16.0f, 8.0f);
        const ImageGeometry g = buildSlicedImageGeometry(Vector2(120.0f, 40.0f), Vector2(0.5f, 0.0f), frame,
                                                         1024.0f, 512.0f, 2.0f);
        // Borders of 16 and 8 pixels at 2 pixels per unit are 8 and 4 units.
        check(near(g.vertices[0].x, -60.0f) && near(g.vertices[1].x, -52.0f) &&
              near(g.vertices[2].x, 52.0f) && near(g.vertices[3].x, 60.0f), "columns keep 8-unit borders");
        check(near(g.vertices[0].y, 0.0f) && near(g.vertices[4].y, 4.0f) &&
              near(g.vertices[8].y, 36.0f) && near(g.vertices[12].y, 40.0f), "rows keep 4-unit borders");
        check(near(g.vertices[1].u, 116.0f / 1024.0f) && near(g.vertices[2].u, 148.0f / 1024.0f),
              "inner u at the border pixels");
        check(near(g.vertices[0].v, 1.0f - 200.0f / 512.0f) && near(g.vertices[12].v, 1.0f - 232.0f / 512.0f),
              "the frame's bottom row samples 1 - y / height");
    }
    {
        // Narrower than twice the left border: the grid shrinks instead of overlapping.
        TextureAtlasFrame frame;
        frame.rect = Vector4(0.0f, 0.0f, 64.0f, 64.0f);
        frame.border = Vector4(16.0f, 16.0f, 16.0f, 16.0f);
        const ImageGeometry g = buildSlicedImageGeometry(Vector2(10.0f, 40.0f), Vector2(0.0f, 0.0f), frame,
                                                         64.0f, 64.0f, 2.0f);
        check(near(g.vertices[0].x, 0.0f) && near(g.vertices[1].x, 5.0f) &&
              near(g.vertices[2].x, 5.0f) && near(g.vertices[3].x, 10.0f),
              "a 10-unit width with 8-unit borders scales them to 5");
    }

    std::cout << "sliced grid against upstream's shader\n";
    {
        TextureAtlasFrame panel;   // upstream ui-atlas 'panel'
        panel.rect = Vector4(548.0f, 660.0f, 128.0f, 128.0f);
        panel.border = Vector4(32.0f, 32.0f, 32.0f, 32.0f);
        TextureAtlasFrame button;   // 'button': an asymmetric bottom border
        button.rect = Vector4(780.0f, 892.0f, 128.0f, 128.0f);
        button.border = Vector4(32.0f, 44.0f, 32.0f, 32.0f);
        TextureAtlasFrame track;   // 'track': not square
        track.rect = Vector4(292.0f, 308.0f, 64.0f, 32.0f);
        track.border = Vector4(16.0f, 16.0f, 16.0f, 16.0f);
        TextureAtlasFrame flat;   // no border at all
        flat.rect = Vector4(412.0f, 660.0f, 128.0f, 128.0f);

        compareWithUpstream("panel 300x200, centre pivot", Vector2(300.0f, 200.0f), Vector2(0.5f, 0.5f), panel, 1024, 1024, 2);
        compareWithUpstream("panel 20x12, shrunk on both axes", Vector2(20.0f, 12.0f), Vector2(0.5f, 0.5f), panel, 1024, 1024, 2);
        compareWithUpstream("button 160x48, pivot (0, 1)", Vector2(160.0f, 48.0f), Vector2(0.0f, 1.0f), button, 1024, 1024, 2);
        compareWithUpstream("button 30x30 at 4 px per unit", Vector2(30.0f, 30.0f), Vector2(0.3f, 0.7f), button, 1024, 1024, 4);
        compareWithUpstream("track 120x12 at 4 px per unit", Vector2(120.0f, 12.0f), Vector2(0.5f, 0.5f), track, 1024, 1024, 4);
        compareWithUpstream("borderless frame", Vector2(50.0f, 70.0f), Vector2(0.5f, 0.5f), flat, 1024, 1024, 2);

        // Negative control: the comparison must SEE a difference. The same frame drawn with a
        // different pivot, or with the fragment stage's v flip left out, is not upstream's.
        const ImageGeometry ours = buildSlicedImageGeometry(Vector2(300.0f, 200.0f), Vector2(0.5f, 0.5f), panel, 1024, 1024, 2);
        const ImageVertex shifted = upstreamSlicedVertex(1, 1, Vector2(300.0f, 200.0f), Vector2(0.4f, 0.5f), panel, 1024, 1024, 2);
        check(std::abs(ours.vertices[5].x - shifted.x) > 1.0f, "control: a different pivot is detected");
        const ImageVertex straight = upstreamSlicedVertex(0, 0, Vector2(300.0f, 200.0f), Vector2(0.5f, 0.5f), panel, 1024, 1024, 2);
        check(std::abs(ours.vertices[0].v - (1.0f - straight.v)) > 0.1f, "control: an unflipped v is detected");
    }

    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
