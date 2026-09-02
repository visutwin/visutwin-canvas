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
#include "scene/graphics/ssaoShaders.h"
#include "platform/graphics/shader.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    namespace
    {
        // Was a GraphicsDevice-level struct while SSAO was a device virtual; it is
        // just this pass's local parameter bundle now.
        struct SsaoPassParams
        {
            Texture* depthTexture = nullptr;
            float aspect = 1.0f;
            float invResolutionX = 0.0f;
            float invResolutionY = 0.0f;
            int sampleCount = 12;
            float spiralTurns = 10.0f;
            float angleIncCos = 0.0f;
            float angleIncSin = 0.0f;
            float invRadiusSquared = 0.0f;
            float minHorizonAngleSineSquared = 0.0f;
            float bias = 0.001f;
            float peak2 = 0.0f;
            float intensity = 0.0f;
            float power = 6.0f;
            float projectionScaleRadius = 0.0f;
            float randomize = 0.0f;
            float cameraNear = 0.1f;
            float cameraFar = 1000.0f;
        };
    }


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
        const float step = (1.0f / (static_cast<float>(_sampleCount) - 0.5f)) * spiralTurns * 2.0f * PI;

        // The radius is in world space and so is independent of the render target scale, which
        // keeps the AO look consistent when rendering at a lower resolution.
        const float worldRadius = _radius;

        const float bias = 0.001f;
        const float peak = 0.1f * worldRadius;
        const float computedIntensity = 2.0f * (peak * 2.0f * PI) * _intensity / static_cast<float>(_sampleCount);

        // Scale the projection by the actual (possibly scaled) render target height, so the
        // sampling disk covers the same screen area regardless of the scale.  The shader divides
        // projectionScaleRadius by view-space Z to get a disk radius in target pixels, which it
        // then converts to UV via invResolution — both must refer to the same render target.
        const float projectionScale = 0.5f * height;

        const float minAngleSin = std::sin(_minAngle * DEG_TO_RAD);

        // Blue noise for randomization (simple PRNG matching upstream BlueNoise behavior)
        if (_randomize) {
            _blueNoiseValue = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        } else {
            _blueNoiseValue = 0.0f;
        }

        SsaoPassParams params;
        params.depthTexture = depthTexture;
        params.aspect = aspect;
        params.invResolutionX = 1.0f / width;
        params.invResolutionY = 1.0f / height;
        params.sampleCount = _sampleCount;
        params.spiralTurns = spiralTurns;
        params.angleIncCos = std::cos(step);
        params.angleIncSin = std::sin(step);
        params.invRadiusSquared = 1.0f / (worldRadius * worldRadius);
        params.minHorizonAngleSineSquared = minAngleSin * minAngleSin;
        params.bias = bias;
        params.peak2 = peak * peak;
        params.intensity = computedIntensity;
        params.power = _power;
        params.projectionScaleRadius = projectionScale * worldRadius;
        params.randomize = _blueNoiseValue;
        params.cameraNear = camera->nearClip();
        params.cameraFar = camera->farClip();

        if (!shader()) {
            constexpr const char* cacheKey = "ssao-quad";
            auto cached = gd->getCachedShader(cacheKey);
            if (!cached) {
                ShaderDefinition definition;
                definition.name = cacheKey;
                definition.vshader = "ssaoVertex";
                definition.fshader = "ssaoFragment";
                cached = createShader(gd.get(), definition,
                    gd->shaderLanguage() == ShaderLanguage::Glsl
                        ? ssao_shaders::SSAO_GLSL : ssao_shaders::SSAO_MSL);
                if (cached) {
                    gd->setCachedShader(cacheKey, cached);
                }
            }
            setShader(cached);
        }
        if (!shader()) {
            return;
        }

        ssao_shaders::SsaoUniforms uniforms{};
        uniforms.aspect = params.aspect;
        uniforms.invResolution[0] = params.invResolutionX;
        uniforms.invResolution[1] = params.invResolutionY;
        uniforms.sampleCount[0] = static_cast<float>(params.sampleCount);
        uniforms.sampleCount[1] = 1.0f / static_cast<float>(params.sampleCount);
        uniforms.spiralTurns = params.spiralTurns;
        uniforms.angleIncCosSin[0] = params.angleIncCos;
        uniforms.angleIncCosSin[1] = params.angleIncSin;
        uniforms.maxLevel = 0.0f;
        uniforms.invRadiusSquared = params.invRadiusSquared;
        uniforms.minHorizonAngleSineSquared = params.minHorizonAngleSineSquared;
        uniforms.bias = params.bias;
        uniforms.peak2 = params.peak2;
        uniforms.intensity = params.intensity;
        uniforms.power = params.power;
        uniforms.projectionScaleRadius = params.projectionScaleRadius;
        uniforms.randomize = params.randomize;
        uniforms.cameraNear = params.cameraNear;
        uniforms.cameraFar = params.cameraFar;

        setQuadTextureBinding(0, params.depthTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
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
