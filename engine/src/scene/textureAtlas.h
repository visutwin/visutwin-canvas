// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A texture divided into named frames (upstream scene/texture-atlas.js).
//
// A frame's rect is in TEXTURE PIXELS, x from the left and y from the BOTTOM of the image
// (upstream's convention, which its generated atlases follow: `ui-atlas.png`'s avatar
// frames are empty read from the top and filled read from the bottom). The border is the
// 9-slice inset in pixels: left, bottom, right, top. The pivot is the point of the frame a
// sprite's position names, 0..1.
//
// DEVIATION: the texture is BORROWED, as a material's textures are, where upstream's
// destroy() destroys it; whoever loaded it must outlive the atlas. Change listeners are a
// version counter rather than events: the image element compares it on each sync.
//
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "core/math/vector2.h"
#include "core/math/vector4.h"

namespace visutwin::canvas
{
    class Texture;

    struct TextureAtlasFrame
    {
        /// x, y (from the bottom), width, height, in texture pixels.
        Vector4 rect = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector2 pivot = Vector2(0.5f, 0.5f);
        /// left, bottom, right, top insets in texture pixels.
        Vector4 border = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    };

    class TextureAtlas
    {
    public:
        Texture* texture() const { return _texture; }
        void setTexture(Texture* value)
        {
            _texture = value;
            ++_version;
        }

        const std::unordered_map<std::string, TextureAtlasFrame>& frames() const { return _frames; }
        void setFrames(std::unordered_map<std::string, TextureAtlasFrame> value)
        {
            _frames = std::move(value);
            ++_version;
        }

        void setFrame(const std::string& key, const TextureAtlasFrame& frame)
        {
            _frames[key] = frame;
            ++_version;
        }

        void removeFrame(const std::string& key)
        {
            if (_frames.erase(key) > 0) {
                ++_version;
            }
        }

        /// The frame named `key`, or null.
        const TextureAtlasFrame* frame(const std::string& key) const
        {
            const auto it = _frames.find(key);
            return it != _frames.end() ? &it->second : nullptr;
        }

        /// Bumped by every change, so a consumer can tell its cached geometry is stale.
        uint64_t version() const { return _version; }

    private:
        Texture* _texture = nullptr;
        std::unordered_map<std::string, TextureAtlasFrame> _frames;
        uint64_t _version = 1;
    };
}
