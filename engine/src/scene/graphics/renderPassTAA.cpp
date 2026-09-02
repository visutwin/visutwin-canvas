// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassTAA.h"

#include <algorithm>
#include <cassert>
#include <string>

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/graphics/taaShaders.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/shader.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    RenderPassTAA::RenderPassTAA(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
        CameraComponent* cameraComponent)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture), _cameraComponent(cameraComponent)
    {
        setup();
    }

    void RenderPassTAA::setup()
    {
        const PixelFormat historyFormat = _sourceTexture ? _sourceTexture->format() : PixelFormat::PIXELFORMAT_RGBA8;
        // Start at the source size, not 4x4: resizeSource corrects the history on the
        // first frameUpdate either way, but until then the resolved texture is what
        // downstream passes size themselves from, and a placeholder propagates — the
        // half-res scene texture lands at 2x2 and the bloom chain builds a one-pass
        // version it throws away a frame later.
        const int historyWidth = _sourceTexture ? std::max(static_cast<int>(_sourceTexture->width()), 1) : 4;
        const int historyHeight = _sourceTexture ? std::max(static_cast<int>(_sourceTexture->height()), 1) : 4;
        for (int i = 0; i < 2; ++i) {
            TextureOptions textureOptions;
            textureOptions.name = "TAA-History-" + std::to_string(i);
            textureOptions.width = historyWidth;
            textureOptions.height = historyHeight;
            textureOptions.format = historyFormat;
            textureOptions.mipmaps = false;
            textureOptions.minFilter = FilterMode::FILTER_LINEAR;
            textureOptions.magFilter = FilterMode::FILTER_LINEAR;
            _historyTextures[i] = std::make_shared<Texture>(device().get(), textureOptions);
            _historyTextures[i]->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _historyTextures[i]->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

            RenderTargetOptions targetOptions;
            targetOptions.graphicsDevice = device().get();
            targetOptions.colorBuffer = _historyTextures[i].get();
            targetOptions.depth = false;
            targetOptions.stencil = false;
            targetOptions.name = "TaaHistoryTarget-" + std::to_string(i);
            _historyRenderTargets[i] = device()->createRenderTarget(targetOptions);
        }

        _historyTexture = _historyTextures[0];

        auto options = std::make_shared<RenderPassOptions>();
        if (_sourceTexture) {
            options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        }
        init(_historyRenderTargets[0], options);
    }

    void RenderPassTAA::before()
    {
        if (!_sourceTexture) {
            return;
        }

        // Keep RT sizing in sync if source changed.
        auto options = std::make_shared<RenderPassOptions>();
        options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        setOptions(options);
    }

    void RenderPassTAA::execute()
    {
        const auto gd = device();
        if (!gd || !_sourceTexture || !_cameraComponent || !_cameraComponent->camera()) {
            return;
        }

        auto* camera = _cameraComponent->camera();
        if (!camera) {
            return;
        }

        Texture* sceneDepth = _depthTexture ? _depthTexture : gd->sceneDepthMap();
        if (!sceneDepth || sceneDepth->width() != _sourceTexture->width() || sceneDepth->height() != _sourceTexture->height()) {
            assert(false && "RenderPassTAA strict parity requires valid matching scene depth texture.");
            return;
        }

        std::array<float, 4> cameraParams = {
            camera->farClip() > 0.0f ? (1.0f / camera->farClip()) : 0.0f,
            camera->farClip(),
            camera->nearClip(),
            camera->projection() == ProjectionType::Orthographic ? 1.0f : 0.0f
        };

        if (!shader()) {
            constexpr const char* cacheKey = "taa-quad";
            auto cached = gd->getCachedShader(cacheKey);
            if (!cached) {
                ShaderDefinition definition;
                definition.name = cacheKey;
                definition.vshader = "taaVertex";
                definition.fshader = "taaFragment";
                cached = createShader(gd.get(), definition,
                    gd->shaderLanguage() == ShaderLanguage::Glsl
                        ? taa_shaders::TAA_GLSL : taa_shaders::TAA_MSL);
                if (cached) {
                    gd->setCachedShader(cacheKey, cached);
                }
            }
            setShader(cached);
        }
        if (!shader()) {
            return;
        }

        // Matrix4::getElement takes (col, row); MSL float4x4 and GLSL mat4 are
        // both column-major, so [col * 4 + row] maps straight across.
        const auto packMatrix = [](const Matrix4& m, float* dest) {
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    dest[col * 4 + row] = m.getElement(col, row);
                }
            }
        };

        Texture* history = _historyTextures[1 - _historyIndex].get();

        taa_shaders::TaaUniforms uniforms{};
        packMatrix(camera->viewProjectionPrevious(), uniforms.viewProjectionPrevious);
        packMatrix(camera->viewProjectionInverse(), uniforms.viewProjectionInverse);
        const auto& jitters = camera->jitters();
        for (int i = 0; i < 4; ++i) {
            uniforms.jitters[i] = jitters[i];
            uniforms.cameraParams[i] = cameraParams[i];
        }
        uniforms.texSizeFlags[0] = _sourceTexture
            ? static_cast<float>(_sourceTexture->width()) : 1.0f;
        uniforms.texSizeFlags[1] = _sourceTexture
            ? static_cast<float>(_sourceTexture->height()) : 1.0f;
        uniforms.texSizeFlags[2] = _highQuality ? 1.0f : 0.0f;
        uniforms.texSizeFlags[3] = _historyValid ? 1.0f : 0.0f;

        setQuadTextureBinding(0, _sourceTexture);
        setQuadTextureBinding(1, history);
        setQuadTextureBinding(2, sceneDepth);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
        _historyValid = true;
    }

    void RenderPassTAA::frameUpdate() const
    {
        RenderPass::frameUpdate();
    }

    std::shared_ptr<Texture> RenderPassTAA::update()
    {
        _historyIndex = 1 - _historyIndex;
        _historyTexture = _historyTextures[_historyIndex];

        auto options = std::make_shared<RenderPassOptions>();
        if (_sourceTexture) {
            options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        }
        init(_historyRenderTargets[_historyIndex], options);

        return _historyTexture;
    }
}
