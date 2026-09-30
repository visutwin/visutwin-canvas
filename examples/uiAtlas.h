// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The frames of upstream's UI kit atlas, assets/ui/ui-atlas.png, as upstream's
// examples/assets/ui/ui-atlas.mjs lists them (46 frames, generated there by
// generate-ui-atlas.mjs). Rects are x, y from the image BOTTOM, width, height; borders are left,
// bottom, right, top; every pivot is the centre. Frames are drawn at 2 pixels per screen unit.
//
#pragma once

#include <memory>
#include <string>

#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "scene/textureAtlas.h"

namespace visutwin::canvas
{
    inline std::shared_ptr<TextureAtlas> createUiAtlas(Texture* texture)
    {
        struct Frame
        {
            const char* name;
            float rect[4];
            float border[4];
        };
        static constexpr Frame frames[] = {
            {"panel", {548, 660, 128, 128}, {32, 32, 32, 32}},
            {"panel-outline", {684, 660, 128, 128}, {32, 32, 32, 32}},
            {"shadow", {4, 796, 224, 224}, {80, 80, 80, 80}},
            {"paper", {820, 660, 128, 128}, {32, 32, 32, 32}},
            {"button", {780, 892, 128, 128}, {32, 44, 32, 32}},
            {"button-hover", {4, 660, 128, 128}, {32, 44, 32, 32}},
            {"button-pressed", {276, 660, 128, 128}, {32, 44, 32, 32}},
            {"button-inactive", {140, 660, 128, 128}, {32, 44, 32, 32}},
            {"track", {292, 308, 64, 32}, {16, 16, 16, 16}},
            {"knob", {76, 276, 64, 64}, {0, 0, 0, 0}},
            {"checkbox", {4, 276, 64, 64}, {0, 0, 0, 0}},
            {"check", {940, 380, 64, 64}, {0, 0, 0, 0}},
            {"radio", {148, 276, 64, 64}, {0, 0, 0, 0}},
            {"radio-dot", {220, 276, 64, 64}, {0, 0, 0, 0}},
            {"circle", {412, 660, 128, 128}, {0, 0, 0, 0}},
            {"icon-close", {316, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-back", {4, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-next", {732, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-down", {524, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-up", {836, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-plus", {108, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-minus", {524, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-info", {212, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-pause", {836, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-gear", {732, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-star", {524, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-trophy", {732, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-heart", {108, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-coin", {420, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-gem", {836, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-potion", {212, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-sword", {628, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-shield", {316, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-key", {316, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-bolt", {108, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-flame", {628, 556, 96, 96}, {0, 0, 0, 0}},
            {"icon-lock", {420, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-music", {628, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-sound", {420, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-grip", {4, 452, 96, 96}, {0, 0, 0, 0}},
            {"icon-pin", {4, 348, 96, 96}, {0, 0, 0, 0}},
            {"icon-chest", {212, 556, 96, 96}, {0, 0, 0, 0}},
            {"avatar-1", {236, 892, 128, 128}, {0, 0, 0, 0}},
            {"avatar-2", {372, 892, 128, 128}, {0, 0, 0, 0}},
            {"avatar-3", {508, 892, 128, 128}, {0, 0, 0, 0}},
            {"avatar-4", {644, 892, 128, 128}, {0, 0, 0, 0}},
        };
        auto atlas = std::make_shared<TextureAtlas>();
        atlas->setTexture(texture);
        for (const Frame& f : frames) {
            atlas->setFrame(f.name, {.rect = Vector4(f.rect[0], f.rect[1], f.rect[2], f.rect[3]),
                                     .pivot = Vector2(0.5f, 0.5f),
                                     .border = Vector4(f.border[0], f.border[1], f.border[2], f.border[3])});
        }
        return atlas;
    }
}
