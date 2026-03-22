// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include "framework/components/camera/cameraComponent.h"
#include "renderPassCoC.h"
#include "renderPassDofBlur.h"
#include "renderPassDownsample.h"

namespace visutwin::canvas
{
    class RenderPassDof : public RenderPass
    {
    public:
        RenderPassDof(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent, Texture* sceneTexture,
            Texture* sceneTextureHalf, bool highQuality, bool nearBlur);

        void frameUpdate() const override;

        float focusDistance = 100.0f;
        float focusRange = 10.0f;
        float blurRadius = 3.0f;
        int blurRings = 4;
        int blurRingPoints = 5;
        bool highQuality = true;
        bool nearBlur = false;

        Texture* cocTexture() const { return _cocTexture.get(); }
        Texture* blurTexture() const { return _blurTexture.get(); }

    private:
        std::shared_ptr<Texture> createTexture(const std::string& name, PixelFormat format) const;
        std::shared_ptr<RenderTarget> createRenderTarget(const std::string& name, PixelFormat format,
            std::shared_ptr<Texture>& outColorTexture) const;

        CameraComponent* _cameraComponent = nullptr;
        Texture* _sceneTexture = nullptr;
        Texture* _sceneTextureHalf = nullptr;

        std::shared_ptr<RenderPassCoC> _cocPass;
        std::shared_ptr<RenderPassDownsample> _farPass;
        std::shared_ptr<RenderPassDofBlur> _blurPass;

        std::shared_ptr<Texture> _cocTexture;
        std::shared_ptr<Texture> _farTexture;
        std::shared_ptr<Texture> _blurTexture;
        std::shared_ptr<RenderTarget> _cocTarget;
        std::shared_ptr<RenderTarget> _farTarget;
        std::shared_ptr<RenderTarget> _blurTarget;
    };
}
