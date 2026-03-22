// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <algorithm>
#include <vector>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class RenderPassBloom : public RenderPass
    {
    public:
        RenderPassBloom(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture, PixelFormat format);
        ~RenderPassBloom() = default;

        void frameUpdate() const override;
        void onDisable() override;

        Texture* bloomTexture() const { return _bloomTexture; }
        void setBlurLevel(const int value) { _blurLevel = std::max(value, 1); }

    private:
        void destroyRenderTargets(int startIndex = 0);
        void destroyRenderPasses();
        std::shared_ptr<RenderTarget> createRenderTarget(int index, std::shared_ptr<Texture>& outTexture) const;
        void createRenderTargets(int count);
        int calcMipLevels(uint32_t width, uint32_t height, int minSize) const;
        void createRenderPasses(int numPasses);

        Texture* _sourceTexture = nullptr;
        PixelFormat _textureFormat = PixelFormat::PIXELFORMAT_RGBA8;
        int _blurLevel = 16;
        std::shared_ptr<RenderTarget> _bloomRenderTarget;
        Texture* _bloomTexture = nullptr;
        std::vector<std::shared_ptr<RenderTarget>> _renderTargets;

        // Textures must outlive the RenderTargets that reference them (RenderTarget
        // stores only a raw Texture*). We keep them alive here.
        std::shared_ptr<Texture> _bloomColorTexture;
        std::vector<std::shared_ptr<Texture>> _ownedTextures;
    };
}
