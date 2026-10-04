// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

#include "render2d.h"

namespace visutwin::canvas
{
    struct FontGlyph;
    struct FontResource;

    /**
     * The performance HUD's text: measures a string and places its glyphs as Render2d
     * quads, cut to a width and ended with an ellipsis when it does not fit.
     *
     * Two styles: REGULAR for labels and BOLD for figures. Both fonts must be MSDF fonts
     * whose digits share one advance, so a changing figure does not shift sideways (the
     * shipped Roboto regular and bold do). A glyph may be on page 0 or 1 of its font;
     * pages beyond that, and code points the font lacks, draw as '?'.
     *
     * Positions are in points from the bottom-left corner, `baseline` the line's
     * baseline. A size is the font size in points, as a text element's: glyph metrics
     * scale by size / 32, the fonts' em.
     *
     * DEVIATION: the HUD's text is drawn from the two MSDF fonts the application passes
     * in, where upstream rasterises system fonts (a proportional face for labels and a
     * semibold monospace one for figures) into an atlas of its own at start-up. Kerning
     * is applied between every pair of glyphs.
     */
    class MiniStatsText
    {
    public:
        enum class Style : uint8_t
        {
            Regular = 0,
            Bold = 1
        };

        static constexpr float kDefaultRegularSize = 11.0f;
        static constexpr float kDefaultBoldSize = 14.0f;

        MiniStatsText(const FontResource* regular, const FontResource* bold,
                      float regularSize = kDefaultRegularSize, float boldSize = kDefaultBoldSize);

        /// Both fonts are present, MSDF and have a page.
        [[nodiscard]] bool valid() const { return _valid; }

        /// The advance of `text` (UTF-8) in points.
        [[nodiscard]] float measure(std::string_view text, Style style) const;

        /**
         * Places `text` with its pen starting at `x` on `baseline` and returns the width
         * it took, at most `maxWidth`. A text wider than `maxWidth` is cut so that an
         * ellipsis fits after it; the glyph the cut falls in is cut, texture included.
         */
        float render(Render2d& renderer, std::string_view text, float x, float baseline, Style style,
                     uint32_t color, float maxWidth = std::numeric_limits<float>::infinity()) const;

        /// The four font pages, with `graph` as the history texture.
        [[nodiscard]] Render2dMaterial::Textures textures(Texture* graph) const;
        /// Gives the renderer the fonts' MSDF range and page sizes.
        void applyMsdf(Render2d& renderer) const;

    private:
        struct Face
        {
            const FontResource* font = nullptr;
            float scale = 1.0f;           // size / 32
            const FontGlyph* fallback = nullptr;
            float ellipsisAdvance = 0.0f;
        };

        [[nodiscard]] const Face& face(Style style) const { return style == Style::Bold ? _bold : _regular; }
        /// The glyph for `code`, or the fallback when the font lacks it or it is on a page
        /// the renderer does not bind.
        [[nodiscard]] const FontGlyph* glyph(const Face& face, char32_t code) const;
        void renderRun(Render2d& renderer, std::u32string_view text, float x, float baseline, Style style,
                       uint32_t color, float limit) const;

        Face _regular;
        Face _bold;
        bool _valid = false;
    };
}
