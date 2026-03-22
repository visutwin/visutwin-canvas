// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassSsao.h"

#include <cmath>
#include <random>

#include "renderPassDepthAwareBlur.h"
#include "core/math/color.h"
#include "core/math/defines.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "scene/camera.h"

namespace visutwin::canvas
{

    RenderPassSsao::RenderPassSsao(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
        CameraComponent* cameraComponent, const bool blurEnabled)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture),
          _cameraComponent(cameraComponent), _blurEnabled(blurEnabled)
    {
        // Create main SSAO render target (R8 format, single-channel occlusion).
        // The texture must be kept alive by the member _ssaoTexture (RenderTarget stores raw pointer).
        _ssaoRenderTarget = createSsaoRenderTarget("SsaoFinalTexture", _ssaoTexture);

        auto options = std::make_shared<RenderPassOptions>();
        options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        init(_ssaoRenderTarget, options);

        // Clear color to white (no occlusion) to avoid load op
        const Color clearWhite(1.0f, 1.0f, 1.0f, 1.0f);
        setClearColor(&clearWhite);

        // Optional bilateral blur passes
        if (blurEnabled) {
            _blurTempRenderTarget = createSsaoRenderTarget("SsaoTempTexture", _blurTempTexture);

            // Horizontal blur: reads from SSAO RT → writes to temp RT
            _blurPassH = std::make_shared<RenderPassDepthAwareBlur>(
                device, _ssaoTexture.get(), _cameraComponent, true);
            auto blurHOptions = std::make_shared<RenderPassOptions>();
            blurHOptions->resizeSource = _ssaoTexture;
            _blurPassH->init(_blurTempRenderTarget, blurHOptions);
            const Color clearBlack(0.0f, 0.0f, 0.0f, 0.0f);
            _blurPassH->setClearColor(&clearBlack);
            addAfterPass(_blurPassH);

            // Vertical blur: reads from temp RT → writes back to SSAO RT
            _blurPassV = std::make_shared<RenderPassDepthAwareBlur>(
                device, _blurTempTexture.get(), _cameraComponent, false);
            auto blurVOptions = std::make_shared<RenderPassOptions>();
            blurVOptions->resizeSource = _ssaoTexture;
            _blurPassV->init(_ssaoRenderTarget, blurVOptions);
            _blurPassV->setClearColor(&clearBlack);
            addAfterPass(_blurPassV);
        }
    }

    RenderPassSsao::~RenderPassSsao() = default;

    std::shared_ptr<RenderTarget> RenderPassSsao::createSsaoRenderTarget(const std::string& name,
        std::shared_ptr<Texture>& outTexture) const
    {
        TextureOptions textureOptions;
        textureOptions.name = name;
        textureOptions.width = 1;
        textureOptions.height = 1;
        textureOptions.format = PixelFormat::PIXELFORMAT_R8;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        outTexture = std::make_shared<Texture>(device().get(), textureOptions);
        outTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        outTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = device().get();
        rtOptions.colorBuffer = outTexture.get();
        rtOptions.depth = false;
        rtOptions.stencil = false;
        rtOptions.name = name;
        return device()->createRenderTarget(rtOptions);
    }

    void RenderPassSsao::execute()
    {
        const auto gd = device();
        if (!gd || !_cameraComponent || !_cameraComponent->camera()) {
            return;
        }

        // Get depth texture from the graphics device
        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            return;
        }

        const auto* camera = _cameraComponent->camera();
        const auto rt = renderTarget();
        if (!rt || !rt->colorBuffer()) {
            return;
        }

        const auto width = static_cast<float>(rt->colorBuffer()->width());
        const auto height = static_cast<float>(rt->colorBuffer()->height());

        if (width <= 0.0f || height <= 0.0f) {
            return;
        }

        // Compute derived SSAO parameters (matching upstream RenderPassSsao.execute())
        const float aspect = width / height;
        const float spiralTurns = 10.0f;
        const float step = (1.0f / (static_cast<float>(sampleCount) - 0.5f)) * spiralTurns * 2.0f * PI;
        const float effectiveRadius = radius / scale();

        const float bias = 0.001f;
        const float peak = 0.1f * effectiveRadius;
        const float computedIntensity = 2.0f * (peak * 2.0f * PI) * intensity / static_cast<float>(sampleCount);
        const float projectionScale = 0.5f * (_sourceTexture ? static_cast<float>(_sourceTexture->height()) : height);

        const float minAngleSin = std::sin(minAngle * DEG_TO_RAD);

        // Blue noise for randomization (simple PRNG matching upstream BlueNoise behavior)
        if (randomize) {
            _blueNoiseValue = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        } else {
            _blueNoiseValue = 0.0f;
        }

        SsaoPassParams params;
        params.depthTexture = depthTexture;
        params.aspect = aspect;
        params.invResolutionX = 1.0f / width;
        params.invResolutionY = 1.0f / height;
        params.sampleCount = sampleCount;
        params.spiralTurns = spiralTurns;
        params.angleIncCos = std::cos(step);
        params.angleIncSin = std::sin(step);
        params.invRadiusSquared = 1.0f / (effectiveRadius * effectiveRadius);
        params.minHorizonAngleSineSquared = minAngleSin * minAngleSin;
        params.bias = bias;
        params.peak2 = peak * peak;
        params.intensity = computedIntensity;
        params.power = power;
        params.projectionScaleRadius = projectionScale * effectiveRadius;
        params.randomize = _blueNoiseValue;
        params.cameraNear = camera->nearClip();
        params.cameraFar = camera->farClip();

        gd->executeSsaoPass(params);
    }

    void RenderPassSsao::after()
    {
        // The SSAO texture is now available for the compose pass
    }

    void RenderPassSsao::setScale(const float value)
    {
        _scale = value;
        auto options = std::make_shared<RenderPassOptions>();
        options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        options->scaleX = value;
        options->scaleY = value;
        setOptions(options);
    }
}
