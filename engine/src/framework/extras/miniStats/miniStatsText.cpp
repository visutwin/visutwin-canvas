// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#include "miniStatsText.h"

#include <algorithm>
#include <string>

#include "framework/components/element/textLayout.h"
#include "framework/handlers/fontResource.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr char32_t kEllipsis = 0x2026;
        constexpr char32_t kFallback = U'?';
        // Pages the renderer binds per font.
        constexpr int kPages = 2;

        Texture* page(const FontResource* font, const int index)
        {
            if (!font || index < 0 || index >= static_cast<int>(font->pages.size())) {
                return nullptr;
            }
            return font->pages[static_cast<size_t>(index)];
        }

        const FontGlyph* findGlyph(const FontResource* font, const char32_t code)
        {
            const auto it = font->glyphs.find(static_cast<int>(code));
            if (it == font->glyphs.end() || it->second.page < 0 || it->second.page >= kPages ||
                !page(font, it->second.page)) {
                return nullptr;
            }
            return &it->second;
        }
    }

    MiniStatsText::MiniStatsText(const FontResource* regular, const FontResource* bold,
                                 const float regularSize, const float boldSize)
    {
        const auto setup = [](Face& f, const FontResource* font, const float size) {
            f.font = font;
            f.scale = size / 32.0f;
            if (!font) {
                return false;
            }
            f.fallback = findGlyph(font, kFallback);
            const FontGlyph* ellipsis = findGlyph(font, kEllipsis);
            const FontGlyph* period = findGlyph(font, U'.');
            f.ellipsisAdvance = ellipsis ? ellipsis->xadvance * f.scale
                : (period ? 3.0f * period->xadvance * f.scale : 0.0f);
            return font->msdf && page(font, 0) != nullptr;
        };
        const bool regularOk = setup(_regular, regular, regularSize);
        const bool boldOk = setup(_bold, bold, boldSize);
        _valid = regularOk && boldOk;
    }

    const FontGlyph* MiniStatsText::glyph(const Face& f, const char32_t code) const
    {
        const FontGlyph* g = findGlyph(f.font, code);
        return g ? g : f.fallback;
    }

    float MiniStatsText::measure(const std::string_view text, const Style style) const
    {
        const Face& f = face(style);
        if (!f.font) {
            return 0.0f;
        }
        float width = 0.0f;
        int prev = -1;
        for (const char32_t code : decodeUtf8(text)) {
            const FontGlyph* g = glyph(f, code);
            if (!g) {
                continue;
            }
            if (prev >= 0) {
                width += f.font->kerningValue(prev, g->id) * f.scale;
            }
            width += g->xadvance * f.scale;
            prev = g->id;
        }
        return width;
    }

    float MiniStatsText::render(Render2d& renderer, const std::string_view text, const float x, const float baseline,
                                const Style style, const uint32_t color, const float maxWidth) const
    {
        const Face& f = face(style);
        if (!f.font || maxWidth <= 0.0f) {
            return 0.0f;
        }
        const float width = measure(text, style);
        const bool truncate = width > maxWidth;
        const float available = truncate ? std::max(0.0f, maxWidth - f.ellipsisAdvance) : width;
        const std::u32string symbols = decodeUtf8(text);
        renderRun(renderer, symbols, x, baseline, style, color, available);
        if (truncate) {
            if (findGlyph(f.font, kEllipsis)) {
                renderRun(renderer, std::u32string_view(&kEllipsis, 1), x + available, baseline, style, color,
                          f.ellipsisAdvance);
            } else {
                renderRun(renderer, U"...", x + available, baseline, style, color, f.ellipsisAdvance);
            }
        }
        return std::min(width, maxWidth);
    }

    void MiniStatsText::renderRun(Render2d& renderer, const std::u32string_view text, const float x,
                                  const float baseline, const Style style, const uint32_t color,
                                  const float limit) const
    {
        const Face& f = face(style);
        float pen = 0.0f;
        int prev = -1;
        for (const char32_t code : text) {
            if (pen >= limit) {
                break;
            }
            const FontGlyph* g = glyph(f, code);
            if (!g) {
                continue;
            }
            if (prev >= 0) {
                pen += f.font->kerningValue(prev, g->id) * f.scale;
            }
            prev = g->id;

            // The glyph's atlas cell, placed as text layout places it: the offsets say
            // where the pen sits inside the cell, so they pull the cell back.
            const float cell = f.scale * (g->width + g->height) * 0.5f / g->scale;
            const float left = pen - g->xoffset * f.scale;
            const float bottom = baseline - g->yoffset * f.scale;
            // Cut at the limit, the texture with it, so a cut glyph is cut, not squashed.
            const float visible = std::min(cell, limit - left);
            if (visible > 0.0f && cell > 0.0f) {
                const Texture* pageTexture = page(f.font, g->page);
                renderer.quad(x + left, bottom, visible, cell, g->x, g->y, g->width * visible / cell, g->height,
                              static_cast<float>(pageTexture->width()), static_cast<float>(pageTexture->height()),
                              Render2d::textMode(style == Style::Bold, g->page), color);
            }
            pen += g->xadvance * f.scale;
        }
    }

    Render2dMaterial::Textures MiniStatsText::textures(Texture* graph) const
    {
        return Render2dMaterial::Textures{
            .regularPage0 = page(_regular.font, 0),
            .boldPage0 = page(_bold.font, 0),
            .regularPage1 = page(_regular.font, 1),
            .boldPage1 = page(_bold.font, 1),
            .graph = graph};
    }

    void MiniStatsText::applyMsdf(Render2d& renderer) const
    {
        const Texture* page0 = page(_regular.font, 0);
        const Texture* page1 = page(_regular.font, 1);
        const auto size = [](const Texture* t) { return t ? static_cast<float>(t->width()) : 1.0f; };
        const auto height = [](const Texture* t) { return t ? static_cast<float>(t->height()) : 1.0f; };
        renderer.setMsdf(_regular.font ? _regular.font->pxRange : 2.0f, size(page0), height(page0),
                         size(page1), height(page1));
    }
}
