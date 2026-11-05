// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025
//
#include "renderPassColorGrab.h"

#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"
#include "sceneGrab.h"

namespace visutwin::canvas
{
    RenderPassColorGrab::~RenderPassColorGrab()
    {
        if (const auto device = this->device();
            device && _grabTexture && device->sceneColorMap() == _grabTexture.get()) {
            device->setSceneColorMap(nullptr);
        }
    }

    void RenderPassColorGrab::prepareShaders()
    {
        if (_camera && _camera->sceneColorMapMipmaps() &&
            _camera->sceneColorMapMipmapFilter() == MipmapFilter::MIPMAPFILTER_GAUSSIAN) {
            _gaussianMipmaps.prepareShaders();
        }
    }

    void RenderPassColorGrab::execute()
    {
        // Copy the scene colour rendered so far (opaque + depth layers) into a
        // mipmapped texture for dynamic grab-pass refraction: rough surfaces read
        // the blurrier mips, unless the camera asks for no mipmaps. The device only offers
        // the generic copy, so the destination and the decision to mip it belong here.
        const auto device = this->device();
        if (!device) {
            return;
        }
        const bool mipmaps = !_camera || _camera->sceneColorMapMipmaps();
        Texture* destination = ensureGrabTexture(device.get(), _source.get(),
            _grabTexture, false, mipmaps, "sceneColorGrab");
        if (!destination) {
            return;
        }
        // The filter the camera asks for applies to this generation; the Gaussian one needs its
        // render targets BEFORE the copy (GaussianMipmapGenerator::prepare).
        if (mipmaps) {
            destination->setMipmapFilter(_camera ? _camera->sceneColorMapMipmapFilter()
                                                 : MipmapFilter::MIPMAPFILTER_BOX);
            if (destination->gaussianMipmaps()) {
                _gaussianMipmaps.prepare(destination);
            }
        }
        device->copyRenderTarget(_source.get(), destination, nullptr);
        if (mipmaps) {
            if (!destination->gaussianMipmaps() || !_gaussianMipmaps.generate(destination)) {
                device->generateMipmaps(destination);
            }
        }
        device->setSceneColorMap(destination);
        if (_camera) {
            _camera->publishSceneColorMap(_grabTexture, _gamma);
        }
    }
}
