// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Text layout on upstream's metrics (text-element.js `_updateMeshes`), split in two so an
// element can know its size without building geometry:
//
// - measureText breaks the symbols into lines — at '\n', and greedily at word boundaries
//   (a word longer than the line breaks between characters) when a line would grow past
//   `maxLineWidth` — and measures the block: its width is the widest line leaving out
//   trailing whitespace, its height runs from the font's highest glyph top above the first
//   line's pen to its lowest glyph bottom below the last line's.
// - placeText positions every glyph in the element's box: horizontal alignment per line
//   (0 left .. 1 right), the block by the vertical alignment, both about the pivot.
//
// Glyph metrics scale by fontSize / 32 (the fonts' em, upstream's MAGIC) and lines step
// by `lineHeight`. `spacing` multiplies every glyph's advance, kerning included (upstream
// `spacing`, 1 by default): it spreads the pen, not the glyphs. Symbols are BYTES: only
// single-byte characters draw. A character the font lacks takes the space's glyph, as
// upstream substitutes it.
//
#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "core/math/vector2.h"

namespace visutwin::canvas
{
    struct FontResource;

    /// Upstream text-element's MAGIC: font units per em.
    inline constexpr float kFontUnitsPerEm = 32.0f;

    struct TextLine
    {
        /// Symbols [begin, end), the line break itself excluded.
        size_t begin = 0;
        size_t end = 0;
        /// Advance of the line without its trailing whitespace.
        float width = 0.0f;
    };

    struct TextMeasure
    {
        std::vector<TextLine> lines;
        float width = 0.0f;
        float height = 0.0f;
        float scale = 1.0f;      // fontSize / 32
        float lineStep = 0.0f;   // lineHeight
        float fontMinY = 0.0f;   // the font's glyph-bounds extent, scaled
        float fontMaxY = 0.0f;
        float spacing = 1.0f;
    };

    struct PlacedGlyph
    {
        size_t symbol = 0;
        int page = 0;
        float x0 = 0.0f;
        float y0 = 0.0f;   // bottom
        float x1 = 0.0f;
        float y1 = 0.0f;   // top
        float u0 = 0.0f;
        float v0 = 0.0f;   // at the top, v from the atlas top
        float u1 = 0.0f;
        float v1 = 0.0f;   // at the bottom
    };

    TextMeasure measureText(const FontResource& font, const std::string& symbols, float fontSize, float lineHeight,
                            float maxLineWidth = std::numeric_limits<float>::infinity(), float spacing = 1.0f);

    std::vector<PlacedGlyph> placeText(const FontResource& font, const std::string& symbols, const TextMeasure& measure,
                                       float boxWidth, float boxHeight, const Vector2& pivot,
                                       float horizontalAlign, float verticalAlign);
}
