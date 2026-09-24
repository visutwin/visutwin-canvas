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
            // Soft-knee high pass on the output (upstream's PREFILTER), for the first
            // bloom downsample. A separate shader variant, so leave it off at threshold 0.
            // Applies to the Karis filter only, as upstream's bloom uses it.
            bool prefilter = false;
        };

        RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture);
        RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
            const Options& options);

        void setSourceTexture(Texture* value);

        bool prefilter() const { return _prefilter; }
        // Brightness below which the output is scaled down, and the width of the
        // quadratic transition beneath it. Ignored unless built with `prefilter`.
        void setPrefilterThreshold(const float value) { _prefilterThreshold = value; }
        void setPrefilterKnee(const float value) { _prefilterKnee = value; }
        void execute() override;

    private:
        Texture* _sourceTexture = nullptr;
        Texture* _premultiplyTexture = nullptr;
        Options _options;
        bool _prefilter = false;
        float _prefilterThreshold = 0.0f;
        float _prefilterKnee = 0.0f;
        float _sourceInvResolution[2] = {1.0f, 1.0f};
    };
}
