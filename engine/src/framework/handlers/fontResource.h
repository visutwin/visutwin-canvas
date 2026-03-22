// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

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
    };

    struct FontResource
    {
        Texture* texture = nullptr;
        int atlasWidth = 0;
        int atlasHeight = 0;
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
