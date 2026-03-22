// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class RenderPassDownsample : public RenderPassShaderQuad
    {
    public:
        struct Options
        {
            bool boxFilter = false;
            Texture* premultiplyTexture = nullptr;
            char premultiplySrcChannel = 'x';
            bool removeInvalid = false;
        };

        RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture);
        RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
            const Options& options);

        void setSourceTexture(Texture* value);
        void execute() override;

    private:
        Texture* _sourceTexture = nullptr;
        Texture* _premultiplyTexture = nullptr;
        Options _options;
        float _sourceInvResolution[2] = {1.0f, 1.0f};
    };
}
