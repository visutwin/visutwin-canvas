// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassBloom.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/math/color.h"
#include "platform/graphics/graphicsDevice.h"
#include "renderPassDownsample.h"
#include "renderPassUpsample.h"

namespace visutwin::canvas
{
    RenderPassBloom::RenderPassBloom(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
        const PixelFormat format)
        : RenderPass(device), _sourceTexture(sourceTexture), _textureFormat(format)
    {
        _bloomRenderTarget = createRenderTarget(0, _bloomColorTexture);
        _bloomTexture = _bloomColorTexture.get();
    }

    void RenderPassBloom::destroyRenderTargets(const int startIndex)
    {
        for (int i = std::max(startIndex, 0); i < static_cast<int>(_renderTargets.size()); ++i) {
            _renderTargets[i].reset();
        }
        if (startIndex <= 0) {
            _renderTargets.clear();
            _ownedTextures.clear();
        } else if (startIndex < static_cast<int>(_renderTargets.size())) {
            _renderTargets.erase(_renderTargets.begin() + startIndex, _renderTargets.end());
            if (startIndex < static_cast<int>(_ownedTextures.size())) {
                _ownedTextures.erase(_ownedTextures.begin() + startIndex, _ownedTextures.end());
            }
        }
    }

    void RenderPassBloom::destroyRenderPasses()
    {
        clearBeforePasses();
    }

    std::shared_ptr<RenderTarget> RenderPassBloom::createRenderTarget(const int index,
        std::shared_ptr<Texture>& outTexture) const
    {
        TextureOptions textureOptions;
        textureOptions.name = "BloomTexture" + std::to_string(index);
        textureOptions.width = 1;
        textureOptions.height = 1;
        textureOptions.format = _textureFormat;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        outTexture = std::make_shared<Texture>(device().get(), textureOptions);
        outTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        outTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.colorBuffer = outTexture.get();
        options.depth = false;
        options.stencil = false;
        options.name = textureOptions.name;
        return device()->createRenderTarget(options);
    }

    void RenderPassBloom::createRenderTargets(const int count)
    {
        _renderTargets.clear();
        _ownedTextures.clear();
        for (int i = 0; i < count; ++i) {
            if (i == 0) {
                _renderTargets.push_back(_bloomRenderTarget);
                _ownedTextures.push_back(_bloomColorTexture);
            } else {
                std::shared_ptr<Texture> tex;
                _renderTargets.push_back(createRenderTarget(i, tex));
                _ownedTextures.push_back(std::move(tex));
            }
        }
    }

    int RenderPassBloom::calcMipLevels(const uint32_t width, const uint32_t height, const int minSize) const
    {
        const int minimum = std::max(static_cast<int>(std::min(width, height)), 1);
        const float levels = std::log2(static_cast<float>(minimum)) - std::log2(static_cast<float>(std::max(minSize, 1)));
        return std::max(static_cast<int>(std::floor(levels)), 1);
    }

    void RenderPassBloom::createRenderPasses(const int numPasses)
    {
        Texture* passSourceTexture = _sourceTexture;
        for (int i = 0; i < numPasses; ++i) {
            auto pass = std::make_shared<RenderPassDownsample>(device(), passSourceTexture);
            auto options = std::make_shared<RenderPassOptions>();
            options->resizeSource = std::shared_ptr<Texture>(passSourceTexture, [](Texture*) {});
            options->scaleX = 0.5f;
            options->scaleY = 0.5f;
            pass->init(_renderTargets[i], options);
            const Color clearBlack(0.0f, 0.0f, 0.0f, 1.0f);
            pass->setClearColor(&clearBlack);
            addBeforePass(pass);
            passSourceTexture = _renderTargets[i]->colorBuffer();
        }

        passSourceTexture = _renderTargets[numPasses - 1]->colorBuffer();
        for (int i = numPasses - 2; i >= 0; --i) {
            auto pass = std::make_shared<RenderPassUpsample>(device(), passSourceTexture);
            pass->init(_renderTargets[i]);
            // Additive blending during progressive upscale accumulates every mip level into
            // bloom_rt[0]. Without this we only see the result of the last (finest) upsample,
            // losing the wide halo from coarser mips — matching PlayCanvas FramePassBloom
            // (render-passes/frame-pass-bloom.js uses BlendState.ADDBLEND on upsample passes).
            // Source alpha = 1.0 from the upsample shader, so SRC_ALPHA == ONE, giving a pure
            // additive accumulation src + dst.
            pass->setBlendState(std::make_shared<BlendState>(BlendState::additiveBlend()));
            addBeforePass(pass);
            passSourceTexture = _renderTargets[i]->colorBuffer();
        }
    }

    void RenderPassBloom::onDisable()
    {
        if (!_renderTargets.empty() && _renderTargets[0]) {
            _renderTargets[0]->resize(1, 1);
        }

        destroyRenderPasses();
        destroyRenderTargets(1);
    }

    void RenderPassBloom::frameUpdate() const
    {
        RenderPass::frameUpdate();
        auto mutableThis = const_cast<RenderPassBloom*>(this);
        if (!mutableThis->_sourceTexture) {
            return;
        }

        const int maxNumPasses = calcMipLevels(mutableThis->_sourceTexture->width(), mutableThis->_sourceTexture->height(), 1);
        const int numPasses = std::clamp(maxNumPasses, 1, mutableThis->_blurLevel);
        if (static_cast<int>(mutableThis->_renderTargets.size()) != numPasses) {
            mutableThis->destroyRenderPasses();
            mutableThis->destroyRenderTargets(1);
            mutableThis->createRenderTargets(numPasses);
            mutableThis->createRenderPasses(numPasses);
        }
    }
}
