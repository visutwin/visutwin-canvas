// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// Text layout on the fonts' metrics, split in two so an
// element can know its size without building geometry:
//
// - measureText breaks the symbols into lines — at a line break ('\n' or '\r'),
//   and greedily at word boundaries
//   (a word longer than the line breaks between characters) when a line would grow past
//   `maxLineWidth` — and measures the block: its width is the furthest any symbol's
//   advance reaches, whitespace included, its height runs from the font's highest glyph top
//   above the first line's pen to its lowest glyph bottom below the last line's. A line
//   closed by a line break or a wrap aligns by its width without trailing whitespace; the
//   last line by its whole advance.
// - placeText positions every glyph in the element's box: horizontal alignment per line
//   (0 left .. 1 right), the block by the vertical alignment, both about the pivot.
//
// Glyph metrics scale by fontSize / 32 (the fonts' em) and lines step
// by `lineHeight`. `spacing` multiplies every glyph's advance, kerning included (1 by
// default): it spreads the pen, not the glyphs. Symbols are CODE POINTS,
// decoded from UTF-8 (`decodeUtf8`), which is what the fonts' glyph ids are. DEVIATION:
// upstream splits text into grapheme-like symbols (a surrogate pair, an emoji sequence);
// a combining sequence here is several symbols. A character the font lacks takes the
// space's glyph.
//
#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "core/math/vector2.h"

namespace visutwin::canvas
{
    struct FontResource;

    /// Font units per em.
    inline constexpr float kFontUnitsPerEm = 32.0f;

    struct TextLine
    {
        /// Symbols (code points) [begin, end), the line break itself excluded.
        size_t begin = 0;
        size_t end = 0;
        /// The width the line is aligned by: its advance without trailing whitespace for a
        /// line closed by a line break or a wrap, its whole advance for the last line.
        float width = 0.0f;
        /// The word gaps a justified line may widen: the gaps between
        /// its words for a line broken at a word wrap, 0 for a line ended by a line break, the
        /// last line, and a word broken mid-word — those keep the plain alignment.
        int gaps = 0;
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

    /// UTF-8 to code points. A malformed or truncated sequence becomes U+FFFD, one per
    /// offending byte, which no font here has, so it draws as the space.
    std::u32string decodeUtf8(std::string_view text);

    /// `maxLines` (negative for none) stops breaking lines once there are that many:
    /// the rest of the text runs on in the last one. Pass it only for text that wraps; it is
    /// ignored otherwise.
    TextMeasure measureText(const FontResource& font, const std::u32string& symbols, float fontSize, float lineHeight,
                            float maxLineWidth = std::numeric_limits<float>::infinity(), float spacing = 1.0f,
                            int maxLines = -1);

    /// `justify`: a line with gaps is stretched flush to both edges of
    /// the box by widening its word gaps evenly, ignoring `horizontalAlign`.
    std::vector<PlacedGlyph> placeText(const FontResource& font, const std::u32string& symbols, const TextMeasure& measure,
                                       float boxWidth, float boxHeight, const Vector2& pivot,
                                       float horizontalAlign, float verticalAlign, bool justify = false);

    /// The same, from UTF-8.
    inline TextMeasure measureText(const FontResource& font, const std::string& text, const float fontSize,
                                   const float lineHeight,
                                   const float maxLineWidth = std::numeric_limits<float>::infinity(),
                                   const float spacing = 1.0f)
    {
        return measureText(font, decodeUtf8(text), fontSize, lineHeight, maxLineWidth, spacing);
    }
}
