// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassTAA.h"

#include <algorithm>
#include <cassert>
#include <string>

#include "scene/graphics/colorTarget.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/graphics/taaShaders.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/shader.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    RenderPassTAA::RenderPassTAA(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
        CameraComponent* cameraComponent, const bool upscale)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture), _cameraComponent(cameraComponent),
          _upscale(upscale)
    {
        setup();
    }

    void RenderPassTAA::setup()
    {
        // The history accumulates each frame with a small weight, so it needs at least half
        // float precision: in a lower one the small steps towards the converged value round
        // away and the history stops converging.
        const auto gd = device();
        PixelFormat historyFormat = _sourceTexture ? _sourceTexture->format() : PixelFormat::PIXELFORMAT_RGBA8;
        if (gd && gd->textureHalfFloatRenderable()) {
            historyFormat = PixelFormat::PIXELFORMAT_RGBA16F;
        } else if (gd && gd->textureFloatRenderable()) {
            historyFormat = PixelFormat::PIXELFORMAT_RGBA32F;
        }
        // Start at the source size, not 4x4: resizeSource corrects the history on the
        // first frameUpdate either way, but until then the resolved texture is what
        // downstream passes size themselves from, and a placeholder propagates — the
        // half-res scene texture lands at 2x2 and the bloom chain builds a one-pass
        // version it throws away a frame later.
        const int historyWidth = _sourceTexture ? std::max(static_cast<int>(_sourceTexture->width()), 1) : 4;
        const int historyHeight = _sourceTexture ? std::max(static_cast<int>(_sourceTexture->height()), 1) : 4;
        for (int i = 0; i < 2; ++i) {
            auto [texture, target] = createColorTarget(device().get(), "TAA-History-" + std::to_string(i),
                historyFormat, historyWidth, historyHeight, "TaaHistoryTarget-" + std::to_string(i));
            _historyTextures[i] = std::move(texture);
            _historyRenderTargets[i] = std::move(target);
        }

        _historyTexture = _historyTextures[0];

        // Upscaling, no options: with no resize source a pass follows the back buffer, which is
        // not what a camera rendering into a texture of its own composes into.
        std::shared_ptr<RenderPassOptions> options;
        if (!_upscale) {
            options = std::make_shared<RenderPassOptions>();
            if (_sourceTexture) {
                options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
            }
        }
        init(_historyRenderTargets[0], options);
    }

    void RenderPassTAA::setOutputSize(const int width, const int height)
    {
        // Both histories: the one read this frame is resized too, a frame of history lost at a
        // resize as the source-sized history loses it.
        for (const auto& target : _historyRenderTargets) {
            if (target && (static_cast<int>(target->width()) != width || static_cast<int>(target->height()) != height)) {
                target->resize(std::max(width, 1), std::max(height, 1));
            }
        }
    }

    void RenderPassTAA::before()
    {
        // Upscaling, the history follows the output size instead (setOutputSize).
        if (!_sourceTexture || _upscale) {
            return;
        }

        // Keep RT sizing in sync if source changed.
        auto options = std::make_shared<RenderPassOptions>();
        options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        setOptions(options);
    }

    void RenderPassTAA::prepareShaders()
    {
        if (!shader()) {
            useSlangShader("taa", _upscale ? "upscale" : "resolve");
        }
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

        // Normally there already: the frame graph prepares every pass's shaders first.
        prepareShaders();
        if (!shader()) {
            return;
        }

        Texture* history = _historyTextures[1 - _historyIndex].get();

        taa_shaders::TaaUniforms uniforms{};
        // MSL float4x4 and GLSL mat4 are both column-major, which is what Matrix4::store writes.
        camera->viewProjectionPrevious().store(uniforms.viewProjectionPrevious);
        camera->viewProjectionInverse().store(uniforms.viewProjectionInverse);
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
        uniforms.blendParams[0] = _displayExposure;
        // The size of the history read, which differs from the source's when upscaling (and
        // from the one written on a frame that resizes it).
        uniforms.blendParams[1] = history ? static_cast<float>(history->width()) : uniforms.texSizeFlags[0];
        uniforms.blendParams[2] = history ? static_cast<float>(history->height()) : uniforms.texSizeFlags[1];

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

        std::shared_ptr<RenderPassOptions> options;
        if (!_upscale) {
            options = std::make_shared<RenderPassOptions>();
            if (_sourceTexture) {
                options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
            }
        }
        init(_historyRenderTargets[_historyIndex], options);

        return _historyTexture;
    }
}
