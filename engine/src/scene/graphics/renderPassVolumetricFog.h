// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <array>
#include <memory>
#include <string>

#include "core/math/matrix4.h"
#include "framework/components/camera/cameraComponent.h"
#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class Light;
    class QuadRender;
    class Scene;
    class Shader;

    /**
     * Ray-marched volumetric fog (upstream FramePassVolumetricFog). Marches from the camera to the
     * scene surface accumulating in-scattered directional light and transmittance into a
     * reduced-resolution RGBA16F texture.
     *
     * The companion RenderPassVolumetricFogCombine upsamples that texture and blends it over the
     * scene. Both are driven from CameraComponent::VolumetricFogSettings.
     *
     * The clustered omni and spot lights (VolumetricFogSettings::localOmniLights /
     * localSpotLights, upstream RenderPassVolumetricFogLocal) are added into the same texture
     * by this pass, after the march: a fullscreen triangle per light, scissored to the
     * screen bounds of its volume, additive in colour and keeping the march's
     * transmittance. DEVIATION: upstream gives them a pass of their own drawing a quad over
     * those bounds; one target and a scissor give the same pixels.
     */
    class RenderPassVolumetricFog : public RenderPassShaderQuad
    {
    public:
        RenderPassVolumetricFog(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
            CameraComponent* cameraComponent);
        ~RenderPassVolumetricFog();

        /// The fog texture: rgb = in-scattered light, a = transmittance.
        Texture* fogTexture() const { return _fogTexture.get(); }

        void setSettings(const VolumetricFogSettings& settings);
        const VolumetricFogSettings& settings() const { return _settings; }

        /// The scene supplies the directional light and the exposure the fog is matched to.
        void setScene(Scene* scene) { _scene = scene; }

        void execute() override;

        void setScale(float value);
        float scale() const { return _scale; }

    private:
        void renderLocalLights(Texture* depthTexture, const Matrix4& cameraWorld, float projScaleX,
            float projScaleY, float exposure, float noiseOffset);

        Texture* _sourceTexture = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        Scene* _scene = nullptr;
        std::shared_ptr<Shader> _localShader;
        std::shared_ptr<QuadRender> _localQuad;

        VolumetricFogSettings _settings;
        float _scale = 0.5f;

        // Cycles the ray-march dither so TAA converges it.
        int _frameIndex = 0;

        std::shared_ptr<Texture> _fogTexture;
        std::shared_ptr<RenderTarget> _fogRenderTarget;
    };

    /**
     * Depth-aware upsample of the fog texture, blended over the scene render target as
     * `scene * transmittance + inscatter`.
     */
    class RenderPassVolumetricFogCombine : public RenderPassShaderQuad
    {
    public:
        RenderPassVolumetricFogCombine(const std::shared_ptr<GraphicsDevice>& device,
            CameraComponent* cameraComponent, Texture* fogTexture);

        void setFogTexture(Texture* value) { _fogTexture = value; }

        void execute() override;

    private:
        CameraComponent* _cameraComponent = nullptr;
        Texture* _fogTexture = nullptr;
    };
}
