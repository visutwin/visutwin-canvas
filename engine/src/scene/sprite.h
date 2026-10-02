// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// A sequence of frames from a texture atlas, drawn by an image
// element: SIMPLE stretches the frame over the element's rectangle; SLICED keeps its
// borders at their authored size (in pixels over pixelsPerUnit) and stretches the middle.
//
// DEVIATION: upstream's Sprite builds a mesh per frame and slices it in the vertex shader
// (innerOffset / outerScale / atlasRect); here a sprite is data only, and the image element
// builds the sliced geometry on the CPU from the same formulas
// (framework/components/element/imageElementGeometry.h). TILED is accepted and drawn as
// SLICED: it needs a fragment-stage repeat of the centre that is not ported.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scene/textureAtlas.h"

namespace visutwin::canvas
{
    /// How an image element draws a sprite: simple, sliced or tiled.
    enum class SpriteRenderMode
    {
        Simple,
        Sliced,
        Tiled
    };

    class Sprite
    {
    public:
        Sprite() = default;
        Sprite(std::shared_ptr<TextureAtlas> atlas, std::vector<std::string> frameKeys,
               const float pixelsPerUnit = 1.0f, const SpriteRenderMode renderMode = SpriteRenderMode::Simple)
            : _atlas(std::move(atlas)), _frameKeys(std::move(frameKeys)), _pixelsPerUnit(pixelsPerUnit),
              _renderMode(renderMode) {}

        const std::shared_ptr<TextureAtlas>& atlas() const { return _atlas; }
        void setAtlas(std::shared_ptr<TextureAtlas> value) { _atlas = std::move(value); ++_version; }

        const std::vector<std::string>& frameKeys() const { return _frameKeys; }
        void setFrameKeys(std::vector<std::string> value) { _frameKeys = std::move(value); ++_version; }

        float pixelsPerUnit() const { return _pixelsPerUnit; }
        void setPixelsPerUnit(const float value) { _pixelsPerUnit = value; ++_version; }

        SpriteRenderMode renderMode() const { return _renderMode; }
        void setRenderMode(const SpriteRenderMode value) { _renderMode = value; ++_version; }

        /// SLICED or TILED: the frame's border keeps its size.
        bool nineSliced() const { return _renderMode != SpriteRenderMode::Simple; }

        /// The atlas frame at `index` into the frame keys, or null.
        const TextureAtlasFrame* frame(const int index) const
        {
            if (!_atlas || index < 0 || index >= static_cast<int>(_frameKeys.size())) {
                return nullptr;
            }
            return _atlas->frame(_frameKeys[static_cast<size_t>(index)]);
        }

        /// Changes to the sprite itself; its atlas keeps its own version, and a consumer
        /// compares both.
        uint64_t version() const { return _version; }

    private:
        std::shared_ptr<TextureAtlas> _atlas;
        std::vector<std::string> _frameKeys;
        float _pixelsPerUnit = 1.0f;
        SpriteRenderMode _renderMode = SpriteRenderMode::Simple;
        uint64_t _version = 1;
    };
}
