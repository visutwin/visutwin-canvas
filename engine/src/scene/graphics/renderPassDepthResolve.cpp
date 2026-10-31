// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// The depth resolve of a render target over multisampled textures: a quad over the resolve
// buffer reads every sample of the depth texture (the Slang program `depth-resolve`) and keeps
// the first, the nearest or the farthest. Neither backend's hardware depth resolve writes a
// colour format, and Metal's min / max filters are optional, so it is a shader pass on both.
//
#include "renderPassDepthResolve.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        struct DepthResolveUniforms
        {
            uint32_t params[4];  // x: the mode, y, z: the size, w: the samples
        };
    }

    RenderPassDepthResolve::RenderPassDepthResolve(const std::shared_ptr<GraphicsDevice>& device,
        const std::shared_ptr<RenderTarget>& source)
        : RenderPassShaderQuad(device), _source(source)
    {
        update();
    }

    void RenderPassDepthResolve::update()
    {
        const auto source = _source.lock();
        Texture* resolve = source ? source->depthResolveBuffer() : nullptr;
        if (!resolve) {
            return;
        }
        const auto width = static_cast<int>(resolve->width());
        const auto height = static_cast<int>(resolve->height());
        if (resolve == _resolveTexture && width == _resolveWidth && height == _resolveHeight && renderTarget()) {
            return;
        }
        _resolveTexture = resolve;
        _resolveWidth = width;
        _resolveHeight = height;

        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.colorBuffer = resolve;
        options.depth = false;
        options.name = "DepthResolve";
        init(device()->createRenderTarget(options));
        // Every texel is written: nothing to clear or load.
        setClearColor(nullptr);
    }

    void RenderPassDepthResolve::prepareShaders()
    {
        if (!shader()) {
            useSlangShader("depth-resolve");
        }
    }

    void RenderPassDepthResolve::execute()
    {
        const auto source = _source.lock();
        Texture* depth = source ? source->depthBuffer() : nullptr;
        if (!depth || depth->samples() <= 1 || !_resolveTexture) {
            return;
        }
        prepareShaders();
        if (!shader()) {
            return;
        }
        const DepthResolveUniforms uniforms{{static_cast<uint32_t>(source->depthResolveMode()),
            static_cast<uint32_t>(_resolveWidth), static_cast<uint32_t>(_resolveHeight), depth->samples()}};
        setQuadUniforms(uniforms);
        setQuadTextureBinding(0, depth);
        RenderPassShaderQuad::execute();
    }
}
