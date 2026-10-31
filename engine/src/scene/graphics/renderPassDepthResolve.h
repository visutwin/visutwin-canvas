// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
#pragma once

#include <memory>

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class RenderTarget;
    class Texture;

    /**
     * Resolves the multisampled depth texture of a render target into its depth resolve buffer
     * (RenderTargetOptions::depthResolveBuffer, R32F), by the target's depth resolve mode: the
     * first sample, or the nearest or farthest. The forward renderer runs it after each block of
     * passes that renders the target. It holds the source target weakly and renders into a
     * target of its own over the resolve buffer, rebuilt when the buffer changes.
     */
    class RenderPassDepthResolve : public RenderPassShaderQuad
    {
    public:
        RenderPassDepthResolve(const std::shared_ptr<GraphicsDevice>& device,
            const std::shared_ptr<RenderTarget>& source);

        /// The target it resolves; null once that target is gone.
        std::shared_ptr<RenderTarget> source() const { return _source.lock(); }

        /// Sets the pass up to render into the source's resolve buffer, when that changed.
        void update();

        void prepareShaders() override;
        void execute() override;

    private:
        std::weak_ptr<RenderTarget> _source;
        Texture* _resolveTexture = nullptr;
        int _resolveWidth = 0;
        int _resolveHeight = 0;
    };
}
