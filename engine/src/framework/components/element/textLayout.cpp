// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "textLayout.h"

#include <algorithm>

#include "framework/handlers/fontResource.h"

namespace visutwin::canvas
{
    namespace
    {
        bool isWhitespace(const char c) { return c == ' ' || c == '\t'; }

        /// The glyph drawn for `code`: the character itself, else the space.
        const FontGlyph* glyphFor(const FontResource& font, const int code)
        {
            if (const auto it = font.glyphs.find(code); it != font.glyphs.end()) {
                return &it->second;
            }
            const auto space = font.glyphs.find(' ');
            return space != font.glyphs.end() ? &space->second : nullptr;
        }

        /// The kerned advance of symbols [begin, end), without trailing whitespace.
        float rangeWidth(const FontResource& font, const std::string& symbols, const size_t begin, const size_t end,
                         const float scale)
        {
            float width = 0.0f;
            float widthMinusTrailing = 0.0f;
            int prev = -1;
            for (size_t i = begin; i < end; ++i) {
                const int code = static_cast<unsigned char>(symbols[i]);
                const FontGlyph* glyph = glyphFor(font, code);
                const float kern = prev >= 0 ? font.kerningValue(prev, code) * scale : 0.0f;
                width += kern + (glyph ? glyph->xadvance * scale : 0.0f);
                if (!isWhitespace(symbols[i])) {
                    widthMinusTrailing = width;
                }
                prev = code;
            }
            return widthMinusTrailing;
        }
    }

    TextMeasure measureText(const FontResource& font, const std::string& symbols, const float fontSize,
                            const float lineHeight, const float maxLineWidth)
    {
        TextMeasure m;
        m.scale = fontSize / kFontUnitsPerEm;
        m.lineStep = lineHeight;
        m.fontMinY = font.minY * m.scale;
        m.fontMaxY = font.maxY * m.scale;

        const auto pushLine = [&](const size_t begin, const size_t end) {
            m.lines.push_back({begin, end, rangeWidth(font, symbols, begin, end, m.scale)});
        };

        size_t start = 0;
        size_t lastBreak = 0;   // the first symbol after the latest whitespace; 0 = none on this line
        for (size_t i = 0; i < symbols.size(); ++i) {
            const char c = symbols[i];
            if (c == '\n') {
                pushLine(start, i);
                start = i + 1;
                lastBreak = 0;
                continue;
            }
            // Greedy wrap: a visible symbol that would take the line past the limit breaks
            // it after the latest whitespace, or before itself inside a word too long for
            // a line of its own.
            if (!isWhitespace(c) && i > start && rangeWidth(font, symbols, start, i + 1, m.scale) > maxLineWidth) {
                const size_t breakAt = lastBreak > start ? lastBreak : i;
                pushLine(start, breakAt);
                start = breakAt;
                lastBreak = 0;
            }
            if (isWhitespace(c)) {
                lastBreak = i + 1;
            }
        }
        pushLine(start, symbols.size());

        // Upstream grows its width and height glyph by glyph, so an empty text measures
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

    std::vector<PlacedGlyph> placeText(const FontResource& font, const std::string& symbols, const TextMeasure& m,
                                       const float boxWidth, const float boxHeight, const Vector2& pivot,
                                       const float horizontalAlign, const float verticalAlign)
    {
        std::vector<PlacedGlyph> glyphs;
        glyphs.reserve(symbols.size());

        // Upstream's vertical placement: the first line's pen at 0 and each next one a line
        // step lower, the block placed by the alignment:
        //   voffset = (1 - pivot.y) H - fontMaxY - (1 - align.y) (H - height)
        // The box may be INVERTED (a split axis whose margins cross); the formula holds.
        const float voffset = (1.0f - pivot.y) * boxHeight - m.fontMaxY - (1.0f - verticalAlign) * (boxHeight - m.height);

        for (size_t li = 0; li < m.lines.size(); ++li) {
            const TextLine& line = m.lines[li];
            float x = -pivot.x * boxWidth + horizontalAlign * (boxWidth - line.width);
            const float penY = voffset - static_cast<float>(li) * m.lineStep;

            int prev = -1;
            for (size_t i = line.begin; i < line.end; ++i) {
                const int code = static_cast<unsigned char>(symbols[i]);
                const FontGlyph* glyph = glyphFor(font, code);
                if (!glyph) {
                    prev = code;
                    continue;
                }
                const FontGlyph& g = *glyph;
                const float kerning = prev >= 0 ? font.kerningValue(prev, code) : 0.0f;

                // Glyph placement mirrors upstream text-element.js exactly:
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
                p.x0 = x - (g.xoffset - kerning) * m.scale;
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

                x += (g.xadvance + kerning) * m.scale;
                prev = code;
            }
        }
        return glyphs;
    }
}
