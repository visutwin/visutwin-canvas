// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.07.2026.
//
#include "areaLightLuts.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas::AreaLightLuts
{
    namespace
    {
#include "areaLightLutsData.inc"

        constexpr uint32_t kLutSize = 64;

        std::shared_ptr<Texture> buildLut(GraphicsDevice* device, const uint16_t* halfData, const char* name)
        {
            TextureOptions options;
            options.name = name;
            options.width = kLutSize;
            options.height = kLutSize;
            options.format = PixelFormat::PIXELFORMAT_RGBA16F;
            options.mipmaps = false;
            options.minFilter = FilterMode::FILTER_LINEAR;
            options.magFilter = FilterMode::FILTER_LINEAR;

            auto texture = std::make_shared<Texture>(device, options);
            texture->setLevelData(0, reinterpret_cast<const uint8_t*>(halfData),
                static_cast<size_t>(kLutSize) * kLutSize * 4 * sizeof(uint16_t));
            texture->upload();
            return texture;
        }
    }

    Textures create(GraphicsDevice* device)
    {
        if (!device) {
            return {};
        }
        return {
            buildLut(device, kLtcMat1Half, "areaLightsLutTex1"),
            buildLut(device, kLtcMat2Half, "areaLightsLutTex2")
        };
    }
}
