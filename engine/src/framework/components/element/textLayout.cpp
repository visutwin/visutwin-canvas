// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
#include "textLayout.h"

#include <algorithm>

#include "framework/handlers/fontResource.h"

namespace visutwin::canvas
{
    namespace
    {
        bool isWhitespace(const char32_t c) { return c == U' ' || c == U'\t'; }
        // '\r' breaks a line as '\n' does (so "\r\n" is two breaks).
        bool isLineBreak(const char32_t c) { return c == U'\n' || c == U'\r'; }

        /// The glyph drawn for `code`: the character itself, else the space.
        const FontGlyph* glyphFor(const FontResource& font, const int code)
        {
            if (const auto it = font.glyphs.find(code); it != font.glyphs.end()) {
                return &it->second;
            }
            const auto space = font.glyphs.find(' ');
            return space != font.glyphs.end() ? &space->second : nullptr;
        }

        /// The kerned advance of symbols [begin, end) times `spacing`, without trailing
        /// whitespace.
        float rangeWidth(const FontResource& font, const std::u32string& symbols, const size_t begin, const size_t end,
                         const float scale, const float spacing)
        {
            float width = 0.0f;
            float widthMinusTrailing = 0.0f;
            int prev = -1;
            for (size_t i = begin; i < end; ++i) {
                const int code = static_cast<int>(symbols[i]);
                // A line break inside a line (the last of `maxLines`) takes no space: it is
                // skipped before any advance.
                if (isLineBreak(symbols[i])) {
                    prev = code;
                    continue;
                }
                const FontGlyph* glyph = glyphFor(font, code);
                const float kern = prev >= 0 ? font.kerningValue(prev, code) * scale : 0.0f;
                width += spacing * (kern + (glyph ? glyph->xadvance * scale : 0.0f));
                if (!isWhitespace(symbols[i])) {
                    widthMinusTrailing = width;
                }
                prev = code;
            }
            return widthMinusTrailing;
        }
    }

    std::u32string decodeUtf8(const std::string_view text)
    {
        std::u32string out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            const auto lead = static_cast<unsigned char>(text[i]);
            int length = 0;
            char32_t code = 0;
            if (lead < 0x80) {
                length = 1;
                code = lead;
            } else if ((lead & 0xE0) == 0xC0) {
                length = 2;
                code = lead & 0x1F;
            } else if ((lead & 0xF0) == 0xE0) {
                length = 3;
                code = lead & 0x0F;
            } else if ((lead & 0xF8) == 0xF0) {
                length = 4;
                code = lead & 0x07;
            }
            bool valid = length > 0 && i + static_cast<size_t>(length) <= text.size();
            for (int k = 1; valid && k < length; ++k) {
                const auto next = static_cast<unsigned char>(text[i + static_cast<size_t>(k)]);
                valid = (next & 0xC0) == 0x80;
                code = (code << 6) | (next & 0x3F);
            }
            // Overlong forms, surrogates and values past U+10FFFF are malformed too.
            static constexpr char32_t kMinimum[5] = {0, 0, 0x80, 0x800, 0x10000};
            if (valid && (code < kMinimum[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))) {
                valid = false;
            }
            if (!valid) {
                out.push_back(U'\uFFFD');
                ++i;
                continue;
            }
            out.push_back(code);
            i += static_cast<size_t>(length);
        }
        return out;
    }

    TextMeasure measureText(const FontResource& font, const std::u32string& symbols, const float fontSize,
                            const float lineHeight, const float maxLineWidth, const float spacing, const int maxLines)
    {
        TextMeasure m;
        m.spacing = spacing;
        m.scale = fontSize / kFontUnitsPerEm;
        m.lineStep = lineHeight;
        m.fontMinY = font.minY * m.scale;
        m.fontMaxY = font.maxY * m.scale;

        const auto pushLine = [&](const size_t begin, const size_t end, const int gaps = 0) {
            m.lines.push_back({begin, end, rangeWidth(font, symbols, begin, end, m.scale, m.spacing), gaps});
        };
        // Gap count: a whitespace run followed by a visible symbol, after the line's
        // first visible symbol. Trailing whitespace is no gap.
        const auto interiorGaps = [&symbols](const size_t begin, const size_t end) {
            int gaps = 0;
            bool seenVisible = false;
            bool afterWhitespace = false;
            for (size_t k = begin; k < end; ++k) {
                if (isWhitespace(symbols[k])) {
                    afterWhitespace = true;
                } else {
                    if (afterWhitespace && seenVisible) {
                        ++gaps;
                    }
                    afterWhitespace = false;
                    seenVisible = true;
                }
            }
            return gaps;
        };

        size_t start = 0;
        size_t lastBreak = 0;   // the first symbol after the latest whitespace; 0 = none on this line
        // `maxLines`: on the last line allowed nothing breaks any more, so the rest of
        // the text runs on in it, past the width, line breaks included (and drawn as nothing).
        const auto mayBreak = [&m, maxLines]() {
            return maxLines < 0 || static_cast<int>(m.lines.size()) + 1 < maxLines;
        };
        for (size_t i = 0; i < symbols.size(); ++i) {
            const char32_t c = symbols[i];
            if (isLineBreak(c)) {
                if (!mayBreak()) {
                    continue;
                }
                pushLine(start, i);
                start = i + 1;
                lastBreak = 0;
                continue;
            }
            // Greedy wrap: a visible symbol that would take the line past the limit breaks
            // it after the latest whitespace, or before itself inside a word too long for
            // a line of its own.
            if (!isWhitespace(c) && i > start && mayBreak() &&
                rangeWidth(font, symbols, start, i + 1, m.scale, m.spacing) > maxLineWidth) {
                const bool atWord = lastBreak > start;
                const size_t breakAt = atWord ? lastBreak : i;
                // Only a line broken at a word may be justified; a word broken mid-word has
                // nothing to stretch.
                pushLine(start, breakAt, atWord ? interiorGaps(start, breakAt) : 0);
                start = breakAt;
                lastBreak = 0;
            }
            if (isWhitespace(c)) {
                lastBreak = i + 1;
            }
        }
        pushLine(start, symbols.size());

        // Width and height grow glyph by glyph, so an empty text measures
        // 0 x 0 and the height ends at the last line that HAS a glyph (a trailing line
        // break adds nothing).
        int lastLineWithGlyph = -1;
        for (size_t li = 0; li < m.lines.size(); ++li) {
            const TextLine& line = m.lines[li];
            if (line.end > line.begin) {
                m.width = std::max(m.width, line.width);
                lastLineWithGlyph = static_cast<int>(li);
            }
        }
        if (lastLineWithGlyph >= 0) {
            const float lastPenY = -static_cast<float>(lastLineWithGlyph) * m.lineStep;
            m.height = m.fontMaxY - (lastPenY + m.fontMinY);
        }
        return m;
    }

    std::vector<PlacedGlyph> placeText(const FontResource& font, const std::u32string& symbols, const TextMeasure& m,
                                       const float boxWidth, const float boxHeight, const Vector2& pivot,
                                       const float horizontalAlign, const float verticalAlign, const bool justify)
    {
        std::vector<PlacedGlyph> glyphs;
        glyphs.reserve(symbols.size());

        // Vertical placement: the first line's pen at 0 and each next one a line
        // step lower, the block placed by the alignment:
        //   voffset = (1 - pivot.y) H - fontMaxY - (1 - align.y) (H - height)
        // The box may be INVERTED (a split axis whose margins cross); the formula holds.
        const float voffset = (1.0f - pivot.y) * boxHeight - m.fontMaxY - (1.0f - verticalAlign) * (boxHeight - m.height);

        for (size_t li = 0; li < m.lines.size(); ++li) {
            const TextLine& line = m.lines[li];
            // A justified line is flush with both edges, and spreads what it has left over
            // evenly between its words instead of aligning.
            const float slack = boxWidth - line.width;
            const bool justified = justify && line.gaps > 0 && slack > 0.0f;
            const float gapWidth = justified ? slack / static_cast<float>(line.gaps) : 0.0f;
            float x = -pivot.x * boxWidth + (justified ? 0.0f : horizontalAlign * slack);
            const float penY = voffset - static_cast<float>(li) * m.lineStep;
            int gapIndex = 0;
            bool seenVisible = false;
            bool afterWhitespace = false;

            int prev = -1;
            for (size_t i = line.begin; i < line.end; ++i) {
                const int code = static_cast<int>(symbols[i]);
                if (isLineBreak(symbols[i])) {   // run on into the last of `maxLines`: no glyph, no advance
                    prev = code;
                    continue;
                }
                // The gaps before this symbol, counted as measureText counts a line's gaps.
                if (isWhitespace(symbols[i])) {
                    afterWhitespace = true;
                } else {
                    if (afterWhitespace && seenVisible) {
                        ++gapIndex;
                    }
                    afterWhitespace = false;
                    seenVisible = true;
                }
                const FontGlyph* glyph = glyphFor(font, code);
                if (!glyph) {
                    prev = code;
                    continue;
                }
                const FontGlyph& g = *glyph;
                const float kerning = prev >= 0 ? font.kerningValue(prev, code) : 0.0f;
                const float gapShift = gapWidth * static_cast<float>(gapIndex);

                // Glyph placement:
                //
                //   left   = pen - (xoffset - kerning) * scale
                //   bottom = penY - yoffset * scale
                //   right/top = left/bottom + quadsize, quadsize = scale * size / glyph scale
                //
                // The offsets are SUBTRACTED, not added: in an MSDF atlas every glyph sits
                // in a fixed cell and xoffset/yoffset say where the pen sits INSIDE it, so
                // the cell is pulled back to line the glyph up. Adding them instead
                // displaces every character by a different amount, which a monospace font
                // hides. The quad is square — size = (width + height) / 2 — so a non-square
                // atlas cell cannot skew it.
                const float quadSize = m.scale * (g.width + g.height) * 0.5f / g.scale;
                PlacedGlyph p;
                p.symbol = i;
                p.page = (g.page >= 0 && g.page < static_cast<int>(font.pages.size())) ? g.page : 0;
                p.x0 = x + gapShift - (g.xoffset - kerning) * m.scale;
                p.x1 = p.x0 + quadSize;
                p.y0 = penY - g.yoffset * m.scale;
                p.y1 = p.y0 + quadSize;

                // UVs are fractions of the glyph's OWN page, v from the atlas top.
                const Texture* page = font.pages.empty() ? font.texture : font.pages[static_cast<size_t>(p.page)];
                const float atlasW = static_cast<float>(std::max(page ? static_cast<int>(page->width()) : font.atlasWidth, 1));
                const float atlasH = static_cast<float>(std::max(page ? static_cast<int>(page->height()) : font.atlasHeight, 1));
                p.u0 = g.x / atlasW;
                p.u1 = (g.x + g.width) / atlasW;
                p.v0 = g.y / atlasH;
                p.v1 = (g.y + g.height) / atlasH;
                glyphs.push_back(p);

                x += m.spacing * (g.xadvance + kerning) * m.scale;
                prev = code;
            }
        }
        return glyphs;
    }
}
