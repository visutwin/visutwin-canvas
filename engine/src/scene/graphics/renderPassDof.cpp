// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassDof.h"

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    RenderPassDof::RenderPassDof(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent,
        Texture* sceneTexture, Texture* sceneTextureHalf, const bool highQualityValue, const bool nearBlurValue)
        : RenderPass(device), _cameraComponent(cameraComponent), _sceneTexture(sceneTexture), _sceneTextureHalf(sceneTextureHalf),
          highQuality(highQualityValue), nearBlur(nearBlurValue)
    {
        const PixelFormat sourceFormat = sceneTexture ? sceneTexture->format() : PixelFormat::PIXELFORMAT_RGBA8;
        Texture* halfSource = sceneTextureHalf ? sceneTextureHalf : sceneTexture;

        const auto cocFormat = nearBlur ? PixelFormat::PIXELFORMAT_RG8 : PixelFormat::PIXELFORMAT_R8;
        _cocTarget = createRenderTarget("CoCTexture", cocFormat, _cocTexture);

        Texture* sourceTexture = sceneTexture;
        if (_cocTarget && sourceTexture) {
            _cocPass = std::make_shared<RenderPassCoC>(device, cameraComponent, nearBlur);
            auto options = std::make_shared<RenderPassOptions>();
            options->resizeSource = std::shared_ptr<Texture>(sourceTexture, [](Texture*) {});
            _cocPass->init(_cocTarget, options);
            const Color clearBlack(0.0f, 0.0f, 0.0f, 1.0f);
            _cocPass->setClearColor(&clearBlack);
            addBeforePass(_cocPass);
        }

        Texture* farSource = highQuality ? sceneTexture : halfSource;
        if (farSource) {
            _farTarget = createRenderTarget("FarDofTexture", farSource->format(), _farTexture);
            if (_farTarget) {
                RenderPassDownsample::Options downsampleOptions;
                downsampleOptions.boxFilter = true;
                downsampleOptions.premultiplyTexture = _cocTexture.get();
                downsampleOptions.premultiplySrcChannel = 'r';
                _farPass = std::make_shared<RenderPassDownsample>(device, farSource, downsampleOptions);
                auto options = std::make_shared<RenderPassOptions>();
                options->resizeSource = std::shared_ptr<Texture>(farSource, [](Texture*) {});
                options->scaleX = 0.5f;
                options->scaleY = 0.5f;
                _farPass->init(_farTarget, options);
                const Color clearBlack(0.0f, 0.0f, 0.0f, 1.0f);
                _farPass->setClearColor(&clearBlack);
                addBeforePass(_farPass);
            }
        }

        if (halfSource) {
            _blurTarget = createRenderTarget("DofBlurTexture", sourceFormat, _blurTexture);
            if (_blurTarget && _farTarget) {
                Texture* farTexture = _farTarget->colorBuffer();
                _blurPass = std::make_shared<RenderPassDofBlur>(device, nearBlur ? halfSource : nullptr, farTexture, _cocTexture.get());
                auto options = std::make_shared<RenderPassOptions>();
                options->resizeSource = std::shared_ptr<Texture>(halfSource, [](Texture*) {});
                options->scaleX = highQuality ? 2.0f : 0.5f;
                options->scaleY = highQuality ? 2.0f : 0.5f;
                _blurPass->init(_blurTarget, options);
                const Color clearBlack(0.0f, 0.0f, 0.0f, 1.0f);
                _blurPass->setClearColor(&clearBlack);
                addBeforePass(_blurPass);
            }
        }
    }

    void RenderPassDof::frameUpdate() const
    {
        RenderPass::frameUpdate();

        if (_cocPass) {
            _cocPass->focusDistance = focusDistance;
            _cocPass->focusRange = focusRange;
        }

        if (_blurPass) {
            _blurPass->blurRadiusNear = blurRadius;
            _blurPass->blurRadiusFar = blurRadius * (highQuality ? 1.0f : 0.5f);
            _blurPass->setBlurRings(blurRings);
            _blurPass->setBlurRingPoints(blurRingPoints);
        }
    }

    std::shared_ptr<Texture> RenderPassDof::createTexture(const std::string& name, const PixelFormat format) const
    {
        TextureOptions textureOptions;
        textureOptions.name = name;
        textureOptions.width = 1;
        textureOptions.height = 1;
        textureOptions.format = format;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device().get(), textureOptions);
        texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        return texture;
    }

    std::shared_ptr<RenderTarget> RenderPassDof::createRenderTarget(const std::string& name, const PixelFormat format,
        std::shared_ptr<Texture>& outColorTexture) const
    {
        outColorTexture = createTexture(name, format);
        if (!outColorTexture) {
            return nullptr;
        }

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device().get();
        targetOptions.colorBuffer = outColorTexture.get();
        targetOptions.depth = false;
        targetOptions.stencil = false;
        targetOptions.name = name;

        return device()->createRenderTarget(targetOptions);
    }
}
