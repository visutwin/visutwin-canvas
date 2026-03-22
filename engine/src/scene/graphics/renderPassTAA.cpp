// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassTAA.h"

#include <cassert>
#include <string>

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    RenderPassTAA::RenderPassTAA(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
        CameraComponent* cameraComponent)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture), _cameraComponent(cameraComponent)
    {
        setup();
    }

    void RenderPassTAA::setup()
    {
        const PixelFormat historyFormat = _sourceTexture ? _sourceTexture->format() : PixelFormat::PIXELFORMAT_RGBA8;
        for (int i = 0; i < 2; ++i) {
            TextureOptions textureOptions;
            textureOptions.name = "TAA-History-" + std::to_string(i);
            textureOptions.width = 4;
            textureOptions.height = 4;
            textureOptions.format = historyFormat;
            textureOptions.mipmaps = false;
            textureOptions.minFilter = FilterMode::FILTER_LINEAR;
            textureOptions.magFilter = FilterMode::FILTER_LINEAR;
            _historyTextures[i] = std::make_shared<Texture>(device().get(), textureOptions);
            _historyTextures[i]->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _historyTextures[i]->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

            RenderTargetOptions targetOptions;
            targetOptions.graphicsDevice = device().get();
            targetOptions.colorBuffer = _historyTextures[i].get();
            targetOptions.depth = false;
            targetOptions.stencil = false;
            targetOptions.name = "TaaHistoryTarget-" + std::to_string(i);
            _historyRenderTargets[i] = device()->createRenderTarget(targetOptions);
        }

        _historyTexture = _historyTextures[0];

        auto options = std::make_shared<RenderPassOptions>();
        if (_sourceTexture) {
            options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        }
        init(_historyRenderTargets[0], options);
    }

    void RenderPassTAA::before()
    {
        if (!_sourceTexture) {
            return;
        }

        // Keep RT sizing in sync if source changed.
        auto options = std::make_shared<RenderPassOptions>();
        options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        setOptions(options);
    }

    void RenderPassTAA::execute()
    {
        const auto gd = device();
        if (!gd || !_sourceTexture || !_cameraComponent || !_cameraComponent->camera()) {
            return;
        }

        auto* camera = _cameraComponent->camera();
        if (!camera) {
            return;
        }

        Texture* sceneDepth = _depthTexture ? _depthTexture : gd->sceneDepthMap();
        if (!sceneDepth || sceneDepth->width() != _sourceTexture->width() || sceneDepth->height() != _sourceTexture->height()) {
            assert(false && "RenderPassTAA strict parity requires valid matching scene depth texture.");
            return;
        }

        std::array<float, 4> cameraParams = {
            camera->farClip() > 0.0f ? (1.0f / camera->farClip()) : 0.0f,
            camera->farClip(),
            camera->nearClip(),
            camera->projection() == ProjectionType::Orthographic ? 1.0f : 0.0f
        };

        gd->executeTAAPass(_sourceTexture,
            _historyTextures[1 - _historyIndex].get(),
            sceneDepth,
            camera->viewProjectionPrevious(),
            camera->viewProjectionInverse(),
            camera->jitters(),
            cameraParams,
            _highQuality,
            _historyValid);
        _historyValid = true;
    }

    void RenderPassTAA::frameUpdate() const
    {
        RenderPass::frameUpdate();
    }

    std::shared_ptr<Texture> RenderPassTAA::update()
    {
        _historyIndex = 1 - _historyIndex;
        _historyTexture = _historyTextures[_historyIndex];

        auto options = std::make_shared<RenderPassOptions>();
        if (_sourceTexture) {
            options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        }
        init(_historyRenderTargets[_historyIndex], options);

        return _historyTexture;
    }
}
