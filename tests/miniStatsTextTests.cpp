// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
// The performance HUD's text layer on a font built in memory, ASCII on page 0 and the
// ellipsis on page 1, as the shipped Roboto has it.
//
// What a render does not show reliably: a glyph placed by ADDING its offsets lands a
// different distance off for every character, which a run of digits hides; a cut
// glyph whose texture is not cut with it is squashed, not cut; an ellipsis drawn from the
// wrong page draws some other glyph; and the width returned for a cut text decides where
// the units after it go.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "framework/extras/miniStats/miniStatsText.h"
#include "framework/handlers/fontResource.h"
#include "platform/graphics/texture.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr int kStride = static_cast<int>(UiGeometryArena::kFloatsPerVertex);
    enum Corner { BL = 0, BR = 1, TR = 2, TL = 3 };
    enum Field { X = 0, Y = 1, MODE = 2, U = 6, V = 7 };

    float field(const Render2d& r, const int quad, const int vertex, const int offset)
    {
        return r.vertices()[static_cast<size_t>((quad * 4 + vertex) * kStride + offset)];
    }

    int quads(const Render2d& r) { return static_cast<int>(r.vertices().size()) / (4 * kStride); }

    // A 64-texel cell per glyph, the pen 20 texels into it and the baseline 40 down.
    std::unique_ptr<FontResource> makeFont(GraphicsDevice* device, const float digitAdvance)
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
        for (char32_t c = U'0'; c <= U'9'; ++c) {
            add(c, 64.0f * static_cast<float>(c - U'0'), 0.0f, digitAdvance, 0);
        }
        add(U'A', 0.0f, 64.0f, 20.0f, 0);
        add(U'V', 64.0f, 64.0f, 20.0f, 0);
        add(U'.', 128.0f, 64.0f, 5.0f, 0);
        add(U'?', 192.0f, 64.0f, 14.0f, 0);
        add(U'W', 256.0f, 64.0f, 30.0f, 2);          // on a page the renderer does not bind
        add(0x2026, 320.0f, 128.0f, 24.0f, 1);       // the ellipsis, page 1
        // A kerning pair, in font units.
        font->kerning[(static_cast<uint64_t>(U'A') << 32u) | U'V'] = -4.0f;
        return font;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {200, 100}, .cpuBuffers = true});
    auto regular = makeFont(device.get(), 18.0f);
    auto bold = makeFont(device.get(), 18.0f);
    // Size 16 regular (scale 0.5), 32 bold (scale 1): the numbers stay whole.
    const MiniStatsText text(regular.get(), bold.get(), 16.0f, 32.0f);
    check(text.valid(), "two MSDF fonts with a page each are valid");
    check(!MiniStatsText(regular.get(), nullptr).valid(), "a missing font is not");

    std::cout << "measuring\n";
    {
        check(text.measure("0123", MiniStatsText::Style::Regular) == 4 * 18.0f * 0.5f,
              "digits: four advances at the regular scale");
        check(text.measure("0123", MiniStatsText::Style::Bold) == 4 * 18.0f, "the bold style has its own scale");
        check(text.measure("AV", MiniStatsText::Style::Bold) == 20.0f - 4.0f + 20.0f, "kerning between a pair");
        check(text.measure("A\xE2\x80\xA6", MiniStatsText::Style::Bold) == 20.0f + 24.0f,
              "UTF-8: the ellipsis is one code point");
        check(text.measure("x", MiniStatsText::Style::Bold) == 14.0f, "a missing code point measures as '?'");
        check(text.measure("W", MiniStatsText::Style::Bold) == 14.0f, "so does one on a page beyond 1");
    }

    std::cout << "\nplacing\n";
    {
        Render2d r(device);
        r.setTargetSize(1000.0f, 1000.0f);
        r.startFrame();
        const float width = text.render(r, "AV", 100.0f, 200.0f, MiniStatsText::Style::Bold, 0xffffffffu);
        r.render(nullptr);
        check(width == 36.0f && quads(r) == 2, "two glyphs, the kerned width returned");
        // Bold scale 1: the cell is 64 points, pulled back by the offsets.
        check(std::abs(field(r, 0, BL, X) * 1000.0f - (100.0f - 20.0f)) < 1e-3f &&
              std::abs(field(r, 0, BL, Y) * 1000.0f - (200.0f - 40.0f)) < 1e-3f,
              "the cell's corner is pen - xoffset, baseline - yoffset");
        check(std::abs(field(r, 1, BL, X) * 1000.0f - (100.0f + 16.0f - 20.0f)) < 1e-3f,
              "the second glyph starts after the first advance plus the kerning");
        check(field(r, 0, BL, MODE) == static_cast<float>(Render2d::Mode::TextBold), "bold, page 0");
        check(std::abs(field(r, 1, TL, U) - 64.0f / 1024.0f) < 1e-6f && std::abs(field(r, 1, TL, V) - 64.0f / 1024.0f) < 1e-6f,
              "the glyph's atlas rectangle, from the page's top-left");
    }

    std::cout << "\ncutting\n";
    {
        Render2d r(device);
        r.setTargetSize(1000.0f, 1000.0f);
        r.startFrame();
        // "0000" is 72 wide in bold; 60 leaves 36 for digits before the 24-wide ellipsis.
        const float width = text.render(r, "0000", 0.0f, 100.0f, MiniStatsText::Style::Bold, 0xffffffffu, 60.0f);
        r.render(nullptr);
        check(width == 60.0f, "a cut text returns the width it was given");
        // Digits at pens 0 and 18 start before 36; the third, at 36, does not.
        check(quads(r) == 3, "two digits, then the ellipsis");
        const float secondRight = field(r, 1, BR, X) * 1000.0f;
        check(std::abs(secondRight - 36.0f) < 1e-3f, "the second digit's cell is cut at the limit");
        // Its cell starts at 18 - 20 = -2, so 38 of its 64 points are kept, texture with it.
        check(std::abs(field(r, 1, BR, U) - (0.0f + 64.0f * 38.0f / 64.0f) / 1024.0f) < 1e-6f,
              "and the texture is cut with it: not squashed");
        check(field(r, 2, BL, MODE) == static_cast<float>(Render2d::Mode::TextBoldPage1),
              "the ellipsis is drawn from page 1");
        check(std::abs(field(r, 2, TL, V) - 128.0f / 512.0f) < 1e-6f, "in page 1's own coordinates");
        check(std::abs(field(r, 2, BL, X) * 1000.0f - (36.0f - 20.0f)) < 1e-3f, "right after the kept text");

        r.startFrame();
        check(text.render(r, "0000", 0.0f, 100.0f, MiniStatsText::Style::Bold, 0xffffffffu, 0.0f) == 0.0f,
              "no room: nothing drawn, width 0");
        r.render(nullptr);
        check(r.vertices().empty(), "and no quads");

        r.startFrame();
        check(text.render(r, "0000", 0.0f, 100.0f, MiniStatsText::Style::Bold, 0xffffffffu, 72.0f) == 72.0f,
              "a text that just fits is not cut");
        r.render(nullptr);
        check(quads(r) == 4, "four digits, no ellipsis");
    }

    std::cout << "\nbinding\n";
    {
        Texture graph(device.get());
        const auto t = text.textures(&graph);
        check(t.regularPage0 == regular->pages[0] && t.regularPage1 == regular->pages[1] &&
              t.boldPage0 == bold->pages[0] && t.boldPage1 == bold->pages[1] && t.graph == &graph,
              "both pages of both fonts, and the graph");
        Render2d r(device);
        text.applyMsdf(r);
        r.render(nullptr);
        check(r.material()->params()[1] == 8.0f && r.material()->params()[2] == 1024.0f &&
              r.material()->params()[3] == 1024.0f && r.material()->pages()[0] == 1024.0f &&
              r.material()->pages()[1] == 512.0f, "the range and each page's size reach the shader");
    }

    return finish("mini-stats text");
}
