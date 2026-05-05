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

        float focusDistance() const { return _focusDistance; }
        void setFocusDistance(const float value) { _focusDistance = value; }

        float focusRange() const { return _focusRange; }
        void setFocusRange(const float value) { _focusRange = value; }

        float blurRadius() const { return _blurRadius; }
        void setBlurRadius(const float value) { _blurRadius = value; }

        int blurRings() const { return _blurRings; }
        void setBlurRings(const int value) { _blurRings = value; }

        int blurRingPoints() const { return _blurRingPoints; }
        void setBlurRingPoints(const int value) { _blurRingPoints = value; }

        bool highQuality() const { return _highQuality; }
        void setHighQuality(const bool value) { _highQuality = value; }

        bool nearBlur() const { return _nearBlur; }
        void setNearBlur(const bool value) { _nearBlur = value; }

        Texture* cocTexture() const { return _cocTexture.get(); }
        Texture* blurTexture() const { return _blurTexture.get(); }

    private:
        std::shared_ptr<Texture> createTexture(const std::string& name, PixelFormat format) const;
        std::shared_ptr<RenderTarget> createRenderTarget(const std::string& name, PixelFormat format,
            std::shared_ptr<Texture>& outColorTexture) const;

        CameraComponent* _cameraComponent = nullptr;
        Texture* _sceneTexture = nullptr;
        Texture* _sceneTextureHalf = nullptr;

        float _focusDistance = 100.0f;
        float _focusRange = 10.0f;
        float _blurRadius = 3.0f;
        int _blurRings = 4;
        int _blurRingPoints = 5;
        bool _highQuality = true;
        bool _nearBlur = false;

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
