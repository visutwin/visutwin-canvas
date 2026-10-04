// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
// An MSDF font built in memory, laid out as the shipped Roboto is: ASCII on page 0
// (1024 x 1024) and the ellipsis on page 1 (1024 x 512).
//
// Every glyph is a 64-texel cell with the pen 20 texels in and the baseline 40 down;
// digits advance by `digitAdvance`, A and V by 20 (with a -4 kerning pair), '.' by 5,
// '?' by 14, the ellipsis by 24, and every other printable ASCII character by 10. W is
// on page 2, which the HUD's renderer does not bind.

#pragma once

#include <cstdint>
#include <memory>

#include "framework/handlers/fontResource.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas::test
{
    inline std::unique_ptr<FontResource> makeMsdfTestFont(GraphicsDevice* device, const float digitAdvance = 18.0f)
    {
        auto font = std::make_unique<FontResource>();
        font->msdf = true;
        font->pxRange = 8.0f;
        TextureOptions page0;
        page0.width = 1024;
        page0.height = 1024;
        TextureOptions page1 = page0;
        page1.height = 512;
        font->pages = {new Texture(device, page0), new Texture(device, page1)};
        font->texture = font->pages[0];
        const auto add = [&font](const char32_t code, const float x, const float y, const float advance, const int page) {
            FontGlyph g;
            g.id = static_cast<int>(code);
            g.x = x;
            g.y = y;
            g.width = 64.0f;
            g.height = 64.0f;
            g.xadvance = advance;
            g.xoffset = 20.0f;
            g.yoffset = 40.0f;
            g.page = page;
            font->glyphs[g.id] = g;
        };
        for (char32_t c = 32; c < 127; ++c) {
            add(c, 0.0f, 192.0f, 10.0f, 0);
        }
        for (char32_t c = U'0'; c <= U'9'; ++c) {
            add(c, 64.0f * static_cast<float>(c - U'0'), 0.0f, digitAdvance, 0);
        }
        add(U'A', 0.0f, 64.0f, 20.0f, 0);
        add(U'V', 64.0f, 64.0f, 20.0f, 0);
        add(U'.', 128.0f, 64.0f, 5.0f, 0);
        add(U'?', 192.0f, 64.0f, 14.0f, 0);
        add(U'W', 256.0f, 64.0f, 30.0f, 2);
        add(0x2026, 320.0f, 128.0f, 24.0f, 1);
        font->kerning[(static_cast<uint64_t>(U'A') << 32u) | U'V'] = -4.0f;
        return font;
    }
}
