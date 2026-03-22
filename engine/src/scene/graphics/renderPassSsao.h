// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <memory>
#include <string>

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class RenderPassDepthAwareBlur;

    /**
     * Render pass implementation of Screen-Space Ambient Occlusion (SSAO) based on
     * the non-linear depth buffer. Uses Scalable Ambient Obscurance algorithm
     * (Morgan McGuire) adapted by Naughty Dog.
     */
    class RenderPassSsao : public RenderPassShaderQuad
    {
    public:
        RenderPassSsao(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
            CameraComponent* cameraComponent, bool blurEnabled);
        ~RenderPassSsao();

        // SSAO parameters (matching upstream RenderPassSsao)
        float radius = 30.0f;
        float intensity = 0.5f;
        float power = 6.0f;
        int sampleCount = 12;
        float minAngle = 10.0f;
        bool randomize = false;

        /// The output SSAO texture (R8, single channel occlusion).
        Texture* ssaoTexture() const { return _ssaoTexture.get(); }

        void execute() override;
        void after() override;

        void setScale(float value);
        float scale() const { return _scale; }

    private:
        std::shared_ptr<RenderTarget> createSsaoRenderTarget(const std::string& name,
            std::shared_ptr<Texture>& outTexture) const;

        Texture* _sourceTexture = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        bool _blurEnabled = true;
        float _scale = 1.0f;

        std::shared_ptr<Texture> _ssaoTexture;
        std::shared_ptr<RenderTarget> _ssaoRenderTarget;

        // Blur temp resources
        std::shared_ptr<Texture> _blurTempTexture;
        std::shared_ptr<RenderTarget> _blurTempRenderTarget;
        std::shared_ptr<RenderPassDepthAwareBlur> _blurPassH;
        std::shared_ptr<RenderPassDepthAwareBlur> _blurPassV;

        float _blueNoiseValue = 0.0f;
    };
}
