// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    /*
     * A class handling runtime allocation of slots in a texture
     */
    class LightTextureAtlas
    {
    public:
        LightTextureAtlas(const std::shared_ptr<GraphicsDevice>& device) : _device(device) {}

    private:
        const std::shared_ptr<GraphicsDevice>& _device;
    };
}
