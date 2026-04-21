// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <memory>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    struct EnvReprojectRect
    {
        int rectX = 0;
        int rectY = 0;
        int rectW = 0;
        int rectH = 0;
        int seamPixels = 1;
    };

    struct EnvReprojectOptions
    {
        std::shared_ptr<Texture> source;
        bool sourceIsCubemap = false;
        std::shared_ptr<Texture> target;
        std::vector<EnvReprojectRect> rects;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    void reprojectTexture(GraphicsDevice* device, const EnvReprojectOptions& options);
}
