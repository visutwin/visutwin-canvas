// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025.
//
#include "renderPassColorGrab.h"

#include "platform/graphics/graphicsDevice.h"
#include "sceneGrab.h"

namespace visutwin::canvas
{
    void RenderPassColorGrab::execute()
    {
        // Copy the scene colour rendered so far (opaque + depth layers) into a
        // mipmapped texture for dynamic grab-pass refraction: rough surfaces read
        // the blurrier mips. The device only offers the generic copy, so the
        // destination and the decision to mip it belong here.
        const auto device = this->device();
        if (!device) {
            return;
        }
        Texture* destination = ensureGrabTexture(device.get(), _source.get(),
            _grabTexture, false, true, "sceneColorGrab");
        if (!destination) {
            return;
        }
        device->copyRenderTarget(_source.get(), destination, nullptr);
        device->generateMipmaps(destination);
        device->setSceneColorMap(destination);
    }
}
