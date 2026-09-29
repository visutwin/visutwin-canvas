// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    struct FontGlyph
    {
        int id = 0;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float xadvance = 0.0f;
        float xoffset = 0.0f;
        float yoffset = 0.0f;
        /// The atlas page (upstream `map`) the glyph's rect is in.
        int page = 0;
    };

    /// A font in upstream's JSON format: glyph metrics plus one atlas image per page
    /// (`<name>.png`, then `<name>1.png`, ...). An MSDF font (its glyphs carry a `range`)
    /// keeps its distance field and is drawn by the MSDF shader path; a plain bitmap font
    /// gets its coverage in alpha at load.
    struct FontResource
    {
        FontResource() = default;
        ~FontResource()
        {
            for (const Texture* page : pages) {
                delete page;
            }
        }

        FontResource(const FontResource&) = delete;
        FontResource& operator=(const FontResource&) = delete;
        FontResource(FontResource&&) = delete;
        FontResource& operator=(FontResource&&) = delete;

        /// Owned atlas pages, in page order.
        std::vector<Texture*> pages;
        /// Page 0, as a single-page font's only texture.
        Texture* texture = nullptr;
        int atlasWidth = 0;
        int atlasHeight = 0;
        /// True for a multi-channel signed distance field font (upstream's fonts are).
        bool msdf = false;
        /// Texels of distance-field spread (upstream `font_pxrange`: scale x range).
        float pxRange = 2.0f;
        /// Upstream `font_sdfIntensity`: 0 draws the glyph at its edge, 1 fattens it most.
        float intensity = 0.0f;
        float lineHeight = 64.0f;
        std::unordered_map<int, FontGlyph> glyphs;
        std::unordered_map<uint64_t, float> kerning;

        float kerningValue(const int left, const int right) const
        {
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(left)) << 32u) |
                static_cast<uint32_t>(right);
            if (const auto it = kerning.find(key); it != kerning.end()) {
                return it->second;
            }
            return 0.0f;
        }
    };

    std::optional<FontResource*> loadBitmapFontResource(const std::string& jsonPath,
        const std::shared_ptr<GraphicsDevice>& graphicsDevice);
}
