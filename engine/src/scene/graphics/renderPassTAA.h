// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <array>

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class CameraComponent;

    class RenderPassTAA : public RenderPassShaderQuad
    {
    public:
        RenderPassTAA(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture, CameraComponent* cameraComponent);

        void before() override;
        void execute() override;
        void frameUpdate() const override;

        std::shared_ptr<Texture> update();
        std::shared_ptr<Texture> historyTexture() const { return _historyTexture; }
        bool historyValid() const { return _historyValid; }
        void setSourceTexture(Texture* sourceTexture) { _sourceTexture = sourceTexture; }
        void setDepthTexture(Texture* depthTexture) { _depthTexture = depthTexture; }
        void setHighQuality(const bool value) { _highQuality = value; }

    private:
        void setup();

        int _historyIndex = 0;
        Texture* _sourceTexture = nullptr;
        Texture* _depthTexture = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        bool _highQuality = true;
        bool _historyValid = false;

        std::array<std::shared_ptr<Texture>, 2> _historyTextures{};
        std::array<std::shared_ptr<RenderTarget>, 2> _historyRenderTargets{};
        std::shared_ptr<Texture> _historyTexture;
    };
}
