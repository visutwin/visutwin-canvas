// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025.
//
#include "renderPassColorGrab.h"

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    void RenderPassColorGrab::execute()
    {
        // Copy the scene color rendered so far (opaque + depth layers) into the
        // device's mipmapped scene-color texture for dynamic grab-pass refraction.
        if (const auto device = this->device()) {
            device->grabSceneColor(_source.get());
        }
    }
}
